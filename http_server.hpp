#pragma once
/*
 * http_server.hpp  -  BigBoyAgent TQC Backtest | Taha Iqbal
 *
 * Production-grade POSIX HTTP/1.1 server — identical architecture to Brain.
 *
 *   - One acceptor thread: poll() + accept(), pushes fd into MPSC ring buffer.
 *   - N worker threads: pop fd, recv request, dispatch handler, send response.
 *   - Connection: close on every response (no keep-alive).
 *   - Case-insensitive Content-Length scanning (RFC 7230 compliant).
 *   - Constant-time auth comparison (timing-safe).
 *   - Hard 8 MiB request body cap (DoS guard).
 *   - Graceful shutdown via poll() timeout in acceptor (stop() is immediate).
 *
 * ── Fixes applied ─────────────────────────────────────────────────────────────
 *
 *  FIX-DANGLE  HttpRequest: deleted copy and move constructors/assignments.
 *              All string_view members point into raw_buf.  A default copy or
 *              move produces views dangling into the old raw_buf.  Deleting
 *              copy/move makes the constraint explicit at compile time.
 *              Handlers must accept const HttpRequest& — never by value.
 *
 *  FIX-HDR     HttpResponse: added extra_headers (empty by default).
 *              Required for CORS preflight headers on OPTIONS responses and for
 *              any future per-response cache-control or rate-limit headers.
 *              All existing handlers that return {status, body} are unaffected.
 */

#include "ring_buffer.hpp"
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <atomic>
#include <cstddef>

namespace tqc {

// ── HTTP types ─────────────────────────────────────────────────────────────────

struct HttpRequest {
    std::string_view method;
    std::string_view path;
    std::string_view api_key;
    std::string_view auth_bearer;
    std::string_view body;
    std::string      raw_buf;    // owns the backing storage; views point here

    HttpRequest() = default;

    // FIX-DANGLE: copying or moving HttpRequest leaves string_view members
    // pointing into the old raw_buf address.  Both operations are deleted so
    // any accidental copy is caught at compile time rather than silently
    // producing dangling views.  Pass by const reference only.
    HttpRequest(const HttpRequest&)            = delete;
    HttpRequest& operator=(const HttpRequest&) = delete;
    HttpRequest(HttpRequest&&)                 = delete;
    HttpRequest& operator=(HttpRequest&&)      = delete;
};

struct HttpResponse {
    int         status       = 200;
    std::string body;
    std::string content_type  = "application/json";

    // FIX-HDR: optional extra header lines, each terminated with \r\n.
    // formatResponse appends these verbatim before the blank line.
    // Example: "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
    // Default empty — all existing {status, body} construction is unaffected.
    std::string extra_headers;
};

using HandlerFn = std::function<HttpResponse(const HttpRequest&)>;

struct Route {
    const char* method;
    const char* path;
    bool        auth;
    HandlerFn   handler;
};

// ── HttpServer ─────────────────────────────────────────────────────────────────

class HttpServer {
public:
    explicit HttpServer(int port = 7860, int n_workers = 4) noexcept
        : port_(port), n_workers_(n_workers) {}

    void addRoute(const char* method, const char* path,
                  bool require_auth, HandlerFn handler);

    // Starts the server.  Blocks the calling thread until stop() is called
    // from another thread and the acceptor's next poll() timeout fires (≤100ms).
    void start();

    // Thread-safe.  Sets running_ = false; the acceptor exits on its next
    // poll() timeout (≤100ms) and workers drain and exit cleanly.
    void stop() noexcept { running_.store(false, std::memory_order_release); }

private:
    int                       port_;
    int                       n_workers_;
    std::atomic<bool>         running_{true};
    std::vector<Route>        routes_;
    MPSCRingBuffer<int, 256>  fd_queue_{};

    void acceptorThread(int server_fd) noexcept;
    void workerThread()                noexcept;

    [[nodiscard]] bool         parseRequest  (std::string& raw,
                                              HttpRequest& req) const noexcept;
    [[nodiscard]] std::string  formatResponse(const HttpResponse& resp) const;
    [[nodiscard]] HttpResponse dispatch      (const HttpRequest&  req)  const;
    [[nodiscard]] bool         checkAuth     (const HttpRequest&  req)  const noexcept;
};

} // namespace tqc
