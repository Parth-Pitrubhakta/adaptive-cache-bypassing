/*
 * I-DSB-BBtracking Implementation
 *
 * File layout
 * ───────────
 * 1. Constructor
 * 2. RNG + small helpers
 * 3. Bypass Buffer (BB) helpers
 * 4. SLRU helpers
 * 5. Set-dueling helpers
 * 6. should_bypass()          ← core bypass decision
 * 7. find_victim()            ← ChampSim hook: returns LLC way or BB_TOTAL
 *                               (BB_TOTAL signals "bypass this block")
 * 8. replacement_cache_fill() ← called after a miss is filled into a way
 * 9. update_replacement_state() ← called on every hit
 *10. replacement_final_stats()
 *
 * Bypass signalling convention (ChampSim limitation workaround)
 * ─────────────────────────────────────────────────────────────
 * ChampSim's replacement interface returns a *way index* from find_victim().
 * To signal "bypass this block" we return the sentinel value NUM_WAY.
 * ChampSim's cache fill logic checks for this sentinel and skips the fill
 * into the LLC (the block is still forwarded to upper levels by the cache
 * pipeline).  We then insert the tag into the BB here.
 *
 * NOTE: ChampSim must have bypass support compiled in for the sentinel to
 * work.  In standard ChampSim the sentinel is already defined — see
 * test/cpp/src/441-replacement-bypass.cc for the expected contract.
 */

#include "bypass_buffer_dsb.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <iterator>
#include <random>

// ═══════════════════════════════════════════════════════════════
// 1. Constructor
// ═══════════════════════════════════════════════════════════════
bypass_buffer_dsb::bypass_buffer_dsb(CACHE* cache)
    : replacement(cache),
      NUM_SET(cache->NUM_SET),
      NUM_WAY(cache->NUM_WAY),
      rrpv(static_cast<std::size_t>(NUM_SET * NUM_WAY), RRPV_LONG),
      bb(static_cast<std::size_t>(BB_TOTAL)),
      bb_lru(static_cast<std::size_t>(BB_TOTAL), 0)
{
  // ── PSEL counters (one per CPU) ──────────────────────────────
  psel.assign(NUM_CPUS, PSEL_INIT);

  // ── Random sampler sets for set-dueling ─────────────────────
  // NUM_CPUS × NUM_POLICY × SDM_SIZE sets
  std::size_t total_sdm = NUM_CPUS * NUM_POLICY * SDM_SIZE;
  std::knuth_b rng{42};
  rand_sets.resize(total_sdm);
  std::generate(rand_sets.begin(), rand_sets.end(), [&]() {
    return static_cast<std::size_t>(rng()) % static_cast<std::size_t>(NUM_SET);
  });
  std::sort(rand_sets.begin(), rand_sets.end());
}

// ═══════════════════════════════════════════════════════════════
// 2. RNG
// ═══════════════════════════════════════════════════════════════
uint64_t bypass_buffer_dsb::next_rand()
{
  // Xorshift64
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}

// ═══════════════════════════════════════════════════════════════
// 3. Bypass Buffer helpers
// ═══════════════════════════════════════════════════════════════

// Map a block address to a BB set index (simple modulo)
long bypass_buffer_dsb::bb_set_of(uint64_t addr) const
{
  return static_cast<long>((addr >> 6) % static_cast<uint64_t>(BB_SETS));
}

// Linear search within a BB set for a matching tag
long bypass_buffer_dsb::bb_find(uint64_t addr)
{
  long base = bb_set_of(addr) * BB_WAYS;
  for (long w = 0; w < BB_WAYS; ++w) {
    long idx = base + w;
    if (bb[idx].valid && bb[idx].tag == addr)
      return idx;
  }
  return -1;
}

// Choose LRU victim within a BB set
long bypass_buffer_dsb::bb_find_victim(long bb_set)
{
  long base = bb_set * BB_WAYS;
  long victim = base;
  for (long w = 1; w < BB_WAYS; ++w) {
    long idx = base + w;
    // Prefer invalid slots first
    if (!bb[idx].valid)
      return idx;
    if (bb_lru[idx] < bb_lru[victim])
      victim = idx;
  }
  return victim;
}

/*
 * Insert a new entry into the BB.
 * If BB set is full the LRU entry is evicted (with back-invalidation).
 *
 * addr       – cache-line aligned block address
 * virt       – true if this is a virtual-bypass entry
 * comp_set   – LLC set of the competitor block
 * comp_way   – LLC way of the competitor block
 */
