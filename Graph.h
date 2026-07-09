#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <set>
#include <cstdint>

#include "ErrorCodes.h"
#include "Types.h"

// simplified unordered_map definitions
using propertiesMap = std::unordered_map<std::string, std::string>;
using edgeAdjacencyMap = std::unordered_map<NodeId, std::vector<EdgeId>>;
using nodeAdjacencyMap = std::unordered_map<NodeId, std::vector<NodeId>>;

struct Node {
    NodeId id;
    std::string label;
    propertiesMap properties;

    // default constructor
    Node()
    {
        id = NodeIdInvalid;
        label = "";
        properties = {};
    }

    // constructor for real Node
    Node(NodeId id_,
        std::string label_,
        propertiesMap props_)
        : id(id_),
        label(std::move(label_)),
        properties(std::move(props_))
    {
    }
};

struct Edge {
    EdgeId id;
    NodeId from;
    NodeId to;
    std::string label;

    // default constructor
    Edge()
    {
        id = EdgeIdInvalid;
        from = NodeIdInvalid;
        to = NodeIdInvalid;
        label = "";
    }

    // constructor for real Edge
    Edge(EdgeId id_,
        NodeId from_,
        NodeId to_,
        std::string label_)
        : id(id_),
        from(from_),
        to(to_),
        label(std::move(label_))
    {
    }
};

// Graph Database Data Structure
class Graph {
private:
    // next node and edge Id (sequential unique identifiers)
    NodeId nextNodeID = NodeId(1);
    EdgeId nextEdgeID = EdgeId(1);
    // NodeId to nodes
    std::unordered_map<NodeId, Node> nodes;
    // EdgeId to edges
    std::unordered_map<EdgeId, Edge> edges;
    // label -> all nodes with that label
    std::unordered_map<std::string, std::vector<NodeId>> labelToNodes;
    // label -> all edges
    std::unordered_map<std::string, std::vector<EdgeId>> labelToEdges;
    // adjacency lists for edge queries
    edgeAdjacencyMap adjacencyOut;
    edgeAdjacencyMap adjacencyIn;
    // adjacency lists for Node traversal 
    std::unordered_map<NodeId, std::vector<NodeId>> neighbourOut;
    std::unordered_map<NodeId, std::vector<NodeId>> neighbourIn;
    // vector for insertion order of nodes
    std::vector<NodeId> nodeInsertionOrder;
    // vector for insertion order of edges
    std::vector<EdgeId> edgeInsertionOrder;

    // ---------------------------------------------------------------------
    // MVCC (Multi-Version Concurrency Control) state
    //
    // Instead of physically copying the whole graph to serve a point-in-time
    // SELECT ... SNAPSHOT, every mutation stamps a monotonically increasing
    // version number, and deletions are recorded as tombstones rather than
    // discarding history. A "snapshot" is simply the version number captured
    // at the moment a CSR_Representation is built. A row is visible to a
    // snapshot at version V iff it was created at or before V and either has
    // not been deleted or was deleted strictly after V.
    //
    // The live maps (nodes/edges above) remain the fast path for LIVE queries
    // and are untouched by this machinery. The parallel history stores below
    // retain rows (including deleted ones) so that older snapshots can still
    // observe them until no active snapshot needs them, at which point
    // garbageCollect() reclaims the space.
    // ---------------------------------------------------------------------

    // History record for a node retained for snapshot visibility.
    struct NodeVersion {
        Node node;                    // full node payload as created
        uint64_t createdAtVersion;    // version at which this node became visible
        uint64_t deletedAtVersion;    // 0 == still live; else version of deletion
    };

    // History record for an edge retained for snapshot visibility.
    struct EdgeVersion {
        Edge edge;                    // full edge payload as created
        uint64_t createdAtVersion;    // version at which this edge became visible
        uint64_t deletedAtVersion;    // 0 == still live; else version of deletion
    };

    // Monotonic global version counter. Bumped before each mutation so that
    // the resulting create/delete stamp is unique and ordered.
    uint64_t currentVersion_ = 0;

    // Versions handed out by captureSnapshot() that have not yet been released.
    // garbageCollect() may only reclaim history older than the oldest of these.
    std::multiset<uint64_t> activeSnapshots_;

    // Retained node/edge history, keyed by id. Mirrors every create; records a
    // deletedAtVersion on delete. Physically erased only by garbageCollect().
    std::unordered_map<NodeId, NodeVersion> mvccNodes_;
    std::unordered_map<EdgeId, EdgeVersion> mvccEdges_;

