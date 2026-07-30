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

// name of the file that stores server counters
static const std::string file_name = "server_counter.txt";

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
// Attempts to read a uint64_t value from the file specified by `file_name`
// (server_counter.txt). Parses the first line as a decimal number using
// std::stoull. If the file does not exist, cannot be opened, or contains
// invalid data, returns false and writes a descriptive error message to `err`.
// Otherwise, stores the parsed value in `serverCounter` and returns true.
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
    // open text file
    std::ifstream in(file_name);
    // check we opened it
    if (!in) {
        err = "Failed to initialise server counter. Check if server_counter.txt exists or if it has been moved elsewhere.";
        return false;
    }
    // read the first line
    std::string sCounter = "";
    std::getline(in, sCounter);
    // convert it to uint64_t
    try {
        // read the value
        serverCounter = std::stoull(sCounter);
        // close the file
        in.close();
    }
    // if it's invalid return false
    catch (...) {
        err = "The file server_counter.txt is corrupted; could not initialise the server counter.";
        return false;
    }
    // everything happened successfully
    return true;
}

// ---------------------------------------------------------------------------
// writeServerCounter: persist the server counter to disk.
//
// Writes a uint64_t value to the file specified by `file_name` (server_counter.txt),
// truncating any existing content. The counter is converted to a decimal string
// and written as-is with no formatting. If the file cannot be opened or created,
// or if an exception occurs during writing, returns false and sets an error
// message in `err`. Otherwise, flushes and closes the file, then returns true.
//
// Parameters:
//   serverCounter [in] : the counter value to write to disk
//   err          [out] : receives an error description on failure
//
// Returns:
//   true  if the counter was successfully written and the file closed
//   false if the file could not be opened/created or writing failed
// ---------------------------------------------------------------------------
static bool writeServerCounter(uint64_t serverCounter, std::string& err) {
    // open text file
    std::ofstream out(file_name);
    // check we opened it
    if (!out) {
        err = "Failed to overwrite written server counter. Check if server_counter.txt exists or if it has been moved elsewhere.";
        return false;
    }
    // overwrite the existing server counter with the new server counter
    try {
        // write the value
        out << std::to_string(serverCounter);
        // close the file
        out.close();
    }
    catch (...) {
        err = "Something went wrong; could not overwrite the existing server counter.";
        return false;
    }
    // everything happened successfully
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
    uint64_t serverCounter = 0;
    // read server counter from text file
    bool readCounter = readServerCounter(serverCounter, err);
    if (!readCounter) 
        return 1;

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

    // write the server counter back to the file
    bool writeCounter = writeServerCounter(serverCounter, err);
    if (!writeCounter)
        return 1;

    // exit code: clean exit / no error -> 0, error -> 1
    int iErr = err.empty() ? 0 : 1;
    return iErr;
}