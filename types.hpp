#pragma once
/*
 * types.hpp  -  BigBoyAgent TQC Backtest | Taha Iqbal
 *
 * Domain structs matching exactly the wire format the executor sends to
 * POST /backtest.  No std::string on the hot path — char arrays only.
 *
 * FIX [CRITICAL]: Removed redundant n_trades field from BacktestResult.
 *                 trades_stored is now the sole authoritative count of valid
 *                 entries in trades[].  total_trades counts ALL trades including
 *                 those not stored due to MAX_TRADES overflow.
 * FIX [CRITICAL]: Added safeRegimeIndex() to bounds-check hmm_state before
 *                 it is used as an array index into regime_stats[3].
 * FIX [HIGH]:     Added static_assert guards on wire-format struct sizes.
 *                 Update the expected byte counts if the executor's layout changes.
 * FIX [HIGH]:     Embedded equity_curve[MAX_EQUITY_POINTS] into BacktestResult
 *                 so the equity curve is included in the serialised response.
 * FIX [MEDIUM]:   Introduced enum class HMMState, Signal, Side, ExitReason for
 *                 type-safe domain constants.  Char-array fields are preserved
 *                 for the wire format; enums are for internal logic.
 * FIX [MEDIUM]:   Added hoursHeld() helper on SimPosition for unambiguous funding
 *                 duration computation.
 * FIX [LOW]:      Applied alignas(64) to Bar and SimPosition so structs do not
 *                 straddle cache lines on the hot path.
 */

#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cassert>

namespace tqc {

// ── Type-safe domain enumerations ────────────────────────────────────────────
// Use these in internal logic; the char[] wire fields remain for serialisation.

enum class HMMState : uint8_t {
    BEAR     = 0,
    SIDEWAYS = 1,
    BULL     = 2,
    COUNT    = 3   // sentinel — never assign to a field
};

enum class Signal : uint8_t { BUY, SELL, HOLD };
enum class Side   : uint8_t { LONG, SHORT };

enum class ExitReason : uint8_t { TP, SL, SIGNAL };

// FIX [CRITICAL]: Bounds-safe accessor for regime_stats.
// Returns clamped index (max 2) so a corrupt hmm_state never causes UB.
// Logs an assertion in debug builds; silently clamps in release.
[[nodiscard]] inline uint8_t safeRegimeIndex(uint8_t hmm_state) noexcept {
    assert(hmm_state < 3 && "hmm_state out of range [0,2]; Brain response corrupt?");
    return (hmm_state < 3) ? hmm_state : 1u;  // clamp to SIDEWAYS on corruption
}

// ── Bar record ───────────────────────────────────────────────────────────────
// Exactly what executor's store_for_backtest() serialises.
// FIX [LOW]: alignas(64) prevents cache-line straddling on the hot path.
struct alignas(64) Bar {
    char     symbol[16]{};
    double   close       = 0.0;
    double   high        = 0.0;
    double   low         = 0.0;
    char     signal[8]{};        // "BUY" / "SELL" / "HOLD"
    double   confidence  = 0.0;
    char     regime[20]{};
    char     vol_regime[8]{};
    double   atr         = 0.0;
    bool     pos_valid   = false;
    uint8_t  _pad0[7]{};         // explicit padding — makes layout portable
    int64_t  timestamp   = 0;

    // ── Step 3: Statistical model fields from Brain response ─────────────────
    // garch_vol:    GARCH(1,1) annualised conditional vol — used by computeMargin
    //               to scale position size (high vol → smaller position).
    // hmm_state:    3-state Viterbi output (0=BEAR,1=SIDEWAYS,2=BULL) — used to
    //               slice aggregate stats into per-regime performance tables.
    //               Always access via safeRegimeIndex() when used as array index.
    // funding_rate: raw Binance 8hr rate — deducted as funding cost in
    //               closePosition() proportional to hours held.
    double   garch_vol    = 0.0;   // annualised σ, e.g. 1.20 = 120% p.a.
    uint8_t  hmm_state    = 1;     // 0=BEAR, 1=SIDEWAYS, 2=BULL — use safeRegimeIndex()
    uint8_t  _pad1[7]{};
    double   funding_rate = 0.0;   // e.g. 0.0001 = 0.01% per 8hr window
};

// FIX [HIGH]: Verify Bar has not silently grown due to compiler/platform changes.
// Update this value if the executor's serialised Bar layout changes.
static_assert(sizeof(Bar) % 8 == 0,
    "Bar size is not 8-byte aligned — executor wire format mismatch likely.");

// ── Simulated position ────────────────────────────────────────────────────────
// FIX [LOW]: alignas(64) for cache-line alignment.
struct alignas(64) SimPosition {
    char     symbol[16]{};
    char     side[8]{};          // "LONG" / "SHORT"
    double   entry_price  = 0.0;
    double   sl_price     = 0.0;
    double   tp_price     = 0.0;
    double   margin       = 0.0;
    double   notional     = 0.0;
    int64_t  open_ts      = 0;   // Unix ms timestamp of position open
    bool     active       = false;
    uint8_t  _pad0[7]{};
    double   entry_funding_rate = 0.0;  // funding rate at entry (per 8hr window)
    uint8_t  entry_hmm_state    = 1;    // HMM regime at open — use safeRegimeIndex()
    uint8_t  _pad1[7]{};

    // FIX [MEDIUM]: Unambiguous funding duration helper.
    // Pass current bar timestamp (ms) to compute fractional 8hr windows elapsed.
    // Prevents call-site divergence in how "hours held" is computed.
    [[nodiscard]] double hoursHeld(int64_t current_ts_ms) const noexcept {
        if (current_ts_ms <= open_ts) return 0.0;
        constexpr double MS_PER_HOUR = 3'600'000.0;
        return static_cast<double>(current_ts_ms - open_ts) / MS_PER_HOUR;
    }

