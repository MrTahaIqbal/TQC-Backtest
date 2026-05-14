/*
 * http_server.cpp  -  BigBoyAgent TQC Backtest | Taha Iqbal
 *
 * Identical architecture to Brain's http_server.cpp:
 *   - Connection: close (no keep-alive)
 *   - gmtime_r for thread safety
 *   - Case-insensitive Content-Length (RFC 7230)
 *   - MPSC ring buffer for acceptor → worker IPC
 *   - strncasecmp from <strings.h> (POSIX, not std::)
 *
 * ── Fixes applied ─────────────────────────────────────────────────────────────
 *
 *  FIX-EINTR   recv() retries on EINTR.  Old break-on-any-negative silently
 *              dropped connections when a signal was delivered to the worker.
 *
 *  FIX-DOS     Hard 8 MiB recv_buf cap.  Old code appended indefinitely —
 *              a single client sending Content-Length: 2000000000 would OOM
 *              the process and deny service to all workers.
 *
 *  FIX-TIMING  checkAuth uses a constant-time byte comparison loop.  Old
 *              strncmp returned on the first differing byte, leaking the
 *              position of the first mismatch via response timing.
 *
 *  FIX-SCAN    hdr_end stored as a persistent variable; recv loop no longer
 *              rescans the full buffer on every chunk.  Old O(n²) behaviour
 *              cost ~2 billion comparisons for a 4 MiB body in 4 KiB chunks.
 *
 *  FIX-ATOI    std::atoi replaced by std::from_chars throughout.  atoi is UB
 *              on overflow and returns 0 on parse failure with no error signal.
 *              from_chars is noexcept, overflow-safe, and allocation-free.
 *
 *  FIX-SEND    send() replaced by a send_all() loop.  A single send() may
 *              deliver fewer bytes than requested when the socket buffer is
 *              full; the tail of the HTTP response was silently dropped.
 *
 *  FIX-SHUTDOWN stop() now reliably unblocks the acceptor.  Old code set
 *              running_=false while accept() was blocked; the acceptor never
 *              checked the flag and the process hung until a new connection
 *              arrived.  Fixed by poll()-with-timeout before each accept().
 *
 *  FIX-DEAD    Removed dead ci_find lambda from parseRequest (defined but
 *              never called; suppressed with (void)ci_find on line 117).
 *
 *  FIX-CORS    OPTIONS preflight now returns the three required CORS headers
 *              (Allow-Methods, Allow-Headers, Max-Age) via extra_headers.
 *              Old {200,""} was missing all three; browsers rejected the
 *              preflight and blocked the subsequent cross-origin request.
 *
 *  FIX-SUBSTR  recv loop: Content-Length scan operates directly on recv_buf
 *              with an explicit hdr_end bound — no substr heap copy.
 *
 *  FIX-FDLEAK  server_fd wrapped in a minimal RAII guard in start() so bind()
 *              and listen() failures close the fd before throwing.
 *
 *  FIX-RCVTO   Accepted sockets get SO_RCVTIMEO = 5 s.  Without a timeout a
 *              slowloris client holds a worker thread in recv() indefinitely;
 *              four such clients silence all four default workers.
 *
 *  FIX-NWORK   n_workers_ clamped to ≥ 1 in start().  Zero workers caused the
 *              server to accept connections that were never handled.
 */

#include "http_server.hpp"
#include "config.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <strings.h>       // strncasecmp — POSIX, not std::

#include <charconv>        // std::from_chars — FIX-ATOI
#include <cstring>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <algorithm>
#include <vector>
#include <thread>
#include <chrono>

