#pragma once

// Vendored single-header JSON library (nlohmann). If your local filename differs,
// adjust this include only - nothing else references the library directly.
#include "../Server/nlohmann_json.hpp"
#include "../Server/http_simple.h"
#include "../Queries and Graph Handlers/Query.h"
#include "../Graph and Searchers/Graph.h"

#include <string>

// json alias
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Section A - inbound parse (never throws)
// ---------------------------------------------------------------------------
struct ParseResult {
    bool ok;            // false -> the bytes weren't valid JSON
    json value;         // only meaningful when ok == true
    std::string error;  // human-readable "why" when ok == false
};

// Parse a request body. NEVER throws - malformed input is a return value.
inline ParseResult parseBody(const std::string& raw) {
    ParseResult result;

    // refuse absurdly large bodies before even attempting to parse
    if (raw.size() > MAX_REQUEST) {
        result.ok = false;
        result.error = "body too large";
        return result;
    }

    // allow_exceptions = false -> returns a discarded value instead of throwing
    json parsed = json::parse(raw, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) {
        result.ok = false;
        result.error = "malformed JSON";
        return result;
    }

    result.ok = true;
    result.value = std::move(parsed);
    return result;
}

// ---------------------------------------------------------------------------
// Section B - extract & validate typed fields
// Return true and fill `out` only if `key` exists AND is the right type;
// otherwise return false with `err` set.
// ---------------------------------------------------------------------------
inline bool requireString(const json& body, const std::string& key, std::string& out, std::string& err) {
    if (!body.is_object() || !body.contains(key)) {
        err = "missing field '" + key + "'";
        return false;
    }
    if (!body[key].is_string()) {
        err = "field '" + key + "' must be a string";
        return false;
    }
    out = body[key].get<std::string>();
    return true;
}

inline bool requireInt(const json& body, const std::string& key, long& out, std::string& err) {
    if (!body.is_object() || !body.contains(key)) {
        err = "missing field '" + key + "'";
        return false;
    }
    if (!body[key].is_number_integer()) {
        err = "field '" + key + "' must be an integer";
        return false;
    }
    out = body[key].get<long>();
    return true;
}

// ---------------------------------------------------------------------------
// Section C - outbound serialisation (QueryResult -> JSON)
// StrongId (NodeId/EdgeId) exposes value() -> uint64_t; properties is an
// unordered_map<string,string> which nlohmann serialises to a JSON object
// (with all escaping handled for us).
// ---------------------------------------------------------------------------
inline json nodeToJson(const Node& node) {
    json json_node;
    json_node["id"] = node.id.value();
    json_node["label"] = node.label;
    json_node["properties"] = node.properties;   // map<string,string> -> JSON object
    return json_node;
}

inline json edgeToJson(const Edge& edge) {
    json json_edge;
    json_edge["id"] = edge.id.value();
    json_edge["from"] = edge.from.value();
    json_edge["to"] = edge.to.value();
    json_edge["label"] = edge.label;
    return json_edge;
}

inline json toJson(const QueryResult& result) {
    json out_json;
    out_json["success"] = result.success;
    out_json["message"] = result.message;

    out_json["nodes"] = json::array();
    for (const Node& n : result.nodes)
        out_json["nodes"].push_back(nodeToJson(n));

    out_json["edges"] = json::array();
    for (const Edge& e : result.edges)
        out_json["edges"].push_back(edgeToJson(e));

    // traversalResult is a list of node ids (used by MATCH / KHOP)
    out_json["traversal"] = json::array();
    for (const NodeId& id : result.traversalResult)
        out_json["traversal"].push_back(id.value());

    return out_json;
}

// ---------------------------------------------------------------------------
// Section D - HTTP reply helpers.
// These operate on OUR HttpResponse (from http_simple.h), not httplib.
// reply() sets status + JSON body together; replyError() wraps {"error": msg}.
// ---------------------------------------------------------------------------
inline void reply(HttpResponse& res, int status, const json& body) {
    res.status = status;
    res.contentType = "application/json";
    res.body = body.dump();
}

inline void replyError(HttpResponse& res, int status, const std::string& message) {
    json json_;
    json_["error"] = message;
    reply(res, status, json_);
}