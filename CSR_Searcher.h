#pragma once

#include "ErrorCodes.h"
#include "Types.h"
#include "CSR_Representation.h"
#include <vector>

/*
  CSR_Searcher
  - Performs CSR-backed graph traversals (shortest path, reachability, k-hop).
  - Uses CSR index space internally for speed.
*/
class CSR_Searcher {
private:
    // Frontier buffers: current level and next level (CSR indices)
    std::vector<size_t> frontier_;
    std::vector<size_t> next_frontier_;

    // Visited flags (uint8_t used for compactness and speed)
    std::vector<uint8_t> visited_;

    // Parent pointers in CSR index space (used for shortest path reconstruction)
    std::vector<size_t> parent_;

    // Optional distances per node (unused in current implementations but available)
    std::vector<int> distance_;

    // Result buffer: stores NodeIds converted from CSR indices after a query
    std::vector<NodeId> result_;

    // Source node (Graph NodeId provided by caller)
    NodeId source_;

    // Reference to the CSR snapshot used for traversal (internal CSR index mapping)
    const CSR_Representation& csrRepresentation_;

    // Reconstruct path from source_ to target (target is a CSR index).
    // Fills result_ with NodeIds in source->target order.
    void reconstruct_path(size_t target)
    {
        // clear any previous result
        result_.clear();

        // if target was never reached, nothing to do
        if (!visited_[target])
            return;

        // temporary buffer to collect CSR indices in reverse order (target -> source)
        std::vector<size_t> reversedPath;
        size_t current = target;

        // Walk up the parent chain until we reach the root sentinel
        while (true)
        {
            reversedPath.push_back(current);

            if (parent_[current] == current) // root reached
                break;

            current = parent_[current];
        }

        // convert CSR indices to Graph NodeIds
        const auto& csrToNode = csrRepresentation_.GetCSRNodeMapping();
        result_.reserve(reversedPath.size());

        // append nodes in source -> target order
        for (auto it = reversedPath.rbegin(); it != reversedPath.rend(); ++it)
        {
            result_.push_back(csrToNode[*it]);
        }
    }

    // BFS specialized for shortest path: stops when targetNode is found.
    // targetNode is a Graph NodeId (converted internally to CSR index).
    void run_shortest_path(NodeId targetNode)
    {
        int mapWarning;

        // Map the query target NodeId to CSR index (may set mapWarning)
        const size_t target = csrRepresentation_.MapGraphNodeToCSR(targetNode, mapWarning);

        bool found = false;

        // Cached CSR arrays to avoid repeated calls in tight loops
        const auto& rowOffsets = csrRepresentation_.GetRowOffsets();
        const auto& columns = csrRepresentation_.GetColumns();

        // Standard level-synchronous BFS using frontier_ and next_frontier_
        while (!frontier_.empty() && !found)
        {
            next_frontier_.clear();

            // iterate over all CSR indices in the current frontier
            for (size_t current_csr_index : frontier_)
            {
                // neighbors of current node are at columns[rowOffsets[current] .. rowOffsets[current+1])
                const size_t nbr_begin = rowOffsets[current_csr_index];
                const size_t nbr_end = rowOffsets[current_csr_index + 1];

                // scan neighbors
                for (size_t edge_idx = nbr_begin; edge_idx < nbr_end; ++edge_idx)
                {
                    const size_t neighbor_csr_index = columns[edge_idx];

                    // skip already visited neighbors
                    if (visited_[neighbor_csr_index])
                        continue;

                    // mark visited and record parent in BFS tree
                    visited_[neighbor_csr_index] = 1;
                    parent_[neighbor_csr_index] = current_csr_index;

                    // if we reached the target CSR index, stop BFS early
                    if (neighbor_csr_index == target)
                    {
                        found = true;
                        // parent already set above; break to unwind loops
                        break;
                    }

                    // otherwise add neighbor to next frontier for the next BFS layer
                    next_frontier_.push_back(neighbor_csr_index);
                }

                if (found)
                    break;
            }

            // move to next layer
            frontier_.swap(next_frontier_);
        }

        // if target was reached, reconstruct path into result_
        if (visited_[target])
            reconstruct_path(target);
    }

    // BFS that explores the entire reachable component from source_
    // Results are stored in result_ as Graph NodeIds.
    void run_reachable()
    {
        // Cached CSR arrays to avoid repeated calls in tight loops
        const auto& rowOffsets = csrRepresentation_.GetRowOffsets();
        const auto& columns = csrRepresentation_.GetColumns();
        const auto& csrToNode = csrRepresentation_.GetCSRNodeMapping();

        // Level-synchronous BFS until no more nodes to explore
        while (!frontier_.empty())
        {
            next_frontier_.clear();
            // drill down each level
            for (size_t current_csr_index : frontier_)
            {
                const size_t nbr_begin = rowOffsets[current_csr_index];
                const size_t nbr_end = rowOffsets[current_csr_index + 1];

                for (size_t edge_idx = nbr_begin; edge_idx < nbr_end; ++edge_idx)
                {
                    const size_t neighbor_csr_index = columns[edge_idx];

                    // skip already visited neighbors
                    if (visited_[neighbor_csr_index])
                        continue;

                    visited_[neighbor_csr_index] = 1;
                    next_frontier_.push_back(neighbor_csr_index);
                }
            }

            // advance to the next BFS layer
            frontier_.swap(next_frontier_);
        }

        // convert visited CSR indices into Graph NodeIds
        for (size_t csr_index = 0; csr_index < visited_.size(); ++csr_index)
        {
            if (visited_[csr_index])
                result_.push_back(csrToNode[csr_index]);
        }
    }

    // K-hop search: finds nodes reachable within k hops from source_.
    // The parameter k is the maximum number of BFS layers to traverse.
    void run_khop(size_t k)
    {
        // Cached CSR arrays to avoid repeated calls in tight loops
        const auto& rowOffsets = csrRepresentation_.GetRowOffsets();
        const auto& columns = csrRepresentation_.GetColumns();
        const auto& csrToNode = csrRepresentation_.GetCSRNodeMapping();

        size_t depth = 0;

        // traverse at most k layers (each layer = one hop)
        while (!frontier_.empty() && depth < k)
        {
            next_frontier_.clear();
            // look through each neighbour of each neighbour
            for (size_t current_csr_index : frontier_)
            {
                const size_t nbr_begin = rowOffsets[current_csr_index];
                const size_t nbr_end = rowOffsets[current_csr_index + 1];

                for (size_t edge_idx = nbr_begin; edge_idx < nbr_end; ++edge_idx)
                {
                    const size_t neighbor_csr_index = columns[edge_idx];

                    if (visited_[neighbor_csr_index])
                        continue;

                    visited_[neighbor_csr_index] = 1;
                    next_frontier_.push_back(neighbor_csr_index);
                }
            }

            frontier_.swap(next_frontier_);
            ++depth;
        }

        // convert visited CSR indices into Graph NodeIds
        for (size_t csr_index = 0; csr_index < visited_.size(); ++csr_index)
        {
            if (visited_[csr_index])
                result_.push_back(csrToNode[csr_index]);
        }
    }

public:
    // Construct a searcher for queries originating from `source`.
    // `csrSnapshot` is the CSR representation snapshot used for traversal.
    CSR_Searcher(NodeId source, CSR_Representation& csrSnapshot);

    // Execute a query.
    // - target: used for SHORTEST_PATH and KHOP (KHOP uses target as k)
    // - mode: traversal strategy
    void Run(NodeId target, CSR_Mode mode, size_t k);

    // (Optional) Accessor to get last query results could be added here, e.g. GetResult()
};