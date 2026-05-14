#pragma once
/*
 * backtest_engine.hpp  -  BigBoyAgent TQC Backtest | Taha Iqbal
 *
 * Vectorised event-driven backtest engine.
 * Processes the bar stream sent by executor_main::send_backtest().
 * Requires C++20.
 *
 * Design rules (matching Brain):
 *   - Zero heap allocation after construction.
 *   - All state in fixed-size stack / static arrays.
 *   - constexpr position sizing mirrors Brain's RiskEngine.
 *
 * ── Fixes applied ─────────────────────────────────────────────────────────────
 *
 *  FIX-SIG    run() signature simplified: the external EquityPoint* and
 *             int& n_equity_points parameters are removed.  BacktestResult
 *             now embeds equity_curve[MAX_EQUITY_POINTS] and n_equity_points
 *             (per the fixed types.hpp); callers read the curve from result
 *             directly.  This eliminates the three-way inconsistency where
 *             equity_arr[], the external pointer, and result.equity_curve
 *             all held different views of the same data.
 *
 *  FIX-DEAD   Removed the [[deprecated]] computeMargin member declaration.
 *             The member was dead code — the run() loop calls the standalone
 *             computeMarginGARCH() exclusively.  A deprecated declaration in
 *             a public header pollutes the interface and misleads readers.
 *             The standalone function is defined static in the .cpp and needs
 *             no header declaration.
 */

#include "types.hpp"
#include <span>

namespace tqc {

// ── Walk-forward types ─────────────────────────────────────────────────────────
// One out-of-sample window in a walk-forward validation run.
// Parameters are NOT re-fitted between folds — this tests parameter stability
// (not curve-fitting) by verifying the edge persists across unseen time windows
// with fixed settings.
struct WalkForwardFold {
    int    fold             = 0;
    int    bars_oos         = 0;      // number of bars in this OOS window
    int    total_trades     = 0;
    double sharpe           = 0.0;
    double sortino          = 0.0;
    double calmar           = 0.0;
    double total_return_pct = 0.0;
    double max_drawdown_pct = 0.0;
    double profit_factor    = 0.0;
    double win_rate         = 0.0;
    double initial_balance  = 0.0;
    double final_balance    = 0.0;
};

// Aggregate result across all OOS folds.
struct WalkForwardResult {
    static constexpr int MAX_FOLDS = 10;
    WalkForwardFold folds[MAX_FOLDS]{};
    int    n_folds           = 0;

    // Aggregates (means across folds; worst-case drawdown)
    double avg_sharpe        = 0.0;
    double avg_sortino       = 0.0;
    double avg_calmar        = 0.0;
    double avg_return_pct    = 0.0;
    double worst_drawdown    = 0.0;    // max of max_drawdown_pct across all folds
    double avg_profit_factor = 0.0;
    double avg_win_rate      = 0.0;
    int    total_trades      = 0;

    // Stability metrics.
    // consistency: fraction of folds with Sharpe > 0 (higher = more robust).
    // stable: true when consistency >= 0.60 AND avg_sharpe > 0.50.
    //         A strategy passing this threshold is suitable for live deployment.
    double consistency       = 0.0;
    bool   stable            = false;
};

class BacktestEngine {
public:
    // Run a full backtest over the bar array.
    // On success returns true and populates result, including result.equity_curve
    // and result.n_equity_points.
    // Returns false if cfg is invalid (e.g. initial_balance <= 0).
    //
    // FIX-SIG: external equity_curve* and n_equity_points parameters removed.
    // Read result.equity_curve / result.n_equity_points after a successful call.
    [[nodiscard]] bool run(std::span<const Bar>  bars,
                           const BacktestConfig& cfg,
                           BacktestResult&       result) noexcept;

    // Walk-forward validation: slice bars into n_folds equal OOS windows and run
    // an independent backtest on each.  Parameters are held fixed (no re-fitting)
    // to test temporal stability of the edge.
    //
    // Minimum bars per fold: 10 (enforced — folds with fewer bars are skipped).
    // n_folds is clamped to [2, MAX_FOLDS].
    [[nodiscard]] WalkForwardResult walk_forward(std::span<const Bar>  bars,
                                                  const BacktestConfig& cfg,
                                                  int                   n_folds) noexcept;

private:
    // Position book — mirrors Brain's SimPosition pool.
    static constexpr int MAX_POS = 8;
    SimPosition positions_[MAX_POS]{};
    int         n_positions_ = 0;

    double balance_     = 0.0;
    double peak_bal_    = 0.0;
    double daily_start_ = 0.0;

    // ── Position management ───────────────────────────────────────────────────
    [[nodiscard]] int  findPosition (const char* symbol) const noexcept;
    [[nodiscard]] bool hasPosition  (const char* symbol) const noexcept;
    void               closePosition(int idx, double price,
                                     const char* reason, int64_t ts,
                                     double fee_pct, double slip_pct,
                                     BacktestResult& result) noexcept;

    // FIX-DEAD: computeMargin member declaration removed.
    // The standalone computeMarginGARCH() (static, defined in .cpp) is the sole
    // position-sizing path.  No header declaration needed for a static function.
};

} // namespace tqc