void bypass_buffer_dsb::bb_insert(uint64_t addr, bool virt,
                                   long comp_set, long comp_way)
{
  long bs   = bb_set_of(addr);
  long base = bs * BB_WAYS;

  // Check for an existing invalid slot
  long slot = -1;
  for (long w = 0; w < BB_WAYS; ++w) {
    if (!bb[base + w].valid) {
      slot = base + w;
      break;
    }
  }

  // If no free slot, evict LRU
  if (slot < 0) {
    slot = bb_find_victim(bs);
    if (bb[slot].valid)
      bb_evict(slot); // back-invalidate upper caches
  }

  bb[slot].valid          = true;
  bb[slot].virtual_bypass = virt;
  bb[slot].tag            = addr;
  bb[slot].competitor_set = comp_set;
  bb[slot].competitor_way = comp_way;
  bb_lru[slot]            = ++bb_cycle;
}

/*
 * Evict a BB entry.
 * In a real implementation this would issue back-invalidations to L1/L2.
 * In ChampSim we model this by marking the entry invalid; the actual
 * coherence cost is approximated (ChampSim does not model back-inval
 * explicitly, but the performance effect is captured because the block
 * stays out of the LLC).
 */
void bypass_buffer_dsb::bb_evict(long bb_idx)
{
  if (!bb[bb_idx].valid)
    return;

  // ── Back-invalidation (modelled): ──────────────────────────
  // In hardware the BB would send an invalidation to upper-level caches
  // for bb[bb_idx].tag.  ChampSim does not expose an explicit
  // back-invalidation API, so we count the event for stats and rely on
  // the fact that the block was bypassed (hence not live in L2/L1 under
  // a well-designed bypassing algorithm) to approximate the behaviour.
  stat_bb_evict_invalidations++;

  bb[bb_idx].valid = false;
}

// ═══════════════════════════════════════════════════════════════
// 4. SLRU helpers
// ═══════════════════════════════════════════════════════════════
unsigned& bypass_buffer_dsb::get_rrpv(long set, long way)
{
  return rrpv.at(static_cast<std::size_t>(set * NUM_WAY + way));
}

// On a cache hit – promote to MRU (RRPV = 0)
void bypass_buffer_dsb::update_slru_hit(long set, long way)
{
  get_rrpv(set, way) = RRPV_NEAR;
}

/*
 * On a cache fill – insert at distant position (RRPV = 3) for SRRIP,
 * or at long position with occasional near for BIP.
 */
void bypass_buffer_dsb::update_slru_miss(long set, long way, bool use_bip)
{
  if (use_bip) {
    get_rrpv(set, way) = RRPV_LONG;
    // Bimodal: occasionally insert near
    if ((next_rand() % 32) == 0)
      get_rrpv(set, way) = RRPV_DISTANT;
  } else {
    // SRRIP: insert at distant-1
    get_rrpv(set, way) = RRPV_DISTANT;
  }
}

// ═══════════════════════════════════════════════════════════════
// 5. Set-dueling helpers
// ═══════════════════════════════════════════════════════════════

// Check if a given LLC set is a sampler set for a specific policy
bool bypass_buffer_dsb::is_sdm_set(uint32_t cpu, long set,
                                    std::size_t policy) const
{
  std::size_t begin_idx = (cpu * NUM_POLICY + policy) * SDM_SIZE;
  std::size_t end_idx   = begin_idx + SDM_SIZE;
  for (std::size_t i = begin_idx; i < end_idx && i < rand_sets.size(); ++i)
    if (rand_sets[i] == static_cast<std::size_t>(set))
      return true;
  return false;
}

// Returns true if BIP should be used for this set (based on PSEL counter)
bool bypass_buffer_dsb::use_bip_for_set(uint32_t cpu, long set) const
{
  // Leader sets vote directly; follower sets follow the PSEL winner
  if (is_sdm_set(cpu, set, 0)) return false; // SRRIP leader
  if (is_sdm_set(cpu, set, 1)) return true;  // BIP leader
  // Follower: PSEL > half → BIP wins
  return (psel[cpu] > (PSEL_INIT));
}

// ═══════════════════════════════════════════════════════════════
// 6. Bypass decision
// ═══════════════════════════════════════════════════════════════
/*
 * Decide whether to bypass this block from the LLC.
 *
 * Algorithm (adapted from DSB paper Section III-A):
 *   - With probability bypass_probability/BYPASS_PROB_MAX, bypass.
 *   - The probability is updated in replacement_cache_fill() and
 *     update_replacement_state() based on competitor pointer outcomes.
 *
 * Returns true  → bypass (block goes to BB + upper caches only)
 *         false → allocate in LLC as normal
 */
bool bypass_buffer_dsb::should_bypass(uint32_t /*cpu*/, long /*set*/,
                                       const champsim::cache_block* /*cs*/,
                                       champsim::address /*full_addr*/)
{
  uint64_t r = next_rand() % (BYPASS_PROB_MAX + 1);
  return r < bypass_probability;
}