    // Insertion order of node ids in the MVCC store (kept for deterministic,
    // insertion-ordered snapshot scans, mirroring nodeInsertionOrder).
    std::vector<NodeId> mvccNodeOrder_;

    // Reclaim history that predates every active snapshot. A node/edge tombstoned
    // at version D can be physically dropped once D is older than the oldest
    // active snapshot (or when there are no active snapshots at all), because no
    // reachable snapshot can observe its pre-deletion state any longer.
    void garbageCollect();

    // checks if node exists
    bool nodeLabelExists(std::string label) {
        // check if the label-node is documented
        auto possibleIds = labelToNodes.find(label);
        return possibleIds != labelToNodes.end();
    }

    // checks if edge is invalid
    bool invalidEndpoints(NodeId from, NodeId to) {
        // check either to or from do not exist
        return (nodes.find(from) == nodes.end()) || (nodes.find(to) == nodes.end());
    }

    // get outgoing edges
    void GetOutgoingEdges(NodeId node, std::vector<Edge>& out, int& warning) {
        // clear the vector
        out.clear();
        // try find the list of edges
        auto it = adjacencyOut.find(node);
        // no key simply means this node has zero outgoing edges -- not an error
        if (it != adjacencyOut.end()) {
            for (EdgeId edgeId : it->second) {
                out.push_back(edges.at(edgeId));
            }
        }
        warning = operationSuccessful;
    }

    // get ingoing edges
    void GetIngoingEdges(NodeId node, std::vector<Edge>& out, int& warning) {
        // clear the vector
        out.clear();
        // try find the list of edges
        auto it = adjacencyIn.find(node);
        // no key simply means this node has zero incoming edges -- not an error
        if (it != adjacencyIn.end()) {
            for (EdgeId edgeId : it->second) {
                out.push_back(edges.at(edgeId));
            }
        }
        warning = operationSuccessful;
    }

    // get either ingoing or outgoing or both
    // Note that this is only used for external queries
    void GetEdges(NodeId node, std::vector<Edge>& out, EdgeDirection dir, int& warning)
    {
        // clear output vector
        out.clear();
        // track Edges in vector
        std::vector<Edge> temp;
        // outgoing edges
        if (dir == OUTGOING || dir == BOTH) {
            GetOutgoingEdges(node, temp, warning);
            // insert outgoing edges into vector
            out.insert(out.end(), temp.begin(), temp.end());
        }

        // incoming edges
        if (dir == INCOMING || dir == BOTH) {
            GetIngoingEdges(node, temp, warning);
            // insert ingoing edges into vector
            out.insert(out.end(), temp.begin(), temp.end());
        }
        // set success state (GetOutgoingEdges/GetIngoingEdges never fail --
        // an empty adjacency list is a valid result, not an error)
        warning = operationSuccessful;
    }

    // GetEdgeIds
    // Note that this supports internal queries and getting neighours
    void GetEdgeIds(NodeId node, std::vector<EdgeId>& out, EdgeDirection dir, int& warning) {
        // clear output
        out.clear();
        // temporary storage
        std::vector<EdgeId> temp;
        // outgoing edges
        if (dir == OUTGOING || dir == BOTH) {
            auto it = adjacencyOut.find(node);
            // no key simply means zero outgoing edges -- not an error
            if (it != adjacencyOut.end()) {
                temp.insert(temp.end(), it->second.begin(), it->second.end());
            }
        }
        // incoming edges
        if (dir == INCOMING || dir == BOTH) {
            auto it = adjacencyIn.find(node);
            // no key simply means zero incoming edges -- not an error
            if (it != adjacencyIn.end()) {
                temp.insert(temp.end(), it->second.begin(), it->second.end());
            }
        }
        // use move constructor to move into out vector
        out = std::move(temp);
        // set warning
        warning = operationSuccessful;
    }

    // GetNeighbours
    // Used to support queries to determine the neighbours of a Node
    void GetNeighbours(NodeId node, std::vector<NodeId>& out, EdgeDirection dir, int& warning) {
        // clear output
        out.clear();
        // track edgeIds
        std::vector<EdgeId> edgeIds;
        // get edge ids using structural layer
        GetEdgeIds(node, edgeIds, dir, warning);
        // early exit on error
        if (warning != operationSuccessful)
            return;
        // convert edges to neighbor nodes
        for (EdgeId edgeId : edgeIds)
        {
            const Edge& edge = edges.at(edgeId);
            // the neighbour is whichever endpoint isn't `node` itself --
            // using the query direction here (rather than the edge's actual
            // direction) would incorrectly push both endpoints for BOTH,
            // including `node` itself as its own neighbour.
            if (edge.from == node)
                out.push_back(edge.to);
            else if (edge.to == node)
                out.push_back(edge.from);
        }
        // set success
        warning = operationSuccessful;
    }