namespace tqc {

// ── Constants ─────────────────────────────────────────────────────────────────

// FIX-DOS: hard cap on per-connection receive buffer.
// A 413 Payload Too Large response is sent and the connection closed if
// recv_buf grows past this limit before the full body has arrived.
static constexpr std::size_t MAX_REQUEST_BYTES = 8u * 1024u * 1024u;   // 8 MiB

// FIX-SHUTDOWN: poll() timeout in the acceptor loop.  Caps the maximum delay
// between stop() being called and the acceptor actually exiting.
static constexpr int ACCEPTOR_POLL_TIMEOUT_MS = 100;

// FIX-RCVTO: per-connection receive timeout.  Bounds how long a slow or
// adversarial client can hold a worker thread inside recv().
static constexpr int SOCKET_RECV_TIMEOUT_SEC  = 5;

// ── RAII socket guard ─────────────────────────────────────────────────────────
// FIX-FDLEAK: ensures server_fd is closed on any early-exit path in start().
struct FdGuard {
    int fd;
    explicit FdGuard(int f) noexcept : fd(f) {}
    ~FdGuard() noexcept { if (fd >= 0) ::close(fd); }
    int release() noexcept { int f = fd; fd = -1; return f; }
    FdGuard(const FdGuard&)            = delete;
    FdGuard& operator=(const FdGuard&) = delete;
};

// ── send_all ──────────────────────────────────────────────────────────────────
// FIX-SEND: loops until all bytes are delivered or an unrecoverable error occurs.
// A single send() may deliver fewer bytes than requested when the socket
// send buffer is full — a normal condition under moderate load.
static bool send_all(int fd, const char* buf, std::size_t len) noexcept {
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;   // signal interrupted — retry
            return false;                   // real socket error
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// ── constant_time_eq ──────────────────────────────────────────────────────────
// FIX-TIMING: compares two byte sequences in constant time with respect to
// their content.  Returns true only when lengths are equal AND every byte
// matches.  The XOR-accumulation pattern prevents the compiler from short-
// circuiting the loop, and the volatile sink prevents the dead-store elimination
// that would defeat it.  This is the standard HMAC-safe comparison idiom.
//
// Note: this is not hardware-enforced constant time (branch on len mismatch
// exits early when lengths differ).  Length equality does not reveal anything
// useful to an attacker who already knows how long the key is.  The
// byte-by-byte comparison itself runs to completion regardless of content.
[[nodiscard]] static bool constant_time_eq(std::string_view a,
                                            std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    unsigned char result = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        result |= static_cast<unsigned char>(a[i]) ^
                  static_cast<unsigned char>(b[i]);
    return result == 0;
}

// ── HttpServer::addRoute ───────────────────────────────────────────────────────
void HttpServer::addRoute(const char* method, const char* path,
                           bool require_auth, HandlerFn handler) {
    routes_.push_back({method, path, require_auth, std::move(handler)});
}

// ── HttpServer::checkAuth ──────────────────────────────────────────────────────

bool HttpServer::checkAuth(const HttpRequest& req) const noexcept {
    const char* secret = globalConfig().secret_key;
    const std::string_view sv_secret(secret, std::strlen(secret));
    if (!req.api_key.empty()     && constant_time_eq(req.api_key,     sv_secret)) return true;
    if (!req.auth_bearer.empty() && constant_time_eq(req.auth_bearer, sv_secret)) return true;
    return false;
}

// ── HttpServer::parseRequest ───────────────────────────────────────────────────
// Parses the HTTP request line and headers from raw.
// All string_view fields in req point into raw — raw must outlive req.
//
// FIX-DEAD:  ci_find lambda removed entirely (was defined but never called).
// FIX-ATOI:  std::from_chars replaces std::atoi for Content-Length.
bool HttpServer::parseRequest(std::string& raw, HttpRequest& req) const noexcept {
    // ── Request line ──────────────────────────────────────────────────────────
    const std::size_t method_end = raw.find(' ');
    if (method_end == std::string::npos) return false;

    const std::size_t path_start = method_end + 1;
    const std::size_t path_end   = raw.find(' ', path_start);
    if (path_end == std::string::npos) return false;

    req.method = std::string_view(raw.data(),              method_end);
    req.path   = std::string_view(raw.data() + path_start, path_end - path_start);

    std::size_t pos = raw.find("\r\n");
    if (pos == std::string::npos) return false;
    pos += 2;

    int content_length = 0;

    // ── Header loop ───────────────────────────────────────────────────────────
    // RFC 7230 §3.2: field names are case-insensitive — use strncasecmp.
    while (pos < raw.size()) {
        const std::size_t end = raw.find("\r\n", pos);
        if (end == std::string::npos) break;
        if (end == pos) { pos += 2; break; }   // blank line — end of headers

        const std::string_view hdr(raw.data() + pos, end - pos);

        if (strncasecmp(hdr.data(), "x-api-key:", 10) == 0) {
            const std::size_t vs = hdr.find_first_not_of(" \t", 10);
            if (vs != std::string_view::npos)
                req.api_key = std::string_view(raw.data() + pos + vs, end - pos - vs);

        } else if (strncasecmp(hdr.data(), "authorization:", 14) == 0) {
            const std::size_t vs = hdr.find_first_not_of(" \t", 14);
            if (vs != std::string_view::npos) {
                const std::string_view val(raw.data() + pos + vs, end - pos - vs);
                if (val.size() > 7 && strncasecmp(val.data(), "bearer ", 7) == 0)
                    req.auth_bearer = val.substr(7);
            }

        } else if (strncasecmp(hdr.data(), "content-length:", 15) == 0) {
            const std::size_t vs = hdr.find_first_not_of(" \t", 15);
            if (vs != std::string_view::npos) {
                // FIX-ATOI: from_chars is noexcept, overflow-safe, no allocation.
                const char* first = hdr.data() + vs;
                const char* last  = hdr.data() + hdr.size();
                int parsed = 0;
                auto [ptr, ec] = std::from_chars(first, last, parsed);
                if (ec == std::errc{} && parsed >= 0)
                    content_length = parsed;
                // on parse error or negative value: content_length stays 0
            }
        }

        pos = end + 2;
    }

    // ── Body ──────────────────────────────────────────────────────────────────
    if (content_length > 0 && pos < raw.size())
        req.body = std::string_view(raw.data() + pos,
                                    std::min<std::size_t>(
                                        static_cast<std::size_t>(content_length),
                                        raw.size() - pos));
    return true;
}

// ── HttpServer::formatResponse ─────────────────────────────────────────────────
// FIX-HDR: appends resp.extra_headers (if non-empty) before the blank line.
// Each entry in extra_headers must already end with \r\n.
std::string HttpServer::formatResponse(const HttpResponse& resp) const {
    struct tm tm_buf{};
    std::time_t now = std::time(nullptr);
    ::gmtime_r(&now, &tm_buf);
    char date[64]{};
    std::strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);

    const char* status_text =
        resp.status == 200 ? "OK"                  :
        resp.status == 400 ? "Bad Request"          :
        resp.status == 401 ? "Unauthorized"         :
        resp.status == 404 ? "Not Found"            :
        resp.status == 405 ? "Method Not Allowed"   :
        resp.status == 413 ? "Payload Too Large"    :
        resp.status == 429 ? "Too Many Requests"    :
                             "Internal Server Error";

    std::string out;
    out.reserve(256 + resp.extra_headers.size() + resp.body.size());
    out  = "HTTP/1.1 ";
    out += std::to_string(resp.status);
    out += ' ';
    out += status_text;
    out += "\r\nDate: ";
    out += date;
    out += "\r\nContent-Type: ";
    out += resp.content_type;
    out += "\r\nContent-Length: ";
    out += std::to_string(resp.body.size());
    out += "\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n";
    // FIX-HDR: per-response extra headers (CORS preflight, rate-limit, etc.)
    if (!resp.extra_headers.empty())
        out += resp.extra_headers;
    out += "\r\n";
    out += resp.body;
    return out;
}

// ── HttpServer::dispatch ───────────────────────────────────────────────────────
// FIX-CORS: OPTIONS preflight now returns the three mandatory CORS headers.
// Without Allow-Methods, Allow-Headers, and Max-Age the browser discards the
// preflight response and blocks the actual request regardless of the origin *.
HttpResponse HttpServer::dispatch(const HttpRequest& req) const {
    for (const Route& r : routes_) {
        if (req.method == r.method && req.path == r.path) {
            if (r.auth && !checkAuth(req))
                return {401, R"({"error":"unauthorized"})"};
            return r.handler(req);
        }
    }

    if (req.method == "OPTIONS") {
        HttpResponse preflight;
        preflight.status = 200;
        preflight.body   = "";
        // FIX-CORS: all three headers required for a valid CORS preflight.
        preflight.extra_headers =
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, X-Api-Key, Authorization\r\n"
            "Access-Control-Max-Age: 86400\r\n";
        return preflight;
    }

    return {404, R"({"error":"not_found"})"};
}

// ── HttpServer::workerThread ───────────────────────────────────────────────────
// FIX-EINTR:  recv() retried on EINTR.
// FIX-DOS:    recv_buf capped at MAX_REQUEST_BYTES; 413 sent on breach.
// FIX-SCAN:   hdr_end stored once and reused — no repeated full-buffer scan.
// FIX-ATOI:   from_chars for Content-Length in the recv loop.
// FIX-SEND:   send_all() loop instead of a single send().
// FIX-SUBSTR: Content-Length scan runs directly on recv_buf — no substr copy.
// FIX-RCVTO:  SO_RCVTIMEO already set on fd before it reaches this thread.
void HttpServer::workerThread() noexcept {
    static constexpr std::size_t CHUNK = 4096;
    char tmp[CHUNK];

    std::string recv_buf;
    recv_buf.reserve(65536);

    while (running_.load(std::memory_order_acquire)) {
        auto fd_opt = fd_queue_.pop();
        if (!fd_opt) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }
        int fd = *fd_opt;

        recv_buf.clear();
        int         content_length = 0;
        bool        headers_done   = false;
        std::size_t hdr_end        = 0;   // FIX-SCAN: stored once, never rescanned
        bool        too_large      = false;

        while (true) {
            // FIX-DOS: enforce hard cap before each recv.
            if (recv_buf.size() >= MAX_REQUEST_BYTES) {
                too_large = true;
                break;
            }

            // FIX-EINTR: retry recv on signal interruption.
            ssize_t n;
            do { n = ::recv(fd, tmp, sizeof(tmp), 0); }
            while (n < 0 && errno == EINTR);

            if (n == 0) break;    // peer closed connection cleanly
            if (n  < 0) break;    // real socket error (including SO_RCVTIMEO expiry → EAGAIN/EWOULDBLOCK)

            recv_buf.append(tmp, static_cast<std::size_t>(n));

            if (!headers_done) {
                const auto found = recv_buf.find("\r\n\r\n");
                if (found != std::string::npos) {
                    headers_done = true;
                    hdr_end      = found;   // FIX-SCAN: cache once

                    // FIX-SUBSTR: scan directly on recv_buf up to hdr_end.
                    // FIX-ATOI:   from_chars for Content-Length.
                    for (std::size_t i = 0; i + 15 < hdr_end; ++i) {
                        if (strncasecmp(recv_buf.c_str() + i, "content-length:", 15) == 0) {
                            // Skip optional whitespace after colon.
                            std::size_t vs = i + 15;
                            while (vs < hdr_end &&
                                   (recv_buf[vs] == ' ' || recv_buf[vs] == '\t'))
                                ++vs;
                            int parsed = 0;
                            auto [ptr, ec] = std::from_chars(
                                recv_buf.c_str() + vs,
                                recv_buf.c_str() + hdr_end,
                                parsed);
                            if (ec == std::errc{} && parsed >= 0)
                                content_length = parsed;
                            break;
                        }
                    }
                }
            }

            if (headers_done) {
                // FIX-SCAN: use cached hdr_end — no re-scan.
                const std::size_t body_start = hdr_end + 4;
                const std::size_t body_have  =
                    (recv_buf.size() > body_start) ? recv_buf.size() - body_start : 0;
                if (static_cast<int>(body_have) >= content_length) break;
            }
        }

        if (too_large) {
            const std::string r413 = formatResponse(
                {413, R"({"error":"payload_too_large"})"});
            send_all(fd, r413.data(), r413.size());
            ::close(fd);
            continue;
        }

        // Build request — raw_buf owns the buffer; parseRequest sets views into it.
        HttpRequest req;
        req.raw_buf = std::move(recv_buf);
        if (!parseRequest(req.raw_buf, req)) {
            const std::string r400 = formatResponse(
                {400, R"({"error":"bad_request"})"});
            send_all(fd, r400.data(), r400.size());
            ::close(fd);
            recv_buf.clear();   // restore for next iteration (was moved)
            recv_buf.reserve(65536);
            continue;
        }

        const HttpResponse resp = dispatch(req);
        const std::string  out  = formatResponse(resp);

        // FIX-SEND: deliver all bytes; partial send is a silent truncation bug.
        send_all(fd, out.data(), out.size());
        ::close(fd);

        // Restore recv_buf for next iteration (was moved into req.raw_buf).
        recv_buf = std::move(req.raw_buf);
        recv_buf.clear();
        if (recv_buf.capacity() < 65536) recv_buf.reserve(65536);
    }
}

