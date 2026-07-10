#include "Graph.h"

// Minimal Graph Construtcor
Graph::Graph()
    : nextNodeID(NodeId(1)),
    nextEdgeID(EdgeId(1))
{
}

// CreateNode
NodeId Graph::CreateNode(std::string label, propertiesMap properties) {
    // increment nextNode and assign unique identifier
    NodeId id = nextNodeID++;
    // create new node
    Node node(id, std::move(label), std::move(properties));
    // add new node to labelToNodes directory
    labelToNodes[node.label].push_back(id);
    // MVCC: stamp this creation with a fresh version and retain a history
    // record so point-in-time snapshots can observe it. Kept before the
    // live `nodes` map takes ownership of `node` via move.
    ++currentVersion_;
    mvccNodes_.emplace(id, NodeVersion{ node, currentVersion_, 0 });
    mvccNodeOrder_.push_back(id);
    // add new node to nodes map
    nodes.emplace(id, std::move(node));
    // track isnertion order
    nodeInsertionOrder.push_back(id);
    // return the id
    return id;
}

// CreateEdge
EdgeId Graph::CreateEdge(NodeId from, NodeId to, std::string label, int& warning) {
    // basic validation (important early)
    if (invalidEndpoints(to, from)) {
        warning = errEdgeDoesntExist;
        return EdgeIdInvalid;
    }
    // assign the id and increment the next id
    EdgeId id = nextEdgeID++;
    // add new edge to labelToEdges directory
    labelToEdges[label].push_back(id);
    // create the next edge
    Edge edge(id, from, to, std::move(label));
    // MVCC: stamp this creation with a fresh version and retain history so
    // snapshots can observe the edge at its point in time.
    ++currentVersion_;
    mvccEdges_.emplace(id, EdgeVersion{ edge, currentVersion_, 0 });
    // add to the edges map
    edges.emplace(id, std::move(edge));
    // add to insertion order
    edgeInsertionOrder.push_back(id);
    // set warning
    warning = operationSuccessful;
    // update adjacency lists
    adjacencyOut[from].push_back(id);
    adjacencyIn[to].push_back(id);
    neighbourOut[from].push_back(to);
    neighbourIn[to].push_back(from);
    // return the id
    return id;
}

// FindNode
void Graph::FindNodes(std::vector<Node>& foundNodes, std::string label, int& warning) {
    foundNodes.clear();
    // check if the node is documented
    if (!nodeLabelExists(label)) {
        warning = errNodeLabelUndocumented;
        return;
    }
    // get all the nodes
    auto possibleIds = labelToNodes.find(label);
    // this is a tradeoff in which we sacrifice performance to ensure data doesn't get corrupted
    for (const NodeId nodeId : possibleIds->second) {
        foundNodes.push_back(nodes.at(nodeId));
    }
    // set warning
    warning = operationSuccessful;
}

// FindEdgesByLabel
void Graph::FindEdgesByLabel(std::vector<Edge>& out, const std::string& label, int& warning) {
    // clear output
    out.clear();
    // check if label exists in edge index
    auto it = labelToEdges.find(label);
    if (it == labelToEdges.end()) {
        warning = errEdgeDoesntExist;
        return;
    }
    // temporary container for edges found via IDs
    std::vector<Edge> temp;
    // expand edge IDs into actual edges
    for (EdgeId edgeId : it->second) {
        temp.push_back(edges.at(edgeId));
    }
    // reuse GetEdges style behavior (consistent expansion step)
    out.insert(out.end(), temp.begin(), temp.end());
    // set success
    warning = operationSuccessful;
}

// FindEdgesByNodeId
void Graph::FindEdgesByNodeId(NodeId node, std::vector<Edge>& out, EdgeDirection dir, int& warning) {
    GetEdges(node, out, dir, warning);
}

// FindEdgesByNodeAndLabel
// This allows users to query all edges that have a particular relationship, that are either ingoing, outgoing or both
void Graph::FindEdgesByNodeAndLabel(NodeId node, const std::string& edgeLabel, std::vector<Edge>& out, EdgeDirection dir, int& warning) {
    // clear output
    out.clear();
    std::vector<Edge> temp;
    // get structurally valid edges first
    GetEdges(node, temp, dir, warning);
    // handle errors
    if (warning != operationSuccessful)
        return;
    // filter by label
    for (const Edge& edge : temp) {
        if (edge.label == edgeLabel) {
            out.push_back(edge);
        }
    }
    // set success
    warning = operationSuccessful;
}

