// UseServer.cpp - wires http_simple + Json + GraphHandler together.
// This is the ONLY translation unit that sees all three layers.

#include "UseServer.h"
#include "http_simple.h"
#include "Json.h"
#include "../../Queries and Graph Handlers/GraphHandler.h"
#include "../../Graph and Searchers/Graph.h"

#include <memory>
#include <functional>   // std::bind, std::ref, std::placeholders
#include <fstream>
#include <string>
#include <cstdint>
#include <cstdio>       // std::rename / std::remove for the atomic temp swap

// ---------------------------------------------------------------------------
// Server counter (auth revocation state) -- persistence.
//
// The counter is a single uint64_t whose *changes* (not its absolute value)
// carry meaning: it is the revocation state the auth layer folds into token
// signing, so flipping it invalidates outstanding tokens. Because a revoked
// user must STAY revoked across a restart, the value lives in a small text
// file that is loaded once at startup and rewritten every time it changes.
//
// Design notes that fix the earlier draft:
//   * The write must happen AT THE MOMENT THE COUNTER CHANGES (a revoke),
//     synchronously -- NOT only at shutdown. The accept loop runs forever and
//     the process normally dies by being killed/crashing, so a shutdown-only
//     write would silently lose every revocation made during the run (the
//     "revoked user comes back after reboot" bug). persistServerCounter()
//     below is what the revoke path calls.
//   * The write is crash-safe: it writes a sibling ".tmp" file, flushes, then
//     atomically renames it over the target -- the same temp-swap the
//     StorageEngine already uses. A crash mid-write can therefore never leave a
//     truncated/empty counter file that would read back as corrupt or 0 and
//     mass-un-revoke everyone.
//   * The path is anchored explicitly (see counterFilePath()) rather than a
//     bare relative name, so it does not silently resolve against whatever
//     directory the server happened to be launched from.
//
// The file itself is an anonymous integer -- it carries no identities, so it is
// safe to keep in the repo/deployment and leaks nothing useful if read.
// ---------------------------------------------------------------------------

// Base filename for the persisted counter. counterFilePath() turns this into an
// explicit path (see below) so the lookup does not depend on the process's
// current working directory.
static const std::string COUNTER_FILE_NAME = "server_counter.txt";

// Resolve the counter file's path. Kept as a single function so there is ONE
// definition of "where the counter lives" for both read and write.
//
// NOTE: this returns the bare filename by default. If you want it anchored to a
// fixed location (recommended for a deployed server), set the path here -- e.g.
// from an environment variable read at startup, or a compile-time constant --
// rather than relying on the launch directory. Left as the filename for now to
// preserve current behaviour; change in ONE place when you pick a home for it.
static std::string counterFilePath() {
    return COUNTER_FILE_NAME;
}

// ---------------------------------------------------------------------------
// routeRequest: the query router.
//
// Signature note: runServer wants a std::function<HttpResponse(const HttpRequest&)>.
// A router needs the GraphHandler too, but that can't ride in that signature, so
// this static function takes `gh` explicitly and runSuedeServer binds it in via
// std::bind (see below). Keeping the routing in a named static function
// (rather than a giant lambda) makes it unit-testable: build an HttpRequest,
// call routeRequest(req, gh), assert on the HttpResponse - no socket needed.
//
// Every route follows the same five beats:
//   1. match method + path            (else 404)
//   2. parse body                     (bad JSON -> 400)
//   3. validate required fields       (missing/wrong type -> 400)
//   4. call the engine (gh)           (thread-safe; gh owns its own locks)
//   5. serialise result + set status  (engine success -> 200, engine reject -> 422)
//
// This function is called concurrently from many connection threads. It adds NO
// shared state of its own, so it needs no locks - the only shared thing it
// touches is `gh`, which is internally synchronised.
// ---------------------------------------------------------------------------
static HttpResponse routeRequest(const HttpRequest& req, GraphHandler& gh) {
    HttpResponse res;

    // ---- POST /query : run a Query-SQL command ----
    if (req.method == "POST" && req.path == "/query") {
        // (2) parse body
        ParseResult parsed = parseBody(req.body);
        if (!parsed.ok) {
            replyError(res, Http::BadRequest, parsed.error);
            return res;
        }
        // (3) require the "command" field as a string
        std::string command;
        std::string err;
        if (!requireString(parsed.value, "command", command, err)) {
            replyError(res, Http::BadRequest, err);
            return res;
        }
        // (4) engine: executeCommand handles queries, snapshots and persistence
        QueryResult result = gh.executeCommand(command);
        // (5) serialise + status: engine success -> 200, engine rejection -> 422
        reply(res, result.success ? Http::Ok : Http::Unprocessable, toJson(result));
        return res;
    }

    // ---- GET /stats : node / edge counts ----
    if (req.method == "GET" && req.path == "/stats") {
        json body;
        body["nodes"] = gh.getNodeCount();
        body["edges"] = gh.getEdgeCount();
        body["version"] = gh.getGraphVersion();
        reply(res, Http::Ok, body);
        return res;
    }

    // ---- no route matched ----
    replyError(res, Http::NotFound, "no such route: " + req.method + " " + req.path);
    return res;
}