// ═══════════════════════════════════════════════════════════════
// 7. find_victim()
// ═══════════════════════════════════════════════════════════════
/*
 * ChampSim calls this to select which way to evict before a fill.
 *
 * Our extension:
 *   Return NUM_WAY (sentinel) to signal "bypass this fill".
 *   In that case replacement_cache_fill() will see way == NUM_WAY and
 *   insert the tag into the BB.
 */
long bypass_buffer_dsb::find_victim(uint32_t triggering_cpu,
                                     uint64_t /*instr_id*/, long set,
                                     const champsim::cache_block* current_set,
                                     champsim::address ip,
                                     champsim::address full_addr,
                                     access_type type)
{
  // Don't bypass writebacks
  if (access_type{type} == access_type::WRITE)
    goto normal_victim;

  // ── Bypass decision ─────────────────────────────────────────
  if (should_bypass(triggering_cpu, set, current_set, full_addr)) {
    stat_bypassed++;
    return NUM_WAY; // bypass sentinel
  }

normal_victim:
  {
    // ── Normal SLRU victim selection ──────────────────────────
    // Find the way with the maximum RRPV
    auto begin = std::next(std::begin(rrpv), set * NUM_WAY);
    auto end   = std::next(begin, NUM_WAY);

    auto victim = std::max_element(begin, end);

    // If no RRPV == maxRRPV, age all lines
    if (*victim < RRPV_LONG) {
      unsigned delta = RRPV_LONG - *victim;
      for (auto it = begin; it != end; ++it)
        *it = std::min(*it + delta, RRPV_LONG);
      victim = std::max_element(begin, end);
    }

    assert(begin <= victim && victim < end);
    return std::distance(begin, victim);
  }
}

// ═══════════════════════════════════════════════════════════════
// 8. replacement_cache_fill()
// ═══════════════════════════════════════════════════════════════
/*
 * Called by ChampSim after a block is placed into 'way' of 'set'.
 * If way == NUM_WAY the block was bypassed — insert tag into BB.
 *
 * Also handles virtual-bypass sampling:
 *   Roughly 1-in-VBYPASS_SAMPLE fills are selected for virtual bypass
 *   to assess what would happen if we bypassed.  In that case a BB
 *   entry is created for the evicted (competitor) block, with the
 *   competitor pointer pointing to the newly filled block's way.
 */
void bypass_buffer_dsb::replacement_cache_fill(uint32_t triggering_cpu,
                                                long set, long way,
                                                champsim::address full_addr,
                                                champsim::address /*ip*/,
                                                champsim::address victim_addr,
                                                access_type type)
{
  if (access_type{type} == access_type::WRITE)
    return;

  uint64_t addr        = full_addr.to<uint64_t>() & ~63ULL; // cache-line align
  uint64_t victim_line = victim_addr.to<uint64_t>() & ~63ULL;

  if (way == NUM_WAY) {
    // ── Real bypass: insert tag into BB ───────────────────────
    // Competitor is the way that *would* have been evicted.
    // Find the SLRU LRU way to use as competitor.
    auto begin  = std::next(std::begin(rrpv), set * NUM_WAY);
    auto end    = std::next(begin, NUM_WAY);
    long comp_w = static_cast<long>(
        std::distance(begin, std::max_element(begin, end)));

    bb_insert(addr, /*virtual=*/false, set, comp_w);
    return;
  }

  // ── Normal fill: update SLRU state ───────────────────────────
  bool bip = use_bip_for_set(triggering_cpu, set);
  update_slru_miss(set, way, bip);

  // Update PSEL for sampler sets
  if (is_sdm_set(triggering_cpu, set, 0)) {
    // SRRIP leader
    if (psel[triggering_cpu] > 0) psel[triggering_cpu]--;
  } else if (is_sdm_set(triggering_cpu, set, 1)) {
    // BIP leader
    if (psel[triggering_cpu] < PSEL_MAX) psel[triggering_cpu]++;
  }

  // ── Virtual bypass sampling ───────────────────────────────────
  if ((next_rand() % VBYPASS_SAMPLE) == 0 && victim_line != 0) {
    // Create a BB entry for the evicted block (competitor = new block's way)
    bb_insert(victim_line, /*virtual=*/true, set, way);
  }

  // ── Check if this fill invalidates a BB competitor pointer ────
  // When a fill lands at (set, way), any BB entry whose competitor
  // points to this (set, way) is no longer valid for tracking.
  // Invalidate such entries (no back-inval needed — they were virtual).
  for (std::size_t i = 0; i < static_cast<std::size_t>(BB_TOTAL); ++i) {
    if (bb[i].valid && bb[i].competitor_set == set
        && bb[i].competitor_way == way) {
      bb[i].valid = false;
    }
  }
}

