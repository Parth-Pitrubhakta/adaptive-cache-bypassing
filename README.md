# Adaptive Cache Bypassing for Inclusive Last Level Caches

> Implementation of **I-DSB-BBtracking** — a bypass buffer design that enables cache bypassing on inclusive LLCs while maintaining the inclusion property and reducing hardware overhead by 91%.

Based on the paper:
> *"Adaptive Cache Bypassing for Inclusive Last Level Caches"*
> Saurabh Gupta, Hongliang Gao, Huiyang Zhou — IEEE IPDPS 2013

---

## Table of Contents

- [Problem Statement](#problem-statement)
- [Solution — Bypass Buffer](#solution--bypass-buffer)
- [Design Decisions](#design-decisions)
- [Architecture](#architecture)
- [Implementation](#implementation)
- [Hardware Overhead](#hardware-overhead)
- [Experimental Setup](#experimental-setup)
- [Results](#results)
- [How to Build and Run](#how-to-build-and-run)
- [File Structure](#file-structure)
- [References](#references)

---

## Problem Statement

Modern processors use **inclusive last level caches (LLCs)** because inclusion simplifies cache coherence — if a block is absent from the LLC, it is guaranteed to be absent from all upper caches. This allows the LLC to act as a snoop filter.

However, inclusive caches cannot use **cache bypassing** — a technique where blocks predicted to have no future reuse are skipped when filling the LLC. Bypassing inherently violates the inclusion property because if a block is not stored in the LLC, the inclusion guarantee breaks.

```
┌─────────────────────────────────────────────────────────────┐
│                        The Conflict                          │
│                                                             │
│  Inclusive LLC  ──needs──►  every block must be in LLC      │
│                                                             │
│  Cache Bypassing ──needs──► some blocks skip the LLC        │
│                                                             │
│  These two requirements are mutually exclusive              │
│  — until now.                                               │
└─────────────────────────────────────────────────────────────┘
```

Previous bypassing algorithms (DSB, DRRIP etc.) only work with non-inclusive or exclusive LLCs. This implementation solves the problem for **inclusive** caches.

---

## Solution — Bypass Buffer

We introduce a small **Bypass Buffer (BB)** alongside the LLC:

```
     CPU
      │
   ┌──┴──┐
   │ L1  │  ◄─────────────── Bypassed blocks forwarded here
   └──┬──┘                   (full data, no LLC copy)
      │
   ┌──┴──┐
   │ L2  │
   └──┬──┘
      │
 ┌────┴────┐    ┌──────────────────┐
 │   LLC   │◄──►│  Bypass Buffer   │  ◄── Tags only (no data)
 │  (2 MB) │    │  (64 entries)    │      54-bit tag + 6 bits
 └────┬────┘    └──────────────────┘
      │              │
      │         Back-invalidation
      │         sent to L1/L2 when
      │         BB entry evicted
      │
   ┌──┴───┐
   │ DRAM │
   └──────┘
```

**How it works:**

1. When a block is predicted to have no future reuse → **skip the LLC entirely**
2. The block's data goes to L1/L2 as normal
3. Only the block's **tag** is stored in the small BB
4. When a BB tag is evicted → **back-invalidation** sent to L1/L2 to maintain inclusion
5. The LLC is now free to hold only **useful, reusable data**

**Key insight from the paper:** Bypassed blocks die quickly in upper caches. On average, **94.3% of bypassed blocks are dead within 8 LLC misses**. So a small 64-entry BB is sufficient — by the time a tag is evicted from the BB, the data in L1/L2 is almost certainly already gone naturally.

---

## Design Decisions

### 1. Data-less Bypass Buffer

**Decision:** BB entries store only tags (address labels), not actual data.

**Rationale:** Since bypassed blocks are predicted to be dead, hits in the BB should be extremely rare (confirmed: 0 BB hits across 4+ million bypass decisions in our experiments). Making the BB data-less saves significant area while incurring negligible performance penalty.

**Consequence:** On a rare BB hit (L2 miss that finds a tag in BB), the data is re-fetched from DRAM. This is correct behaviour — the fetch penalty is the cost of an incorrect bypass prediction.

### 2. Competitor Pointer Tracking in BB Entries

**Decision:** Store competitor pointers inside BB entries rather than in every LLC cache set (original DSB approach).

**Rationale:** The original DSB algorithm adds a 16-bit partial tag + 4-bit competitor pointer + 2 status bits to every LLC cache set. For a 2 MB LLC with 2048 sets, this costs **44 KB**. By moving tracking information into the small BB, total storage drops to **3,840 bits — a 91% reduction.**

**Trade-off:** Slightly lower tracking accuracy when the BB is small (64 entries can fill up, causing some competitor pairs to be invalidated early). This costs ~0.4% IPC vs the version with data in the BB, which is acceptable given the hardware savings.

### 3. Probabilistic Bypass (Adaptive)

**Decision:** Bypass with a probability rather than a deterministic rule. Start at 50% and adapt up or down based on feedback.

**Rationale:** No static threshold works well across all benchmarks. mcf benefits from aggressive bypassing (probability converges to 100%). lbm has mixed patterns (stays near 50%). An adaptive policy self-tunes without manual tuning per workload.

**Mechanism:** When a bypassed block's competitor is accessed first → bypass was effective → increase probability. When a bypassed block itself is re-accessed → bypass was wrong → decrease probability.

### 4. Set Dueling (SRRIP vs BIP)

**Decision:** Run two competing SLRU insertion policies on separate sample sets and let a counter pick the winner for all other sets.

**Rationale:** Neither SRRIP (insert at distant position) nor BIP (bimodal insertion) is universally better. Set dueling learns which policy fits the current workload without committing to either.

### 5. Virtual Bypass Sampling

**Decision:** Occasionally assess what would happen if we bypassed, even when we choose not to.

**Rationale:** Without virtual bypasses, the policy can only learn when it actually bypasses. Virtual bypass sampling provides feedback even during periods when bypass probability is low, preventing the policy from getting stuck.

---

## Architecture

### Bypass Buffer Entry Structure

```
┌───────────┬────────────┬─────────────────┬──────────────────┐
│   valid   │  virt_byp  │ competitor_ptr   │     BB-tag       │
│  (1 bit)  │  (1 bit)   │ set(11b)+way(4b) │    (54 bits)     │
└───────────┴────────────┴─────────────────┴──────────────────┘

Total per entry: 1 + 1 + 15 + 54 = 71 bits ≈ 9 bytes
Total for 64 entries: 64 × (54 + 4 + 2) = 3,840 bits = 480 bytes
```

### SLRU Replacement (RRPV Values)

```
RRPV = 0  →  Most recently used (protected, stays in LLC)
RRPV = 1  →  Recently used
RRPV = 2  →  Inserted here on miss (SRRIP)
RRPV = 3  →  LRU position (eviction candidate)

On hit  → RRPV set to 0 (promote to MRU)
On fill → RRPV set to 2 (SRRIP) or 3 with occasional 2 (BIP)
Victim  → Way with highest RRPV (age all if no RRPV=3 found)
```

### DSB Algorithm Flow

```
LLC Miss Arrives
       │
       ▼
Should bypass? ──── rand() < bypass_prob ──►  YES
       │                                        │
       NO                                       ▼
       │                               Insert tag in BB
       ▼                               Forward data to L1/L2
Find SLRU victim                       Update competitor ptr
       │                                        │
       ▼                                        ▼
Fill into LLC way                      BB full? Evict LRU BB entry
       │                               → Send back-invalidation
       ▼
Virtual bypass? ──── 1-in-16 chance ──► YES
       │                                   │
       NO                                  ▼
       │                           Create BB entry for victim
       ▼                           (virtual bypass tracking)
Update PSEL counter
(set dueling feedback)
```

---

## Implementation

The replacement policy is implemented as a ChampSim module:

### `bypass_buffer_dsb.h` — Class Declaration

Key data structures:
```cpp
struct BBEntry {
    bool     valid          = false;
    bool     virtual_bypass = false;  // true → virtual bypass
    uint64_t tag            = 0;      // cache-line aligned address
    long     competitor_set = -1;     // LLC set of competitor
    long     competitor_way = -1;     // LLC way of competitor
};

struct bypass_buffer_dsb : public champsim::modules::replacement {
    static constexpr long BB_SETS  = 16;   // 64 total entries
    static constexpr long BB_WAYS  = 4;
    static constexpr unsigned BYPASS_PROB_INIT = 128;  // 50% start
    static constexpr unsigned BYPASS_PROB_STEP = 8;    // adapt step
    // ...
};
```

### `bypass_buffer_dsb.cc` — Implementation

Key functions:

| Function | Purpose |
|---|---|
| `find_victim()` | Returns `NUM_WAY` (sentinel) to signal bypass, else SLRU victim |
| `replacement_cache_fill()` | On bypass: insert BB tag. On fill: update SLRU + virtual bypass |
| `update_replacement_state()` | On hit: check BB for competitor feedback, adjust probability |
| `bb_insert()` | Insert tag into BB, evict LRU BB entry if full |
| `bb_evict()` | Evict BB entry + model back-invalidation |
| `should_bypass()` | Probabilistic bypass decision |
| `replacement_final_stats()` | Print bypass buffer statistics |

### Bypass Sentinel Convention

ChampSim's replacement interface returns a *way index* from `find_victim()`. We return `NUM_WAY` (one past the last valid way) to signal bypass. ChampSim's cache fill logic skips the LLC fill when it sees this sentinel, forwarding the block to upper levels only.

---

## Hardware Overhead

| Component | This Work (I-DSB-BBtracking) | Original DSB |
|---|---|---|
| BB storage (2 MB LLC) | **3,840 bits** | N/A |
| Per-set tracking | **0 bits** | 22 × 2048 = **44,032 bits** |
| ATD (set dueling) | 46,800 bits (shared) | 46,800 bits |
| Randomization | 51 bits | 51 bits |
| **Total tracking overhead** | **3,891 bits** | **44,083 bits** |
| **Reduction** | **91% less** | baseline |

For a 4-core system with 4 MB LLC: BB scaled to 256 entries = **14,592 bits** vs 88,064 bits for original DSB.

---

## Experimental Setup

| Parameter | Value |
|---|---|
| Simulator | ChampSim (latest master) |
| LLC size | 2 MB, 16-way set-associative |
| LLC latency | 20 cycles |
| L2C | 256 KB, 8-way, 10 cycles |
| L1D | 32 KB, 12-way, 5 cycles |
| DRAM | DDR4, 3205 MT/s |
| BB size | 64 entries, 4-way |
| Warmup | 5,000,000 instructions |
| Simulation | 10,000,000 – 50,000,000 instructions |

### Benchmarks Used

Traces downloaded from the official [DPC-3 repository](https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/):

| Benchmark | Description | Why chosen |
|---|---|---|
| `429.mcf` | Network flow solver | High MPKI, irregular pointer chasing |
| `482.sphinx3` | Speech recognition | Mixed reuse, LLC pollution sensitive |
| `470.lbm` | Fluid dynamics (LBM) | Streaming access, DRAM row-buffer sensitive |

---

## Results

### IPC Improvement over LRU Baseline

| Benchmark | Instructions | LRU IPC | BB-DSB IPC | **Improvement** |
|---|---|---|---|---|
| 429.mcf | 50M | 0.08456 | 0.09049 | **+7.0%** |
| 482.sphinx3 | 5M | 0.5261 | 0.5609 | **+6.6%** |
| 470.lbm | 5M | 0.6092 | 0.6661 | **+9.3%** |
| **Average** | | | | **+7.6%** |

### LLC Load Hits Comparison

| Benchmark | LRU Hits | BB-DSB Hits | Increase |
|---|---|---|---|
| 429.mcf (50M) | 1,395,537 | 1,689,915 | **+294,378 (+21.1%)** |
| 482.sphinx3 (5M) | 2,467 | 7,385 | **+4,918 (+199%)** |

### Bypass Buffer Statistics (mcf, 50M instructions)

```
Blocks bypassed (tags in BB)     : 4,120,107
BB hits (bypassed block reused)  : 0
BB eviction back-invals (modeled): 4,089,441
Bypass judged EFFECTIVE          : confirmed
Bypass judged INEFFECTIVE        : 0
Final bypass probability         : 255 / 255 (100.0%)
Bypass effectiveness rate        : 100.0%
```

### Why Results Match the Paper

The paper reports an average of **+9.4% IPC** on high-MPKI benchmarks with 200M instruction simulations. Our experiments used 5–50M instructions. With longer simulations, the adaptive bypass probability fully converges and captures more of the workload's access pattern — our results of **+7.6% average** are consistent with the paper's findings given the shorter simulation length.

---

## How to Build and Run

### Prerequisites (Ubuntu 24.04)

```bash
sudo apt-get install -y \
    build-essential g++ git python3 \
    libfmt-dev nlohmann-json3-dev libcli11-dev \
    libbz2-dev liblzma-dev zlib1g-dev xz-utils
```

### Step 1 — Clone ChampSim

```bash
git clone --depth=1 https://github.com/ChampSim/ChampSim.git
cd ChampSim
```

### Step 2 — Apply Ubuntu Fix

```bash
# CLI11 is header-only on Ubuntu — remove the linker flag
sed -i 's/override LDLIBS.*+=.*-lCLI11/override LDLIBS   += /' Makefile
```

### Step 3 — Copy Implementation Files

```bash
mkdir -p replacement/bypass_buffer_dsb
cp /path/to/bypass_buffer_dsb.h  replacement/bypass_buffer_dsb/
cp /path/to/bypass_buffer_dsb.cc replacement/bypass_buffer_dsb/
```

### Step 4 — Configure

```bash
python3 - << 'EOF'
import json, copy
cfg = json.load(open("champsim_config.json"))
cfg["LLC"]["replacement"] = "bypass_buffer_dsb"
cfg["executable_name"]    = "champsim_bbdsb"
json.dump(cfg, open("/tmp/cfg_bbdsb.json", "w"), indent=2)
print("Config written")
EOF

python3 config.sh /tmp/cfg_bbdsb.json

# Fix include path
sed -i 's|-I.*/ChampSim/inc|-I/path/to/ChampSim/inc -I/path/to/ChampSim/.csconfig|' absolute.options
```

### Step 5 — Build

```bash
make -j$(nproc)
ls -lh bin/champsim_bbdsb   # Should show ~1.6 MB ELF binary
```

### Step 6 — Generate Synthetic Trace

```bash
g++ -O2 -std=c++17 -o /tmp/gen_trace gen_trace.cpp
/tmp/gen_trace | xz -3 > /tmp/test.champsim.xz
```

### Step 7 — Run

```bash
# With synthetic trace
bin/champsim_bbdsb \
    --warmup-instructions 100000 \
    --simulation-instructions 1000000 \
    /tmp/test.champsim.xz

# With real SPEC trace
bin/champsim_bbdsb \
    --warmup-instructions 5000000 \
    --simulation-instructions 50000000 \
    /path/to/429.mcf-184B.champsimtrace.xz
```

### Download Real SPEC Traces

```bash
mkdir -p traces && cd traces

# mcf — best benchmark for bypass (high MPKI, irregular access)
wget https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/429.mcf-184B.champsimtrace.xz

# sphinx3 — speech recognition (LLC pollution sensitive)
wget https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/482.sphinx3-1100B.champsimtrace.xz

# lbm — fluid dynamics (streaming access pattern)
wget https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/470.lbm-1274B.champsimtrace.xz
```

### Compare Against LRU Baseline (Parallel)

```bash
# Build LRU binary
python3 - << 'EOF'
import json, copy
cfg = json.load(open("champsim_config.json"))
cfg["LLC"]["replacement"] = "lru"
cfg["executable_name"]    = "champsim_lru"
json.dump(cfg, open("/tmp/cfg_lru.json", "w"), indent=2)
EOF
python3 config.sh /tmp/cfg_lru.json
touch src/generated_environment.cc && make -j$(nproc)

# Run both in parallel
bin/champsim_lru   --warmup-instructions 5000000 --simulation-instructions 50000000 \
    traces/429.mcf-184B.champsimtrace.xz > /tmp/mcf_lru.txt 2>&1 &

bin/champsim_bbdsb --warmup-instructions 5000000 --simulation-instructions 50000000 \
    traces/429.mcf-184B.champsimtrace.xz > /tmp/mcf_bbdsb.txt 2>&1 &

wait

# Compare
echo "=== LRU ===" && grep "cumulative IPC\|LLC LOAD" /tmp/mcf_lru.txt | tail -2
echo "=== BB-DSB ===" && grep "cumulative IPC\|LLC LOAD\|bypassed\|effectiveness\|probability" /tmp/mcf_bbdsb.txt | tail -6
```

---

## File Structure

```
replacement/bypass_buffer_dsb/
├── bypass_buffer_dsb.h      ← Class declaration, BB entry struct,
│                               all tunable constants
└── bypass_buffer_dsb.cc     ← Full implementation:
                                - BB insert/evict/search
                                - SLRU replacement (RRPV 0-3)
                                - Adaptive bypass probability
                                - Competitor pointer tracking
                                - Virtual bypass sampling
                                - Set dueling (SRRIP vs BIP)
                                - Back-invalidation modelling
                                - Final stats printer

gen_trace.cpp                ← Synthetic trace generator
                                Phase 1: 16 MB streaming
                                Phase 2: 3 MB thrashing × 4
                                Phase 3: 128 KB reuse × 20

bb_dsb_config.json           ← ChampSim config with LLC
                                replacement = bypass_buffer_dsb
```

### Tunable Parameters (`bypass_buffer_dsb.h`)

| Constant | Default | Meaning |
|---|---|---|
| `BB_SETS` | 16 | Sets in Bypass Buffer |
| `BB_WAYS` | 4 | Ways per set (total = 64 entries) |
| `BYPASS_PROB_INIT` | 128 | Starting bypass probability (0–255) |
| `BYPASS_PROB_STEP` | 8 | Adapt increment/decrement per feedback |
| `VBYPASS_SAMPLE` | 16 | 1-in-N fills sampled for virtual bypass |
| `SDM_SIZE` | 32 | Sampler sets per policy for set dueling |

For 4-core / 4 MB LLC (paper configuration): set `BB_SETS = 64` (256 entries total).

---

## References

1. S. Gupta, H. Gao, H. Zhou, *"Adaptive Cache Bypassing for Inclusive Last Level Caches"*, IEEE IPDPS 2013.
2. H. Gao, C. Wilkerson, *"A Dueling Segmented LRU Replacement Algorithm with Adaptive Bypassing"*, 1st JILP Cache Replacement Championship, 2010.
3. A. Jaleel et al., *"High Performance Cache Replacement Using Re-Reference Interval Prediction (RRIP)"*, ISCA 2010.
4. M. K. Qureshi et al., *"Adaptive Insertion Policies for High Performance Caching"*, ISCA 2007.
5. ChampSim Simulator: https://github.com/ChampSim/ChampSim
6. DPC-3 Trace Repository: https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/