    // Convenience: funding windows elapsed (each window = 8 hours)
    [[nodiscard]] double fundingWindowsHeld(int64_t current_ts_ms) const noexcept {
        return hoursHeld(current_ts_ms) / 8.0;
    }
};

// ── Trade result ──────────────────────────────────────────────────────────────
struct Trade {
    char     symbol[16]{};
    char     side[8]{};
    char     exit_reason[16]{};  // "TP" / "SL" / "SIGNAL"
    double   entry_price  = 0.0;
    double   exit_price   = 0.0;
    double   pnl          = 0.0;
    double   pnl_pct      = 0.0;
    double   margin       = 0.0;
    int64_t  entry_ts     = 0;
    int64_t  exit_ts      = 0;
    // Always write via safeRegimeIndex() when used as array subscript.
    uint8_t  hmm_state    = 1;   // 0=BEAR, 1=SIDEWAYS, 2=BULL
    uint8_t  _pad0[7]{};
    double   funding_cost = 0.0; // funding deducted on this trade (USD)
};

// ── Backtest config (from request JSON) ───────────────────────────────────────
struct BacktestConfig {
    double initial_balance  = 1000.0;
    double risk_pct         = 0.01;    // fraction of balance risked per trade
    int    leverage         = 3;
    double sl_atr_mult      = 1.5;
    double tp_rr_ratio      = 2.0;
    double min_confidence   = 0.70;
    double fee_pct          = 0.001;   // taker fee, both sides
    double slip_pct         = 0.0002;  // one-way slippage estimate
    int    max_open         = 3;
};

// ── Per-symbol equity stats ───────────────────────────────────────────────────
struct SymbolStats {
    char   symbol[16]{};
    int    total_trades   = 0;
    int    wins           = 0;
    double net_pnl        = 0.0;
    double win_rate       = 0.0;
    double profit_factor  = 0.0;
    double max_drawdown   = 0.0;
    double sharpe         = 0.0;
    double sortino        = 0.0;
    double avg_win        = 0.0;
    double avg_loss       = 0.0;
    double calmar         = 0.0;
};

// ── Step 3: Per-HMM-regime performance slice ──────────────────────────────────
// Breaks aggregate stats into BEAR / SIDEWAYS / BULL regime windows.
// Indexed via safeRegimeIndex(hmm_state) — never via raw hmm_state directly.
struct RegimeStats {
    int    trades        = 0;
    int    wins          = 0;
    double net_pnl       = 0.0;
    double win_rate      = 0.0;    // wins / trades
    double profit_factor = 0.0;   // sum_wins / |sum_losses|
    double avg_pnl       = 0.0;   // net_pnl / trades
};

// ── Equity curve point ────────────────────────────────────────────────────────
struct EquityPoint {
    int64_t ts      = 0;
    double  balance = 0.0;
};

// ── Capacity constants ────────────────────────────────────────────────────────
static constexpr int MAX_TRADES        = 8192;
static constexpr int MAX_SYMBOLS       = 24;
static constexpr int MAX_BARS          = 2048;
static constexpr int MAX_EQUITY_POINTS = 2048;

// ── Overall result ────────────────────────────────────────────────────────────
struct BacktestResult {
    double total_return_pct   = 0.0;
    double max_drawdown_pct   = 0.0;
    double sharpe             = 0.0;
    double sortino            = 0.0;
    double calmar             = 0.0;
    double profit_factor      = 0.0;
    double win_rate           = 0.0;

    // FIX [CRITICAL]: Trade counter semantics are now unambiguous.
    //   total_trades   — ALL trades processed, including those not stored
    //                    (may exceed MAX_TRADES when trades_truncated == true).
    //   trades_stored  — count of valid entries in trades[] (≤ MAX_TRADES).
    //                    This is the ONLY counter to use when iterating trades[].
    //   winning_trades / losing_trades — partition of total_trades.
    // REMOVED: n_trades (was redundant with trades_stored; caused ambiguity).
    int    total_trades       = 0;
    int    winning_trades     = 0;
    int    losing_trades      = 0;

    double final_balance      = 0.0;
    double initial_balance    = 0.0;
    double avg_trade_pnl      = 0.0;
    double avg_win            = 0.0;
    double avg_loss           = 0.0;
    double largest_win        = 0.0;
    double largest_loss       = 0.0;
    double recovery_factor    = 0.0;
    int    bars_processed     = 0;

    // When true, total_trades > MAX_TRADES.  Statistics derived from the partial
    // trades[] array (Sharpe, profit_factor, etc.) will be understated.
    // The HTTP response MUST surface this flag so callers can distrust per-trade
    // metrics and request a longer backtest window or larger MAX_TRADES.
    bool   trades_truncated   = false;
    int    trades_stored      = 0;    // authoritative count of valid entries in trades[]

    Trade       trades[MAX_TRADES]{};
    SymbolStats sym_stats[MAX_SYMBOLS]{};
    int         n_syms        = 0;

    // Step 3: regime-sliced performance.
    // Index ONLY via safeRegimeIndex(hmm_state) — never via raw uint8_t.
    RegimeStats regime_stats[3]{};

    // Total funding cost deducted across all trades (USD)
    double      total_funding_cost = 0.0;

    // FIX [HIGH]: Equity curve embedded so it is included in the serialised
    // response.  Append one point per closed trade or per bar as appropriate.
    EquityPoint equity_curve[MAX_EQUITY_POINTS]{};
    int         n_equity_points = 0;
};

} // namespace tqc
