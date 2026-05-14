/*
 * config.cpp  -  BigBoyAgent TQC Backtest | Taha Iqbal
 *
 * Reads BACKTEST_SECRET from environment (mirrors Brain's BRAIN_SECRET pattern).
 *
 * FIX [HIGH]:   Entire function body wrapped in try/catch so the noexcept
 *               guarantee is unconditionally upheld.  Previously only the JSON
 *               parse was protected; std::ifstream constructor and operator>>
 *               can throw std::ios_base::failure if the stream exception mask is
 *               ever set, violating the declared noexcept contract.
 *
 * FIX [MEDIUM]: Added explicit comment documenting the intentional return-true
 *               contract when settings.json is malformed.  Built-in defaults
 *               remain valid; this is not a failure condition.
 *
 * FIX [LOW]:    Added PRECONDITION comment on std::getenv thread-safety.
 *               Must be called before any worker threads are spawned.
 */

#include "config.hpp"
#include "json.hpp"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>

using json = nlohmann::json;

namespace tqc {

static AppConfig g_config;

AppConfig& globalConfig() noexcept { return g_config; }

bool loadConfig(AppConfig& cfg, const char* json_path) noexcept {
    // FIX [HIGH]: Outer try/catch makes the noexcept guarantee unconditional.
    // Any exception from ifstream, operator>>, or any future code path is caught
    // here rather than propagating past the noexcept boundary (which would call
    // std::terminate).
    try {
        // ── 1. Secret key from environment ────────────────────────────────────
        // PRECONDITION [LOW]: std::getenv is not thread-safe on Linux if
        // setenv/putenv may run concurrently.  Call loadConfig() from the main
        // thread before spawning any worker threads.
        const char* secret = std::getenv("BACKTEST_SECRET");
        if (!secret || secret[0] == '\0') {
            std::fprintf(stderr,
                "[CONFIG] ERROR: BACKTEST_SECRET environment variable is not set.\n"
                "         Set it to the shared API key used by executor_main.\n");
            return false;
        }
        std::strncpy(cfg.secret_key, secret, sizeof(cfg.secret_key) - 1);
        cfg.secret_key[sizeof(cfg.secret_key) - 1] = '\0';   // guaranteed null terminator

        // ── 2. Optional settings.json ─────────────────────────────────────────
        std::ifstream f(json_path);
        if (!f.is_open()) {
            std::fprintf(stderr,
                "[CONFIG] INFO: %s not found — using built-in defaults.\n", json_path);
            return true;
        }

        // FIX [MEDIUM]: If settings.json exists but is malformed, we log a
        // warning and return true intentionally.  Built-in defaults in
        // BacktestConfig are valid and safe; a parse failure is not a fatal
        // condition.  Future work: parse tunable override fields here and apply
        // them to g_config before returning.
        try {
            json j;
            f >> j;
            (void)j; // placeholder: parse optional overrides here when fields are added
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[CONFIG] WARNING: %s is malformed (%s) — using built-in defaults.\n",
                json_path, e.what());
            // Intentional fall-through: return true below (defaults are valid).
        }

        std::fprintf(stderr, "[CONFIG] Config loaded OK.\n");
        return true;

    } catch (...) {
        // FIX [HIGH]: Catch-all ensures noexcept is never violated.
        // This path should never be reached under normal operation.
        std::fprintf(stderr,
            "[CONFIG] FATAL: Unexpected exception in loadConfig — using no config.\n");
        return false;
    }
}

} // namespace tqc