// GetNeighboursById
// called on by GetNeighboursByLabel and maps Ids to Nodes
// used for slow, user-centric queries
void Graph::GetNeighboursById(NodeId node, std::vector<Node>& foundNeighbours, EdgeDirection dir, int& warning) {
    // clear neigbours vector
    foundNeighbours.clear();
    // track ids of neighbours found with edge direction
    std::vector<NodeId> neighbourIds;
    // get ids of all neighbours
    GetNeighbours(node, neighbourIds, dir, warning);
    // handle errors
    if (warning != operationSuccessful)
        return;
    // get all neighbours from their node ids
    for (NodeId nodeId : neighbourIds) {
        auto it = nodes.find(nodeId);
        if (it != nodes.end())
            foundNeighbours.push_back(it->second);
    }
    warning = operationSuccessful;
}

// DeleteEdge
void Graph::DeleteEdge(EdgeId id, int& warning)
{
    // check edge exists
    auto it = edges.find(id);
    if (it == edges.end()) {
        warning = errEdgeDoesntExist;
        return;
    }
    // get edge before removal
    const Edge& edge = it->second;
    // MVCC: tombstone the retained history record with a fresh version so
    // snapshots older than this deletion still observe the edge, while newer
    // ones (and the live graph) do not. The record itself is retained until
    // garbageCollect() determines no active snapshot needs it.
    ++currentVersion_;
    auto histIt = mvccEdges_.find(id);
    if (histIt != mvccEdges_.end() && histIt->second.deletedAtVersion == 0)
        histIt->second.deletedAtVersion = currentVersion_;
    // remove from edge label index
    EraseValue(labelToEdges[edge.label], id);
    // remove from order
    EraseValue(edgeInsertionOrder, id);
    // remove from adjacency structures (centralised logic)
    RemoveAdjacency(edge);
    // remove from main edge storage
    edges.erase(it);
    // set success
    warning = operationSuccessful;
}

// DeleteNode
void Graph::DeleteNode(NodeId id, int& warning) {
    // check node exists
    auto it = nodes.find(id);
    if (it == nodes.end()) {
        warning = errNodeDoesntExist;
        return;
    }
    // delete outgoing edges
    auto outIt = adjacencyOut.find(id);
    if (outIt != adjacencyOut.end()) {
        // copy because DeleteEdge modifies adjacencyOut
        std::vector<EdgeId> edgesToDelete = outIt->second;

        for (const EdgeId edgeId : edgesToDelete)
            DeleteEdge(edgeId, warning);
    }
    // delete incoming edges
    auto inIt = adjacencyIn.find(id);
    if (inIt != adjacencyIn.end()) {
        // copy because DeleteEdge modifies adjacencyIn
        std::vector<EdgeId> edgesToDelete = inIt->second;

        for (const EdgeId edgeId : edgesToDelete)
            DeleteEdge(edgeId, warning);
    }
    // MVCC: tombstone the retained node history with a fresh version. The
    // cascade-deleted edges above were already tombstoned by DeleteEdge; this
    // records the node's own deletion so snapshots taken before now still
    // see it. The record is retained until garbageCollect() reclaims it.
    ++currentVersion_;
    auto nHistIt = mvccNodes_.find(id);
    if (nHistIt != mvccNodes_.end() && nHistIt->second.deletedAtVersion == 0)
        nHistIt->second.deletedAtVersion = currentVersion_;
    // remove node from label index
    EraseValue(labelToNodes[it->second.label], id);
    // remove adjacency lists
    adjacencyOut.erase(id);
    adjacencyIn.erase(id);

    neighbourOut.erase(id);
    neighbourIn.erase(id);
    // remove node from storage
    nodes.erase(it);
    // remove from insertion order
    EraseValue(nodeInsertionOrder, id);
    // MVCC: deletions may have made older history reclaimable if no snapshot
    // needs it. Opportunistically collect now that the mutation is complete.
    garbageCollect();
    // set success
    warning = operationSuccessful;
}

