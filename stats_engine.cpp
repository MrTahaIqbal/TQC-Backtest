/*
 * stats_engine.cpp  -  BigBoyAgent TQC Backtest | Taha Iqbal
 *
 * ── Fixes applied ─────────────────────────────────────────────────────────────
 *
 *  FIX-WELF   computeSharpe: replaced algebraic one-pass variance formula with
 *             Welford's online algorithm.  One-pass formula
 *             (sq/(n-1) - mean²·n/(n-1)) suffers catastrophic cancellation when
 *             mean is large relative to spread — a common condition in
 *             low-variance return series.  Welford accumulates (x - running_mean)²,
 *             operating on near-zero values, eliminating cancellation entirely.
 *
 *  FIX-ZERO   computeSymbolStats: zero-initialise `out` at function entry.
 *             Old code accumulated into whatever bytes the caller passed,
 *             silently double-counting on reused structs.
 *
 *  FIX-PEAK   computeMaxDrawdown: guard peak == 0 before division.  Old code
 *             divided by equity[0] which could be 0.0 (first bar break-even),
 *             producing NaN/Inf that silently poisoned max_drawdown and Calmar.
 *
 *  FIX-EQ     computeSymbolStats equity curve: running starts at initial_balance
 *             not 0.0.  Starting at 0 made eq[0] equal to first trade PnL; a
 *             losing first trade produced a negative peak in computeMaxDrawdown,
 *             inverting the drawdown sign.
 *
 *  FIX-BOUNDS pnls[]/rets[] writes now guarded by (np < MAX_TRADES) before
 *             increment.  Old code wrote unconditionally — out-of-bounds UB
 *             when n_trades > MAX_TRADES.
 *
 *  FIX-EVEN   Break-even trades (pnl == 0.0) are now an explicit third branch
 *             in both computeProfitFactor and computeSymbolStats.  Old else-branch
 *             counted them as losses, deflating win_rate and avg_loss.
 *
 *  FIX-DEAD   Removed unreachable `if (down_dev == 0.0)` in computeSortino.
 *             After the `down_n == 0` guard, down_sq > 0 always; replaced with
 *             assert() to make the assumption explicit in debug builds.
 *
 *  FIX-SORT   computeSortino downside semi-variance now uses (n-1) Bessel
 *             correction, consistent with computeSharpe.  Old code used n
 *             (population), creating an inconsistency between the two ratios.
 *
 *  FIX-1PASS  computeSymbolStats: merged two O(n) scans of trades[] into one.
 *             Both pnls[np] and rets[np] are filled in the same loop iteration,
 *             eliminating the second scan and the ri < np early-exit ambiguity.
 *
 *  FIX-CMP    strncmp/strncpy limits updated to sizeof(Trade::symbol) = 16.
 *             Old limit of 15 left the 16th byte uncompared, allowing two
 *             different 15-char symbols to incorrectly match.
 *
 *  FIX-SPAN   All function signatures updated to std::span<const T> per header.
 *             Internal loops use span.size() — no separate int n argument.
 *
 *  FIX-SQRT   std::sqrt(ANNUAL_FACTOR) replaced by SQRT_ANNUAL_FACTOR (compile-
 *             time constant from header).  Eliminates repeated runtime sqrt().
 */

#include "stats_engine.hpp"
#include <cstring>
#include <cmath>
#include <cassert>
#include <algorithm>
#include <span>

namespace tqc {

// ── computeSharpe ─────────────────────────────────────────────────────────────
// Annualised Sharpe using Welford's numerically stable online variance.
//
// Welford accumulates M2 = Σ(x - mean_so_far)² in a single pass.
// Sample variance = M2 / (n-1).  This avoids the catastrophic cancellation
// inherent in the one-pass formula (Σx² - n·x̄²)/(n-1) when values cluster
// near a non-zero mean.
//
// FIX-WELF, FIX-SPAN, FIX-SQRT
double computeSharpe(std::span<const double> returns) noexcept {
    const auto n = static_cast<int>(returns.size());
    if (n < 2) return 0.0;

    double mean = 0.0, M2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double delta  = returns[i] - mean;
        mean               += delta / static_cast<double>(i + 1);
        const double delta2 = returns[i] - mean;
        M2                 += delta * delta2;   // Welford update
    }

    // Sample variance (Bessel-corrected): M2 / (n-1)
    const double var = M2 / static_cast<double>(n - 1);
    if (var <= 0.0) return 0.0;

    // FIX-SQRT: SQRT_ANNUAL_FACTOR is compile-time, no runtime sqrt() call.
    return (mean / std::sqrt(var)) * SQRT_ANNUAL_FACTOR;
}

