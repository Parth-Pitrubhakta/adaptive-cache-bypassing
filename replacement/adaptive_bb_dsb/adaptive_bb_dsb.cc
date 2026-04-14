/*
 * Adaptive-BB I-DSB-BBtracking — Implementation
 * ═════════════════════════════════════════════════════════════════════════════
 *
 * File layout
 * ───────────
 *  1.  Constructor
 *  2.  RNG
 *  3.  SLRU helpers
 *  4.  Bypass Buffer (BB) helpers
 *  5.  Adaptive resize logic          ← NOVELTY
 *  6.  should_bypass()
 *  7.  find_victim()
 *  8.  replacement_cache_fill()
 *  9.  update_replacement_state()
 * 10.  replacement_final_stats()
 *
 * Sections marked ★ contain the novel adaptive resize logic.
 * All other sections are faithful reproductions of the original
 * bypass_buffer_dsb implementation so that comparisons are fair.
 *
 * Bypass sentinel convention (unchanged):
 *   Return NUM_WAY from find_victim() to signal "bypass this block".
 */

#include "adaptive_bb_dsb.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <iterator>
#include <numeric>
#include <random>

// ═════════════════════════════════════════════════════════════════════════════
// 1. Constructor
// ═════════════════════════════════════════════════════════════════════════════
adaptive_bb_dsb::adaptive_bb_dsb(CACHE* cache)
    : replacement(cache),
      NUM_SET(cache->NUM_SET),
      NUM_WAY(cache->NUM_WAY),
      rrpv(static_cast<std::size_t>(NUM_SET * NUM_WAY), RRPV_LONG),
      // Allocate full physical BB at maximum size; only active window is used
      bb(static_cast<std::size_t>(BB_MAX_ENTRIES)),
      bb_lru(static_cast<std::size_t>(BB_MAX_ENTRIES), 0),
      bb_active_sets(BB_INIT_SETS)
{
  psel.assign(NUM_CPUS, PSEL_INIT);

  std::size_t total_sdm = NUM_CPUS * NUM_POLICY * SDM_SIZE;
  std::knuth_b rng{42};
  rand_sets.resize(total_sdm);
  std::generate(rand_sets.begin(), rand_sets.end(), [&]() {
    return static_cast<std::size_t>(rng()) % static_cast<std::size_t>(NUM_SET);
  });
  std::sort(rand_sets.begin(), rand_sets.end());
}

// ═════════════════════════════════════════════════════════════════════════════
// 2. RNG (Xorshift64, unchanged)
// ═════════════════════════════════════════════════════════════════════════════
uint64_t adaptive_bb_dsb::next_rand()
{
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}

// ═════════════════════════════════════════════════════════════════════════════
// 3. SLRU helpers (unchanged from original)
// ═════════════════════════════════════════════════════════════════════════════
unsigned& adaptive_bb_dsb::get_rrpv(long set, long way)
{
  return rrpv.at(static_cast<std::size_t>(set * NUM_WAY + way));
}

void adaptive_bb_dsb::update_slru_hit(long set, long way)
{
  get_rrpv(set, way) = RRPV_NEAR;
}

