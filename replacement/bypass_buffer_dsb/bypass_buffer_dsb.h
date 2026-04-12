#ifndef REPLACEMENT_BYPASS_BUFFER_DSB_H
#define REPLACEMENT_BYPASS_BUFFER_DSB_H

/*
 * I-DSB-BBtracking: Adaptive Cache Bypassing for Inclusive Last Level Caches
 *
 * Implementation based on:
 *   "Adaptive Cache Bypassing for Inclusive Last Level Caches"
 *   Gupta, Gao, Zhou — IEEE IPDPS 2013
 *
 * Design: DSB (Dueling Segmented LRU + Adaptive Bypassing) adapted for
 * inclusive LLCs via a small Bypass Buffer (BB) that:
 *   1. Stores tags of bypassed blocks (data-less) to maintain inclusion
 *   2. Tracks bypass effectiveness via competitor pointers
 *   3. Sends back-invalidations when BB entries are evicted
 *
 * Three SLRU segments per set:
 *   - Protected  (high-priority, recently promoted)
 *   - Probationary (inserted here first)
 *   Bypass: block skips LLC entirely, tag goes into BB
 *
 * Bypass Probability Control:
 *   - A global saturating counter (bypass_prob_counter) tracks effectiveness
 *   - A random number compared against bypass_probability decides bypass
 *   - Effectiveness checked via BB competitor pointer mechanism
 */

#include <array>
#include <cstdint>
#include <vector>

#include "cache.h"
#include "modules.h"

// ────────────────────────────────────────────────────────────────
//  Bypass Buffer Entry
//  Each entry is data-less; it only keeps the tag + tracking info
// ────────────────────────────────────────────────────────────────
struct BBEntry {
  bool          valid          = false;
  bool          virtual_bypass = false; // true → virtual bypass (block IS in LLC)
  uint64_t      tag            = 0;     // full block address (cache-line aligned)
  long          competitor_set = -1;    // which LLC set the competitor lives in
  long          competitor_way = -1;    // which LLC way the competitor lives in
};

// ────────────────────────────────────────────────────────────────
//  Main replacement class
// ────────────────────────────────────────────────────────────────
struct bypass_buffer_dsb : public champsim::modules::replacement {

  // ── Tunable parameters (matching paper defaults) ───────────────
  static constexpr long     BB_SETS           = 16;   // 64 entries / 4 ways
  static constexpr long     BB_WAYS           = 4;
  static constexpr long     BB_TOTAL          = BB_SETS * BB_WAYS; // 64 entries

  // SLRU: ways split into protected (upper half) + probationary (lower half)
  // RRPV encoding: 0 = MRU/protected, 3 = LRU/evict candidate
  static constexpr unsigned RRPV_LONG         = 3;
  static constexpr unsigned RRPV_DISTANT      = 2;
  static constexpr unsigned RRPV_NEAR         = 0;

  // Bypass probability: 0–255 scale; start at ~50%
  static constexpr unsigned BYPASS_PROB_MAX   = 255;
  static constexpr unsigned BYPASS_PROB_INIT  = 128;
  static constexpr unsigned BYPASS_PROB_STEP  = 8;

  // Virtual bypass sampling: ~6% of fills (matches DSB paper)
  static constexpr unsigned VBYPASS_SAMPLE    = 16;  // 1-in-16

  // Set-dueling: 32 sets per CPU per policy
  static constexpr std::size_t SDM_SIZE       = 32;
  static constexpr std::size_t NUM_POLICY     = 2;   // SRRIP vs BIP
  static constexpr unsigned    PSEL_WIDTH     = 10;
  static constexpr unsigned    PSEL_MAX       = (1u << PSEL_WIDTH) - 1;
  static constexpr unsigned    PSEL_INIT      = PSEL_MAX / 2;

  // ── Per-instance state ──────────────────────────────────────────
  long NUM_SET, NUM_WAY;

  // RRPV table for SLRU (one entry per LLC way per set)
  std::vector<unsigned> rrpv;

  // Bypass Buffer: BB_SETS × BB_WAYS entries
  std::vector<BBEntry> bb;

  // LRU age for BB ways (for BB replacement within its own sets)
  std::vector<uint64_t> bb_lru;
  uint64_t bb_cycle = 0;

  // Bypass probability (global, one per cache)
  unsigned bypass_probability = BYPASS_PROB_INIT;

  // Random number state (simple LCG for speed)
  uint64_t rng_state = 12345678ULL;

  // PSEL counter per CPU for set dueling
  std::vector<unsigned> psel; // [NUM_CPUS]

  // Sampler sets for set-dueling
  std::vector<std::size_t> rand_sets; // NUM_CPUS * NUM_POLICY * SDM_SIZE entries

  // Stats
  uint64_t stat_bypassed   = 0;
  uint64_t stat_bb_hits    = 0;
  uint64_t stat_bb_evict_invalidations = 0;
  uint64_t stat_bypass_effective   = 0;
  uint64_t stat_bypass_ineffective = 0;

  // ── Constructor ────────────────────────────────────────────────
  explicit bypass_buffer_dsb(CACHE* cache);

  // ── ChampSim replacement interface ────────────────────────────
  long     find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set,
                       const champsim::cache_block* current_set,
                       champsim::address ip, champsim::address full_addr,
                       access_type type);

  void     replacement_cache_fill(uint32_t triggering_cpu, long set, long way,
                                  champsim::address full_addr,
                                  champsim::address ip,
                                  champsim::address victim_addr,
                                  access_type type);

  void     update_replacement_state(uint32_t triggering_cpu, long set, long way,
                                    champsim::address full_addr,
                                    champsim::address ip,
                                    champsim::address victim_addr,
                                    access_type type, uint8_t hit);

  void     replacement_final_stats();

private:
  // ── Helpers ────────────────────────────────────────────────────
  unsigned& get_rrpv(long set, long way);

  // BB helpers
  long     bb_find(uint64_t addr);          // returns BB index or -1
  long     bb_find_victim(long bb_set);     // LRU victim within a BB set
  long     bb_set_of(uint64_t addr) const;
  void     bb_insert(uint64_t addr, bool virt,
                     long comp_set, long comp_way);
  void     bb_evict(long bb_idx);           // evict + back-invalidate

  // Bypass decision
  bool     should_bypass(uint32_t cpu, long set,
                         const champsim::cache_block* current_set,
                         champsim::address full_addr);

  // SLRU update helpers
  void     update_slru_hit(long set, long way);
  void     update_slru_miss(long set, long way, bool use_bip);

  // Set-dueling helpers
  bool     is_sdm_set(uint32_t cpu, long set, std::size_t policy) const;
  bool     use_bip_for_set(uint32_t cpu, long set) const;

  // RNG
  uint64_t next_rand();
};

#endif // REPLACEMENT_BYPASS_BUFFER_DSB_H