    // EraseValue
    // used to erase values from vectors
    template<typename T>
    void EraseValue(std::vector<T>& vec, const T& value) {
        vec.erase(std::remove(vec.begin(),
            vec.end(),
            value),
            vec.end());
    }

    // RemoveAdjacency
    // used internally when deleting edges to keep graph indices consistent
    void RemoveAdjacency(const Edge& edge) {
        // remove edge from outgoing adjacency list
        EraseValue(adjacencyOut[edge.from], edge.id);

        // remove edge from incoming adjacency list
        EraseValue(adjacencyIn[edge.to], edge.id);

        // remove neighbour relationship (node-to-node traversal graph)
        EraseValue(neighbourOut[edge.from], edge.to);
        EraseValue(neighbourIn[edge.to], edge.from);
    }

public:

    Graph();

    NodeId CreateNode(std::string label, propertiesMap properties);

    EdgeId CreateEdge(NodeId from, NodeId to, std::string label, int& warning);

    void FindNodes(std::vector<Node>& foundNodes, std::string label, int& warning);

    void FindEdgesByLabel(std::vector<Edge>& out, const std::string& label, int& warning);

    void FindEdgesByNodeId(NodeId node, std::vector<Edge>& out, EdgeDirection dir, int& warning);

    void FindEdgesByNodeAndLabel(NodeId node, const std::string& edgeLabel, std::vector<Edge>& out, EdgeDirection dir, int& warning);

    void GetNeighboursById(NodeId node, std::vector<Node>& foundNeighbours, EdgeDirection dir, int& warning);

    void DeleteEdge(EdgeId id, int& warning);

    void DeleteNode(NodeId id, int& warning);

    // UpdateNodeProperties
    // Merge the supplied key=value pairs into node `id`'s property map (existing
    // keys overwritten, new keys added, unmentioned keys left untouched). The
    // structural fields (id, label) are not affected. MVCC: bumps the version and
    // refreshes the retained history record so it mirrors the live node.
    // warning == operationSuccessful on success, errNodeDoesntExist if absent.
    void UpdateNodeProperties(NodeId id, const propertiesMap& updates, int& warning);

    // UpdateEdgeLabel
    // Change edge `id`'s label to `newLabel`, keeping the label index and MVCC
    // history consistent. Edge structure (from/to) is immutable, so only the
    // label may change. warning == operationSuccessful on success,
    // errEdgeDoesntExist if absent.
    void UpdateEdgeLabel(EdgeId id, const std::string& newLabel, int& warning);

    void GetNeighbourIdsForTraversal(NodeId node, std::vector<NodeId>& neighbours, EdgeDirection dir, int& warning) const;

    void GetNodeIdOrder(std::vector<NodeId>& out) const;

    void GetAllEdgeIds(std::vector<EdgeId>& out) const;

    bool GetNode(NodeId id, Node& out) const;

    bool GetEdge(EdgeId id, Edge& out) const;

    // ----------------------------- MVCC API -----------------------------

    // The graph's current logical version (number of mutations applied).
    uint64_t GetVersion() const { return currentVersion_; }

    // Register interest in the current version and return it as a snapshot
    // id. History visible at this version is guaranteed to remain readable
    // until the matching ReleaseSnapshot(). CSR_Representation calls this at
    // construction time.
    uint64_t CaptureSnapshot();

    // Release a snapshot previously obtained from CaptureSnapshot() and run
    // opportunistic garbage collection. Safe to call with a version that was
    // captured; releasing an unknown version is a no-op on the multiset.
    void ReleaseSnapshot(uint64_t version);

    // Number of snapshots currently held (exposed for testing/introspection).
    size_t ActiveSnapshotCount() const { return activeSnapshots_.size(); }

    // Total node history records currently retained, including tombstones
    // (exposed so tests can assert that garbage collection reclaims space).
    size_t MvccNodeHistorySize() const { return mvccNodes_.size(); }

    // Collect every node visible to the snapshot taken at `snapshotVersion`,
    // in insertion order. A node is visible iff it was created at or before
    // the version and not deleted at or before it. Used by SELECT ... SNAPSHOT.
    void GetNodesAtVersion(std::vector<Node>& out, uint64_t snapshotVersion) const;

    // Collect every edge visible to the snapshot taken at `snapshotVersion`.
    void GetEdgesAtVersion(std::vector<Edge>& out, uint64_t snapshotVersion) const;
};