// UpdateNodeProperties
void Graph::UpdateNodeProperties(NodeId id, const propertiesMap& updates, int& warning) {
    // locate the live node
    auto it = nodes.find(id);
    if (it == nodes.end()) {
        warning = errNodeDoesntExist;
        return;
    }
    // merge the updates into the live node's property map. Existing keys are
    // overwritten; new keys are added; unmentioned keys are left as-is.
    for (const auto& kv : updates)
        it->second.properties[kv.first] = kv.second;
    // MVCC: bump the version to mark that a mutation occurred, then refresh the
    // retained history record so it mirrors the live node. The history store keeps
    // a single record per id (keyed by NodeId, scanned via mvccNodeOrder_), so the
    // update is reflected in place: the record's payload is refreshed and its
    // createdAtVersion advanced to the new version. NOTE: because only one record
    // per node is retained, a SELECT ... SNAPSHOT taken *before* an update sees the
    // node as created at the update's version; property history is not versioned
    // independently (snapshots reflect create/delete presence, not per-property
    // revisions).
    ++currentVersion_;
    auto histIt = mvccNodes_.find(id);
    if (histIt != mvccNodes_.end()) {
        histIt->second.node = it->second;               // refresh payload
        histIt->second.createdAtVersion = currentVersion_;
        histIt->second.deletedAtVersion = 0;            // still live
    }
    else {
        // no retained record (e.g. already reclaimed): re-create one and ensure the
        // id participates in ordered snapshot scans.
        mvccNodes_[id] = NodeVersion{ it->second, currentVersion_, 0 };
        mvccNodeOrder_.push_back(id);
    }
    warning = operationSuccessful;
}

// UpdateEdgeLabel
void Graph::UpdateEdgeLabel(EdgeId id, const std::string& newLabel, int& warning) {
    // locate the live edge
    auto it = edges.find(id);
    if (it == edges.end()) {
        warning = errEdgeDoesntExist;
        return;
    }
    Edge& edge = it->second;
    // if the label is unchanged there is nothing to do; report success and avoid a
    // spurious version bump / history churn.
    if (edge.label == newLabel) {
        warning = operationSuccessful;
        return;
    }
    // maintain the label -> edge-id index: drop from the old bucket, add to the new.
    EraseValue(labelToEdges[edge.label], id);
    labelToEdges[newLabel].push_back(id);
    // apply the new label to the live edge
    edge.label = newLabel;
    // MVCC: bump the version and refresh the retained edge history in place so it
    // mirrors the live edge (single record per id, same caveat as
    // UpdateNodeProperties).
    ++currentVersion_;
    auto histIt = mvccEdges_.find(id);
    if (histIt != mvccEdges_.end()) {
        histIt->second.edge = edge;
        histIt->second.createdAtVersion = currentVersion_;
        histIt->second.deletedAtVersion = 0;
    }
    else {
        mvccEdges_[id] = EdgeVersion{ edge, currentVersion_, 0 };
    }
    warning = operationSuccessful;
}

// get neighbouring node ids for graph traversal
// used internally by BFS searchers
// this differs from the other functions because it is designed for traversal and not user access
void Graph::GetNeighbourIdsForTraversal(NodeId node, std::vector<NodeId>& neighbours, EdgeDirection dir, int& warning) const {
    // clear neighbours
    neighbours.clear();
    // insert ingoing and outgoing neighbours depending on direction
    // outgoing neighbours
    if (dir == OUTGOING || dir == BOTH)
    {
        auto it = neighbourOut.find(node);

        if (it != neighbourOut.end())
        {
            neighbours.insert(neighbours.end(),
                it->second.begin(),
                it->second.end());
        }
    }
    // ingoing neighbours
    if (dir == INCOMING || dir == BOTH)
    {
        auto it = neighbourIn.find(node);

        if (it != neighbourIn.end())
        {
            neighbours.insert(neighbours.end(),
                it->second.begin(),
                it->second.end());
        }
    }
    // set warning to success
    warning = operationSuccessful;
}

// gets all nodeIds in the graph and places them in a single vector
// used to create CSR representation of the graph
void Graph::GetNodeIdOrder(std::vector<NodeId>& out) const {
    // clear the graph
    out.clear();
    out = nodeInsertionOrder;
}

// get all edge Ids, irrespective of their order
void Graph::GetAllEdgeIds(std::vector<EdgeId>& out) const {
    // clear the output
    out.clear();
    // for each pair of EdgeId we insert it
    for (EdgeId edgeId : edgeInsertionOrder) {
        out.push_back(edgeId);
    }
}

// get node
bool Graph::GetNode(NodeId nodeId, Node& out) const {
    auto it = nodes.find(nodeId);
    // error: no node found
    if (it == nodes.end())
        return false;
    // node found
    out = it->second;
    return true;
}

bool Graph::GetEdge(EdgeId edgeId, Edge& out) const {
    auto it = edges.find(edgeId);
    // error no edge found
    if (it == edges.end())
        return false;
    // edge found
    out = it->second;
    return true;
}

// ------------------------------- MVCC ---------------------------------------