// ═══════════════════════════════════════════════════════════════
// 9. update_replacement_state()
// ═══════════════════════════════════════════════════════════════
/*
 * Called on every cache HIT.
 *
 * Additionally, if the hit address matches a BB entry we know the
 * bypassed block is still live in upper caches — this means the bypass
 * decision was *ineffective* (the block was reused before it died).
 * We decrease bypass_probability.
 *
 * If a competitor block is hit we know the competitor is live and was
 * accessed before the bypassed block expired — bypass was *effective*.
 * We increase bypass_probability.
 */
void bypass_buffer_dsb::update_replacement_state(uint32_t triggering_cpu,
                                                   long set, long way,
                                                   champsim::address full_addr,
                                                   champsim::address /*ip*/,
                                                   champsim::address /*victim*/,
                                                   access_type type,
                                                   uint8_t hit)
{
  if (access_type{type} == access_type::WRITE)
    return;

  if (hit) {
    update_slru_hit(set, way);
  }

  uint64_t addr = full_addr.to<uint64_t>() & ~63ULL;

  // ── Check if the hit address appears in the BB ────────────────
  long bb_idx = bb_find(addr);
  if (bb_idx >= 0) {
    stat_bb_hits++;
    BBEntry& entry = bb[bb_idx];

    if (entry.virtual_bypass) {
      // Virtual bypass: the bypassed (evicted) block was re-accessed before
      // the new (competitor) block → bypassing would have been effective.
      stat_bypass_effective++;
      if (bypass_probability < BYPASS_PROB_MAX)
        bypass_probability = std::min(bypass_probability + BYPASS_PROB_STEP,
                                      (unsigned)BYPASS_PROB_MAX);
    } else {
      // Real bypass: the bypassed block is being re-accessed from upper
      // cache → bypass was INEFFECTIVE (block had reuse value).
      stat_bypass_ineffective++;
      if (bypass_probability >= BYPASS_PROB_STEP)
        bypass_probability -= BYPASS_PROB_STEP;
    }

    // Invalidate BB entry (tracking consumed)
    bb[bb_idx].valid = false;
    return;
  }

  // ── Check if the hit is on a *competitor* of a BB entry ───────
  // Competitor hit means: the block that would have been evicted (without
  // bypass) is still alive and being used → bypass was EFFECTIVE.
  for (std::size_t i = 0; i < static_cast<std::size_t>(BB_TOTAL); ++i) {
    BBEntry& entry = bb[i];
    if (!entry.valid) continue;
    if (entry.competitor_set != set || entry.competitor_way != way) continue;

    if (!entry.virtual_bypass) {
      // Real bypass competitor hit → bypass was effective
      stat_bypass_effective++;
      if (bypass_probability < BYPASS_PROB_MAX)
        bypass_probability = std::min(bypass_probability + BYPASS_PROB_STEP,
                                      (unsigned)BYPASS_PROB_MAX);
    } else {
      // Virtual bypass competitor hit → NOT bypassing was better here
      stat_bypass_ineffective++;
      if (bypass_probability >= BYPASS_PROB_STEP)
        bypass_probability -= BYPASS_PROB_STEP;
    }

    // Invalidate: tracking pair consumed
    entry.valid = false;
    break;
  }
}

// ═══════════════════════════════════════════════════════════════
// 10. replacement_final_stats()
// ═══════════════════════════════════════════════════════════════
void bypass_buffer_dsb::replacement_final_stats()
{
  std::printf("\n[bypass_buffer_dsb] === Final Stats ===\n");
  std::printf("  Blocks bypassed (tags in BB)     : %lu\n", stat_bypassed);
  std::printf("  BB hits (bypassed block reused)  : %lu\n", stat_bb_hits);
  std::printf("  BB eviction back-invals (modeled): %lu\n",
              stat_bb_evict_invalidations);
  std::printf("  Bypass judged EFFECTIVE          : %lu\n",
              stat_bypass_effective);
  std::printf("  Bypass judged INEFFECTIVE        : %lu\n",
              stat_bypass_ineffective);
  std::printf("  Final bypass probability         : %u / %u (%.1f%%)\n",
              bypass_probability, BYPASS_PROB_MAX,
              100.0 * bypass_probability / BYPASS_PROB_MAX);

  if (stat_bypass_effective + stat_bypass_ineffective > 0) {
    double eff_pct = 100.0 * static_cast<double>(stat_bypass_effective)
                     / static_cast<double>(stat_bypass_effective + stat_bypass_ineffective);
    std::printf("  Bypass effectiveness rate        : %.1f%%\n", eff_pct);
  }
}