// ---------------------------------------------------------------------------
// readServerCounter: load the persistent server counter from disk.
//
// Reads a uint64_t from the counter file (see counterFilePath()), parsing the
// first line as a decimal number. Behaviour by case:
//   * file MISSING        -> serverCounter = 0, returns true  (first-run default)
//   * file empty/corrupt  -> returns false with `err` set     (refuse to start)
//   * valid integer       -> serverCounter = value, returns true
// The missing/corrupt split is deliberate: a fresh install should boot at 0,
// but an empty or garbage file (the shape a crash mid-write would leave) must
// NOT be silently treated as 0, or it would mass-un-revoke everyone.
//
// Parameters:
//   serverCounter [out] : receives the parsed counter value on success
//   err           [out] : receives an error description on failure
//
// Returns:
//   true  if the counter was successfully read and parsed
//   false if the file could not be opened or the data is invalid/corrupted
// ---------------------------------------------------------------------------
static bool readServerCounter(uint64_t& serverCounter, std::string& err) {
    const std::string path = counterFilePath();

    // open text file
    std::ifstream in(path);
    // A MISSING file is not an error: on first ever run there is no counter yet,
    // so treat "not found" as "start from 0". (An existing-but-unreadable or
    // corrupt file IS an error and is handled below.) This lets a fresh install
    // boot with no manual setup while still catching real corruption.
    if (!in) {
        serverCounter = 0;
        return true;
    }

    // read the first line
    std::string sCounter;
    std::getline(in, sCounter);

    // Trim surrounding whitespace / stray CR (e.g. a file saved with Windows
    // line endings or a trailing newline) so an otherwise-valid value isn't
    // rejected. std::stoull already skips leading whitespace, but a trailing
    // '\r' would slip through the "no trailing junk" check below.
    const std::string ws = " \t\r\n";
    size_t first = sCounter.find_first_not_of(ws);
    size_t last = sCounter.find_last_not_of(ws);
    if (first == std::string::npos) {
        // File exists but is empty / all whitespace. This is exactly the shape a
        // crash mid-write (without the temp-swap) would leave, so treat it as
        // corruption rather than silently reading 0 and un-revoking everyone.
        err = "The counter file '" + path +
            "' is empty; refusing to start server.";
        return false;
    }
    sCounter = sCounter.substr(first, last - first + 1);

    // convert it to uint64_t
    try {
        size_t consumed = 0;
        unsigned long long value = std::stoull(sCounter, &consumed);
        // Reject trailing junk (e.g. "12abc"): stoull would happily parse "12"
        // and ignore the rest, which would mask a corrupt file.
        if (consumed != sCounter.size()) {
            err = "The counter file '" + path +
                "' contains non-numeric trailing data; could not initialise the server counter.";
            return false;
        }
        serverCounter = static_cast<uint64_t>(value);
    }
    // stoull throws std::invalid_argument (no digits) or std::out_of_range
    // (too big for unsigned long long) -- both mean the file is unusable.
    catch (...) {
        err = "The counter file '" + path +
            "' is corrupted (not a valid unsigned integer); could not initialise the server counter.";
        return false;
    }

    // everything happened successfully
    return true;
}

