#pragma once

#include <queue>
#include <unordered_set>

#include "Graph.h"

// Searches the graph using BFS an performs operations with the result
// This is useful for graph traversal and extensive queries
class BFS_Searcher {
	private:
		// data structures used when performing BFS
		std::queue<NodeId> visitQueue; // tracks current node being visited
		std::unordered_set<NodeId> visited; // tracks which nodes we have previously visited
		std::unordered_map<NodeId, NodeId> parent; // tracks order of traversal
		// order of visitation
		std::vector<NodeId> visitOrder;
		// warning for unsuccessful operations
		int warning = 0;
		
		// graph we are using (which should be read only modified)
		const Graph& graph;

		// source node
		NodeId source = NodeIdInvalid;
	
	public:
		BFS_Searcher(Graph& g, NodeId src);

		// resets BFS_Searcher inbetween queries
		void Reset();

		// runs an optimised BFS algorithm
		void Run_BFS(EdgeDirection direction);

		// gets visited node Ids in order
		void GetVisitedNodeIdOrder(std::vector<NodeId>& visitOrder);

		// gets sortest path between two nodes
		void GetShortestPath(NodeId target, std::vector<NodeId>& path);

		// checks if a node has been visited
		bool HasVisited(NodeId node) const;

		// get parent of visited node
		NodeId GetParentOfVisited(NodeId node) const;
};