// SuedeServer.cpp : Program entry point.
//
// Two modes, chosen by the command line:
//
//   SuedeServer.exe                         -> start the HTTP server (default).
//   SuedeServer.exe --mint <ip> [ttl_secs]  -> OPERATOR ACTION: mint one bearer
//                                              token for <ip>, print it to
//                                              stdout, and exit. Does NOT start
//                                              the server.
//
// The --mint branch is handled entirely here, BEFORE any server/socket work, and
// it always exits when done -- minting and serving are mutually exclusive. This
// matters for security: minting needs the secret key, so it must only ever run
// as a deliberate local command by whoever holds the key, and can NEVER be
// reachable through the running server. Keeping it a pre-server branch that
// returns guarantees that.

#include <iostream>
#include <string>
#include <cstdlib>      // std::strtoull
#include <ctime>        // std::time for the default expiry

// header file includes
#include "../SuedeServer/Server_Core/UseServer.h"
#include "../SuedeServer/Server_Core/Auth.h"   // AuthState, for the --mint branch

// Default token lifetime when no ttl is given on the command line: 1 hour.
// Deliberately SHORT so an accidentally-minted or leaked token self-destructs
// quickly. Pass an explicit ttl argument to override for longer-lived tokens.
static const uint64_t DEFAULT_TOKEN_TTL_SECONDS = 3600;

// ---------------------------------------------------------------------------
// runMint: the --mint branch. Loads the secret key + current counter, mints one
// token for `ip` valid `ttl` seconds from now, prints ONLY the token to stdout,
// and returns an exit code. On ANY failure it writes to stderr and prints
// nothing to stdout (so a script capturing stdout never mistakes an error for a
// token). Returns 0 on success, non-zero on failure.
// ---------------------------------------------------------------------------
static int runMint(const std::string& ip, uint64_t ttlSeconds) {
    std::string err;
    AuthState authState;

    // load the secret key (from SUEDE_SECRET_KEY) -- fail closed
    if (!authState.loadSecretKey(err)) {
        std::cerr << "mint failed: " << err << std::endl;
        return 1;
    }
    // load the current revocation generation, so the token is minted under it
    if (!authState.readAuthState(err)) {
        std::cerr << "mint failed: " << err << std::endl;
        return 1;
    }

    // expiry = now + ttl. (std::time gives current Unix seconds.)
    const uint64_t now = (uint64_t)std::time(nullptr);
    const uint64_t expiry = now + ttlSeconds;

    std::string token;
    if (!authState.mintToken(ip, expiry, token, err)) {
        std::cerr << "mint failed: " << err << std::endl;
        return 1;
    }

    // success: ONLY the token on stdout, nothing else, so it can be captured with
    //   $env:SUEDE_TEST_TOKEN = (SuedeServer.exe --mint 127.0.0.1)
    std::cout << token << std::endl;
    return 0;
}

// ---------------------------------------------------------------------------
// parseTtl: parse the optional ttl argument as an unsigned integer number of
// seconds. Returns true and fills `out` on success; false on junk / overflow.
// ---------------------------------------------------------------------------
static bool parseTtl(const std::string& text, uint64_t& out) {
    if (text.empty())
        return false;
    char* end = nullptr;
    errno = 0;
    unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    // reject if there was no number, trailing junk, or overflow
    if (end == text.c_str() || *end != '\0' || errno != 0)
        return false;
    out = (uint64_t)value;
    return true;
}

// parsePortNumber: parse a port string as an integer in the valid TCP range.
// Returns true and fills `port` on success; false with `err` set otherwise.
//
// NOTE: this only validates the NUMBER (range 1-65535). It deliberately does NOT
// pre-bind the socket to "check availability" -- runServer's makeListener does
// the single real bind and reports if the port is taken. Pre-binding here would
// bind the port twice, with a race window between the test-close and the real
// bind, for no benefit.
static bool parsePortNumber(const std::string& text, uint64_t& port, std::string& err) {
    if (text.empty()) {
        err = "no port number provided";
        return false;
    }
    char* end = nullptr;
    errno = 0;
    unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    // reject if there was no number, trailing junk, or overflow
    if (end == text.c_str() || *end != '\0' || errno != 0) {
        err = "expected a port number and instead got: " + text;
        return false;
    }
    // validate port is in the valid TCP range (1-65535)
    if (value < 1 || value > 65535) {
        err = "port number must be between 1 and 65535, got: " + text;
        return false;
    }
    port = value;
    return true;
}

