/*
 * main.cpp  -  BigBoyAgent TQC Backtest Space | Taha Iqbal
 * ============================================================
 * ARCHITECTURE:
 *
 *   executor_main  →  POST /backtest  →  HttpServer (worker threads)
 *                                              │
 *                                       parseBars() [shared helper]
 *                                              │
 *                                       BacktestEngine::run()
 *                                              │
 *                        ┌────────────────────────────────────┐
 *                        ▼                                    ▼
 *                  StatsEngine                       result.equity_curve
 *              (Sharpe/Sortino/Calmar)               (embedded in result)
 *                        │
 *                        ▼
 *                  nlohmann::json serialisation (response path, not hot path)
 *                        │
 *                        ▼
 *                  HTTP Response → executor_main
 *
 * Endpoints:
 *   GET  /              — info page (no auth)
 *   GET  /health        — uptime + status (no auth)
 *   POST /backtest      — run simulation (auth required)
 *   POST /walk_forward  — walk-forward validation (auth required)
 *   GET  /results       — last backtest summary (auth required)
 *
 * Auth:
 *   Header:  x-api-key: <BACKTEST_SECRET>
 *   or:      Authorization: Bearer <BACKTEST_SECRET>
 *
 * Environment variables:
 *   BACKTEST_SECRET  — required — shared secret with executor
 *   PORT             — optional — HTTP port, [1,65535], default 7860
 *   BACKTEST_WORKERS — optional — worker thread count, [1,64], default 4
 *
 * ── Fixes applied ─────────────────────────────────────────────────────────────
 *
 *  FIX-RACE    Replaced static Bar bars_buf[], static BacktestResult result, and
 *              static EquityPoint eq_curve[] in both handlers with thread_local.
 *              Static locals are shared across all worker threads; concurrent
 *              requests wrote into the same arrays with no synchronisation.
 *
 *  FIX-NTRD    Replaced all result.n_trades references with result.trades_stored.
 *              n_trades was removed from BacktestResult in the types.hpp session;
 *              trades_stored is the authoritative write cursor and iteration bound.
 *
 *  FIX-SIG     engine.run() called with the corrected 3-argument signature.
 *              The old 5-argument form (with external eq_curve* and n_eq&) was
 *              removed in the backtest_engine session; the equity curve is now
 *              embedded in BacktestResult::equity_curve / n_equity_points.
 *
 *  FIX-STOP    Signal handler now calls g_server->stop().  Old handler only set
 *              g_running=false, which the blocking server.start() never checked —
 *              the process silently ignored SIGINT and SIGTERM.
 *
 *  FIX-EQG     Removed global g_equity_curve[] and g_n_equity.  The equity curve
 *              is embedded in BacktestResult; g_last_result already holds it.
 *              The std::memcpy from the removed eq_curve static local is deleted.
 *
 *  FIX-ATOI    std::atoi replaced by std::from_chars with range validation for
 *              both PORT ([1,65535]) and BACKTEST_WORKERS ([1,64]).  atoi returns
 *              0 on non-numeric input and is UB on overflow.
 *
 *  FIX-LAT     g_total_lat_ms accumulated via fetch_add (C++20 atomic<double>
 *              specialisation).  The old load()+store() had a lost-update race
 *              window under concurrent requests.
 *
 *  FIX-STEP3   Bar parsing now reads garch_vol, funding_rate, and hmm_state.
 *              Omitting these disabled GARCH position scaling, zeroed all funding
 *              costs, and forced every trade into the SIDEWAYS regime bucket.
 *
 *  FIX-DUP     Extracted parseBars() helper shared by both handlers, eliminating
 *              ~35 lines of duplicated parsing logic and its maintenance hazard.
 *
 *  FIX-RESP    /backtest response now includes regime_stats (BEAR/SIDEWAYS/BULL),
 *              total_funding_cost, trades_stored, and trades_truncated.
 *
 *  FIX-EQS     Equity curve sampling uses result.equity_curve and
 *              result.n_equity_points (the embedded fields) instead of the
 *              removed local n_eq / eq_curve variables.
 *
 *  FIX-TRUNC   trades_truncated surfaced in both /backtest and /results responses.
 *
 *  FIX-LOG     Boot log now lists all five endpoints including /walk_forward.
 *
 *  FIX-SZ      strncpy limits use sizeof(field)-1 throughout parseBars().
 *
 *  FIX-TMP     uptimeStr() result assigned to a named variable before .c_str()
 *              is passed to snprintf, making the temporary's lifetime explicit.
 * ============================================================
 */

