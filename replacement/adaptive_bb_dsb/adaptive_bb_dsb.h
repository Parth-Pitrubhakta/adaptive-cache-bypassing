#ifndef REPLACEMENT_ADAPTIVE_BB_DSB_H
#define REPLACEMENT_ADAPTIVE_BB_DSB_H

/*
 * Adaptive-BB I-DSB-BBtracking
 * ═════════════════════════════════════════════════════════════════════════════
 * NOVELTY: Dynamically resizes the Bypass Buffer based on runtime workload
 * pressure, instead of using a fixed 64-entry BB as in the original paper.
 *
 * MOTIVATION (from the paper itself):
 *   Section V-C (Figure 8) shows that performance of I-DSB-BBtracking
 *   improves from ~1.085x to ~1.10x as BB size grows from 32→128 entries,
 *   then saturates. The paper uses a fixed 64-entry BB as a compromise, but
 *   never explores making the size adaptive. Different benchmarks need very
 *   different BB sizes:
 *     - mcf:    aggressive bypassing → needs larger BB to hold more tags
 *     - lbm:    streaming access    → 64 entries is already sufficient
 *     - sphinx: mixed patterns      → fluctuates over simulation time
 *
 * HOW IT WORKS:
 *   The BB is managed as a logical pool of up to BB_MAX_ENTRIES entries,
 *   but only bb_active_sets × BB_WAYS entries are considered "active".
 *
 *   A resize controller monitors two pressure signals every RESIZE_INTERVAL
 *   LLC accesses:
 *
 *   1. EVICTION PRESSURE (too small):
 *      If the ratio of BB evictions to BB insertions exceeds
 *      EVICT_PRESSURE_THRESHOLD, the BB is under pressure — bypassed tags
 *      are being flushed early, triggering unnecessary back-invalidations.
 *      → Grow BB by RESIZE_STEP sets (up to BB_MAX_SETS).
 *
 *   2. IDLE PRESSURE (too large):
 *      If the fraction of BB entries that are valid falls below
 *      IDLE_THRESHOLD, the BB is oversized — we are wasting storage for
 *      entries that will never be used before the bypassed block dies
 *      naturally in L1/L2.
 *      → Shrink BB by RESIZE_STEP sets (down to BB_MIN_SETS).
 *
 *   The active size is always kept as a multiple of BB_WAYS so the set-
 *   associative structure is always geometrically clean.
 *
 * HARDWARE COST ANALYSIS:
 *   Original fixed BB  : 64  × (54+4+2) bits = 3,840 bits
 *   This design max BB : 256 × (54+4+2) bits = 15,360 bits (worst case)
 *   This design typical: adapts to ~64–128 entries for most workloads
 *   Extra resize logic : 2 saturating counters + comparators ≈ ~60 bits
 *   Net overhead vs paper: negligible; saves hardware when workload is calm.
 *
 * IMPLEMENTATION NOTE:
 *   The physical bb[] vector is always allocated at BB_MAX_ENTRIES so there
 *   is no dynamic memory allocation at runtime. The "active" window is just
 *   [0 .. bb_active_sets × BB_WAYS). Entries beyond the window are simply
 *   never inserted into and are treated as invalid.
 *
 * Base paper:
 *   "Adaptive Cache Bypassing for Inclusive Last Level Caches"
 *   Gupta, Gao, Zhou — IEEE IPDPS 2013
 */

#include <array>
#include <cstdint>
#include <vector>

#include "cache.h"
#include "modules.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Bypass Buffer Entry (unchanged from original)
// ─────────────────────────────────────────────────────────────────────────────
struct AdaptiveBBEntry {
  bool     valid          = false;
  bool     virtual_bypass = false;
  uint64_t tag            = 0;
  long     competitor_set = -1;
  long     competitor_way = -1;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Main replacement class
// ─────────────────────────────────────────────────────────────────────────────
struct adaptive_bb_dsb : public champsim::modules::replacement {

  // ── Fixed BB geometry ─────────────────────────────────────────────────────
  static constexpr long BB_WAYS     = 4;    // ways per BB set (unchanged)
  static constexpr long BB_MIN_SETS = 4;    // minimum active sets  (16 entries)
  static constexpr long BB_MAX_SETS = 64;   // maximum active sets (256 entries)
  static constexpr long BB_MAX_ENTRIES = BB_MAX_SETS * BB_WAYS; // 256 physical slots
  static constexpr long BB_INIT_SETS   = 16; // starting size (64 entries, paper default)

  // ── Adaptive resize policy ────────────────────────────────────────────────
  // How often (in LLC accesses) to evaluate resize
  static constexpr uint64_t RESIZE_INTERVAL = 1024;

  // Grow if (evictions / insertions) > this threshold (numerator, denom=16)
  // 10/16 = 62.5%: if over 62% of inserted tags get evicted early → too small
  static constexpr unsigned EVICT_PRESSURE_NUM   = 10;
  static constexpr unsigned EVICT_PRESSURE_DENOM = 16;

  // Shrink if (valid entries / active capacity) < this threshold (num, denom=16)
  // 4/16 = 25%: if fewer than 25% of slots are in use → too large
  static constexpr unsigned IDLE_THRESHOLD_NUM   = 4;
  static constexpr unsigned IDLE_THRESHOLD_DENOM = 16;

  // How many sets to add/remove per resize event
  static constexpr long RESIZE_STEP = 4; // ±16 entries at a time

  // ── SLRU / RRPV (unchanged from original) ────────────────────────────────
  static constexpr unsigned RRPV_LONG    = 3;
  static constexpr unsigned RRPV_DISTANT = 2;
  static constexpr unsigned RRPV_NEAR    = 0;