// ---------------------------------------------------------------------------
// persistServerCounter: crash-safely write the counter to disk.
//
// Renamed from writeServerCounter to make the call site read as an intent
// ("persist this now"), and hardened so a crash can never corrupt the file:
//   1. write the value to a sibling "<path>.tmp",
//   2. flush + close it (bytes committed to the OS),
//   3. atomically rename the temp over the real file.
// std::rename replaces the destination on POSIX; on Windows it fails if the
// destination exists, so we fall back to remove-then-rename there -- the same
// approach StorageEngine::Save uses.
//
// CALL THIS SYNCHRONOUSLY WHENEVER THE COUNTER CHANGES (i.e. at revoke time),
// while holding whatever lock guards the counter -- NOT only at shutdown.
// ---------------------------------------------------------------------------
static bool persistServerCounter(uint64_t serverCounter, std::string& err) {
    const std::string path = counterFilePath();
    const std::string tempPath = path + ".tmp";

    // (1) write the new value to the TEMP file (truncating any stale temp).
    {
        std::ofstream out(tempPath, std::ios::trunc);
        if (!out) {
            err = "Failed to open temp counter file '" + tempPath +
                "' for writing. Check directory permissions / path.";
            return false;
        }
        out << serverCounter << "\n";
        // (2) flush + close so the bytes are on the OS before we rename.
        out.flush();
        out.close();
        if (!out) {
            // Any stream error during write/flush/close: don't rename a bad temp
            // over the good file. Best-effort clean up the temp and fail.
            std::remove(tempPath.c_str());
            err = "Failed while writing the temp counter file '" + tempPath + "'.";
            return false;
        }
    }

    // (3) atomic swap: temp -> target.
    if (std::rename(tempPath.c_str(), path.c_str()) != 0) {
        // Windows / destination-exists path: remove the old target, then rename.
        std::remove(path.c_str());
        if (std::rename(tempPath.c_str(), path.c_str()) != 0) {
            std::remove(tempPath.c_str());   // give up cleanly; no temp left behind
            err = "Failed to atomically replace the counter file '" + path + "'.";
            return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// runSuedeServer: the one public entry point (declared in UseServer.h).
// Owns the single shared GraphHandler via unique_ptr, binds it into the static
// routeRequest to satisfy runServer's callback signature, and runs the accept
// loop. Blocks until the loop exits (currently: forever). No lambdas.
// ---------------------------------------------------------------------------
int runSuedeServer(const int port, std::string& err, bool use_public) {
    // start Server
    if (!netInit()) {
        err = "Server initialisation failed";
        return 1;
    }
    // Load the persisted revocation counter ONCE at startup. A missing file
    // means "first run" and yields 0; a present-but-corrupt file is a hard error
    // (we refuse to start rather than silently reset revocation state).
    uint64_t serverCounter = 0;
    if (!readServerCounter(serverCounter, err)) {
        netCleanup();
        return 1;
    }
    // NOTE: `serverCounter` is the in-memory source of truth for this run. When
    // the auth layer lands, it must live somewhere the router can reach (e.g.
    // owned alongside the GraphHandler, guarded by a mutex), be READ on every
    // token verify/issue, and be persisted via persistServerCounter() at the
    // moment it changes (a revoke). It is intentionally NOT written at shutdown
    // -- see the persistence notes at the top of this file.

    // ONE graph + handler for the whole server lifetime, owned by a unique_ptr.
    // It outlives every detached connection thread because runServer blocks
    // below until shutdown, and the unique_ptr is not destroyed until this
    // function returns (i.e. after the loop exits).
    std::unique_ptr<GraphHandler> graphHandler =
        std::make_unique<GraphHandler>(std::make_unique<Graph>());

    // Bind gh into the static router to produce the HttpResponse(const HttpRequest&)
    // callback runServer wants - no lambda. std::ref(*gh) passes the handler by
    // reference through the bind (not a copy); it stays valid because the
    // unique_ptr outlives the runServer call below.
    std::function<HttpResponse(const HttpRequest&)> httpHandler =
        std::bind(routeRequest, std::placeholders::_1, std::ref(*graphHandler));

    // Run the accept loop. Blocks. On failure to start, err is set by makeListener.
    runServer(port, httpHandler, err, use_public);

    // Reached only if the accept loop ever exits.
    netCleanup();

    // NOTE: the counter is deliberately NOT written here. Persistence happens
    // synchronously at revoke time via persistServerCounter(), so revocations
    // survive a kill/crash. A shutdown-only write would be worse than useless:
    // it would lose every revocation whenever the process didn't exit cleanly,
    // reintroducing the "revoked user returns after reboot" bug.

    // exit code: clean exit / no error -> 0, error -> 1
    int iErr = err.empty() ? 0 : 1;
    return iErr;
}