#include "config.hpp"
#include "http_server.hpp"
#include "backtest_engine.hpp"
#include "stats_engine.hpp"
#include "types.hpp"
#include "json.hpp"

#include <charconv>
#include <chrono>
#include <atomic>
#include <mutex>
#include <string>
#include <span>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

using json = nlohmann::json;
using namespace tqc;

// ── Startup timestamp ─────────────────────────────────────────────────────────
static const auto g_boot = std::chrono::steady_clock::now();

// ── Last backtest result cache ────────────────────────────────────────────────
// Protected by g_result_mutex for write; equity curve embedded in g_last_result.
// FIX-EQG: global g_equity_curve[] and g_n_equity removed — BacktestResult now
//          embeds equity_curve[MAX_EQUITY_POINTS] and n_equity_points directly.
static std::mutex     g_result_mutex;
static BacktestResult g_last_result{};
static bool           g_has_result       = false;
static int64_t        g_last_run_ts      = 0;
static double         g_last_cfg_balance = 0.0;

// ── Latency tracking ──────────────────────────────────────────────────────────
// FIX-LAT: C++20 atomic<double> fetch_add for race-free accumulation.
static std::atomic<double> g_last_lat_ms{0.0};
static std::atomic<int>    g_total_runs{0};
static std::atomic<double> g_total_lat_ms{0.0};

// ── Signal handling ───────────────────────────────────────────────────────────
// FIX-STOP: g_server pointer allows the signal handler to call server.stop().
// Declared before main() so the handler lambda can capture it.
// Assigned in main() before server.start() and before signals are installed.
static HttpServer* g_server = nullptr;

static void sig_handler(int) noexcept {
    // FIX-STOP: calling stop() causes the acceptor's next poll() iteration
    // (≤100ms) to exit, allowing workers to drain and main() to return.
    // std::atomic::store is safe to call from a signal handler on all
    // platforms where this server runs (Linux x86-64 / ARM64).
    if (g_server) g_server->stop();
}

// ── Uptime helpers ────────────────────────────────────────────────────────────
static double uptimeSeconds() noexcept {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - g_boot).count();
}

static std::string uptimeStr() {
    int secs = static_cast<int>(uptimeSeconds());
    const int h = secs / 3600; secs %= 3600;
    const int m = secs / 60;   secs %= 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02dh %02dm %02ds", h, m, secs);
    return buf;
}

// ── parseBars ─────────────────────────────────────────────────────────────────
// FIX-DUP:   shared by handleBacktest and handleWalkForward — eliminates ~35
//            lines of duplicated parsing logic in the original.
// FIX-STEP3: parses garch_vol, funding_rate, hmm_state (Step 3 fields).
//            Omitting these disabled GARCH position sizing, zeroed funding costs,
//            and forced every trade into the SIDEWAYS regime bucket.
// FIX-SZ:    strncpy limits use sizeof(field)-1 throughout.
//
// Writes into buf[0..MAX_BARS-1].  Returns the number of valid bars written.
// A bar is valid when close > 0.0.
[[nodiscard]] static int parseBars(const json& bars_json,
                                    Bar*         buf,
                                    int          max_bars) noexcept {
    int n = 0;
    for (const auto& b : bars_json) {
        if (n >= max_bars) break;
        if (!b.is_object()) continue;

        const std::string sym = b.value("symbol", "");
        if (sym.empty()) continue;

        Bar& bar = buf[n];

        // FIX-SZ: sizeof(field)-1 so any field-width change in types.hpp
        //         is automatically reflected here.
        std::strncpy(bar.symbol, sym.c_str(), sizeof(bar.symbol) - 1);
        bar.symbol[sizeof(bar.symbol) - 1] = '\0';

        bar.close     = b.value("close",     0.0);
        bar.high      = b.value("high",      bar.close);
        bar.low       = b.value("low",       bar.close);
        bar.atr       = b.value("atr",       0.0);
        bar.confidence = b.value("confidence", 0.0);
        bar.pos_valid  = b.value("pos_valid",  false);
        bar.timestamp  = b.value("timestamp",  static_cast<int64_t>(0));

        // FIX-STEP3: garch_vol, funding_rate, hmm_state were absent in the
        // original handlers, silently disabling GARCH scaling and regime stats.
        bar.garch_vol    = b.value("garch_vol",    0.0);
        bar.funding_rate = b.value("funding_rate", 0.0);
        bar.hmm_state    = static_cast<uint8_t>(b.value("hmm_state", 1));

        const std::string sig = b.value("signal", "HOLD");
        std::strncpy(bar.signal, sig.c_str(), sizeof(bar.signal) - 1);
        bar.signal[sizeof(bar.signal) - 1] = '\0';

        const std::string reg = b.value("regime", "NOISE");
        std::strncpy(bar.regime, reg.c_str(), sizeof(bar.regime) - 1);
        bar.regime[sizeof(bar.regime) - 1] = '\0';

        const std::string vol = b.value("vol_regime", "NORMAL");
        std::strncpy(bar.vol_regime, vol.c_str(), sizeof(bar.vol_regime) - 1);
        bar.vol_regime[sizeof(bar.vol_regime) - 1] = '\0';

        if (bar.close > 0.0) ++n;
    }
    return n;
}