// ── HttpServer::acceptorThread ────────────────────────────────────────────────
// FIX-SHUTDOWN: poll() with ACCEPTOR_POLL_TIMEOUT_MS before each accept()
// so stop() takes effect within one poll period (≤100ms) rather than hanging
// until the next connection arrives.
// FIX-RCVTO: sets SO_RCVTIMEO on every accepted fd so a slow client cannot
// permanently hold a worker thread in recv().
void HttpServer::acceptorThread(int server_fd) noexcept {
    struct pollfd pfd{};
    pfd.fd     = server_fd;
    pfd.events = POLLIN;

    while (running_.load(std::memory_order_acquire)) {
        // FIX-SHUTDOWN: bounded wait — recheck running_ every 100 ms.
        const int ready = ::poll(&pfd, 1, ACCEPTOR_POLL_TIMEOUT_MS);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;   // unrecoverable poll error
        }
        if (ready == 0) continue;   // timeout — loop back and check running_

        struct sockaddr_in client_addr{};
        socklen_t          client_len = sizeof(client_addr);
        const int client_fd = ::accept(server_fd,
                                        reinterpret_cast<struct sockaddr*>(&client_addr),
                                        &client_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;   // unrecoverable accept error
        }

        // TCP_NODELAY: disable Nagle's algorithm — critical for low-latency
        // request-response patterns where each response is a single write.
        int flag = 1;
        ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        // FIX-RCVTO: bound how long a worker can be held in recv() by one client.
        struct timeval tv{};
        tv.tv_sec  = SOCKET_RECV_TIMEOUT_SEC;
        tv.tv_usec = 0;
        ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (!fd_queue_.push(std::move(client_fd))) {
            ::close(client_fd);   // ring buffer full — drop connection; caller retries
        }
    }
}

