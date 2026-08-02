// UseServer.cpp - wires http_simple + Json + GraphHandler together.
// This is the ONLY translation unit that sees all three layers.

#include "UseServer.h"
#include "http_simple.h"
#include "Json.h"
#include "Auth.h"
#include "../../Queries and Graph Handlers/GraphHandler.h"
#include "../../Graph and Searchers/Graph.h"

#include <string>

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
    AuthState authState;
    if (!authState.readAuthState(err)) {
        netCleanup();
        return 1;
    }
    // `authState` is the in-memory source of truth for the revocation generation
    // this run. It is loaded once above. STILL TO DO when the auth layer lands:
    //   * pass `authState` by reference into routeRequest (alongside `gh`, via the
    //     std::bind below) so token verification can READ the generation on every
    //     request -- authState.getCounter().
    //   * choose a revoke TRIGGER that calls authState.revokeAll() (e.g. a signal
    //     handler that trips a flag the accept loop checks, mirroring the shutdown
    //     handler). revokeAll() already increments + persists atomically.
    // The shutdown flush below is a SUPPLEMENT to revoke-time persistence, not a
    // replacement -- see the persistence notes at the top of this file.

    // Install SIGINT/SIGTERM handlers so Ctrl+C or `kill` stops the accept loop
    // gracefully (runServer returns) instead of hard-killing the process. This is
    // what makes the shutdown counter-flush below actually reachable.
    installShutdownSignalHandlers();

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

    // Reached when the accept loop exits gracefully -- i.e. requestServerShutdown()
    // was called (SIGINT/SIGTERM via the handlers installed above, or a test
    // harness calling it directly). A hard kill (SIGKILL) or crash still skips
    // this tail, which is exactly why revoke-time persistence -- not this flush --
    // is the durability guarantee.
    netCleanup();

    // Belt-and-suspenders flush of the counter on CLEAN shutdown.
    //
    // IMPORTANT: this is a SUPPLEMENT, not the durability mechanism. The counter
    // MUST already have been persisted synchronously at the moment it changed
    // (inside the revoke path, via persistServerCounter) -- because the accept
    // loop can be killed or crash and never reach this line. If this were the
    // ONLY write, every revocation from this run would be lost on a non-clean
    // exit, reintroducing the "revoked user returns after reboot" bug. Given
    // revoke-time persistence, this final write is normally a no-op re-write of
    // the already-saved value; it just guarantees the on-disk file matches the
    // in-memory counter after an orderly stop. A failure here does not lose data
    // (the revoke-time writes already happened), so we record it in `err` but do
    // not treat it as fatal beyond the exit code.
    // NOTE: this is persistCurrent(), NOT revokeAll(). The flush must re-write the
    // counter unchanged; calling revokeAll() here would increment the generation
    // on every clean shutdown and silently revoke everyone each time the server
    // stops.
    std::string flushErr;
    if (!authState.persistCurrent(flushErr)) {
        // A failed flush loses no data (revoke-time writes already happened), so
        // only surface it if nothing more relevant already failed.
        if (err.empty())
            err = "Warning: final counter flush on shutdown failed: " + flushErr;
    }

    // exit code: clean exit / no error -> 0, error -> 1
    int iErr = err.empty() ? 0 : 1;
    return iErr;
}