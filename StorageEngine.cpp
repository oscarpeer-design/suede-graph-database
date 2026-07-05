#include "StorageEngine.h"

// constructor
StorageEngine::StorageEngine(std::string path): graphFilePath(path) {}

// ValidateGraph
// verifies that the reconstructed graph matches the file header
bool StorageEngine::ValidateGraph(const Graph& graph)
{
    // get all nodes
    std::vector<NodeId> nodeIds;
    graph.GetNodeIdOrder(nodeIds);

    // incorrect number of nodes
    if (nodeIds.size() != nodeCount)
        return false;

    // get all edges
    std::vector<EdgeId> edgeIds;
    graph.GetAllEdgeIds(edgeIds);

    // incorrect number of edges
    if (edgeIds.size() != edgeCount)
        return false;

    // graph reconstructed successfully
    return true;
}

// Load Graph into memory
bool StorageEngine::Load(Graph& graph)
{
    // open graph file for reading
    if (!OpenInputFile())
        return false;

    // verify file header
    if (!ReadHeader())
        return false;

    // reconstruct nodes
    if (!ReadNodes(graph))
        return false;

    // reconstruct edges
    if (!ReadEdges(graph))
        return false;

    // verify graph integrity
    if (!ValidateGraph(graph))
        return false;

    // close file
    CloseInputFile();

    return true;
}

// save Graph
bool StorageEngine::Save(const Graph& graph)
{
    // open graph file for writing
    if (!OpenOutputFile())
        return false;

    // write file header
    if (!WriteHeader(graph))
        return false;

    // write every node
    if (!WriteNodes(graph))
        return false;

    // write every edge
    if (!WriteEdges(graph))
        return false;

    // flush and close file
    CloseOutputFile();

    return true;
}

// Import CSV
bool StorageEngine::ImportCSV(Graph& graph, const std::string& csvFilePath)
{
    // open csv file
    if (!OpenCSV(csvFilePath))
        return false;

    // import all nodes
    if (!ImportNodes(graph))
        return false;

    // import all edges
    if (!ImportEdges(graph))
        return false;

    // close csv file
    CloseCSV();

    return true;
}

// Export CSV
bool StorageEngine::ExportCSV(const Graph& graph, const std::string& csvFilePath)
{
    // create csv file
    if (!CreateCSV(csvFilePath))
        return false;

    // export nodes
    if (!ExportNodes(graph))
        return false;

    // export edges
    if (!ExportEdges(graph))
        return false;

    // close csv file
    CloseCSV();

    return true;
}