// ── parseConfig ───────────────────────────────────────────────────────────────
// Parses BacktestConfig fields from a JSON object.
// Validation errors return a non-empty error string; empty string means success.
[[nodiscard]] static std::string parseConfig(const json& j,
                                              BacktestConfig& cfg) noexcept {
    cfg.initial_balance = j.value("initial_balance", 1000.0);
    cfg.risk_pct        = j.value("risk_pct",        0.01);
    cfg.leverage        = j.value("leverage",         3);
    cfg.sl_atr_mult     = j.value("sl_atr_mult",      1.5);
    cfg.tp_rr_ratio     = j.value("tp_rr_ratio",      2.0);
    cfg.min_confidence  = j.value("min_confidence",   0.70);
    cfg.fee_pct         = j.value("fee_pct",          0.001);
    cfg.slip_pct        = j.value("slip_pct",         0.0002);
    cfg.max_open        = j.value("max_open",         3);

    if (cfg.initial_balance <= 0.0 || cfg.initial_balance > 1e8)
        return R"({"error":"invalid_initial_balance"})";
    if (cfg.leverage < 1 || cfg.leverage > 125)
        return R"({"error":"invalid_leverage"})";
    if (cfg.risk_pct <= 0.0 || cfg.risk_pct > 1.0)
        return R"({"error":"invalid_risk_pct"})";
    if (cfg.fee_pct < 0.0 || cfg.fee_pct > 0.1)
        return R"({"error":"invalid_fee_pct"})";
    if (cfg.min_confidence < 0.0 || cfg.min_confidence > 1.0)
        return R"({"error":"invalid_min_confidence"})";

    return "";  // success
}

