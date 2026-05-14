/*
 * backtest_engine.cpp  -  BigBoyAgent TQC Backtest | Taha Iqbal
 *
 * Event-driven simulation engine.
 *
 * Per-bar logic:
 *   1. Check all open positions for SL / TP hit (use high/low of bar).
 *   2. If a new BUY or SELL signal arrives and pos_valid==true and no open
 *      position for that symbol, open a new position.
 *   3. If a HOLD or opposite signal arrives on an open position, close it.
 *
 * Position sizing mirrors Brain's RiskEngine::sizePosition:
 *   margin   = balance * risk_pct / sl_pct   (scaled by GARCH vol when present)
 *   sl_dist  = ATR * sl_atr_mult
 *   notional = margin * leverage
 *   sl_price = entry ± sl_dist
 *   tp_price = entry ± (sl_dist * tp_rr_ratio)
 *
 * Fees and slippage are applied symmetrically on open and close.
 *
 * ── Fixes applied ─────────────────────────────────────────────────────────────
 *
 *  FIX-NTRD   Replaced every reference to result.n_trades (removed from types.hpp
 *             as redundant) with result.trades_stored, which is now the sole
 *             authoritative write cursor and iteration bound for trades[].
 *             result.total_trades counts all trades including overflow;
 *             result.trades_stored counts entries actually written to trades[].
 *
 *  FIX-SPAN   All stats-engine call sites updated to construct std::span
 *             explicitly, matching the updated stats_engine.hpp signatures.
 *             computeMaxDrawdown, computeSharpe, computeSortino,
 *             computeProfitFactor, and computeSymbolStats all now receive spans.
 *
 *  FIX-CMP    All strncmp / strncpy limits updated from 15 to sizeof(field)-1
 *             (i.e. sizeof(Bar::symbol)-1 = 15 for comparison, which is correct,
 *             but the null-terminator byte was previously uncopied in strncpy
 *             calls where limit=15 left position 15 uninitialised — now explicit
 *             null termination is applied uniformly).  Field-size constants are
 *             used so any upstream resize is automatically reflected.
 *
 *  FIX-EQC    result.equity_curve and result.n_equity_points (embedded in
 *             BacktestResult per the fixed types.hpp) are now populated directly.
 *             The local equity_arr[] and the external EquityPoint* parameter
 *             are both removed.  computeMaxDrawdown receives a span projected
 *             from result.equity_curve to avoid a second local array.
 *             walk_forward no longer needs thread_local fold_equity[].
 *
 *  FIX-NAME   Renamed daily_rets -> bar_rets throughout.  These are per-60-second-
 *             bar returns annualised by sqrt(525600), not daily returns.
 *             The old name implied daily cadence (sqrt(252)) and would mislead
 *             any developer auditing the annualisation factor.
 *
 *  FIX-EVEN   Regime stats: break-even trades (pnl == 0.0) now have an explicit
 *             third branch.  Old else-clause contributed 0 to rs_loss_sum (harmless)
 *             but left rs.wins unchanged, understating win_rate for regimes that
 *             include break-even trades.
 *
 *  FIX-ROLL   Removed the misleading comment around the balance rollback block.
 *             The pre-check `margin <= balance_ * 0.95` guarantees balance can
 *             never go negative after deducting margin + open_fee (which is at
 *             most margin * leverage * fee_pct = margin * 0.003 at defaults,
 *             well within the guaranteed headroom of margin * 0.0526).
 *             The rollback guard is kept as a defensive belt-and-suspenders
 *             check for non-default configs, but the comment no longer implies
 *             it is a frequent real-world path.
 */

#include "backtest_engine.hpp"
#include "stats_engine.hpp"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <span>