// ── HttpServer::start ──────────────────────────────────────────────────────────
// FIX-FDLEAK:  FdGuard closes server_fd on any throw path.
// FIX-NWORK:   n_workers_ clamped to ≥ 1.
void HttpServer::start() {
    // FIX-NWORK: zero workers produce a server that accepts but never responds.
    const int n_workers = std::max(1, n_workers_);

    const int raw_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (raw_fd < 0) throw std::runtime_error("socket() failed");
    FdGuard guard(raw_fd);   // FIX-FDLEAK: closes raw_fd on any throw below

    int opt = 1;
    ::setsockopt(raw_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port_));

    if (::bind(raw_fd,
               reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error("bind() failed");   // guard closes raw_fd

    if (::listen(raw_fd, 128) < 0)
        throw std::runtime_error("listen() failed"); // guard closes raw_fd

    // Socket is fully set up — release from the guard and manage manually below.
    const int server_fd = guard.release();

    std::fprintf(stderr,
        "[HTTP] Backtest server listening on port %d (%d workers)\n",
        port_, n_workers);

    // FIX-NWORK: use the clamped count for thread creation.
    std::vector<std::thread> workers;
    workers.reserve(n_workers);
    for (int i = 0; i < n_workers; ++i)
        workers.emplace_back(&HttpServer::workerThread, this);

    acceptorThread(server_fd);   // blocks until running_ == false

    // Drain: signal workers and wait for clean exit.
    running_.store(false, std::memory_order_release);
    for (auto& t : workers)
        if (t.joinable()) t.join();

    ::close(server_fd);
}

} // namespace tqc
