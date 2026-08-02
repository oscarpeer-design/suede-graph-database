#pragma once
// dependencies
#include <unordered_map>
#include <map>
#include <vector>
#include <string>
#include <functional>
#include <thread>
#include <atomic>    // std::atomic for the shutdown flag
#include <csignal>   // std::signal / SIGINT / SIGTERM for graceful shutdown
#include <sstream>   // for stringstream
#include <cctype>    // for std::tolower
#include <cstring>   // for std::strerror
#include <cstdint>

// ---------------------------------------------------------------------------
// Cross-platform socket compatibility shim.
//
// Everything below this block is plain sockets logic that is identical on every
// platform. The ONLY thing that actually differs between Windows (Winsock) and
// POSIX (Linux/macOS/BSD) is a handful of primitives: the socket handle type,
// the "invalid"/"error" sentinels, how you close a socket, how you start/stop
// the networking subsystem, and the type of a byte count returned by recv/send.
// We paper over exactly those here so the rest of the file compiles unchanged on
// both. No functional change on Windows -- the _WIN32 branch maps straight back
// onto the original Winsock calls (winsock2.h, ws2_32.lib, closesocket, WSA*).
// ---------------------------------------------------------------------------
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>   // inet_pton, getaddrinfo, and the newer address helpers
#pragma comment(lib, "ws2_32.lib")   // link the Winsock library (MSVC)

// On Windows a socket is an opaque SOCKET (unsigned); the sentinels are macros.
using socket_t = SOCKET;
using sockopt_len_t = int;                 // setsockopt/getsockopt length type
using recv_len_t = int;                    // recv/send return an int here
// INVALID_SOCKET / SOCKET_ERROR come from <winsock2.h> as-is.

// Close a socket handle.
inline int closeSocket(socket_t s) { return closesocket(s); }

// setsockopt on Windows wants (const char*); POSIX wants (const void*). This
// wrapper hides the cast so call sites are identical on both platforms.
inline int setSocketOption(socket_t s, int level, int name,
    const void* val, sockopt_len_t len) {
    return setsockopt(s, level, name, reinterpret_cast<const char*>(val), len);
}

// Last socket error, as a human-readable string (for `err` messages).
inline std::string socketErrorString() {
    return "winsock error " + std::to_string(WSAGetLastError());
}
#else
    // POSIX headers for sockets, address helpers, close(), and errno.
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>   // inet_pton, htons/htonl
#include <netdb.h>       // getaddrinfo
#include <unistd.h>      // close()
#include <cerrno>

// On POSIX a socket is just an int fd; there are no INVALID_SOCKET /
// SOCKET_ERROR macros, so we define our own with the standard POSIX values
// (a failed socket()/accept() returns -1; a failed syscall returns -1).
using socket_t = int;
using sockopt_len_t = socklen_t;
using recv_len_t = ssize_t;                // recv/send return ssize_t here

#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#ifndef SOCKET_ERROR
#define SOCKET_ERROR   (-1)
#endif

inline int closeSocket(socket_t s) { return ::close(s); }

inline int setSocketOption(socket_t s, int level, int name,
    const void* val, sockopt_len_t len) {
    return setsockopt(s, level, name, val, len);
}

inline std::string socketErrorString() {
    return std::strerror(errno);
}
#endif

// ---------------------------------------------------------------------------
// HTTP status codes.
// NOTE: this namespace must be declared BEFORE HttpResponse (which defaults its
// status to Http::Ok) and before HttpCodeToName / statusText which reference it.
// ---------------------------------------------------------------------------
namespace Http {
    constexpr int Ok = 200;             // request understood AND engine succeeded
    constexpr int BadRequest = 400;     // couldn't understand the request itself
    constexpr int NotFound = 404;       // no such route, or no such id
    constexpr int RequestTimeout = 408; // server would like to shut down this unused connection
    constexpr int Conflict = 409;       // request conflicts with the current state of the server
    constexpr int ContentTooLarge = 413;// request body larger than server limits
    constexpr int Unprocessable = 422;  // understood perfectly, engine said no
    constexpr int Locked = 423;         // the resource being accessed is locked
    constexpr int ServerError = 500;    // something threw - the catch-all
}

// define http request
struct HttpRequest {
    std::string method;   // "GET", "POST"
    std::string path;     // "/query"
    std::string body;     // raw body bytes (JSON text, but http_simple doesn't care)
    std::map<std::string, std::string> headers;  // lowercased keys
};