namespace tqc {

// Minimum bars per walk-forward fold.
static constexpr int MIN_FOLD_BARS = 10;

// ── Helpers ───────────────────────────────────────────────────────────────────

int BacktestEngine::findPosition(const char* symbol) const noexcept {
    // FIX-CMP: iterate all MAX_POS slots (sparse pool) and compare full field.
    for (int i = 0; i < MAX_POS; ++i)
        if (positions_[i].active &&
            std::strncmp(positions_[i].symbol, symbol,
                         sizeof(SimPosition::symbol) - 1) == 0)
            return i;
    return -1;
}

bool BacktestEngine::hasPosition(const char* symbol) const noexcept {
    return findPosition(symbol) >= 0;
}

void BacktestEngine::closePosition(int idx, double price,
                                    const char* reason, int64_t ts,
                                    double fee_pct, double slip_pct,
                                    BacktestResult& result) noexcept {
    SimPosition& pos = positions_[idx];
    if (!pos.active) return;

    const bool is_long = std::strncmp(pos.side, "LONG", 4) == 0;

    // Slippage on exit: adverse for the position direction.
    const double exit_price = is_long ? price * (1.0 - slip_pct)
                                      : price * (1.0 + slip_pct);

    // PnL: notional * (exit - entry) / entry * direction, minus fees.
    const double direction  = is_long ? 1.0 : -1.0;
    const double pnl_gross  = pos.notional * direction *
                              (exit_price - pos.entry_price) / pos.entry_price;
    const double fee        = (pos.notional * fee_pct) * 2.0;   // open + close

    // ── Funding cost deduction ─────────────────────────────────────────────────
    // Binance perpetual funding is paid every 8 hours by the position holder
    // (long pays when funding > 0; short pays when funding < 0).
    // Cost = notional * |funding_rate| * n_periods, where n_periods = hours / 8.
    // Uses hoursHeld() helper from the fixed SimPosition (types.hpp) to ensure
    // consistent duration computation across all call sites.
    double funding_cost = 0.0;
    if (pos.open_ts > 0 && ts > pos.open_ts) {
        const double n_periods  = pos.fundingWindowsHeld(ts);
        const bool pays_funding = (is_long  && pos.entry_funding_rate > 0.0)
                               || (!is_long && pos.entry_funding_rate < 0.0);
        if (pays_funding)
            funding_cost = pos.notional * std::abs(pos.entry_funding_rate) * n_periods;
    }

    const double pnl_net = pnl_gross - fee - funding_cost;

    balance_ += pos.margin + pnl_net;
    result.total_funding_cost += funding_cost;
    if (balance_ > peak_bal_) peak_bal_ = balance_;

    // FIX-NTRD: trades_stored is the authoritative write cursor and count.
    // total_trades counts every trade (including overflow); trades_stored counts
    // only entries actually written into trades[].
    if (result.trades_stored < MAX_TRADES) {
        Trade& t = result.trades[result.trades_stored];

        // FIX-CMP: copy full fields with explicit null termination.
        std::strncpy(t.symbol,      pos.symbol, sizeof(Trade::symbol) - 1);
        t.symbol[sizeof(Trade::symbol) - 1] = '\0';
        std::strncpy(t.side,        pos.side,   sizeof(Trade::side) - 1);
        t.side[sizeof(Trade::side) - 1] = '\0';
        std::strncpy(t.exit_reason, reason,     sizeof(Trade::exit_reason) - 1);
        t.exit_reason[sizeof(Trade::exit_reason) - 1] = '\0';

        t.entry_price  = pos.entry_price;
        t.exit_price   = exit_price;
        t.pnl          = pnl_net;
        t.pnl_pct      = (pos.margin > 0.0) ? (pnl_net / pos.margin) * 100.0 : 0.0;
        t.margin       = pos.margin;
        t.entry_ts     = pos.open_ts;
        t.exit_ts      = ts;
        t.hmm_state    = pos.entry_hmm_state;
        t.funding_cost = funding_cost;

        ++result.trades_stored;   // advance cursor AFTER writing, not before
    } else {
        result.trades_truncated = true;
    }

    ++result.total_trades;
    if      (pnl_net > 0.0) ++result.winning_trades;
    else if (pnl_net < 0.0) ++result.losing_trades;
    // pnl_net == 0.0: break-even — counted in total_trades, not win or loss.

    pos.active = false;
    --n_positions_;
}

// ── GARCH-scaled position sizing ──────────────────────────────────────────────
// Mirrors Brain's live RiskEngine.  High predicted vol → smaller position.
// Scale factor: TARGET_VOL / garch_vol, clamped to [0.5, 1.0].
// When garch_vol == 0 (field not populated) falls back to standard sizing.
[[nodiscard]] static double computeMarginGARCH(double balance, double risk_pct,
                                                double price, double atr,
                                                double sl_atr_mult, int leverage,
                                                double garch_vol) noexcept {
    if (price <= 0.0 || atr <= 0.0) return 0.0;
    const double sl_dist = atr * sl_atr_mult;
    const double sl_pct  = sl_dist / price;
    if (sl_pct <= 0.0) return 0.0;

    double margin = (balance * risk_pct) / sl_pct;

    if (garch_vol > 0.1) {
        static constexpr double TARGET_VOL = 0.80;
        const double scale = std::clamp(TARGET_VOL / garch_vol, 0.5, 1.0);
        margin *= scale;
    }

    // Cap: margin ≤ 30% of balance to prevent oversizing.
    margin = std::min(margin, balance * 0.30);
    // Min notional guard: reject positions below exchange minimum.
    if (margin * leverage < 5.0) return 0.0;
    return margin;
}

// ── Main run ──────────────────────────────────────────────────────────────────

bool BacktestEngine::run(std::span<const Bar>  bars,
                          const BacktestConfig& cfg,
                          BacktestResult&       result) noexcept {

    if (cfg.initial_balance <= 0.0) return false;

    // Initialise engine state.
    balance_     = cfg.initial_balance;
    peak_bal_    = cfg.initial_balance;
    daily_start_ = cfg.initial_balance;
    n_positions_ = 0;

    // Zero-initialise result and stamp the starting balance.
    result = BacktestResult{};
    result.initial_balance = cfg.initial_balance;

    // FIX-NAME: bar_rets (was daily_rets) — per-60-second-bar fractional returns,
    // annualised by SQRT_ANNUAL_FACTOR = sqrt(525600).  Not daily returns.
    // Stack allocation: MAX_EQUITY_POINTS × 8 bytes = 16 KiB.  Safe on all
    // supported platforms (Linux default stack 8 MB, Windows per-thread 1 MB).
    double bar_rets[MAX_EQUITY_POINTS];
    int    n_rets = 0;

    double prev_bal = balance_;

    // Per-symbol last-seen close prices — needed for accurate EOD closing.
    // B-02 FIX (preserved): tracks the last close per symbol so positions are
    // closed at their own symbol's price, not the last bar's price globally.
    char   last_px_sym[MAX_SYMBOLS][16]{};
    double last_px_val[MAX_SYMBOLS]{};
    int    last_px_n = 0;

    auto update_last_px = [&](const char* sym, double px) noexcept {
        for (int i = 0; i < last_px_n; ++i)
            if (std::strncmp(last_px_sym[i], sym,
                             sizeof(Bar::symbol) - 1) == 0)
                { last_px_val[i] = px; return; }
        if (last_px_n < MAX_SYMBOLS) {
            std::strncpy(last_px_sym[last_px_n], sym, sizeof(Bar::symbol) - 1);
            last_px_sym[last_px_n][sizeof(Bar::symbol) - 1] = '\0';
            last_px_val[last_px_n] = px;
            ++last_px_n;
        }
    };

    auto get_last_px = [&](const char* sym) noexcept -> double {
        for (int i = 0; i < last_px_n; ++i)
            if (std::strncmp(last_px_sym[i], sym,
                             sizeof(Bar::symbol) - 1) == 0) return last_px_val[i];
        return 0.0;
    };

    // ── Bar loop ─────────────────────────────────────────────────────────────
    for (const Bar& bar : bars) {
        if (!bar.pos_valid && bar.close <= 0.0) continue;

        ++result.bars_processed;
        const int64_t ts = bar.timestamp;

        if (bar.close > 0.0) update_last_px(bar.symbol, bar.close);

        // ── 1. Check SL/TP on all open positions ──────────────────────────────
        for (int i = 0; i < MAX_POS; ++i) {
            SimPosition& pos = positions_[i];
            if (!pos.active) continue;
            // Only evaluate on the matching symbol's bar.
            if (std::strncmp(pos.symbol, bar.symbol,
                             sizeof(SimPosition::symbol) - 1) != 0) continue;

            const bool is_long = std::strncmp(pos.side, "LONG", 4) == 0;

            // When both SL and TP fall within the bar's high-low range, resolve
            // ordering by proximity to entry: the closer trigger is hit first.
            // This eliminates the pessimistic bias of always checking SL first
            // and matches the Zipline / Backtrader proximity heuristic.
            if (is_long) {
                const bool sl_hit = (bar.low  <= pos.sl_price) && (pos.sl_price > 0.0);
                const bool tp_hit = (bar.high >= pos.tp_price) && (pos.tp_price > 0.0);

                if (sl_hit && tp_hit) {
                    const double sl_dist = std::abs(pos.entry_price - pos.sl_price);
                    const double tp_dist = std::abs(pos.tp_price    - pos.entry_price);
                    if (sl_dist <= tp_dist)
                        closePosition(i, pos.sl_price, "SL", ts, cfg.fee_pct, cfg.slip_pct, result);
                    else
                        closePosition(i, pos.tp_price, "TP", ts, cfg.fee_pct, cfg.slip_pct, result);
                } else if (sl_hit) {
                    closePosition(i, pos.sl_price, "SL", ts, cfg.fee_pct, cfg.slip_pct, result);
                } else if (tp_hit) {
                    closePosition(i, pos.tp_price, "TP", ts, cfg.fee_pct, cfg.slip_pct, result);
                }
            } else {
                const bool sl_hit = (bar.high >= pos.sl_price) && (pos.sl_price > 0.0);
                const bool tp_hit = (bar.low  <= pos.tp_price) && (pos.tp_price > 0.0);

                if (sl_hit && tp_hit) {
                    const double sl_dist = std::abs(pos.sl_price    - pos.entry_price);
                    const double tp_dist = std::abs(pos.entry_price - pos.tp_price);
                    if (sl_dist <= tp_dist)
                        closePosition(i, pos.sl_price, "SL", ts, cfg.fee_pct, cfg.slip_pct, result);
                    else
                        closePosition(i, pos.tp_price, "TP", ts, cfg.fee_pct, cfg.slip_pct, result);
                } else if (sl_hit) {
                    closePosition(i, pos.sl_price, "SL", ts, cfg.fee_pct, cfg.slip_pct, result);
                } else if (tp_hit) {
                    closePosition(i, pos.tp_price, "TP", ts, cfg.fee_pct, cfg.slip_pct, result);
                }
            }
        }

        // ── 2. Process signal ─────────────────────────────────────────────────
        const bool is_buy    = std::strncmp(bar.signal, "BUY",  3) == 0;
        const bool is_sell   = std::strncmp(bar.signal, "SELL", 4) == 0;
        const bool tradeable = bar.confidence >= cfg.min_confidence && bar.pos_valid;

        if (tradeable) {
            // Close any position whose direction conflicts with the new signal.
            {
                int existing = findPosition(bar.symbol);
                if (existing >= 0) {
                    SimPosition& epos = positions_[existing];
                    const bool epos_long = std::strncmp(epos.side, "LONG", 4) == 0;
                    if ((is_buy && !epos_long) || (is_sell && epos_long))
                        closePosition(existing, bar.close, "SIGNAL", ts,
                                      cfg.fee_pct, cfg.slip_pct, result);
                }
            }

            // Open new position when slot is available and balance sufficient.
            if ((is_buy || is_sell) &&
                !hasPosition(bar.symbol) &&
                n_positions_ < std::min(MAX_POS, cfg.max_open) &&
                balance_ > cfg.initial_balance * 0.10) {

                const double margin = computeMarginGARCH(balance_, cfg.risk_pct,
                                                          bar.close, bar.atr,
                                                          cfg.sl_atr_mult,
                                                          cfg.leverage,
                                                          bar.garch_vol);

                // Pre-check: margin must be positive and within 95% of balance.
                // This guarantees balance_ - margin - open_fee > 0 for all
                // default configs (headroom = margin * 0.0526 >> open_fee = margin * 0.003).
                // The rollback below is a defensive check for non-default params only.
                if (margin > 0.0 && margin <= balance_ * 0.95) {
                    int slot = -1;
                    for (int i = 0; i < MAX_POS; ++i)
                        if (!positions_[i].active) { slot = i; break; }

                    if (slot >= 0) {
                        const double entry = is_buy
                            ? bar.close * (1.0 + cfg.slip_pct)
                            : bar.close * (1.0 - cfg.slip_pct);

                        const double sl_dist = bar.atr * cfg.sl_atr_mult;
                        const double tp_dist = sl_dist * cfg.tp_rr_ratio;

                        SimPosition& pos = positions_[slot];
                        std::strncpy(pos.symbol, bar.symbol, sizeof(SimPosition::symbol) - 1);
                        pos.symbol[sizeof(SimPosition::symbol) - 1] = '\0';
                        std::strncpy(pos.side, is_buy ? "LONG" : "SHORT",
                                     sizeof(SimPosition::side) - 1);
                        pos.side[sizeof(SimPosition::side) - 1] = '\0';

                        pos.entry_price        = entry;
                        pos.sl_price           = is_buy ? entry - sl_dist : entry + sl_dist;
                        pos.tp_price           = is_buy ? entry + tp_dist : entry - tp_dist;
                        pos.margin             = margin;
                        pos.notional           = margin * cfg.leverage;
                        pos.open_ts            = ts;
                        pos.active             = true;
                        pos.entry_funding_rate = bar.funding_rate;
                        pos.entry_hmm_state    = bar.hmm_state;
                        ++n_positions_;

                        // Deduct margin and open fee atomically so a single
                        // rollback covers both.
                        const double open_fee = pos.notional * cfg.fee_pct;
                        balance_ -= margin + open_fee;

                        // FIX-ROLL: Defensive rollback for non-default configurations
                        // where the 0.95 pre-check headroom might be insufficient.
                        // Under all default parameters this branch is unreachable.
                        if (balance_ < 0.0) {
                            pos.active = false;
                            --n_positions_;
                            balance_ += margin + open_fee;
                        }
                    }
                }
            }
        }

        // ── 3. Equity snapshot ────────────────────────────────────────────────
        // FIX-EQC: write directly into result.equity_curve (embedded in
        // BacktestResult per fixed types.hpp).  Local equity_arr[] removed.
        if (result.n_equity_points < MAX_EQUITY_POINTS) {
            result.equity_curve[result.n_equity_points++] = {ts, balance_};
        }

        // FIX-NAME: bar_rets — per-bar fractional returns for Sharpe/Sortino.
        if (n_rets < MAX_EQUITY_POINTS && prev_bal > 0.0) {
            bar_rets[n_rets++] = (balance_ - prev_bal) / prev_bal;
        }
        prev_bal = balance_;
    }

    // ── Close all remaining open positions at their own symbol's last price ───
    {
        const int64_t last_ts = bars.empty() ? 0 : bars.back().timestamp;
        for (int i = 0; i < MAX_POS; ++i) {
            if (!positions_[i].active) continue;
            double px = get_last_px(positions_[i].symbol);
            if (px <= 0.0) px = bars.empty() ? 0.0 : bars.back().close;
            closePosition(i, px, "EOD", last_ts, cfg.fee_pct, cfg.slip_pct, result);
        }
    }

    // ── Aggregate stats ────────────────────────────────────────────────────────
    result.final_balance = balance_;
    result.total_return_pct = (cfg.initial_balance > 0.0)
        ? ((balance_ - cfg.initial_balance) / cfg.initial_balance) * 100.0
        : 0.0;

    // FIX-EQC + FIX-SPAN: project a span of balance values from result.equity_curve
    // for computeMaxDrawdown — no second local array needed.
    // We use a thread_local projection buffer to avoid VLA / stack growth.
    thread_local double eq_vals[MAX_EQUITY_POINTS];
    for (int i = 0; i < result.n_equity_points; ++i)
        eq_vals[i] = result.equity_curve[i].balance;

    // FIX-SPAN: all stats calls now use std::span, matching the updated
    // stats_engine.hpp signatures.
    result.max_drawdown_pct =
        computeMaxDrawdown(
            std::span<const double>{eq_vals,
                static_cast<std::size_t>(result.n_equity_points)}) * 100.0;

    result.sharpe =
        computeSharpe(std::span<const double>{bar_rets,
            static_cast<std::size_t>(n_rets)});

    result.sortino =
        computeSortino(std::span<const double>{bar_rets,
            static_cast<std::size_t>(n_rets)});

    result.calmar =
        computeCalmar(result.total_return_pct,
                      result.max_drawdown_pct / 100.0);

    result.profit_factor =
        computeProfitFactor(
            std::span<const Trade>{result.trades,
                static_cast<std::size_t>(result.trades_stored)});

    result.win_rate = (result.total_trades > 0)
        ? static_cast<double>(result.winning_trades) / result.total_trades
        : 0.0;

    // Avg / largest trade stats — iterate only stored trades.
    double sum_win = 0.0, sum_loss = 0.0;
    result.largest_win  = 0.0;
    result.largest_loss = 0.0;
    for (int i = 0; i < result.trades_stored; ++i) {
        const double pnl = result.trades[i].pnl;
        if (pnl > 0.0) {
            sum_win += pnl;
            if (pnl > result.largest_win) result.largest_win = pnl;
        } else if (pnl < 0.0) {
            sum_loss += pnl;
            if (pnl < result.largest_loss) result.largest_loss = pnl;
        }
        // pnl == 0.0: break-even, excluded from win/loss aggregates.
    }

    // avg_trade_pnl denominator: when truncated, use trades_stored (what we
    // actually have) to avoid understating the average by the truncation ratio.
    {
        const int denom = result.trades_truncated
                          ? result.trades_stored : result.total_trades;
        result.avg_trade_pnl = (denom > 0)
            ? (sum_win + sum_loss) / static_cast<double>(denom) : 0.0;
    }
    result.avg_win  = (result.winning_trades > 0)
        ? sum_win  / static_cast<double>(result.winning_trades) : 0.0;
    result.avg_loss = (result.losing_trades > 0)
        ? sum_loss / static_cast<double>(result.losing_trades)  : 0.0;
    result.recovery_factor = (result.max_drawdown_pct > 0.0)
        ? result.total_return_pct / result.max_drawdown_pct : 0.0;

    // ── Regime-sliced stats ────────────────────────────────────────────────────
    // FIX-NTRD: iterate result.trades_stored (not the removed n_trades field).
    // FIX-EVEN: explicit three-way branch for win / loss / break-even.
    {
        double rs_win_sum [3]{};
        double rs_loss_sum[3]{};

        for (int i = 0; i < result.trades_stored; ++i) {
            const Trade& t  = result.trades[i];
            const int    r  = safeRegimeIndex(t.hmm_state);
            RegimeStats& rs = result.regime_stats[r];
            ++rs.trades;
            rs.net_pnl += t.pnl;
            if      (t.pnl > 0.0) { ++rs.wins; rs_win_sum[r]  += t.pnl;          }
            else if (t.pnl < 0.0) {             rs_loss_sum[r] += std::abs(t.pnl);}
            // pnl == 0.0: break-even — counted in rs.trades, not wins or losses.
        }

        for (int r = 0; r < 3; ++r) {
            RegimeStats& rs = result.regime_stats[r];
            rs.win_rate      = (rs.trades > 0)
                ? static_cast<double>(rs.wins) / rs.trades : 0.0;
            rs.profit_factor = (rs_loss_sum[r] > 0.0)
                ? rs_win_sum[r] / rs_loss_sum[r]
                : (rs_win_sum[r] > 0.0 ? 999.0 : 1.0);
            rs.avg_pnl       = (rs.trades > 0)
                ? rs.net_pnl / rs.trades : 0.0;
        }
    }

    // ── Per-symbol stats ───────────────────────────────────────────────────────
    // FIX-CMP:  full field width in strncmp / strncpy.
    // FIX-NTRD: iterate result.trades_stored.
    // FIX-SPAN: pass span to computeSymbolStats.
    {
        char seen[MAX_SYMBOLS][16]{};
        int  n_seen = 0;

        for (int i = 0; i < result.trades_stored; ++i) {
            bool found = false;
            for (int j = 0; j < n_seen; ++j)
                if (std::strncmp(seen[j], result.trades[i].symbol,
                                 sizeof(Trade::symbol) - 1) == 0)
                    { found = true; break; }
            if (!found && n_seen < MAX_SYMBOLS) {
                std::strncpy(seen[n_seen], result.trades[i].symbol,
                             sizeof(Trade::symbol) - 1);
                seen[n_seen][sizeof(Trade::symbol) - 1] = '\0';
                ++n_seen;
            }
        }

        result.n_syms = 0;
        const std::span<const Trade> stored_trades{
            result.trades, static_cast<std::size_t>(result.trades_stored)};

        for (int j = 0; j < n_seen && result.n_syms < MAX_SYMBOLS; ++j) {
            computeSymbolStats(seen[j], stored_trades,
                               result.sym_stats[result.n_syms++],
                               cfg.initial_balance);
        }
    }

    return true;
}

// ── Walk-forward validation ───────────────────────────────────────────────────
//
// Each fold is an independent fresh BacktestEngine starting from
// cfg.initial_balance — performance is per-fold, not compounded.
// Parameters are held fixed (no re-fitting) to test temporal stability.
// Stability verdict: consistency >= 60% AND avg_sharpe > 0.50.

WalkForwardResult BacktestEngine::walk_forward(std::span<const Bar>  bars,
                                                const BacktestConfig& cfg,
                                                int                   n_folds) noexcept {
    WalkForwardResult wf{};

    n_folds = std::clamp(n_folds, 2, static_cast<int>(WalkForwardResult::MAX_FOLDS));

    const int total_bars = static_cast<int>(bars.size());
    if (total_bars < n_folds * MIN_FOLD_BARS) return wf;

    const int fold_size = total_bars / n_folds;

    // thread_local: one BacktestResult per HTTP worker thread; avoids stack
    // overflow from the large trades[] array (~950 KiB) while remaining
    // thread-safe under concurrent /walk_forward requests.
    // FIX-EQC: fold_equity[] removed — equity curve is now inside fold_result.
    thread_local BacktestResult fold_result;

    double sum_sharpe  = 0.0, sum_sortino = 0.0, sum_calmar = 0.0;
    double sum_ret     = 0.0, sum_pf      = 0.0, sum_wr     = 0.0;
    int    n_positive  = 0;

    for (int f = 0; f < n_folds; ++f) {
        const int start = f * fold_size;
        const int end   = (f == n_folds - 1) ? total_bars : start + fold_size;
        const int nbars = end - start;
        if (nbars < MIN_FOLD_BARS) continue;

        // Fresh engine per fold: no position or balance state bleeds across folds.
        BacktestEngine fold_engine;
        // FIX-SIG: run() no longer takes external equity_curve parameters.
        if (!fold_engine.run(bars.subspan(start, nbars), cfg, fold_result))
            continue;

        WalkForwardFold& wff = wf.folds[wf.n_folds++];
        wff.fold             = f + 1;
        wff.bars_oos         = nbars;
        wff.total_trades     = fold_result.total_trades;
        wff.sharpe           = fold_result.sharpe;
        wff.sortino          = fold_result.sortino;
        wff.calmar           = fold_result.calmar;
        wff.total_return_pct = fold_result.total_return_pct;
        wff.max_drawdown_pct = fold_result.max_drawdown_pct;
        wff.profit_factor    = fold_result.profit_factor;
        wff.win_rate         = fold_result.win_rate;
        wff.initial_balance  = fold_result.initial_balance;
        wff.final_balance    = fold_result.final_balance;

        sum_sharpe  += wff.sharpe;
        sum_sortino += wff.sortino;
        sum_calmar  += wff.calmar;
        sum_ret     += wff.total_return_pct;
        sum_pf      += wff.profit_factor;
        sum_wr      += wff.win_rate;
        if (wff.sharpe > 0.0) ++n_positive;
        if (wff.max_drawdown_pct > wf.worst_drawdown)
            wf.worst_drawdown = wff.max_drawdown_pct;
        wf.total_trades += wff.total_trades;
    }

    if (wf.n_folds == 0) return wf;

    const double inv         = 1.0 / static_cast<double>(wf.n_folds);
    wf.avg_sharpe            = sum_sharpe  * inv;
    wf.avg_sortino           = sum_sortino * inv;
    wf.avg_calmar            = sum_calmar  * inv;
    wf.avg_return_pct        = sum_ret     * inv;
    wf.avg_profit_factor     = sum_pf      * inv;
    wf.avg_win_rate          = sum_wr      * inv;
    wf.consistency           = static_cast<double>(n_positive)
                               / static_cast<double>(wf.n_folds);
    wf.stable                = (wf.consistency >= 0.60 && wf.avg_sharpe > 0.50);

    return wf;
}

} // namespace tqc