// Register interest in the current version. The returned value is the snapshot
// id; history visible at this version stays readable until ReleaseSnapshot().
uint64_t Graph::CaptureSnapshot() {
    activeSnapshots_.insert(currentVersion_);
    return currentVersion_;
}

// Release a previously captured snapshot and opportunistically reclaim history
// that no remaining snapshot can observe.
void Graph::ReleaseSnapshot(uint64_t version) {
    auto it = activeSnapshots_.find(version);
    if (it != activeSnapshots_.end())
        activeSnapshots_.erase(it);   // erase a single instance, not all equal keys
    garbageCollect();
}

// Reclaim tombstoned history that predates every active snapshot. When no
// snapshot is active, every tombstone is reclaimable. A live (deletedAtVersion
// == 0) record is never reclaimed here -- it is still part of the current graph.
void Graph::garbageCollect() {
    // The oldest version any active snapshot can still observe. With no active
    // snapshots, nothing needs to be preserved, so use the current version as
    // the cutoff (every tombstone strictly older than "now" is reclaimable).
    uint64_t cutoff = activeSnapshots_.empty()
        ? currentVersion_
        : *activeSnapshots_.begin();

    // A record tombstoned at version D is observable by a snapshot at version V
    // iff D > V (see visibility rule). It is therefore reclaimable once D is <=
    // the oldest active snapshot version, i.e. no active snapshot precedes the
    // deletion. Reclaim node history.
    for (auto it = mvccNodes_.begin(); it != mvccNodes_.end(); ) {
        const NodeVersion& nv = it->second;
        if (nv.deletedAtVersion != 0 && nv.deletedAtVersion <= cutoff) {
            NodeId id = it->first;
            it = mvccNodes_.erase(it);
            EraseValue(mvccNodeOrder_, id);   // keep the ordered scan list consistent
        }
        else {
            ++it;
        }
    }

    // Reclaim edge history under the same rule.
    for (auto it = mvccEdges_.begin(); it != mvccEdges_.end(); ) {
        const EdgeVersion& ev = it->second;
        if (ev.deletedAtVersion != 0 && ev.deletedAtVersion <= cutoff) {
            it = mvccEdges_.erase(it);
        }
        else {
            ++it;
        }
    }
}

// Collect nodes visible to the snapshot at `snapshotVersion`, in insertion
// order. Visible iff created at/before the version and (still live or deleted
// strictly after the version).
void Graph::GetNodesAtVersion(std::vector<Node>& out, uint64_t snapshotVersion) const {
    out.clear();
    for (NodeId id : mvccNodeOrder_) {
        auto it = mvccNodes_.find(id);
        if (it == mvccNodes_.end())
            continue;                                  // reclaimed; not visible
        const NodeVersion& nv = it->second;
        const bool createdInView = nv.createdAtVersion <= snapshotVersion;
        const bool notYetDeleted = (nv.deletedAtVersion == 0) ||
            (nv.deletedAtVersion > snapshotVersion);
        if (createdInView && notYetDeleted)
            out.push_back(nv.node);
    }
}

// Fetch one node's payload at `snapshotVersion` via an O(1) history lookup,
// applying the same visibility rule as GetNodesAtVersion. Unlike GetNode (which
// reads the live map), this observes tombstones: a node deleted only after the
// version is still visible; one deleted at or before it is not.
bool Graph::GetNodeAtVersion(NodeId id, uint64_t snapshotVersion, Node& out) const {
    auto it = mvccNodes_.find(id);
    if (it == mvccNodes_.end())
        return false;                                  // absent or reclaimed
    const NodeVersion& nv = it->second;
    const bool createdInView = nv.createdAtVersion <= snapshotVersion;
    const bool notYetDeleted = (nv.deletedAtVersion == 0) ||
        (nv.deletedAtVersion > snapshotVersion);
    if (!(createdInView && notYetDeleted))
        return false;                                  // not visible at this version
    out = nv.node;
    return true;
}

// Collect edges visible to the snapshot at `snapshotVersion` under the same
// visibility rule. Order is unspecified (mirrors GetAllEdgeIds semantics).
void Graph::GetEdgesAtVersion(std::vector<Edge>& out, uint64_t snapshotVersion) const {
    out.clear();
    for (const auto& kv : mvccEdges_) {
        const EdgeVersion& ev = kv.second;
        const bool createdInView = ev.createdAtVersion <= snapshotVersion;
        const bool notYetDeleted = (ev.deletedAtVersion == 0) ||
            (ev.deletedAtVersion > snapshotVersion);
        if (createdInView && notYetDeleted)
            out.push_back(ev.edge);
    }
}