// ── computeSortino ────────────────────────────────────────────────────────────
// Annualised Sortino using Bessel-corrected downside semi-variance.
//
// Downside semi-variance = Σ min(r,0)² / (n-1)   [only negative r contribute]
// Using (n-1) is consistent with computeSharpe and standard financial practice.
//
// FIX-SORT, FIX-DEAD, FIX-SPAN, FIX-SQRT
double computeSortino(std::span<const double> returns) noexcept {
    const auto n = static_cast<int>(returns.size());
    if (n < 2) return 0.0;

    double sum     = 0.0;
    double down_sq = 0.0;
    int    down_n  = 0;

    for (int i = 0; i < n; ++i) {
        sum += returns[i];
        if (returns[i] < 0.0) {
            down_sq += returns[i] * returns[i];
            ++down_n;
        }
    }

    const double mean = sum / static_cast<double>(n);

    // No negative returns: Sortino is theoretically infinite when mean > 0.
    if (down_n == 0) return (mean > 0.0) ? 999.0 : 0.0;

    // FIX-SORT: Bessel-corrected downside semi-variance (n-1), consistent
    // with computeSharpe.  down_sq > 0 guaranteed here (down_n >= 1,
    // all contributing returns are strictly < 0).
    const double down_dev = std::sqrt(down_sq / static_cast<double>(n - 1));

    // FIX-DEAD: Removed unreachable `if (down_dev == 0.0)` branch.
    // down_sq > 0 when down_n >= 1 with strictly negative returns,
    // so down_dev > 0 unconditionally.  Assert in debug; never fires.
    assert(down_dev > 0.0 && "down_dev == 0 is unreachable after down_n > 0 guard");

    // FIX-SQRT: compile-time constant.
    return (mean / down_dev) * SQRT_ANNUAL_FACTOR;
}

// ── computeMaxDrawdown ────────────────────────────────────────────────────────
// Maximum drawdown as a fraction (0.15 = 15%).
//
// equity[] must be actual account balance values (e.g. starting at initial_balance),
// NOT cumulative PnL deltas starting from 0.  Pass initial_balance as equity[0]
// to ensure peak is always positive and drawdown is meaningful.
//
// FIX-PEAK: peak == 0 guard prevents division by zero / NaN propagation.
// FIX-SPAN
double computeMaxDrawdown(std::span<const double> equity) noexcept {
    const auto n = static_cast<int>(equity.size());
    if (n < 2) return 0.0;

    double peak   = equity[0];
    double max_dd = 0.0;

    for (int i = 1; i < n; ++i) {
        if (equity[i] > peak) {
            peak = equity[i];
        }
        // FIX-PEAK: skip division when peak is zero or negative.
        // A zero/negative peak means the equity curve started at or below zero,
        // which indicates the caller did not pass initial_balance as equity[0].
        // We skip rather than assert so this degrades gracefully in release.
        if (peak <= 0.0) continue;

        const double dd = (peak - equity[i]) / peak;
        if (dd > max_dd) max_dd = dd;
    }
    return max_dd;
}

// ── computeCalmar ─────────────────────────────────────────────────────────────
double computeCalmar(double annual_return_pct, double max_drawdown_frac) noexcept {
    if (max_drawdown_frac <= 0.0) return 0.0;
    return (annual_return_pct / 100.0) / max_drawdown_frac;
}

// ── computeProfitFactor ───────────────────────────────────────────────────────
// FIX-EVEN: break-even trades (pnl == 0.0) are explicitly skipped — they
// contribute neither to gross_win nor gross_loss.  Old else-branch added 0.0
// to gross_loss, which was numerically harmless but semantically wrong and
// masked the third-case distinction.
// FIX-SPAN
double computeProfitFactor(std::span<const Trade> trades) noexcept {
    double gross_win  = 0.0;
    double gross_loss = 0.0;

    for (const Trade& t : trades) {
        if      (t.pnl > 0.0) gross_win  += t.pnl;
        else if (t.pnl < 0.0) gross_loss -= t.pnl;   // gross_loss accumulates as positive
        // pnl == 0.0: break-even — intentionally ignored
    }

    if (gross_loss == 0.0) return (gross_win > 0.0) ? 999.0 : 1.0;
    return gross_win / gross_loss;
}

