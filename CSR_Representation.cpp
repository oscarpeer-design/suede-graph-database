#include "CSR_Representation.h"

// constructor only takes in a Graph. Registering with the graph's MVCC registry
// captures the current version as this snapshot's point in time and guarantees
// the graph retains history visible at that version until this object is
// destroyed.
CSR_Representation::CSR_Representation(Graph& g)
	: graph(g), owner_(&g)
{
	snapshotVersion_ = owner_->CaptureSnapshot();
}

// Destructor: release the snapshot registration so retained history can be
// garbage-collected once no other snapshot needs it.
CSR_Representation::~CSR_Representation()
{
	if (owner_)
		owner_->ReleaseSnapshot(snapshotVersion_);
}

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