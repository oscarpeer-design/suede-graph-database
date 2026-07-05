#pragma once
#include "graph.h"
#include "Types.h"

// This is a immutable representation of a Graph database using Compressed Sparse Rows
// It takes a graph and creates a fast representation that is designed for fast retrieval and quick lookups
class CSR_Representation {
	private:
		// this holds the non-zero data in the matrix. For this CSR representation, all valid nodes in the graph are nonzero
		std::vector<size_t> columns;
		// this stores the row offsets so we can extract rows using slices
		std::vector<size_t> row_offsets;
		// the graph we are trying to represent which is immutable and should be read only
		const Graph& graph;
		// maps NodeId values to CSR_ID
		std::unordered_map<NodeId, size_t> nodeToCSR;
		// maps CSR_ID values to NodeId
		std::vector<NodeId> csrToNode;
		// data structure that tracks neighouring nodes
		std::vector<std::vector<NodeId>> adjacency_cache;

		// Build Adjacency cache
		void BuildAdjacencyCache(std::vector<NodeId>& nodeOrder) {
			// track neighbours in adjacency cache
			adjacency_cache.resize(nodeOrder.size());
			int warning;
			// for each node, get neighbouring nodes and store them in adjacency cacne
			for (size_t i = 0; i < nodeOrder.size(); ++i)
			{
				NodeId node = nodeOrder[i];
				// get neighbours
				std::vector<NodeId> neighbours;
				graph.GetNeighbourIdsForTraversal(node, neighbours, BOTH, warning);
				// move neighbours into adjacency cache
				adjacency_cache[i] = std::move(neighbours);
			}
		}

		// Map CSR IDs to Node Ids and vice versa
		void MapCsrsAndNodes(std::vector<NodeId>& nodeOrder) {
			// reserve size ahead (give capacity and size)
			csrToNode.resize(nodeOrder.size());
			// for each node assign a csr ID
			size_t csrId = 0;
			for (size_t idx = 0; idx < nodeOrder.size(); idx++) {
				// get the nodeId
				NodeId nodeId = nodeOrder[idx];
				// map nodeId value to csrId
				nodeToCSR[nodeId] = csrId;
				// map csrID to nodeId
				csrToNode[csrId] = nodeId;
				// increment csrId
				csrId++;
			}
		}

		// Fill row_offsets based on the number of neighbours of each node
		void FillRowOffsets(std::vector<NodeId>& nodeOrder) {
			// reserve size ahead
			row_offsets.resize(nodeOrder.size() + 1);
			// initialise offsets
			size_t offset = 0;
			row_offsets[0] = 0;
			// using the adjacency cache we can determine and store the offsets of the neighbours of each node
			for (size_t i = 0; i < nodeOrder.size(); ++i)
			{
				offset += adjacency_cache[i].size();
				row_offsets[i + 1] = offset;
			}
		}

		// Fill columns
		void FillColumns(std::vector<NodeId>& nodeOrder) {
			// resize columns
			columns.resize(row_offsets.back());
			// track index within columns
			size_t idx = 0;
			for (size_t i = 0; i < nodeOrder.size(); ++i)
			{
				for (NodeId neighbour : adjacency_cache[i])
				{
					// for each index, emplace the index of the neighbours, with a check for exceptions
					auto it = nodeToCSR.find(neighbour);
					if (it != nodeToCSR.end())
						columns[idx] = it->second;
					idx++;
				}
			}
		}

	public:
		// constructor
		CSR_Representation(Graph& g);

		// load CSR
		void Load_CSR();

		// reset CSR
		void Reset_CSR();

		// Map graph to CSR (size_t)
		size_t MapGraphNodeToCSR(NodeId nodeId, int& warning) const;

		// Map CSR to graph (NodeId)
		NodeId MapCSR_ToGraphNode(size_t csr, int& warning) const;

		// Size of CSR Graph
		size_t Size() const;

		const std::vector<size_t>& GetRowOffsets() const;

		const std::vector<size_t>& GetColumns() const;

		const std::vector<NodeId>& GetCSRNodeMapping() const;
};