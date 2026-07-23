#include "CSR_Searcher.h"

// Constructor
CSR_Searcher::CSR_Searcher(NodeId source, CSR_Representation& csrSnapshot)
    : source_(source)
    , csrRepresentation_(csrSnapshot)
{
    // Intentionally empty: members are default-initialized; buffers resized per query.
}

/// Run
/// Executes a graph traversal query over the CSR snapshot.
///
/// This function is the entry point for CSR-based graph queries. It prepares
/// traversal buffers, maps the source node to CSR index space, marks the source
/// as visited and dispatches to the appropriate traversal kernel.
///
/// Parameters:
/// - target: destination node (only used for shortest path queries, or
///           used as `k` when mode == KHOP)
/// - mode: selects traversal strategy
void CSR_Searcher::Run(NodeId target, CSR_Mode mode, size_t k)
{
    // clear any previous results
    result_.clear();

    // total number of CSR nodes in the snapshot
    const size_t nodeCount = csrRepresentation_.Size();

    // reset visited flags for this traversal
    visited_.assign(nodeCount, 0);

    // reset parent tracking (use self-parent as sentinel)
    parent_.assign(nodeCount, nodeCount);

    // clear frontier buffers
    frontier_.clear();
    next_frontier_.clear();

    // map source NodeId into CSR index space
    int mapWarning;
    const size_t startIndex = csrRepresentation_.MapGraphNodeToCSR(source_, mapWarning);

    // initialize BFS frontier with the source CSR index
    frontier_.push_back(startIndex);
    visited_[startIndex] = 1;
    parent_[startIndex] = startIndex; // sentinel parent

    // select traversal strategy
    switch (mode)
    {
    case CSR_Mode::SHORTEST_PATH:
        // run BFS until target is found and parent tree is built
        run_shortest_path(target);
        break;

    case CSR_Mode::REACHABLE:
        // explore entire connected component from source
        run_reachable();
        break;

    case CSR_Mode::KHOP:
        // run BFS limited to `k` hops (target parameter used as k)
        run_khop(k);
        break;
    }
}