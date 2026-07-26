// UseServer.cpp - wires http_simple + Json + GraphHandler together.
// This is the ONLY translation unit that sees all three layers.

#include "UseServer.h"
#include "../Server/http_simple.h"
#include "../Server/Json.h"
#include "../Queries and Graph Handlers/GraphHandler.h"
#include "../Graph and Searchers/Graph.h"

#include <memory>
#include <functional>   // std::bind, std::ref, std::placeholders

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
    // start Winsock
    if (!netInit()) {
        err = "Winsock initialisation failed";
        return 1;
    }

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
    // exit code: clean exit / no error -> 0, error -> 1
    int iErr = err.empty() ? 0 : 1;
    return iErr;
}