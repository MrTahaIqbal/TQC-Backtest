#pragma once
/*
 * stats_engine.hpp  -  BigBoyAgent TQC Backtest | Taha Iqbal
 *
 * Stateless statistical functions used by BacktestEngine.
 * All operate on std::span — zero heap allocation, zero pointer/size mismatch risk.
 * Requires C++20.
 *
 * ── Constants ─────────────────────────────────────────────────────────────────
 *
 *  ANNUAL_FACTOR      525,600  (60-second bars: 60 × 24 × 365)
 *                     Exported so callers can de-annualise or log the cadence.
 *
 *  SQRT_ANNUAL_FACTOR sqrt(525,600) ≈ 725.0
 *                     Pre-computed compile-time constant; eliminates repeated
 *                     std::sqrt() calls at runtime in computeSharpe/computeSortino.
 *
 *  All Sharpe and Sortino values produced by this engine use the 60-second-bar
 *  annualisation convention.  They are NOT comparable to daily-bar figures
 *  (daily sqrt(252) ≈ 15.87 vs. 60-sec sqrt(525600) ≈ 725.0).
 *
 * ── Fixes applied ─────────────────────────────────────────────────────────────
 *
 *  FIX-SPAN   Replaced all (const T*, int n) raw pairs with std::span<const T>.
 *             C++20 span is zero-overhead, carries size intrinsically, and
 *             eliminates the entire class of n-mismatch bugs.
 *
 *  FIX-DEFLT  Removed dangerous default for initial_balance in computeSymbolStats.
 *             The old default of 1000.0 silently produced wrong Calmar for any
 *             backtest not starting at exactly $1,000.  Callers must supply it.
 *
 *  FIX-CONST  Exported ANNUAL_FACTOR and SQRT_ANNUAL_FACTOR as inline constexpr
 *             so callers and the .cpp share the same definition (ODR-safe).
 */

#include "types.hpp"
#include <cstddef>
#include <cmath>
#include <span>

namespace tqc {

// ── Annualisation constants ────────────────────────────────────────────────────
// FIX-CONST: Exported here so callers can reference the cadence used and
// the .cpp does not need its own private copy.
inline constexpr double ANNUAL_FACTOR      = 525'600.0;   // 60-sec bars per year
inline constexpr double SQRT_ANNUAL_FACTOR = 725.0;       // std::sqrt(525600), pre-computed

// Static assertion: verify the pre-computed constant is within floating-point
// tolerance of the true sqrt.  Evaluated at compile time; zero runtime cost.
static_assert(SQRT_ANNUAL_FACTOR * SQRT_ANNUAL_FACTOR >= 525'598.0 &&
              SQRT_ANNUAL_FACTOR * SQRT_ANNUAL_FACTOR <= 525'602.0,
    "SQRT_ANNUAL_FACTOR deviates from sqrt(ANNUAL_FACTOR) by more than 2 ULP");

// ── Function declarations ──────────────────────────────────────────────────────

// Annualised Sharpe ratio using Bessel-corrected sample variance (Welford).
// Returns 0.0 for fewer than 2 samples.
// FIX-SPAN: takes span<const double> — no separate size argument needed.
[[nodiscard]] double computeSharpe(std::span<const double> returns) noexcept;

// Annualised Sortino ratio (downside semi-variance, Bessel-corrected).
// Returns 999.0 if no negative returns and mean > 0 (infinite Sortino).
// Returns 0.0 for fewer than 2 samples.
[[nodiscard]] double computeSortino(std::span<const double> returns) noexcept;

// Maximum drawdown as a fraction (0.15 = 15% drawdown).
// equity[] must contain actual account balance values, not PnL deltas.
// Requires equity[0] > 0 — pass initial_balance as equity[0].
[[nodiscard]] double computeMaxDrawdown(std::span<const double> equity) noexcept;

// Calmar ratio = annualised return / max drawdown fraction.
// Returns 0.0 if max_drawdown_frac <= 0.
[[nodiscard]] double computeCalmar(double annual_return_pct,
                                    double max_drawdown_frac) noexcept;

// Profit factor = gross profit / |gross loss| across all trades in the span.
// Returns 999.0 if gross_loss == 0 and gross_win > 0.
// Returns 1.0  if both are zero (no directional edge).
[[nodiscard]] double computeProfitFactor(std::span<const Trade> trades) noexcept;

// Fill SymbolStats for a single symbol from the trade log.
// out is zero-initialised inside this function before accumulation.
// initial_balance: starting account balance in the same currency as Trade::pnl
//                  and Trade::margin.  No default — caller must always supply it
//                  to prevent silent Calmar mis-scaling.
// FIX-DEFLT: removed dangerous default initial_balance = 1000.0.
void computeSymbolStats(const char*           symbol,
                        std::span<const Trade> trades,
                        SymbolStats&          out,
                        double                initial_balance) noexcept;

} // namespace tqc