// define HttpResponse
struct HttpResponse {
    int status = Http::Ok;
    std::string body;
    std::string contentType = "application/json";
};

// the maximum size of a request is 4 Megabytes
const size_t MAX_REQUEST = 4 * 1024 * 1024;   // 4 MB hard cap - refuse anything larger
// the size of a chunk of data read from a socket
const size_t CHUNK_SIZE = 4096;

// ---------------------------------------------------------------------------
// statusText: map a status code to its STANDARD HTTP reason phrase.
// This is used to build the status line "HTTP/1.1 <code> <phrase>", so it must
// return ONLY the phrase (e.g. "OK", "Bad Request") - no code, no colon, no
// "Success/Error" prefix. writeResponse() prepends the numeric code itself.
// ---------------------------------------------------------------------------
const std::unordered_map<int, std::string> HttpCodeToReason = {
    {Http::Ok,              "OK"},
    {Http::BadRequest,      "Bad Request"},
    {Http::NotFound,        "Not Found"},
    {Http::RequestTimeout,  "Request Timeout"},
    {Http::Conflict,        "Conflict"},
    {Http::ContentTooLarge, "Content Too Large"},
    {Http::Unprocessable,   "Unprocessable Entity"},
    {Http::Locked,          "Locked"},
    {Http::ServerError,     "Internal Server Error"}
};

inline std::string statusText(const int code) {
    auto it = HttpCodeToReason.find(code);
    if (it == HttpCodeToReason.end())
        return "Internal Server Error";   // safe fallback phrase for unknown codes
    return it->second;
}

// ---------------------------------------------------------------------------
// Networking lifecycle
//
// Windows requires WSAStartup/WSACleanup to bring the Winsock subsystem up and
// down; POSIX needs no such global init. netInit/netCleanup keep the same call
// contract on both so runSuedeServer's start/stop code is platform-agnostic --
// on POSIX they are simply no-ops that always succeed.
// ---------------------------------------------------------------------------
#ifdef _WIN32
const int windows_version = 2; // use version 2.2

// WSAStartup - call once at program start. Returns false if networking could
// not be initialised; the caller must NOT proceed to any socket call in that case.
inline bool netInit() {
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(windows_version, windows_version), &wsaData);
    return result == 0;
}

// WSACleanup - call once at shutdown.
inline void netCleanup() {
    WSACleanup();
}
#else
// POSIX has no networking subsystem to start; these succeed trivially so the
// caller can treat init/cleanup identically on every platform.
inline bool netInit() { return true; }
inline void netCleanup() {}
#endif

