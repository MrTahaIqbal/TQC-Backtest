# BigBoyAgent — TQC Backtest Engine

> **Event-driven perpetual futures backtest engine written in C++20.**
> Designed as the simulation counterpart to TQC Brain — mirrors its live risk engine, signal format, and statistical model fields exactly.

<br>

## Table of Contents

- [Overview](#overview)
- [System Architecture](#system-architecture)
- [Feature Set](#feature-set)
- [File Structure](#file-structure)
- [Build Instructions](#build-instructions)
- [Docker Deployment](#docker-deployment)
- [API Reference](#api-reference)
- [Configuration](#configuration)
- [Statistical Methodology](#statistical-methodology)
- [Security Model](#security-model)
- [Hardware Requirements](#hardware-requirements)
- [Design Decisions](#design-decisions)

<br>

## Overview

TQC Backtest is a **lock-free, multi-threaded HTTP backtest server** that receives bar streams from TQC Executor and returns full performance analytics. It is not a generic backtest framework — every component mirrors a specific live system decision:

| Live System (Brain/Executor) | Backtest Mirror |
|---|---|
| `RiskEngine::sizePosition` with GARCH scaling | `computeMarginGARCH()` |
| HMM 3-state regime classification (0=BEAR, 1=SIDEWAYS, 2=BULL) | `safeRegimeIndex()` + `regime_stats[3]` |
| Binance 8-hour perpetual funding deduction | `SimPosition::fundingWindowsHeld()` |
| Dual SL/TP trigger with proximity tie-breaking | Bar high/low evaluation in `BacktestEngine` |
| Signal confidence threshold filtering | `min_confidence` check pre-open |
| Taker fee + one-way slippage on open and close | Symmetric fee model in `closePosition()` |

The result is a backtest whose P&L numbers are **directly comparable** to live executor output — not an approximation.

<br>

## System Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        TQC Backtest Space                           │
│                                                                     │
│  ┌──────────────┐     ┌──────────────────────────────────────────┐  │
│  │   Acceptor   │     │           Worker Thread Pool             │  │
│  │   Thread     │     │  ┌────────┐ ┌────────┐ ┌────────┐      │  │
│  │              │     │  │ Worker │ │ Worker │ │ Worker │  ...  │  │
│  │  poll()      │────▶│  │   1    │ │   2    │ │   3    │      │  │
│  │  accept()    │     │  └───┬────┘ └───┬────┘ └───┬────┘      │  │
│  └──────────────┘     │      │          │          │            │  │
│         │             └──────┼──────────┼──────────┼────────────┘  │
│  ┌──────▼──────┐            │          │          │               │
│  │  MPSC Ring  │            ▼          ▼          ▼               │
│  │  Buffer     │     ┌─────────────────────────────────────┐      │
│  │  fd_queue   │     │         Request Pipeline            │      │
│  └─────────────┘     │                                     │      │
│                       │  parseRequest() → checkAuth()       │      │
│                       │        ↓                            │      │
│                       │  parseBars() [thread_local buf]     │      │
│                       │        ↓                            │      │
│                       │  parseConfig()                      │      │
│                       │        ↓                            │      │
│                       │  BacktestEngine::run()              │      │
│                       │    ├── SL/TP evaluation per bar     │      │
│                       │    ├── computeMarginGARCH()         │      │
│                       │    ├── fundingWindowsHeld()         │      │
│                       │    └── closePosition()              │      │
│                       │        ↓                            │      │
│                       │  StatsEngine                        │      │
│                       │    ├── computeSharpe()   [Welford]  │      │
│                       │    ├── computeSortino()             │      │
│                       │    ├── computeMaxDrawdown()         │      │
│                       │    ├── computeCalmar()              │      │
│                       │    ├── computeProfitFactor()        │      │
│                       │    └── computeSymbolStats()         │      │
│                       │        ↓                            │      │
│                       │  nlohmann::json serialisation       │      │
│                       │        ↓                            │      │
│                       │  send_all() → HTTP response         │      │
│                       └─────────────────────────────────────┘      │
└─────────────────────────────────────────────────────────────────────┘
```

### Data Flow

```
TQC Executor
     │
     │  POST /backtest
     │  {bars: [...], initial_balance, risk_pct, leverage, ...}
     │
     ▼
HttpServer (MPSC ring buffer — lock-free acceptor → worker handoff)
     │
     ├── Authentication (constant-time key comparison — timing-safe)
     │
     ▼
parseBars()
  Reads: close, high, low, atr, confidence, pos_valid, timestamp,
         signal, regime, vol_regime,
         garch_vol, hmm_state, funding_rate   ← Step 3 statistical fields
     │
     ▼
BacktestEngine::run()
  Per bar:
    1. Evaluate SL/TP on all open positions (proximity tie-breaking)
    2. Apply signal filter (confidence ≥ min_confidence AND pos_valid)
    3. Close conflicting positions on direction change
    4. Open new positions: computeMarginGARCH() → notional → SL/TP prices
    5. Append equity point to result.equity_curve
     │
  On close:
    - PnL = notional × direction × (exit - entry) / entry − fees − funding
    - funding = notional × |rate| × fundingWindowsHeld(ts)
     │
     ▼
StatsEngine
  - Welford's online variance → Sharpe / Sortino (numerically stable)
  - Monotonic equity curve from initial_balance → max drawdown
  - Regime-sliced stats: BEAR / SIDEWAYS / BULL performance tables
  - Per-symbol stats: Sharpe, Sortino, Calmar, profit_factor per asset
     │
     ▼
JSON Response → TQC Executor
```

<br>

## Feature Set

### Engine
- **Proximity-based SL/TP tie-breaking** — when both SL and TP fall within a bar's high-low range, the trigger closer to entry is filled first (Zipline / Backtrader convention)
- **GARCH-scaled position sizing** — high predicted volatility reduces position size via `scale = clamp(TARGET_VOL / garch_vol, 0.5, 1.0)`
- **Perpetual funding cost deduction** — pro-rated per 8-hour Binance window using `fundingWindowsHeld(ts)`
- **Regime-sliced performance tables** — BEAR / SIDEWAYS / BULL performance broken down by HMM state
- **Multi-symbol support** — up to 24 symbols, 8 simultaneous positions, per-symbol statistics
- **Walk-forward validation** — N-fold temporal OOS testing with stability verdict (consistency ≥ 60% AND avg\_Sharpe > 0.50)
- **Equity curve embedding** — full `EquityPoint[MAX_EQUITY_POINTS]` embedded in result, sampled to ≤200 points in response

### Concurrency
- **Lock-free MPSC ring buffer** — unbounded monotonic counters eliminate ABA hazard; per-slot cache-line-padded commit flags eliminate false sharing
- **`thread_local` request buffers** — `bars_buf[MAX_BARS]` and `BacktestResult` per worker; zero shared mutable state on the hot path
- **`fetch_add` latency accumulation** — race-free atomic double accumulation (C++20)
- **Graceful shutdown** — `poll()` timeout in acceptor; `stop()` callable from signal handler; workers drain cleanly

### HTTP Server
- **Constant-time auth comparison** — XOR-accumulation pattern; immune to timing oracle attacks
- **8 MiB request cap** — 413 response on breach; protects all workers from OOM DoS
- **`send_all()` loop** — guarantees full response delivery when socket buffer is transiently full
- **`SO_RCVTIMEO = 5s`** — bounds per-connection worker hold time; prevents slowloris
- **CORS preflight** — all three required headers (Allow-Methods, Allow-Headers, Max-Age)
- **EINTR retry** — `recv()` retried on signal delivery; connection never silently dropped

### Statistics
- **Welford's online algorithm** — numerically stable variance; immune to catastrophic cancellation in low-spread return series
- **Bessel correction consistency** — both Sharpe and Sortino use `(n-1)` sample variance denominator
- **`std::span`-based API** — zero pointer/size mismatch risk; no raw `(T*, int n)` pairs
- **Compile-time `SQRT_ANNUAL_FACTOR`** — `sqrt(525600)` pre-computed; no runtime `std::sqrt` in Sharpe/Sortino hot path

<br>

## File Structure

```
tqc-backtest/
│
├── types.hpp               Core domain structs (Bar, Trade, BacktestResult, ...)
│                           Wire-format aligned with TQC Executor
│
├── config.hpp              AppConfig declaration; loadConfig() contract
├── config.cpp              BACKTEST_SECRET loading; noexcept-safe JSON parse
│
├── ring_buffer.hpp         Lock-free MPSC ring buffer (C++20 concepts, NothrowMovable)
│                           Unbounded cursors, per-slot cache-line padding
│
├── stats_engine.hpp        Statistical function declarations (std::span API)
├── stats_engine.cpp        Welford Sharpe, Sortino, drawdown, Calmar, symbol stats
│
├── backtest_engine.hpp     BacktestEngine + WalkForwardResult declarations
├── backtest_engine.cpp     Full event-driven simulation; GARCH sizing; funding
│
├── http_server.hpp         HttpServer, HttpRequest, HttpResponse declarations
├── http_server.cpp         POSIX HTTP/1.1 server; timing-safe auth; DoS guards
│
├── main.cpp                Bootstrap, route registration, signal handling
│
├── CMakeLists.txt          C++20 build; Release AVX2+LTO; Debug ASan baseline
├── Dockerfile              Two-stage build; debian:bookworm-slim runtime
└── settings.json           Optional runtime overrides (documented field template)
```

### What Each File Owns

| File | Responsibility | Depends On |
|---|---|---|
| `types.hpp` | All shared domain types and constants | — |
| `config.hpp/cpp` | Secret loading, global config singleton | `types.hpp` |
| `ring_buffer.hpp` | Lock-free fd handoff, acceptor→worker | `types.hpp` |
| `stats_engine.hpp/cpp` | All statistical computations | `types.hpp` |
| `backtest_engine.hpp/cpp` | Simulation loop, position management | `types.hpp`, `stats_engine` |
| `http_server.hpp/cpp` | HTTP parsing, auth, routing, send | `ring_buffer.hpp`, `config.hpp` |
| `main.cpp` | Bootstrap, handlers, JSON serialisation | All of the above |

<br>

## Build Instructions

### Prerequisites
- GCC 13+ or Clang 16+ with C++20 support
- CMake ≥ 3.22
- Ninja (recommended) or GNU Make
- `nlohmann/json` — downloaded automatically by Dockerfile; for local builds:
  ```bash
  curl -fsSL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp \
       -o json.hpp
  ```

### Release Build (AVX2 — production)
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target backtest -- -j$(nproc)
./build/backtest
```

### Debug Build (ASan + UBSan — development)
```bash
cmake -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --target backtest -- -j$(nproc)
./build-debug/backtest
```

The Debug build uses `-march=x86-64` (portable baseline — no AVX2 required) and enables AddressSanitizer + UndefinedBehaviorSanitizer. The Release build uses `-march=x86-64-v3` (requires AVX2 + FMA + BMI2).



<br>

**CPU requirement:** The production binary is compiled with `-march=x86-64-v3`. It requires **AVX2 + FMA + BMI2** (Intel Haswell 2013+, AMD Zen 1 2017+). 

<br>


---

### `GET /health`
No auth required.
```json
{
  "status": "ok",
  "uptime_sec": 142.3,
  "uptime_str": "00h 02m 22s",
  "total_runs": 7,
  "last_lat_ms": 38.4,
  "avg_lat_ms": 41.2
}
```

---

### `POST /backtest`
Auth required. Body: JSON object.

**Request fields:**

| Field | Type | Default | Description |
|---|---|---|---|
| `bars` | array | — | **Required.** Array of bar objects (see below) |
| `initial_balance` | number | 1000.0 | Starting account balance (USD) |
| `risk_pct` | number | 0.01 | Fraction of balance risked per trade |
| `leverage` | integer | 3 | Notional leverage multiplier |
| `sl_atr_mult` | number | 1.5 | Stop-loss distance in ATR multiples |
| `tp_rr_ratio` | number | 2.0 | Take-profit reward-to-risk ratio |
| `min_confidence` | number | 0.70 | Minimum signal confidence to open |
| `fee_pct` | number | 0.001 | Taker fee per side |
| `slip_pct` | number | 0.0002 | One-way slippage estimate |
| `max_open` | integer | 3 | Maximum simultaneous open positions |

**Bar object fields:**

| Field | Type | Required | Description |
|---|---|---|---|
| `symbol` | string | ✓ | Trading pair (e.g. `"BTCUSDT"`) |
| `close` | number | ✓ | Bar close price |
| `high` | number | | Bar high (defaults to close) |
| `low` | number | | Bar low (defaults to close) |
| `atr` | number | | Average True Range for SL sizing |
| `signal` | string | | `"BUY"` / `"SELL"` / `"HOLD"` |
| `confidence` | number | | Signal confidence [0, 1] |
| `pos_valid` | boolean | | Position allowed flag from Brain |
| `timestamp` | integer | | Unix millisecond timestamp |
| `garch_vol` | number | | GARCH(1,1) annualised vol (Step 3) |
| `hmm_state` | integer | | HMM regime: 0=BEAR 1=SIDEWAYS 2=BULL |
| `funding_rate` | number | | Binance 8hr funding rate |
| `regime` | string | | Regime label string |
| `vol_regime` | string | | Volatility regime label |

**Response** (abbreviated):
```json
{
  "status": "success",
  "initial_balance": ,
  "final_balance": ,
  "total_return_pct": 24.78,
  "max_drawdown_pct": 8.34,
  "sharpe": 1.84,
  "sortino": 2.41,
  "calmar": 2.97,
  "profit_factor": 1.73,
  "win_rate": 0.61,
  "total_trades": 142,
  "trades_stored": 142,
  "trades_truncated": false,
  "total_funding_cost": 3.21,
  "regime_stats": [
    {"regime": "bear",     "trades": 31, "win_rate": 0.48, ...},
    {"regime": "sideways", "trades": 67, "win_rate": 0.58, ...},
    {"regime": "bull",     "trades": 44, "win_rate": 0.72, ...}
  ],
  "symbol_stats": [...],
  "equity_curve": [{"ts": 1700000000000, "balance": 1000.0}, ...],
  "recent_trades": [...]
}
```

---

### `POST /walk_forward`
Auth required.

Same fields as `/backtest`, plus:

| Field | Type | Default | Description |
|---|---|---|---|
| `n_folds` | integer | 5 | Number of OOS folds [2, 10] |

**Response:**
```json
{
  "status": "success",
  "n_folds_completed": 5,
  "avg_sharpe": 1.42,
  "avg_sortino": 1.87,
  "consistency": 0.80,
  "stable": true,
  "stability_note": "PASS: consistency>=60% and avg_sharpe>0.50 — edge is temporally stable",
  "folds": [...]
}
```

**Stability verdict:** `"stable": true` requires **consistency ≥ 60%** (fraction of folds with Sharpe > 0) AND **avg\_Sharpe > 0.50**. Do not deploy capital to live trading without a passing walk-forward verdict.

---

### `GET /results`
Auth required. Returns the last `/backtest` summary without the full trade log.

<br>

## Configuration

`settings.json` in the working directory is optional — built-in defaults are safe. All fields are documented in the file itself. Example override:

```json
{
  "initial_balance": ,
  "risk_pct": 0.015,
  "leverage": 5,
  "min_confidence": 0.75
}
```

<br>

## Statistical Methodology

### Sharpe Ratio
Computed using **Welford's online algorithm** for numerically stable variance. Returns are per 60-second bar, annualised by `sqrt(525,600)`. Sample variance (Bessel-corrected, `n−1` denominator).

### Sortino Ratio
Identical annualisation and Bessel correction as Sharpe. Downside semi-variance uses only strictly negative returns. Returns `999.0` when no negative returns exist and mean > 0 (theoretically infinite Sortino).

### Maximum Drawdown
Computed on the absolute equity curve starting from `initial_balance` (not cumulative PnL from zero). This prevents the negative-peak division error that produces NaN when early trades are losses.

### Calmar Ratio
`(annualised_return_pct / 100) / max_drawdown_fraction`. Requires `max_drawdown > 0`; returns `0.0` otherwise.

### Funding Cost
`notional × |funding_rate| × (hours_held / 8)`. Paid by longs when `funding_rate > 0`, by shorts when `funding_rate < 0`. Matches Binance perpetual settlement mechanics.

### Annualisation Convention
All ratios use `sqrt(525,600)` — the number of **60-second bars per year**. This is **not** comparable to daily-bar systems which use `sqrt(252) ≈ 15.87`. The 60-second convention produces ratios approximately 45× larger than daily-bar equivalents for the same underlying edge.

<br>

## Security Model

- **Authentication:** Constant-time XOR-accumulation comparison — immune to timing oracle attacks
- **DoS protection:** 8 MiB per-request body cap with 413 response; `SO_RCVTIMEO = 5s` per connection
- **No secret in source:** `BACKTEST_SECRET` loaded exclusively from environment variable
- **Principle of least privilege
- **Signal safety:** `stop()` is the only operation called from the signal handler; it performs only an atomic store

<br>

## Hardware Requirements

| Tier | Requirement |
|---|---|
| **Production (Release build)** | x86-64-v3: AVX2 + FMA + BMI2 (Intel Haswell 2013+, AMD Zen 1 2017+) |
| **Development (Debug build)** | Any x86-64 CPU; ARM64 supported (Debug only) |
| **Memory** | ~200 MB RSS under full load (4 workers × thread_local buffers) |
| **OS** | Linux (POSIX sockets, `poll`, `gmtime_r`); tested on Debian Bookworm |

<br>

## Design Decisions

**Why no `std::string` on hot paths?**
`char[]` fixed arrays in `Bar`, `Trade`, and `SimPosition` match the executor's wire format exactly and allow `static_assert` size verification. `std::string` introduces heap allocation and makes wire-format alignment non-deterministic.

**Why MPSC and not a work-stealing queue?**
One acceptor thread feeding N workers is the canonical MPSC pattern. Work-stealing adds complexity for no benefit in this topology — the acceptor is the sole producer.

**Why `thread_local` over a mutex-protected shared buffer?**
A mutex protecting a shared `bars_buf` would serialise all request parsing, eliminating parallelism. `thread_local` gives each worker its own 2048-bar buffer at the cost of ~200 KiB per thread — negligible.

**Why walk-forward with fixed parameters?**
Re-fitting parameters per fold tests curve-fitting ability, not edge stability. Fixed parameters test whether the same strategy settings produce positive expectancy across unseen time windows — the relevant question before deploying capital.

**Why `sqrt(525600)` and not `sqrt(252)`?**
The system processes 60-second bars. Using daily annualisation would overstate performance by a factor of `sqrt(525600/252) ≈ 45.7`. The convention is documented explicitly to prevent misinterpretation when comparing against daily-bar benchmarks.

---

*BigBoyAgent TQC Backtest — Taha Iqbal 
Email for buisness related queries (tahaiqbal773@gmail.com)