// ── Route: GET / ──────────────────────────────────────────────────────────────
static HttpResponse handleHome(const HttpRequest&) {
    const std::string body = R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>BigBoyAgent Backtest Space</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: 'Segoe UI', system-ui, sans-serif;
         background: #0a0a0f; color: #e2e8f0; min-height: 100vh;
         display: flex; align-items: center; justify-content: center; }
  .card { background: #111827; border: 1px solid #1e40af;
          border-radius: 16px; padding: 40px 48px; max-width: 680px;
          width: 90%; box-shadow: 0 0 60px rgba(30,64,175,0.25); }
  .badge { display: inline-block; background: #1e3a8a; color: #93c5fd;
           font-size: 11px; font-weight: 700; padding: 3px 10px;
           border-radius: 20px; letter-spacing: 1px; margin-bottom: 24px; }
  h1 { font-size: 28px; font-weight: 800; color: #fff; margin-bottom: 6px; }
  .sub { color: #64748b; font-size: 14px; margin-bottom: 32px; }
  .endpoints { background: #0f172a; border-radius: 10px;
               padding: 20px 24px; margin-bottom: 28px; }
  .endpoints h3 { font-size: 12px; font-weight: 700; color: #475569;
                  letter-spacing: 1px; text-transform: uppercase;
                  margin-bottom: 14px; }
  .ep { display: flex; align-items: center; gap: 12px;
        margin-bottom: 10px; font-size: 13px; }
  .ep:last-child { margin-bottom: 0; }
  .method { font-weight: 700; font-size: 11px; padding: 2px 8px;
            border-radius: 4px; min-width: 42px; text-align: center; }
  .get  { background: #052e16; color: #4ade80; }
  .post { background: #1e1b4b; color: #a5b4fc; }
  .path { color: #e2e8f0; font-family: monospace; }
  .desc { color: #64748b; margin-left: auto; }
  .auth-note { background: #1c1917; border-left: 3px solid #f59e0b;
               border-radius: 4px; padding: 14px 18px; font-size: 13px;
               color: #d97706; margin-bottom: 24px; }
  .footer { display: flex; justify-content: space-between;
            font-size: 12px; color: #334155; }
</style>
</head>
<body>
<div class="card">
  <div class="badge">BIGBOYAGENT &middot; TQC BACKTEST</div>
  <h1>Backtest Space</h1>
  <div class="sub">Event-driven simulation engine &middot; C++20 &middot; Taha Iqbal</div>
  <div class="endpoints">
    <h3>Endpoints</h3>
    <div class="ep">
      <span class="method get">GET</span>
      <span class="path">/health</span>
      <span class="desc">Uptime &amp; status</span>
    </div>
    <div class="ep">
      <span class="method post">POST</span>
      <span class="path">/backtest</span>
      <span class="desc">Run simulation</span>
    </div>
    <div class="ep">
      <span class="method post">POST</span>
      <span class="path">/walk_forward</span>
      <span class="desc">Walk-forward validation</span>
    </div>
    <div class="ep">
      <span class="method get">GET</span>
      <span class="path">/results</span>
      <span class="desc">Last run summary</span>
    </div>
  </div>
  <div class="auth-note">
    Authenticated endpoints require <strong>x-api-key</strong> or
    <strong>Authorization: Bearer</strong> header.
  </div>
  <div class="footer">
    <span>v1.0.0 &middot; C++20 &middot; AVX2</span>
    <span>BigBoyAgent &copy; Taha Iqbal</span>
  </div>
</div>
</body>
</html>)";
    return {200, body, "text/html"};
}

// ── Route: GET /health ────────────────────────────────────────────────────────
// FIX-TMP: uptimeStr() result stored in named variable so the temporary's
// lifetime is unambiguous before .c_str() is passed to snprintf.
static HttpResponse handleHealth(const HttpRequest&) {
    const std::string uptime = uptimeStr();   // FIX-TMP
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        R"({"status":"ok","service":"backtest","version":"1.0.0",)"
        R"("uptime_sec":%.1f,"uptime_str":"%s","total_runs":%d,)"
        R"("last_lat_ms":%.2f,"avg_lat_ms":%.2f})",
        uptimeSeconds(),
        uptime.c_str(),
        g_total_runs.load(std::memory_order_relaxed),
        g_last_lat_ms.load(std::memory_order_relaxed),
        g_total_runs.load(std::memory_order_relaxed) > 0
            ? g_total_lat_ms.load(std::memory_order_relaxed) /
              g_total_runs.load(std::memory_order_relaxed)
            : 0.0);
    return {200, buf};
}

// ── Route: GET /results ───────────────────────────────────────────────────────
// FIX-TRUNC: trades_truncated now surfaced so callers can detect partial stats.
static HttpResponse handleResults(const HttpRequest&) {
    std::lock_guard<std::mutex> lk(g_result_mutex);
    if (!g_has_result)
        return {200, R"({"status":"no_results","message":"No backtest has been run yet."})"};

    const BacktestResult& r = g_last_result;

    // Compact summary — full trade log is large; /backtest response has detail.
    // Buffer: ~380 chars fixed + ~25 chars per numeric field × ~22 fields = ~930 chars.
    // 4096 bytes provides ample headroom for maximum-magnitude values.
    char buf[4096];
    std::snprintf(buf, sizeof(buf),
        R"({"status":"ok","last_run_ts":%lld,"initial_balance":%.2f,)"
        R"("final_balance":%.2f,"total_return_pct":%.4f,)"
        R"("max_drawdown_pct":%.4f,"sharpe":%.4f,"sortino":%.4f,)"
        R"("calmar":%.4f,"profit_factor":%.4f,"win_rate":%.4f,)"
        R"("total_trades":%d,"trades_stored":%d,"trades_truncated":%s,)"
        R"("winning_trades":%d,"losing_trades":%d,)"
        R"("avg_win_usd":%.4f,"avg_loss_usd":%.4f,)"
        R"("largest_win_usd":%.4f,"largest_loss_usd":%.4f,)"
        R"("recovery_factor":%.4f,"total_funding_cost_usd":%.4f,)"
        R"("bars_processed":%d,"n_symbols":%d})",
        static_cast<long long>(g_last_run_ts),
        r.initial_balance, r.final_balance,
        r.total_return_pct, r.max_drawdown_pct,
        r.sharpe, r.sortino, r.calmar, r.profit_factor,
        r.win_rate,
        r.total_trades, r.trades_stored,          // FIX-NTRD
        r.trades_truncated ? "true" : "false",    // FIX-TRUNC
        r.winning_trades,  r.losing_trades,
        r.avg_win,   r.avg_loss,
        r.largest_win, r.largest_loss,
        r.recovery_factor,
        r.total_funding_cost,                     // FIX-RESP
        r.bars_processed, r.n_syms);
    return {200, buf};
}

// ── Route: POST /backtest ─────────────────────────────────────────────────────
// FIX-RACE:  thread_local bars_buf and result so each worker has its own copy.
// FIX-SIG:   engine.run() called with the corrected 3-argument signature.
// FIX-NTRD:  result.trades_stored used throughout (n_trades removed).
// FIX-EQS:   equity curve sampled from result.equity_curve / n_equity_points.
// FIX-RESP:  regime_stats, total_funding_cost, trades_stored, trades_truncated
//            now included in the response JSON.
static HttpResponse handleBacktest(const HttpRequest& req) {
    const auto t_start = std::chrono::steady_clock::now();

    // ── 1. Parse JSON ──────────────────────────────────────────────────────────
    json j;
    try { j = json::parse(req.body); }
    catch (...) { return {400, R"({"error":"invalid_json"})"}; }

    if (!j.contains("bars") || !j["bars"].is_array())
        return {400, R"({"error":"missing_field_bars"})"};

    // ── 2. Parse config ────────────────────────────────────────────────────────
    BacktestConfig cfg;
    if (const std::string err = parseConfig(j, cfg); !err.empty())
        return {400, err};

    // ── 3. Parse bars ──────────────────────────────────────────────────────────
    // FIX-RACE: thread_local — each worker has its own bars_buf; no data race.
    thread_local Bar bars_buf[MAX_BARS];
    const int n_bars = parseBars(j["bars"], bars_buf, MAX_BARS);  // FIX-DUP

    if (n_bars < 2)
        return {400, R"({"error":"insufficient_bars","min":2})"};

    // ── 4. Run backtest ────────────────────────────────────────────────────────
    // FIX-RACE:  thread_local result — no sharing between concurrent requests.
    // FIX-SIG:   3-argument run() — equity curve embedded in result.
    thread_local BacktestResult result;
    BacktestEngine engine;
    if (!engine.run(std::span<const Bar>(bars_buf, n_bars), cfg, result))
        return {500, R"({"error":"engine_run_failed"})"};

    // ── 5. Latency bookkeeping ─────────────────────────────────────────────────
    const double lat_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_start).count();
    g_last_lat_ms.store(lat_ms, std::memory_order_relaxed);
    // FIX-LAT: fetch_add is a single atomic RMW — no lost-update race.
    g_total_lat_ms.fetch_add(lat_ms, std::memory_order_relaxed);
    g_total_runs.fetch_add(1,        std::memory_order_relaxed);

    // ── 6. Cache result ────────────────────────────────────────────────────────
    // FIX-EQG: g_equity_curve[] and g_n_equity removed. The equity curve is now
    //          embedded in BacktestResult; copying result copies the curve too.
    {
        std::lock_guard<std::mutex> lk(g_result_mutex);
        g_last_result      = result;
        g_has_result       = true;
        g_last_run_ts      = static_cast<int64_t>(std::time(nullptr));
        g_last_cfg_balance = cfg.initial_balance;
    }

    // ── 7. Build JSON response ────────────────────────────────────────────────
    // nlohmann::json is used here — this is the response path, not the hot path.
    json resp;
    resp["status"]            = "success";
    resp["bars_processed"]    = result.bars_processed;
    resp["initial_balance"]   = result.initial_balance;
    resp["final_balance"]     = result.final_balance;
    resp["total_return_pct"]  = result.total_return_pct;
    resp["max_drawdown_pct"]  = result.max_drawdown_pct;
    resp["sharpe"]            = result.sharpe;
    resp["sortino"]           = result.sortino;
    resp["calmar"]            = result.calmar;
    resp["profit_factor"]     = result.profit_factor;
    resp["win_rate"]          = result.win_rate;
    resp["total_trades"]      = result.total_trades;
    resp["trades_stored"]     = result.trades_stored;           // FIX-NTRD
    resp["trades_truncated"]  = result.trades_truncated;        // FIX-TRUNC
    resp["winning_trades"]    = result.winning_trades;
    resp["losing_trades"]     = result.losing_trades;
    resp["avg_trade_pnl"]     = result.avg_trade_pnl;
    resp["avg_win"]           = result.avg_win;
    resp["avg_loss"]          = result.avg_loss;
    resp["largest_win"]       = result.largest_win;
    resp["largest_loss"]      = result.largest_loss;
    resp["recovery_factor"]   = result.recovery_factor;
    resp["total_funding_cost"]= result.total_funding_cost;      // FIX-RESP
    resp["latency_ms"]        = lat_ms;
    resp["run_ts"]            = static_cast<int64_t>(std::time(nullptr));

    // FIX-RESP: Regime-sliced stats (BEAR=0, SIDEWAYS=1, BULL=2).
    // These were computed in backtest_engine but never serialised in the original.
    {
        const char* regime_names[3] = {"bear", "sideways", "bull"};
        json regime_arr = json::array();
        for (int r = 0; r < 3; ++r) {
            const RegimeStats& rs = result.regime_stats[r];
            regime_arr.push_back({
                {"regime",        regime_names[r]},
                {"trades",        rs.trades},
                {"wins",          rs.wins},
                {"net_pnl",       rs.net_pnl},
                {"win_rate",      rs.win_rate},
                {"profit_factor", rs.profit_factor},
                {"avg_pnl",       rs.avg_pnl}
            });
        }
        resp["regime_stats"] = regime_arr;
    }

    // Per-symbol stats
    {
        json sym_arr = json::array();
        for (int i = 0; i < result.n_syms; ++i) {
            const SymbolStats& s = result.sym_stats[i];
            sym_arr.push_back({
                {"symbol",        std::string(s.symbol)},
                {"total_trades",  s.total_trades},
                {"wins",          s.wins},
                {"net_pnl",       s.net_pnl},
                {"win_rate",      s.win_rate},
                {"profit_factor", s.profit_factor},
                {"max_drawdown",  s.max_drawdown},
                {"sharpe",        s.sharpe},
                {"sortino",       s.sortino},
                {"avg_win",       s.avg_win},
                {"avg_loss",      s.avg_loss},
                {"calmar",        s.calmar}
            });
        }
        resp["symbol_stats"] = sym_arr;
    }

    // Equity curve — sampled to ≤200 points for payload size.
    // FIX-EQS: use result.equity_curve and result.n_equity_points (embedded
    //          fields from the fixed BacktestResult) rather than the removed
    //          local n_eq / static eq_curve variables.
    {
        const int n_eq = result.n_equity_points;
        const int step = std::max(1, n_eq / 200);
        json eq_arr = json::array();
        for (int i = 0; i < n_eq; i += step) {
            eq_arr.push_back({
                {"ts",      result.equity_curve[i].ts},
                {"balance", result.equity_curve[i].balance}
            });
        }
        resp["equity_curve"] = eq_arr;
    }

    // Last 50 trades — FIX-NTRD: trades_stored is the authoritative bound.
    {
        const int trade_start = std::max(0, result.trades_stored - 50);
        json trades_arr = json::array();
        for (int i = trade_start; i < result.trades_stored; ++i) {
            const Trade& t = result.trades[i];
            trades_arr.push_back({
                {"symbol",       std::string(t.symbol)},
                {"side",         std::string(t.side)},
                {"exit_reason",  std::string(t.exit_reason)},
                {"entry_price",  t.entry_price},
                {"exit_price",   t.exit_price},
                {"pnl",          t.pnl},
                {"pnl_pct",      t.pnl_pct},
                {"margin",       t.margin},
                {"entry_ts",     t.entry_ts},
                {"exit_ts",      t.exit_ts},
                {"funding_cost", t.funding_cost},
                {"hmm_state",    t.hmm_state}
            });
        }
        resp["recent_trades"] = trades_arr;
    }

    return {200, resp.dump()};
}

// ── Route: POST /walk_forward ─────────────────────────────────────────────────
// Walk-forward validation: slices the bar stream into n_folds equal OOS windows
// and runs an independent BacktestEngine::run() on each.
//
// Parameters are held FIXED across all folds (no re-fitting) to test temporal
// stability of the edge rather than curve-fitting ability.
//
// Stability verdict:
//   "stable": true  →  consistency ≥ 60% AND avg_sharpe > 0.50
//   Deploy capital only when stable == true.
//
// FIX-RACE: thread_local bars_buf — see handleBacktest for rationale.
static HttpResponse handleWalkForward(const HttpRequest& req) {
    const auto t_start = std::chrono::steady_clock::now();

    // ── 1. Parse JSON ──────────────────────────────────────────────────────────
    json j;
    try { j = json::parse(req.body); }
    catch (...) { return {400, R"({"error":"invalid_json"})"}; }

    if (!j.contains("bars") || !j["bars"].is_array())
        return {400, R"({"error":"missing_field_bars"})"};

    // ── 2. n_folds ─────────────────────────────────────────────────────────────
    int n_folds = std::clamp(j.value("n_folds", 5), 2, 10);

    // ── 3. Config ──────────────────────────────────────────────────────────────
    BacktestConfig cfg;
    if (const std::string err = parseConfig(j, cfg); !err.empty())
        return {400, err};

    // ── 4. Parse bars ──────────────────────────────────────────────────────────
    // FIX-RACE: thread_local — own copy per worker thread.
    // FIX-DUP:  parseBars() shared helper, not a duplicate copy of the logic.
    thread_local Bar bars_buf[MAX_BARS];
    const int n_bars = parseBars(j["bars"], bars_buf, MAX_BARS);

    // Minimum 10 bars per fold for meaningful per-fold statistics.
    if (n_bars < n_folds * 10) {
        char err[128];
        std::snprintf(err, sizeof(err),
            R"({"error":"insufficient_bars_for_folds","bars":%d,"folds":%d,"min_required":%d})",
            n_bars, n_folds, n_folds * 10);
        return {400, err};
    }

    // ── 5. Run walk-forward ────────────────────────────────────────────────────
    BacktestEngine engine;
    const WalkForwardResult wf = engine.walk_forward(
        std::span<const Bar>(bars_buf, n_bars), cfg, n_folds);

    const double lat_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_start).count();

    // ── 6. Serialise ───────────────────────────────────────────────────────────
    json resp;
    resp["status"]            = "success";
    resp["n_folds_requested"] = n_folds;
    resp["n_folds_completed"] = wf.n_folds;
    resp["avg_sharpe"]        = wf.avg_sharpe;
    resp["avg_sortino"]       = wf.avg_sortino;
    resp["avg_calmar"]        = wf.avg_calmar;
    resp["avg_return_pct"]    = wf.avg_return_pct;
    resp["worst_drawdown"]    = wf.worst_drawdown;
    resp["avg_profit_factor"] = wf.avg_profit_factor;
    resp["avg_win_rate"]      = wf.avg_win_rate;
    resp["total_trades"]      = wf.total_trades;
    resp["consistency"]       = wf.consistency;
    resp["stable"]            = wf.stable;
    resp["latency_ms"]        = lat_ms;
    resp["stability_note"]    = wf.stable
        ? "PASS: consistency>=60% and avg_sharpe>0.50 — edge is temporally stable"
        : "FAIL: edge is not temporally stable — do not deploy capital";

    json folds_arr = json::array();
    for (int i = 0; i < wf.n_folds; ++i) {
        const WalkForwardFold& f = wf.folds[i];
        folds_arr.push_back({
            {"fold",             f.fold},
            {"bars_oos",         f.bars_oos},
            {"total_trades",     f.total_trades},
            {"sharpe",           f.sharpe},
            {"sortino",          f.sortino},
            {"calmar",           f.calmar},
            {"total_return_pct", f.total_return_pct},
            {"max_drawdown_pct", f.max_drawdown_pct},
            {"profit_factor",    f.profit_factor},
            {"win_rate",         f.win_rate},
            {"initial_balance",  f.initial_balance},
            {"final_balance",    f.final_balance}
        });
    }
    resp["folds"] = folds_arr;

    return {200, resp.dump()};
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main() {
    std::fprintf(stderr,
        "============================================================\n"
        "BigBoyAgent-Backtest v1.0  |  C++20  |  Taha Iqbal\n"
        "Event-driven simulation: SL/TP/GARCH/funding/walk-forward\n"
        "============================================================\n");

    // ── Config bootstrap ───────────────────────────────────────────────────────
    // loadConfig must be called before any worker threads are spawned —
    // getenv is not thread-safe under concurrent setenv (see config.cpp).
    AppConfig& cfg = globalConfig();
    if (!loadConfig(cfg)) {
        std::fprintf(stderr,
            "[BOOT] FATAL: loadConfig() failed. "
            "Is BACKTEST_SECRET set?\n");
        return 1;
    }
    std::fprintf(stderr,
        "[BOOT] Auth secret: loaded (%zu chars)\n",
        std::strlen(cfg.secret_key));

    // ── PORT parsing ───────────────────────────────────────────────────────────
    // FIX-ATOI: from_chars with range check — atoi is UB on overflow and
    //           returns 0 (random ephemeral bind) on non-numeric input.
    int port = 7860;
    if (const char* p = std::getenv("PORT"); p && *p) {
        int parsed = 0;
        auto [ptr, ec] = std::from_chars(p, p + std::strlen(p), parsed);
        if (ec == std::errc{} && parsed >= 1 && parsed <= 65535)
            port = parsed;
        else
            std::fprintf(stderr,
                "[BOOT] WARNING: invalid PORT='%s'; using default %d.\n",
                p, port);
    }

    // ── BACKTEST_WORKERS parsing ───────────────────────────────────────────────
    int workers = 4;
    if (const char* w = std::getenv("BACKTEST_WORKERS"); w && *w) {
        int parsed = 0;
        auto [ptr, ec] = std::from_chars(w, w + std::strlen(w), parsed);
        if (ec == std::errc{} && parsed >= 1 && parsed <= 64)
            workers = parsed;
        else
            std::fprintf(stderr,
                "[BOOT] WARNING: invalid BACKTEST_WORKERS='%s'; using default %d.\n",
                w, workers);
    }

    std::fprintf(stderr,
        "[BOOT] HTTP port: %d  workers: %d\n"
        "------------------------------------------------------------\n"
        "[BOOT] Endpoints:\n"
        "       GET  /             (public)\n"
        "       GET  /health       (public)\n"
        "       POST /backtest     (auth required)\n"
        "       POST /walk_forward (auth required)\n"   // FIX-LOG
        "       GET  /results      (auth required)\n"
        "============================================================\n",
        port, workers);

    // ── Server construction ────────────────────────────────────────────────────
    HttpServer server(port, workers);

    // FIX-STOP: expose server pointer to signal handler BEFORE installing
    // signal handlers.  Assigning g_server then installing handlers avoids
    // any window where a signal fires before g_server is initialised.
    g_server = &server;
    std::signal(SIGINT,  sig_handler);
    std::signal(SIGTERM, sig_handler);
    // SIGPIPE: generated when send() writes to a closed socket.  Ignored here —
    // MSG_NOSIGNAL in send_all() suppresses it on Linux, but this is belt-and-
    // suspenders for any code path that uses write() directly.
    std::signal(SIGPIPE, SIG_IGN);

    // ── Route registration ─────────────────────────────────────────────────────
    server.addRoute("GET",  "/",             false, handleHome);
    server.addRoute("GET",  "/health",       false, handleHealth);
    server.addRoute("POST", "/backtest",     true,  handleBacktest);
    server.addRoute("POST", "/walk_forward", true,  handleWalkForward);
    server.addRoute("GET",  "/results",      true,  handleResults);

    // Blocks until sig_handler calls server.stop(), which causes acceptorThread
    // to exit on its next poll() timeout (≤100ms), after which workers drain.
    server.start();

    std::fprintf(stderr, "[BOOT] Server stopped cleanly.\n");
    g_server = nullptr;
    return 0;
}
