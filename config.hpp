#pragma once
/*
 * config.hpp  -  BigBoyAgent TQC Backtest | Taha Iqbal
 *
 * 
 * 
 *
 * FIX [LOW]: Removed version[16] field.  It was initialised to "1.0.0" but
 *            loadConfig() discarded all JSON content, so the field could never
 *            reflect a settings.json override.  A dead, misleading field is
 *            more dangerous than an absent one.  Re-add with a real parse path
 *            if versioning is needed.
 *
 * CONTRACT for loadConfig():
 *   - Must be called from the main thread before any worker threads are spawned
 *     (std::getenv is not thread-safe when setenv/putenv may run concurrently).
 *   - Returns false only when BACKTEST_SECRET is absent or empty.
 *   - Returns true even if settings.json is absent or malformed; built-in
 *     defaults remain valid in that case.
 */

#include <cstddef>
#include <cstring>

namespace tqc {

struct AppConfig {
    char secret_key[128]{};
};

[[nodiscard]] AppConfig& globalConfig() noexcept;

// FIX [HIGH]: Full function body is wrapped in try/catch in config.cpp so the
// noexcept guarantee is unconditionally upheld, not contingent on the default
// stream exception mask.
[[nodiscard]] bool loadConfig(AppConfig& cfg,
                               const char* json_path = "settings.json") noexcept;

} // namespace tqc
