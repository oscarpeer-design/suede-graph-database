#include "Graph.h"

    // Minimal Graph Construtcor
    Graph::Graph()
        : nextNodeID(NodeId(1)),
          nextEdgeID(EdgeId(1))
    {}

    // CreateNode
    NodeId Graph::CreateNode(std::string label, propertiesMap properties) {
        // increment nextNode and assign unique identifier
        NodeId id = nextNodeID++;
        // create new node
        Node node(id, std::move(label), std::move(properties));
        // add new node to labelToNodes directory
        labelToNodes[node.label].push_back(id);
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
        for (const  NodeId nodeId : possibleIds->second) {
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
        // set success
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
    for (EdgeId edgeId: edgeInsertionOrder) {
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