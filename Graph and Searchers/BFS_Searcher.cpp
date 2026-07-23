#include "BFS_Searcher.h"

BFS_Searcher::BFS_Searcher(Graph& g, NodeId src)
    : graph(g), source(src)
{}

void BFS_Searcher::Reset() {
	// replace the visitQueue with an empty queue
	std::queue<NodeId> emptyQueue = {};
	visitQueue = emptyQueue;
	// clear records of visited ids and the order in which they were visited
	visited.clear();
	parent.clear();
    visitOrder.clear();
}

// run optimised BFS algorithm
void BFS_Searcher::Run_BFS(EdgeDirection direction) {
    // neighbouring nodes discovered during traversal
    std::vector<NodeId> neighbours;
    // visit the source node
    visited.insert(source);
    visitQueue.push(source);
    // continue until every reachable node has been visited
    while (!visitQueue.empty())
    {
        // get next node to visit
        NodeId current = visitQueue.front();
        visitQueue.pop();
        // track order
        visitOrder.push_back(current);
        // get neighbouring node ids using fast traversal system
        graph.GetNeighbourIdsForTraversal(current, neighbours, direction, warning);
        // terminate on graph error
        if (warning != operationSuccessful)
            return;
        // visit all neighbouring nodes
        for (const NodeId neighbour : neighbours)
        {
            // insert() performs lookup and insertion together
            if (visited.insert(neighbour).second)
            {
                // record shortest path tree
                parent.emplace(neighbour, current);
                // queue neighbour for traversal
                visitQueue.push(neighbour);
            }
        }
    }
    // traversal completed successfully
    warning = operationSuccessful;
}

// checks if a node has been visited
bool BFS_Searcher::HasVisited(NodeId nodeId) const {
    return visited.find(nodeId) != visited.end();
}

// gets visited node Ids in order
void BFS_Searcher::GetVisitedNodeIdOrder(std::vector<NodeId>& out)
{
    out = visitOrder;
}

// gets shortest path between two nodes
void BFS_Searcher::GetShortestPath(NodeId target, std::vector<NodeId>& path) {
    path.clear();
    // edge case
    if (source == NodeIdInvalid || target == NodeIdInvalid)
        return;
    // ensure BFS was run and target is reachable
    if (visited.find(target) == visited.end())
        return;
    // walk backwards from target using parent map
    NodeId current = target;
    while (current != NodeIdInvalid)
    {
        path.push_back(current);

        if (current == source)
            break;

        auto it = parent.find(current);
        if (it == parent.end())
            break;

        current = it->second;
    }
    // reverse to get source to target order
    std::reverse(path.begin(), path.end());
}

NodeId BFS_Searcher::GetParentOfVisited(NodeId node) const {
    // try and find a parent node
    auto it = parent.find(node);
    // return an invalid node if the parent doesn't exist
    if (it == parent.end())
        return NodeIdInvalid;
    return it->second;
}