// ---------------------------------------------------------------------------
// makeListener: create + bind + listen. Returns a socket bound to
// 127.0.0.1:port (or 0.0.0.0:port if use_public) and listening, or
// INVALID_SOCKET with err filled on failure.
// ---------------------------------------------------------------------------
inline socket_t makeListener(const int port, std::string& err, bool use_public = false) {
    // first, create the socket
    socket_t listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd == INVALID_SOCKET) {
        err = "socket() failed: " + socketErrorString();
        return INVALID_SOCKET;
    }
    // second, allow immediate reuse of the port on restart
    int yes = 1;
    setSocketOption(listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    // bind to host
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    // localhost for testing, public when explicitly requested by the operator
    if (!use_public)
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // localhost only
    else
        addr.sin_addr.s_addr = htonl(INADDR_ANY);        // public

    addr.sin_port = htons((u_short)port);                // network byte order

    if (bind(listenFd, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        err = "bind() failed - is port " + std::to_string(port) + " already in use? ("
            + socketErrorString() + ")";
        closeSocket(listenFd);
        return INVALID_SOCKET;
    }

    if (listen(listenFd, SOMAXCONN) == SOCKET_ERROR) {
        err = "listen() failed: " + socketErrorString();
        closeSocket(listenFd);
        return INVALID_SOCKET;
    }

    return listenFd;   // success - ready to accept()
}

// ---------------------------------------------------------------------------
// parseHead: parse the HTTP request header block into method, path, headers.
//   headerBlock: raw header text, lines separated by "\r\n" (blank line already
//                stripped by the caller).
// Returns true on success; false with err set on malformed input.
// ---------------------------------------------------------------------------
static inline bool parseHead(const std::string& headerBlock, HttpRequest& out, std::string& err) {
    // ---- STEP 1: split header block into lines on "\r\n" ----
    std::vector<std::string> headerLines;
    size_t currentPosition = 0;
    const size_t increment = 2;   // length of "\r\n"
    while (true) {
        size_t delimiterPosition = headerBlock.find("\r\n", currentPosition);
        if (delimiterPosition == std::string::npos) {
            headerLines.push_back(headerBlock.substr(currentPosition));
            break;
        }
        headerLines.push_back(headerBlock.substr(currentPosition, delimiterPosition - currentPosition));
        currentPosition = delimiterPosition + increment;
    }

    // must have at least the request line
    if (headerLines.empty() || headerLines[0].empty()) {
        err = "empty request line";
        return false;
    }

    // ---- STEP 2: parse the request line "METHOD PATH VERSION" ----
    {
        std::istringstream requestLineStream(headerLines[0]);
        std::string httpVersion;
        if (!(requestLineStream >> out.method >> out.path >> httpVersion)) {
            err = "malformed request line";
            return false;
        }
        // httpVersion extracted but not validated (optional improvement)
    }

    // ---- STEP 3: parse header lines "Key: value" ----
    for (size_t lineIndex = 1; lineIndex < headerLines.size(); ++lineIndex) {
        const std::string& currentHeaderLine = headerLines[lineIndex];
        if (currentHeaderLine.empty())
            continue;

        size_t colonPosition = currentHeaderLine.find(':');
        if (colonPosition == std::string::npos) {
            err = "header line missing ':'";
            return false;
        }

        std::string headerName = currentHeaderLine.substr(0, colonPosition);
        std::string headerValue = currentHeaderLine.substr(colonPosition + 1);

        // ---- STEP 4: normalise: lowercase the key, trim the value ----
        for (char& character : headerName)
            character = (char)std::tolower((unsigned char)character);

        size_t firstNonWhitespace = headerValue.find_first_not_of(" \t");
        size_t lastNonWhitespace = headerValue.find_last_not_of(" \t");
        if (firstNonWhitespace == std::string::npos)
            headerValue = "";
        else
            headerValue = headerValue.substr(firstNonWhitespace, lastNonWhitespace - firstNonWhitespace + 1);

        out.headers[headerName] = headerValue;
    }

    return true;
}

// ---------------------------------------------------------------------------
// readRequest: read exactly one complete HTTP request off the connection.
// Imposes message boundaries on the raw TCP byte stream: read until the header
// block is complete (\r\n\r\n), then read exactly Content-Length body bytes.
// Returns true on a complete request; false with err set on malformed input,
// timeout, oversize, or disconnect.
// ---------------------------------------------------------------------------
inline bool readRequest(socket_t conn, HttpRequest& out, std::string& err) {
    size_t headerEnd = std::string::npos;
    bool headerEndFound = false;
    std::string buffer;                 // accumulates raw bytes as they arrive
    char chunk[CHUNK_SIZE];

    // STEP 1: read until we have the full header block (up to \r\n\r\n)
    while (!headerEndFound) {
        recv_len_t n = recv(conn, chunk, sizeof(chunk), 0);
        if (n == 0) {
            err = "client closed connection before sending a request";
            return false;
        }
        if (n < 0) {
            err = "recv() failed or timed out";
            return false;
        }
        buffer.append(chunk, n);
        if (buffer.size() > MAX_REQUEST) {
            err = "request too large";
            return false;
        }
        headerEnd = buffer.find("\r\n\r\n");
        headerEndFound = (headerEnd != std::string::npos);
    }

    // STEP 2: parse the request line + headers
    std::string headerBlock = buffer.substr(0, headerEnd);
    if (!parseHead(headerBlock, out, err))
        return false;

    // STEP 3: figure out the body length from Content-Length
    size_t contentLength = 0;
    auto it = out.headers.find("content-length");
    if (it != out.headers.end()) {
        // guard against a non-numeric Content-Length: reject as a bad request
        // rather than letting std::stoul throw (which would surface as a 500).
        try {
            size_t consumed = 0;
            unsigned long parsed = std::stoul(it->second, &consumed);
            if (consumed != it->second.size()) {
                err = "malformed Content-Length";
                return false;
            }
            contentLength = (size_t)parsed;
        }
        catch (...) {
            err = "malformed Content-Length";
            return false;
        }
        if (contentLength > MAX_REQUEST) {
            err = "declared body too large";
            return false;
        }
    }
    // no Content-Length header -> assume no body (fine for GET /stats etc.)

    // STEP 4: we may already have some/all of the body in `buffer`
    std::string body = buffer.substr(headerEnd + 4);   // +4 skips the \r\n\r\n

    // STEP 5: keep reading until body has exactly contentLength bytes
    while (body.size() < contentLength) {
        recv_len_t n = recv(conn, chunk, sizeof(chunk), 0);
        if (n == 0) {
            err = "client closed connection mid-body";
            return false;
        }
        if (n < 0) {
            err = "recv() failed or timed out mid-body";
            return false;
        }
        body.append(chunk, n);
        if (body.size() > MAX_REQUEST) {
            err = "body exceeded limit";
            return false;
        }
    }

    out.body = body.substr(0, contentLength);   // trim any bytes beyond this request
    return true;
}

// ---------------------------------------------------------------------------
// writeResponse: serialise an HttpResponse to valid HTTP/1.1 bytes and send all
// of them. Content-Length is always resp.body.size(). Returns false if the
// socket write fails (e.g. the client already disconnected).
// ---------------------------------------------------------------------------
inline bool writeResponse(socket_t conn, const HttpResponse& resp) {
    if (conn == INVALID_SOCKET)
        return false;

    const std::string sep = "\r\n";

    std::string statusLine =
        "HTTP/1.1 " + std::to_string(resp.status) + " " + statusText(resp.status) + sep;
    std::string contentTypeHeader = "Content-Type: " + resp.contentType + sep;
    std::string contentLengthHeader = "Content-Length: " + std::to_string(resp.body.size()) + sep;
    std::string connectionHeader = "Connection: close" + sep;
    // blank line ends the headers; body (possibly empty) follows
    std::string body = sep + resp.body;

    std::string response = statusLine + contentTypeHeader + contentLengthHeader
        + connectionHeader + body;

    // send-all loop: send() may take only part of the buffer, so loop until done
    size_t totalSent = 0;
    while (totalSent < response.size()) {
        recv_len_t n = send(conn, response.data() + totalSent,
            (recv_len_t)(response.size() - totalSent), 0);
        if (n == SOCKET_ERROR || n == 0)
            return false;
        totalSent += (size_t)n;
    }
    return true;
}

// ---------------------------------------------------------------------------
// handleConnection: the full life of ONE client connection. Contains every
// per-client failure so that nothing a client does can escape to the accept
// loop or crash a thread.
// ---------------------------------------------------------------------------
inline void handleConnection(socket_t conn,
    const std::function<HttpResponse(const HttpRequest&)>& handler) {
    // (1) read timeout so a silent client can't park this thread forever.
    // SO_RCVTIMEO takes a raw millisecond DWORD on Windows but a `struct timeval`
    // on POSIX -- the one genuinely different call shape, so it is branched here.
    const int timeoutMs = 10000;   // 10 seconds
#ifdef _WIN32
    DWORD winTimeout = (DWORD)timeoutMs;
    setSocketOption(conn, SOL_SOCKET, SO_RCVTIMEO, &winTimeout, sizeof(winTimeout));
#else
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setSocketOption(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    try {
        // (2) read one request
        HttpRequest req;
        std::string err;
        if (!readRequest(conn, req, err)) {
            // malformed / timed out / client vanished -> best-effort tell them, then close
            HttpResponse bad;
            bad.status = Http::BadRequest;
            bad.body = R"({"error":"bad request"})";
            writeResponse(conn, bad);
            closeSocket(conn);
            return;
        }

        // (3) hand the request to the routing lambda -> get a response
        HttpResponse resp = handler(req);   // where GraphHandler eventually runs

        // (4) send the response
        writeResponse(conn, resp);
    }
    catch (...) {
        // (5) any exception escaping the handler becomes a 500 - never kill the thread
        HttpResponse oops;
        oops.status = Http::ServerError;
        oops.body = R"({"error":"internal server error"})";
        writeResponse(conn, oops);
    }

    // (6) always close - every path ends here
    closeSocket(conn);
}

// ---------------------------------------------------------------------------
// Graceful-shutdown control.
//
// accept() BLOCKS. A plain "while (!stop)" flag is therefore not enough on its
// own: once the loop is parked inside accept() waiting for the next connection,
// setting a flag from another thread (or a signal handler) never gets re-checked
// until a connection happens to arrive. The portable way to unblock a waiting
// accept() is to CLOSE THE LISTENING SOCKET from the shutdown path -- that makes
// the blocked accept() return an error immediately, at which point the loop sees
// the flag and exits cleanly.
//
// So shutdown needs two pieces of shared state:
//   * g_shutdownRequested -- an atomic flag distinguishing "we asked to stop"
//     from an ordinary transient accept() error (which must NOT stop the server).
//   * g_listenerSocket    -- the listener handle, published so the shutdown path
//     can close it to break accept(). Stored as an atomic of the widest socket
//     type so a signal handler can read/close it without a lock.
//
// requestServerShutdown() is async-signal-safe enough for our use: it only sets
// an atomic flag and calls closeSocket() (close()/closesocket()), which is the
// standard "close the listener to wake accept()" idiom.
// ---------------------------------------------------------------------------

inline std::atomic<bool>& shutdownRequested() {
    static std::atomic<bool> flag{ false };
    return flag;
}

// -1 is a sentinel "no listener yet"; matches INVALID_SOCKET's value on POSIX and
// is never a valid handle on Windows either.
inline std::atomic<long long>& listenerSocket() {
    static std::atomic<long long> sock{ -1 };
    return sock;
}

// Request a graceful shutdown of the running server: set the flag, then close the
// listening socket so a blocked accept() returns and the loop can exit. Safe to
// call from a signal handler or another thread; idempotent.
inline void requestServerShutdown() {
    shutdownRequested().store(true);
    // Swap the listener out to -1 so we only close it once even if called twice.
    long long s = listenerSocket().exchange(-1);
    if (s != -1)
        closeSocket(static_cast<socket_t>(s));
}

// Signal handler: translate SIGINT (Ctrl+C) / SIGTERM into a graceful shutdown
// request. Must do as little as possible -- just trip the shutdown path.
inline void serverSignalHandler(int /*signum*/) {
    requestServerShutdown();
}

// Install SIGINT/SIGTERM handlers so Ctrl+C (or `kill`) stops the server cleanly
// instead of hard-killing the process. Call once before runServer if you want
// signal-driven shutdown; runServer works without it too (e.g. a test harness can
// call requestServerShutdown() directly).
inline void installShutdownSignalHandlers() {
    std::signal(SIGINT, serverSignalHandler);
    std::signal(SIGTERM, serverSignalHandler);
}

// ---------------------------------------------------------------------------
// runServer: open the listener, then accept until a graceful shutdown is
// requested - spawning one detached thread per connection. Returns when the
// listener cannot be created OR when requestServerShutdown() is called.
// ---------------------------------------------------------------------------
inline void runServer(const int port,
    std::function<HttpResponse(const HttpRequest&)> handler,
    std::string& err,
    const bool use_public = false) {
    // first, create the listening socket (the only early-return path)
    socket_t listener = makeListener(port, err, use_public);
    if (listener == INVALID_SOCKET)
        return;

    // publish the listener so requestServerShutdown() can close it to wake accept()
    listenerSocket().store(static_cast<long long>(listener));

    // second, accept connections until shutdown is requested
    while (!shutdownRequested().load()) {
        socket_t conn = accept(listener, nullptr, nullptr);   // blocks until a client connects
        if (conn == INVALID_SOCKET) {
            // Distinguish the two ways accept() can fail:
            //   * shutdown requested -> the listener was closed on purpose to wake
            //     us; break out of the loop and exit cleanly.
            //   * otherwise -> a transient accept() error; do NOT kill the server,
            //     just loop back and try again (original behaviour).
            if (shutdownRequested().load())
                break;
            continue;
        }
        // hand this one client to a detached thread, then loop back to accept the next
        std::thread(handleConnection, conn, handler).detach();
    }

    // Graceful exit path. requestServerShutdown() may already have closed and
    // cleared the listener (that's what woke accept()); guard against a double
    // close by only closing if it's still published to us.
    long long still = listenerSocket().exchange(-1);
    if (still != -1)
        closeSocket(static_cast<socket_t>(still));

    // NOTE: detached handleConnection threads may still be running and referencing
    // `handler` / the GraphHandler behind it. This function returning does NOT
    // wait for them. For a single-user/small-team server the practical shutdown
    // sequence is: stop accepting (here), persist the counter (caller does this),
    // then let the process exit -- in-flight requests finish or are dropped by the
    // OS on exit. If you later need a hard guarantee that no request is mid-flight
    // when the GraphHandler is destroyed, add a live-connection counter and join
    // on it here before returning.
}