// ── computeSymbolStats ────────────────────────────────────────────────────────
// Fills SymbolStats for one symbol from the full trade log.
//
// Key invariants:
//   - out is zero-initialised here; callers must not pre-fill it.
//   - equity curve starts at initial_balance (not 0.0) to ensure peak > 0.
//   - Single O(n) pass over trades[] fills both pnls[] and rets[] together.
//   - Break-even trades do not count as wins or losses.
//   - np capped at MAX_TRADES to prevent out-of-bounds write.
//
// FIX-ZERO, FIX-EQ, FIX-1PASS, FIX-EVEN, FIX-BOUNDS, FIX-CMP, FIX-SPAN
void computeSymbolStats(const char*            symbol,
                        std::span<const Trade>  trades,
                        SymbolStats&            out,
                        double                  initial_balance) noexcept {
    // FIX-ZERO: zero-initialise before any accumulation.
    // Prevents double-counting when `out` is reused across calls.
    out = SymbolStats{};
    std::strncpy(out.symbol, symbol, sizeof(out.symbol) - 1);
    out.symbol[sizeof(out.symbol) - 1] = '\0';

    double win_sum  = 0.0;
    double loss_sum = 0.0;

    // FIX-SPAN: thread_local scratch arrays — one allocation per thread for
    // the process lifetime.  MAX_TRADES × 8 bytes × 3 arrays = 192 KiB per
    // thread; acceptable thread-local storage, avoids stack overflow.
    thread_local double pnls[MAX_TRADES];
    thread_local double rets[MAX_TRADES];   // fractional return = pnl / margin
    int np = 0;

    // FIX-1PASS: single pass fills both pnls[] and rets[] simultaneously.
    // FIX-CMP:   compare full 16-byte field (sizeof(Trade::symbol)).
    // FIX-EVEN:  explicit three-way branch for win / loss / break-even.
    // FIX-BOUNDS: cap at MAX_TRADES before writing to scratch arrays.
    for (const Trade& t : trades) {
        if (std::strncmp(t.symbol, symbol, sizeof(Trade::symbol)) != 0) continue;

        // FIX-BOUNDS: never write past the end of the scratch arrays.
        if (np >= MAX_TRADES) {
            // Truncated: total_trades still counts, metrics are partial.
            // This mirrors the trades_truncated flag in BacktestResult.
            ++out.total_trades;
            if      (t.pnl > 0.0) { ++out.wins; win_sum  += t.pnl; }
            else if (t.pnl < 0.0) {              loss_sum -= t.pnl; }
            out.net_pnl += t.pnl;
            continue;
        }

        ++out.total_trades;
        out.net_pnl += t.pnl;
        pnls[np] = t.pnl;

        // Fractional return = PnL / margin.  Dimensionless, required for Sharpe.
        rets[np] = (t.margin > 0.0) ? t.pnl / t.margin : 0.0;

        if      (t.pnl > 0.0) { ++out.wins; win_sum  += t.pnl; }
        else if (t.pnl < 0.0) {              loss_sum -= t.pnl; }
        // pnl == 0.0: break-even — FIX-EVEN: counted in total, not win or loss

        ++np;
    }

    if (out.total_trades == 0) return;

    const int losses = out.total_trades - out.wins;  // includes break-even trades
    // For avg_loss denominator: count only strictly losing trades
    int strict_losses = 0;
    for (int i = 0; i < np; ++i) if (pnls[i] < 0.0) ++strict_losses;

    out.win_rate      = static_cast<double>(out.wins) / static_cast<double>(out.total_trades);
    out.avg_win       = (out.wins > 0)        ? win_sum  / static_cast<double>(out.wins)         : 0.0;
    out.avg_loss      = (strict_losses > 0)   ? loss_sum / static_cast<double>(strict_losses)    : 0.0;
    out.profit_factor = (loss_sum > 0.0)      ? win_sum  / loss_sum
                                              : (win_sum > 0.0 ? 999.0 : 1.0);

    // Sharpe and Sortino on fractional returns (dimensionless, per-trade).
    out.sharpe  = computeSharpe (std::span<const double>{rets, static_cast<std::size_t>(np)});
    out.sortino = computeSortino(std::span<const double>{rets, static_cast<std::size_t>(np)});

    // ── Drawdown on absolute equity curve ─────────────────────────────────────
    // FIX-EQ: equity starts at initial_balance so eq[0] > 0 always,
    // preventing the negative-peak / division-by-zero in computeMaxDrawdown.
    // We build the curve into rets[] (reused as eq[] scratch) to save memory.
    thread_local double eq[MAX_TRADES + 1];  // +1 for initial_balance seed point
    eq[0] = initial_balance;
    for (int i = 0; i < np; ++i) eq[i + 1] = eq[i] + pnls[i];

    out.max_drawdown = computeMaxDrawdown(
        std::span<const double>{eq, static_cast<std::size_t>(np + 1)});

    // Calmar: convert net PnL to a return percentage of initial_balance.
    const double ret_pct = (initial_balance > 0.0)
        ? (out.net_pnl / initial_balance) * 100.0
        : 0.0;
    out.calmar = computeCalmar(ret_pct, out.max_drawdown);
}

} // namespace tqc
