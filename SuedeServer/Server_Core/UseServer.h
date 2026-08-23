#pragma once

#include "http_simple.h"
#include "Json.h"
#include "Auth.h"
#include "../../Queries and Graph Handlers/GraphHandler.h"
#include "../../Graph and Searchers/Graph.h"

#include <string>
#include <ctime>        // std::time for token-expiry checks

// Launch the Suede HTTP server: construct a GraphHandler, wire up the routes,
// run the accept loop. Blocks until the server stops. Returns false if it could
// not start (e.g. port in use); reason written to `err`.
int runSuedeServer(const uint64_t port, std::string& err, bool use_public = false);