// ---------------------------------------------------------------------------
// printHelp: print full usage to stdout. Covers both modes, the one-time key
// setup, and the token flow -- i.e. everything you need to actually get running.
// ---------------------------------------------------------------------------
static void printHelp() {
    std::cout <<
        "Suede Graph Database server\n"
        "\n"
        "USAGE\n"
        "  SuedeServer [port]              Start the HTTP server (default port 8080).\n"
        "  SuedeServer --mint <ip> [ttl]   Mint a bearer token for <ip>, print it, exit.\n"
        "  SuedeServer --help | -h         Show this help.\n"
        "\n"
        "BEFORE YOU START (one-time): set the secret key\n"
        "  The server signs auth tokens with a secret key read from the\n"
        "  SUEDE_SECRET_KEY environment variable, and refuses to start without it.\n"
        "\n"
        "  Windows (PowerShell) -- generate a key and save it permanently:\n"
        "    $key = -join ((1..64) | ForEach-Object { '{0:x2}' -f (Get-Random -Max 256) })\n"
        "    [Environment]::SetEnvironmentVariable(\"SUEDE_SECRET_KEY\", $key, \"User\")\n"
        "  Linux / macOS (bash) -- add to ~/.bashrc or ~/.profile:\n"
        "    export SUEDE_SECRET_KEY=$(openssl rand -hex 64)\n"
        "\n"
        "  NOTE: env vars are only seen by terminals opened AFTER you set them --\n"
        "  open a NEW terminal before running the server or minting a token.\n"
        "\n"
        "RUNNING (typical local session)\n"
        "  1. Start the server (leave it running):\n"
        "       SuedeServer\n"
        "  2. In another terminal, mint a token for your client IP:\n"
        "       SuedeServer --mint 127.0.0.1\n"
        "     (127.0.0.1 = same machine. The token is BOUND to this IP and expires\n"
        "      after 1 hour by default; pass a ttl in seconds to change it, e.g.\n"
        "      'SuedeServer --mint 127.0.0.1 86400' for a day.)\n"
        "  3. Open http://localhost:8080/ in a browser, paste the token into the\n"
        "     visualiser, and run queries. Or call the API directly with the header:\n"
        "       Authorization: Bearer <token>\n"
        "\n"
        "ROUTES\n"
        "  GET  /            The graph visualiser page (public, no token).\n"
        "  POST /query       Run a query        (requires a valid bearer token).\n"
        "  GET  /stats       Node / edge counts (requires a valid bearer token).\n";
}

int main(int argc, char** argv)
{
    // ---- --help mode: print usage and exit (before anything else) ----
    if (argc >= 2) {
        const std::string a1 = argv[1];
        if (a1 == "--help" || a1 == "-h" || a1 == "/?") {
            printHelp();
            return 0;
        }
    }

    // ---- --mint mode (operator action): mint a token and exit ----
    // Usage: SuedeServer.exe --mint <ip> [ttl_seconds]
    if (argc >= 2 && std::string(argv[1]) == "--mint") {
        if (argc < 3) {
            std::cerr << "usage: SuedeServer --mint <ip> [ttl_seconds]" << std::endl;
            std::cerr << "run 'SuedeServer --help' for details." << std::endl;
            return 2;
        }
        const std::string ip = argv[2];

        uint64_t ttl = DEFAULT_TOKEN_TTL_SECONDS;
        if (argc >= 4) {
            if (!parseTtl(argv[3], ttl)) {
                std::cerr << "mint failed: ttl_seconds must be a non-negative integer"
                    << std::endl;
                return 2;
            }
        }

        return runMint(ip, ttl);   // mints, prints token, exits -- never starts the server
    }

    // ---- server mode ----
    // Usage: SuedeServer.exe [port]
    //   no args      -> default port 8080
    //   one port arg -> use it (must be a valid 1-65535 integer)
    uint64_t port = 8080;
    std::string err;

    // A port argument exists ONLY when argc >= 2. Otherwise default port 8080 is used.
    if (argc >= 2) {
        if (!parsePortNumber(argv[1], port, err)) {
            std::cerr << err << std::endl;
            std::cerr << "usage: SuedeServer [port]" << std::endl;
            return 2;   // hard exit on a bad port -- do NOT silently fall back to 8080
        }
    }

    // ---- start the HTTP server ----
    bool use_public = false;
    std::cout << "Starting Suede Server on port " << port << std::endl;
    int rc = runSuedeServer(port, err, use_public);
    if (!err.empty())
        std::cerr << "server error: " << err << std::endl;
    return rc;
}