#pragma once
#include <string>

// Launch the Suede HTTP server: construct a GraphHandler, wire up the routes,
// run the accept loop. Blocks until the server stops. Returns false if it could
// not start (e.g. port in use); reason written to `err`.
int runSuedeServer(const int port, std::string& err, bool use_public = false);