void adaptive_bb_dsb::update_slru_miss(long set, long way, bool use_bip)
{
  if (use_bip) {
    get_rrpv(set, way) = RRPV_LONG;
    if ((next_rand() % 32) == 0)
      get_rrpv(set, way) = RRPV_DISTANT;
  } else {
    get_rrpv(set, way) = RRPV_DISTANT;
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// 4. Bypass Buffer helpers
// ═════════════════════════════════════════════════════════════════════════════

/*
 * Map block address to BB set index.
 * ★ ADAPTIVE: the modulo uses bb_active_sets (not a compile-time constant)
 *   so the mapping automatically rebalances when the BB grows or shrinks.
 *   This is safe because the BB is searched linearly within a set — there is
 *   no "stale tag in wrong set" hazard: when we shrink, entries in the
 *   removed sets are invalidated by shrink_bb() before the mapping changes.
 */
long adaptive_bb_dsb::bb_set_of(uint64_t addr) const
{
  return static_cast<long>((addr >> 6) % static_cast<uint64_t>(bb_active_sets));
}

// Search the active window for a matching tag
long adaptive_bb_dsb::bb_find(uint64_t addr)
{
  long base = bb_set_of(addr) * BB_WAYS;
  for (long w = 0; w < BB_WAYS; ++w) {
    long idx = base + w;
    if (bb[idx].valid && bb[idx].tag == addr)
      return idx;
  }
  return -1;
}

// LRU victim within a BB set (prefers invalid slots)
long adaptive_bb_dsb::bb_find_victim(long bb_set)
{
  long base   = bb_set * BB_WAYS;
  long victim = base;
  for (long w = 0; w < BB_WAYS; ++w) {
    long idx = base + w;
    if (!bb[idx].valid) return idx;
    if (bb_lru[idx] < bb_lru[victim]) victim = idx;
  }
  return victim;
}

void adaptive_bb_dsb::bb_insert(uint64_t addr, bool virt,
                                 long comp_set, long comp_way)
{
  long bs   = bb_set_of(addr);
  long base = bs * BB_WAYS;

  long slot = -1;
  for (long w = 0; w < BB_WAYS; ++w) {
    if (!bb[base + w].valid) { slot = base + w; break; }
  }

  if (slot < 0) {
    // No free slot — evict LRU, count as a capacity-driven eviction
    slot = bb_find_victim(bs);
    if (bb[slot].valid) {
      bb_evict(slot);
      interval_bb_evictions++;   // ★ track for resize pressure
    }
  }

  bb[slot].valid          = true;
  bb[slot].virtual_bypass = virt;
  bb[slot].tag            = addr;
  bb[slot].competitor_set = comp_set;
  bb[slot].competitor_way = comp_way;
  bb_lru[slot]            = ++bb_cycle;

  interval_bb_insertions++;      // ★ track for resize pressure
}

void adaptive_bb_dsb::bb_evict(long bb_idx)
{
  if (!bb[bb_idx].valid) return;
  stat_bb_evict_invalidations++;
  bb[bb_idx].valid = false;
}

// ═════════════════════════════════════════════════════════════════════════════
// 5. ★ NOVEL: Adaptive resize logic
// ═════════════════════════════════════════════════════════════════════════════

/*
 * grow_bb()
 * ─────────
 * Increases bb_active_sets by RESIZE_STEP (capped at BB_MAX_SETS).
 * The new slots are already allocated in bb[] and pre-initialised to
 * invalid=false in the constructor, so no extra work is needed — they
 * simply become reachable by bb_set_of() and bb_insert().
 */
void adaptive_bb_dsb::grow_bb()
{
  if (bb_active_sets >= BB_MAX_SETS) return;
  bb_active_sets = std::min(bb_active_sets + RESIZE_STEP, BB_MAX_SETS);
  stat_resize_grow_events++;
}

/*
 * shrink_bb()
 * ───────────
 * Decreases bb_active_sets by RESIZE_STEP (capped at BB_MIN_SETS).
 *
 * IMPORTANT: Before reducing bb_active_sets we MUST invalidate every
 * entry in the sets that are about to leave the active window.  If we
 * didn't, those entries could be "stranded" — their tags would no longer
 * be reachable (bb_set_of would hash them to a different set), so they
 * would never receive back-invalidations, violating inclusion.
 *
 * We model the back-invalidation by incrementing stat_bb_evict_invalidations
 * for each valid entry we clear — same as bb_evict() does.
 */
void adaptive_bb_dsb::shrink_bb()
{
  if (bb_active_sets <= BB_MIN_SETS) return;

  long new_active_sets = std::max(bb_active_sets - RESIZE_STEP, BB_MIN_SETS);

  // Invalidate entries in sets [new_active_sets .. bb_active_sets)
  for (long s = new_active_sets; s < bb_active_sets; ++s) {
    long base = s * BB_WAYS;
    for (long w = 0; w < BB_WAYS; ++w) {
      long idx = base + w;
      if (bb[idx].valid) {
        stat_bb_evict_invalidations++;  // model back-invalidation
        bb[idx].valid = false;
      }
    }
  }

  bb_active_sets = new_active_sets;
  stat_resize_shrink_events++;
}

/*
 * maybe_resize()
 * ──────────────
 * Called on every LLC access.  Every RESIZE_INTERVAL accesses, evaluate
 * the two pressure signals and decide whether to grow, shrink, or hold.
 *
 * Signal 1 — EVICTION PRESSURE (BB too small):
 *   eviction_ratio = interval_bb_evictions / interval_bb_insertions
 *   If eviction_ratio > EVICT_PRESSURE_NUM / EVICT_PRESSURE_DENOM → grow.
 *
 * Signal 2 — IDLE PRESSURE (BB too large):
 *   Count valid entries in the active window.
 *   utilisation = valid_count / bb_active_capacity()
 *   If utilisation < IDLE_THRESHOLD_NUM / IDLE_THRESHOLD_DENOM → shrink.
 *
 * Grow takes precedence over shrink: if the BB is simultaneously under
 * eviction pressure AND underutilised (pathological case), we grow — a
 * small and underutilised BB suggests all valid entries are very recently
 * inserted and haven't had time to die naturally, so more space is needed.
 *
 * After evaluation, interval counters are reset for the next window.
 */
void adaptive_bb_dsb::maybe_resize()
{
  resize_access_counter++;
  if (resize_access_counter < RESIZE_INTERVAL) return;
  resize_access_counter = 0;

  // ── Record size sample for reporting ────────────────────────────────────
  long current_size = bb_active_capacity();
  stat_bb_size_sum     += static_cast<uint64_t>(current_size);
  stat_bb_size_samples++;
  if (current_size < stat_bb_size_min) stat_bb_size_min = current_size;
  if (current_size > stat_bb_size_max) stat_bb_size_max = current_size;

  // ── Signal 1: eviction pressure ─────────────────────────────────────────
  bool eviction_pressure = false;
  if (interval_bb_insertions > 0) {
    // Use integer cross-multiplication to avoid floating point:
    //   eviction_ratio > NUM/DENOM
    //   ⟺  interval_bb_evictions * DENOM > interval_bb_insertions * NUM
    eviction_pressure =
      (interval_bb_evictions * EVICT_PRESSURE_DENOM) >
      (interval_bb_insertions * EVICT_PRESSURE_NUM);
  }

  // ── Signal 2: idle pressure ──────────────────────────────────────────────
  bool idle_pressure = false;
  if (bb_active_capacity() > 0) {
    long valid_count = 0;
    for (long i = 0; i < bb_active_capacity(); ++i)
      if (bb[i].valid) valid_count++;

    // valid_count / capacity < IDLE_THRESHOLD_NUM / IDLE_THRESHOLD_DENOM
    // ⟺  valid_count * IDLE_THRESHOLD_DENOM < capacity * IDLE_THRESHOLD_NUM
    idle_pressure =
      (static_cast<unsigned>(valid_count) * IDLE_THRESHOLD_DENOM) <
      (static_cast<unsigned>(bb_active_capacity()) * IDLE_THRESHOLD_NUM);
  }

  // ── Decision ─────────────────────────────────────────────────────────────
  if (eviction_pressure) {
    grow_bb();    // need more room for bypassed tags
  } else if (idle_pressure) {
    shrink_bb();  // wasting hardware; give it back
  }
  // else: BB is well-sized — no change

  // ── Reset interval counters ───────────────────────────────────────────────
  interval_bb_insertions = 0;
  interval_bb_evictions  = 0;
}

// ═════════════════════════════════════════════════════════════════════════════
// Set dueling helpers (unchanged from original)
// ═════════════════════════════════════════════════════════════════════════════
bool adaptive_bb_dsb::is_sdm_set(uint32_t cpu, long set,
                                   std::size_t policy) const
{
  std::size_t begin_idx = (cpu * NUM_POLICY + policy) * SDM_SIZE;
  std::size_t end_idx   = begin_idx + SDM_SIZE;
  for (std::size_t i = begin_idx; i < end_idx && i < rand_sets.size(); ++i)
    if (rand_sets[i] == static_cast<std::size_t>(set))
      return true;
  return false;
}

bool adaptive_bb_dsb::use_bip_for_set(uint32_t cpu, long set) const
{
  if (is_sdm_set(cpu, set, 0)) return false;
  if (is_sdm_set(cpu, set, 1)) return true;
  return (psel[cpu] > PSEL_INIT);
}

// ═════════════════════════════════════════════════════════════════════════════
// 6. Bypass decision (unchanged from original)
// ═════════════════════════════════════════════════════════════════════════════
bool adaptive_bb_dsb::should_bypass(uint32_t /*cpu*/, long /*set*/,
                                     const champsim::cache_block* /*cs*/,
                                     champsim::address /*full_addr*/)
{
  uint64_t r = next_rand() % (BYPASS_PROB_MAX + 1);
  return r < bypass_probability;
}

// ═════════════════════════════════════════════════════════════════════════════
// 7. find_victim()
// ═════════════════════════════════════════════════════════════════════════════
long adaptive_bb_dsb::find_victim(uint32_t triggering_cpu,
                                   uint64_t /*instr_id*/, long set,
                                   const champsim::cache_block* current_set,
                                   champsim::address ip,
                                   champsim::address full_addr,
                                   access_type type)
{
  // ★ Tick the resize counter on every access
  maybe_resize();

  if (access_type{type} == access_type::WRITE)
    goto normal_victim;

  if (should_bypass(triggering_cpu, set, current_set, full_addr)) {
    stat_bypassed++;
    return NUM_WAY;  // bypass sentinel
  }

normal_victim:
  {
    auto begin  = std::next(std::begin(rrpv), set * NUM_WAY);
    auto end    = std::next(begin, NUM_WAY);
    auto victim = std::max_element(begin, end);

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

// ═════════════════════════════════════════════════════════════════════════════
// 8. replacement_cache_fill()
// ═════════════════════════════════════════════════════════════════════════════
void adaptive_bb_dsb::replacement_cache_fill(uint32_t triggering_cpu,
                                               long set, long way,
                                               champsim::address full_addr,
                                               champsim::address /*ip*/,
                                               champsim::address victim_addr,
                                               access_type type)
{
  if (access_type{type} == access_type::WRITE) return;

  uint64_t addr        = full_addr.to<uint64_t>()   & ~63ULL;
  uint64_t victim_line = victim_addr.to<uint64_t>()  & ~63ULL;

  if (way == NUM_WAY) {
    // Real bypass: insert tag into active BB window
    auto begin  = std::next(std::begin(rrpv), set * NUM_WAY);
    auto end    = std::next(begin, NUM_WAY);
    long comp_w = static_cast<long>(
        std::distance(begin, std::max_element(begin, end)));
    bb_insert(addr, /*virtual=*/false, set, comp_w);
    return;
  }

  // Normal fill
  bool bip = use_bip_for_set(triggering_cpu, set);
  update_slru_miss(set, way, bip);

  if (is_sdm_set(triggering_cpu, set, 0)) {
    if (psel[triggering_cpu] > 0) psel[triggering_cpu]--;
  } else if (is_sdm_set(triggering_cpu, set, 1)) {
    if (psel[triggering_cpu] < PSEL_MAX) psel[triggering_cpu]++;
  }

  // Virtual bypass sampling
  if ((next_rand() % VBYPASS_SAMPLE) == 0 && victim_line != 0)
    bb_insert(victim_line, /*virtual=*/true, set, way);

  // Invalidate BB entries whose competitor pointer this fill overwrites
  for (long i = 0; i < bb_active_capacity(); ++i) {
    if (bb[i].valid && bb[i].competitor_set == set
        && bb[i].competitor_way == way) {
      bb[i].valid = false;
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// 9. update_replacement_state()
// ═════════════════════════════════════════════════════════════════════════════
void adaptive_bb_dsb::update_replacement_state(uint32_t /*triggering_cpu*/,
                                                long set, long way,
                                                champsim::address full_addr,
                                                champsim::address /*ip*/,
                                                champsim::address /*victim*/,
                                                access_type type, uint8_t hit)
{
  if (access_type{type} == access_type::WRITE) return;

  if (hit) update_slru_hit(set, way);

  uint64_t addr = full_addr.to<uint64_t>() & ~63ULL;

  // Check if hit address is in BB
  long bb_idx = bb_find(addr);
  if (bb_idx >= 0) {
    stat_bb_hits++;
    AdaptiveBBEntry& entry = bb[bb_idx];

    if (entry.virtual_bypass) {
      stat_bypass_effective++;
      bypass_probability = std::min(bypass_probability + BYPASS_PROB_STEP,
                                    (unsigned)BYPASS_PROB_MAX);
    } else {
      stat_bypass_ineffective++;
      if (bypass_probability >= BYPASS_PROB_STEP)
        bypass_probability -= BYPASS_PROB_STEP;
    }
    bb[bb_idx].valid = false;
    return;
  }

  // Check if hit is on a BB entry's competitor
  for (long i = 0; i < bb_active_capacity(); ++i) {
    AdaptiveBBEntry& entry = bb[i];
    if (!entry.valid) continue;
    if (entry.competitor_set != set || entry.competitor_way != way) continue;

    if (!entry.virtual_bypass) {
      stat_bypass_effective++;
      bypass_probability = std::min(bypass_probability + BYPASS_PROB_STEP,
                                    (unsigned)BYPASS_PROB_MAX);
    } else {
      stat_bypass_ineffective++;
      if (bypass_probability >= BYPASS_PROB_STEP)
        bypass_probability -= BYPASS_PROB_STEP;
    }
    entry.valid = false;
    break;
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// 10. replacement_final_stats()
// ═════════════════════════════════════════════════════════════════════════════
void adaptive_bb_dsb::replacement_final_stats()
{
  // ── Original stats (for apples-to-apples comparison) ──────────────────────
  std::printf("\n[adaptive_bb_dsb] === Final Stats ===\n");
  std::printf("  Blocks bypassed (tags in BB)       : %lu\n", stat_bypassed);
  std::printf("  BB hits (bypassed block reused)    : %lu\n", stat_bb_hits);
  std::printf("  BB eviction back-invals (modeled)  : %lu\n",
              stat_bb_evict_invalidations);
  std::printf("  Bypass judged EFFECTIVE            : %lu\n",
              stat_bypass_effective);
  std::printf("  Bypass judged INEFFECTIVE          : %lu\n",
              stat_bypass_ineffective);
  std::printf("  Final bypass probability           : %u / %u (%.1f%%)\n",
              bypass_probability, BYPASS_PROB_MAX,
              100.0 * bypass_probability / BYPASS_PROB_MAX);

  if (stat_bypass_effective + stat_bypass_ineffective > 0) {
    double eff_pct = 100.0 * static_cast<double>(stat_bypass_effective)
                   / static_cast<double>(stat_bypass_effective
                                         + stat_bypass_ineffective);
    std::printf("  Bypass effectiveness rate          : %.1f%%\n", eff_pct);
  }

  // ── ★ Adaptive resize stats ───────────────────────────────────────────────
  std::printf("\n  [Adaptive BB Resize Stats]\n");
  std::printf("  Initial BB size (entries)          : %ld\n",
              BB_INIT_SETS * BB_WAYS);
  std::printf("  Final BB size   (entries)          : %ld\n",
              bb_active_capacity());
  std::printf("  BB size min observed (entries)     : %ld\n",
              stat_bb_size_min);
  std::printf("  BB size max observed (entries)     : %ld\n",
              stat_bb_size_max);

  if (stat_bb_size_samples > 0) {
    double avg = static_cast<double>(stat_bb_size_sum)
               / static_cast<double>(stat_bb_size_samples);
    std::printf("  BB size average (entries)          : %.1f\n", avg);
  }

  std::printf("  Grow  events                       : %lu\n",
              stat_resize_grow_events);
  std::printf("  Shrink events                      : %lu\n",
              stat_resize_shrink_events);
  std::printf("  Resize check interval (accesses)   : %lu\n",
              (uint64_t)RESIZE_INTERVAL);
  std::printf("  Grow  threshold (evict ratio)      : %u/%u = %.1f%%\n",
              EVICT_PRESSURE_NUM, EVICT_PRESSURE_DENOM,
              100.0 * EVICT_PRESSURE_NUM / EVICT_PRESSURE_DENOM);
  std::printf("  Shrink threshold (util ratio)      : %u/%u = %.1f%%\n",
              IDLE_THRESHOLD_NUM, IDLE_THRESHOLD_DENOM,
              100.0 * IDLE_THRESHOLD_NUM / IDLE_THRESHOLD_DENOM);
}
