#include "CSR_Representation.h"

// constructor only takes in a Graph
CSR_Representation::CSR_Representation(Graph& g) : graph(g) { }

// Load_CSR
void CSR_Representation::Load_CSR() {
	// get all the nodes created, in their insertion order
	std::vector<NodeId> nodeOrder;
	graph.GetNodeIdOrder(nodeOrder);
	// Build adjacency cache
	BuildAdjacencyCache(nodeOrder);
	// map CSR IDs to Nodes and vice versa
	MapCsrsAndNodes(nodeOrder);
	// get row offsets
	FillRowOffsets(nodeOrder);
	// fill columns
	FillColumns(nodeOrder);
}

// Reset_CSR
void CSR_Representation::Reset_CSR() {
	// reset all internal data structures
	columns.clear();
	row_offsets.clear();
	nodeToCSR.clear();
	csrToNode.clear();
	adjacency_cache.clear();
}

// convert graph node to CSR_ID
size_t CSR_Representation::MapGraphNodeToCSR(NodeId nodeId, int& warning) const
{
	auto it = nodeToCSR.find(nodeId);
	if (it == nodeToCSR.end())
	{
		warning = errCsrNotMappedForThisNode;
		return 0;
	}
	warning = operationSuccessful;
	return it->second;
}

// convert CSR_ID back to graph node
NodeId CSR_Representation::MapCSR_ToGraphNode(size_t csr, int& warning) const
{
	// bounds check first (critical for safety)
	if (csr >= csrToNode.size())
	{
		warning = errNodeNotMappedForThisCsr;
		return NodeIdInvalid;
	}

	// direct lookup (no exceptions, no UB)
	warning = operationSuccessful;
	return csrToNode[csr];
}

// Get size of CSR Representation
size_t CSR_Representation::Size() const
{
	return csrToNode.size();
}

// Get Row Offsets
const std::vector<size_t>& CSR_Representation::GetRowOffsets() const
{
	return row_offsets;
}

// Get Columns
const std::vector<size_t>& CSR_Representation::GetColumns() const
{
	return columns;
}

// Get Node Mapping
const std::vector<NodeId>& CSR_Representation::GetCSRNodeMapping() const
{
	return csrToNode;
}