  // ── Bypass probability (unchanged from original) ─────────────────────────
  static constexpr unsigned BYPASS_PROB_MAX  = 255;
  static constexpr unsigned BYPASS_PROB_INIT = 128;
  static constexpr unsigned BYPASS_PROB_STEP = 8;

  // ── Virtual bypass sampling (unchanged) ──────────────────────────────────
  static constexpr unsigned VBYPASS_SAMPLE = 16;

  // ── Set dueling (unchanged) ───────────────────────────────────────────────
  static constexpr std::size_t SDM_SIZE   = 32;
  static constexpr std::size_t NUM_POLICY = 2;
  static constexpr unsigned    PSEL_WIDTH = 10;
  static constexpr unsigned    PSEL_MAX   = (1u << PSEL_WIDTH) - 1;
  static constexpr unsigned    PSEL_INIT  = PSEL_MAX / 2;

  // ══════════════════════════════════════════════════════════════════════════
  //  Per-instance state
  // ══════════════════════════════════════════════════════════════════════════
  long NUM_SET, NUM_WAY;

  // SLRU RRPV table
  std::vector<unsigned> rrpv;

  // ── Bypass Buffer ─────────────────────────────────────────────────────────
  // Physical storage: always BB_MAX_ENTRIES slots allocated.
  // Only [0 .. bb_active_sets * BB_WAYS) are logically active.
  std::vector<AdaptiveBBEntry> bb;
  std::vector<uint64_t>        bb_lru;   // LRU timestamps for BB ways
  uint64_t                     bb_cycle = 0;

  // Current active BB size (in sets); changes at runtime
  long bb_active_sets = BB_INIT_SETS;

  // ── Resize tracking counters (reset every RESIZE_INTERVAL) ───────────────
  uint64_t resize_access_counter = 0;    // LLC accesses since last resize check
  uint64_t interval_bb_insertions = 0;   // BB inserts this interval
  uint64_t interval_bb_evictions  = 0;   // BB evictions (due to capacity) this interval

  // ── Bypass probability ────────────────────────────────────────────────────
  unsigned bypass_probability = BYPASS_PROB_INIT;

  // ── RNG ───────────────────────────────────────────────────────────────────
  uint64_t rng_state = 12345678ULL;

  // ── Set dueling ───────────────────────────────────────────────────────────
  std::vector<unsigned>     psel;
  std::vector<std::size_t>  rand_sets;

  // ── Statistics ────────────────────────────────────────────────────────────
  uint64_t stat_bypassed                = 0;
  uint64_t stat_bb_hits                 = 0;
  uint64_t stat_bb_evict_invalidations  = 0;
  uint64_t stat_bypass_effective        = 0;
  uint64_t stat_bypass_ineffective      = 0;

  // Adaptive resize history (for reporting)
  uint64_t stat_resize_grow_events   = 0;
  uint64_t stat_resize_shrink_events = 0;
  long     stat_bb_size_min          = BB_INIT_SETS * BB_WAYS;
  long     stat_bb_size_max          = BB_INIT_SETS * BB_WAYS;
  uint64_t stat_bb_size_sum          = 0;   // for computing average
  uint64_t stat_bb_size_samples      = 0;

  // ══════════════════════════════════════════════════════════════════════════
  //  Constructor
  // ══════════════════════════════════════════════════════════════════════════
  explicit adaptive_bb_dsb(CACHE* cache);

  // ══════════════════════════════════════════════════════════════════════════
  //  ChampSim replacement interface
  // ══════════════════════════════════════════════════════════════════════════
  long find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set,
                   const champsim::cache_block* current_set,
                   champsim::address ip, champsim::address full_addr,
                   access_type type);

  void replacement_cache_fill(uint32_t triggering_cpu, long set, long way,
                               champsim::address full_addr,
                               champsim::address ip,
                               champsim::address victim_addr,
                               access_type type);

  void update_replacement_state(uint32_t triggering_cpu, long set, long way,
                                 champsim::address full_addr,
                                 champsim::address ip,
                                 champsim::address victim_addr,
                                 access_type type, uint8_t hit);

  void replacement_final_stats();

private:
  // ── SLRU helpers ─────────────────────────────────────────────────────────
  unsigned& get_rrpv(long set, long way);
  void      update_slru_hit(long set, long way);
  void      update_slru_miss(long set, long way, bool use_bip);

  // ── BB helpers ────────────────────────────────────────────────────────────
  long bb_active_capacity() const { return bb_active_sets * BB_WAYS; }
  long bb_set_of(uint64_t addr) const;
  long bb_find(uint64_t addr);
  long bb_find_victim(long bb_set);
  void bb_insert(uint64_t addr, bool virt, long comp_set, long comp_way);
  void bb_evict(long bb_idx);

  // ── Adaptive resize logic ─────────────────────────────────────────────────
  void maybe_resize();
  void grow_bb();
  void shrink_bb();

  // ── Bypass decision ───────────────────────────────────────────────────────
  bool should_bypass(uint32_t cpu, long set,
                     const champsim::cache_block* current_set,
                     champsim::address full_addr);

  // ── Set dueling helpers ───────────────────────────────────────────────────
  bool is_sdm_set(uint32_t cpu, long set, std::size_t policy) const;
  bool use_bip_for_set(uint32_t cpu, long set) const;

  // ── RNG ───────────────────────────────────────────────────────────────────
  uint64_t next_rand();
};

#endif // REPLACEMENT_ADAPTIVE_BB_DSB_H
