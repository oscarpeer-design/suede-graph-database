#pragma once

#include <string>
#include <fstream>
#include <cstring>
#include <sstream>
#include <cstdio>       // std::remove / std::rename for the atomic temp swap

#include "../Graph and Searchers/ErrorCodes.h"
#include "../Graph and Searchers/Types.h"
#include "../Graph and Searchers/Graph.h"
#include "../Graph and Searchers/CSR_Representation.h"
#include "../Storage/Crc32.h"      // integrity checksum for corruption detection

// constant expressions for File header and version.
// FILE_VERSION is bumped to 2: the format now ends with a CRC32 TRAILER over the
// whole payload (everything written after the header). A version-1 file has no
// trailer; Load rejects it (there is no forward-compat requirement here -- old
// files are re-savable). If you must read old files, special-case version 1 to
// skip the CRC check.
static constexpr char FILE_MAGIC[8] = "GRAPHDB";
static constexpr uint32_t FILE_VERSION = 2;

// File Header Format
struct FileHeader
{
	char magic[8];
	uint32_t version;
	uint64_t nodeCount;
	uint64_t edgeCount;
};

// stores the graph on disk and loads it in and out of memory
class StorageEngine {
private:
	// filepath to binary file
	// NOTE: stored by value (not by reference). The constructor is passed
	// the path by value, so a reference member would dangle the moment the
	// constructor returned. Owning the string keeps it valid for the
	// lifetime of the StorageEngine.
	std::string graphFilePath;

	// currently opened binary graph file
	std::fstream graphFile;

	// currently opened csv file
	std::fstream csvFile;

	// number of nodes stored in file
	uint64_t nodeCount;

	// number of edges stored in file
	uint64_t edgeCount;

	// order in which NodeIds are read
	std::vector<NodeId> loadedNodes;

	// lookup NodeId to index
	std::unordered_map<NodeId, size_t> nodeIndex;

	// ---- integrity (CRC) + safe-write state -----------------------------
	// Running CRC over the payload. On SAVE it is folded as each payload byte is
	// written; on LOAD it is folded as each payload byte is read, then compared
	// against the trailer. Initialised to 0xFFFFFFFF at the start of each pass.
	uint32_t crc = 0xFFFFFFFFu;

	// Bytes of PAYLOAD remaining to be read on load (total file size minus the
	// header minus the 4-byte trailer). Every read helper decrements this and,
	// before trusting an on-disk length/count, checks it against this bound so a
	// corrupt length can never trigger a huge allocation or read past end-of-file.
	uint64_t payloadRemaining = 0;

	// The temp path we write to before the atomic rename (graphFilePath + ".tmp").
	std::string TempPath() const { return graphFilePath + ".tmp"; }

	// crcRead / crcWrite: a stream read/write that ALSO folds the bytes into `crc`
	// and (on read) decrements payloadRemaining. Using these instead of raw
	// graphFile.read/write keeps the running checksum correct without a separate
	// pass, and gives every read a single choke point for the bounds check.
	bool crcWrite(const void* data, size_t len) {
		graphFile.write(reinterpret_cast<const char*>(data), len);
		if (!graphFile.good()) return false;
		crc = crc32Update(crc, data, len);
		return true;
	}
	// Read `len` bytes into `data`, folding them into the CRC. Fails if fewer than
	// `len` payload bytes remain (bounds check) or the stream read fails.
	bool crcRead(void* data, size_t len) {
		if (len > payloadRemaining) return false;   // would read past the payload
		graphFile.read(reinterpret_cast<char*>(data), len);
		if (!graphFile.good()) return false;
		crc = crc32Update(crc, data, len);
		payloadRemaining -= len;
		return true;
	}

	// N.B. Production Graph Databases hold nodes and edges in two CSV Files when exporting to CSV format

	// open graph file for reading
	bool OpenInputFile() {
		// reset any previous stream state
		graphFile.clear();

		graphFile.open(graphFilePath, std::ios::binary | std::ios::in);

		return graphFile.is_open();
	}

	// open graph file for writing
	bool OpenOutputFile()
	{
		// reset any previous stream state
		graphFile.clear();

		graphFile.open(graphFilePath, std::ios::binary | std::ios::out | std::ios::trunc);

		return graphFile.is_open();
	}

	// close graph input file
	void CloseInputFile()
	{
		if (graphFile.is_open())
			graphFile.close();
	}

	// close graph output file
	void CloseOutputFile()
	{
		if (graphFile.is_open())
			graphFile.close();
	}

	// open CSV
	bool OpenCSV(const std::string& csvFilePath)
	{
		// reset any previous stream state
		csvFile.clear();

		csvFile.open(csvFilePath, std::ios::in);

		return csvFile.is_open();
	}

	// close CSV
	void CloseCSV()
	{
		if (csvFile.is_open())
			csvFile.close();
	}

	// create CSV
	bool CreateCSV(const std::string& csvFilePath)
	{
		// reset any previous stream state
		csvFile.clear();

		csvFile.open(csvFilePath, std::ios::out | std::ios::trunc);

		return csvFile.is_open();
	}

	// ReadHeader
	// reads the binary file header and validates the graph file
	bool ReadHeader()
	{
		// structure that stores the header
		FileHeader header;

		// attempt to read header from file
		graphFile.read(reinterpret_cast<char*>(&header), sizeof(FileHeader));

		// check file read succeeded
		if (!graphFile)
			return false;

		// verify this is one of our graph files
		if (std::strncmp(header.magic, "GRAPHDB", 7) != 0)
			return false;

		// verify file version is supported
		if (header.version != 1)
			return false;

		// store node and edge counts for later loading
		nodeCount = header.nodeCount;
		edgeCount = header.edgeCount;

		// success
		return true;
	}

	// WriteHeader
	// writes the binary graph file header
	bool WriteHeader(const Graph& graph)
	{
		// structure that stores the header
		FileHeader header;

		// write graph file identifier
		std::memcpy(header.magic, "GRAPHDB", 8);

		// write current file version
		header.version = 1;

		// determine number of nodes
		std::vector<NodeId> nodeIds;
		graph.GetNodeIdOrder(nodeIds);
		header.nodeCount = nodeIds.size();

		// determine number of edges
		std::vector<EdgeId> edgeIds;
		graph.GetAllEdgeIds(edgeIds);
		header.edgeCount = edgeIds.size();

		// write header to disk
		graphFile.write(reinterpret_cast<const char*>(&header), sizeof(FileHeader));

		// check write succeeded
		return graphFile.good();
	}

	// WriteNodes
	// writes every node in insertion order. All writes go through crcWrite so the
	// running payload CRC is accumulated as we stream (no separate pass needed).
	bool WriteNodes(const Graph& graph)
	{
		// get node insertion order
		std::vector<NodeId> nodeIds;
		graph.GetNodeIdOrder(nodeIds);
		// write every node
		for (NodeId nodeId : nodeIds)
		{
			// get node
			Node node;
			if (!graph.GetNode(nodeId, node))
				return false;
			// write label length + label
			uint64_t labelLength = node.label.size();
			if (!crcWrite(&labelLength, sizeof(labelLength))) return false;
			if (!crcWrite(node.label.data(), labelLength)) return false;
			// write property count
			uint64_t propertyCount = node.properties.size();
			if (!crcWrite(&propertyCount, sizeof(propertyCount))) return false;
			// write every property (key length + key, value length + value)
			for (const auto& property : node.properties)
			{
				uint64_t keyLength = property.first.size();
				if (!crcWrite(&keyLength, sizeof(keyLength))) return false;
				if (!crcWrite(property.first.data(), keyLength)) return false;
				uint64_t valueLength = property.second.size();
				if (!crcWrite(&valueLength, sizeof(valueLength))) return false;
				if (!crcWrite(property.second.data(), valueLength)) return false;
			}
		}

		return graphFile.good();
	}

	// ReadString
	// Read a length-prefixed string: an 8-byte length, then that many bytes. The
	// length is bounds-checked against payloadRemaining BEFORE allocating, so a
	// corrupt length (e.g. 4 billion) can never trigger a huge allocation or a read
	// past end-of-file -- it just fails the load cleanly. crcRead folds every byte
	// into the running CRC and decrements payloadRemaining.
	bool ReadString(std::string& out)
	{
		// read 8-byte string to check for corruption
		uint64_t len;
		// reads + bounds-checks the 8 bytes
		if (!crcRead(&len, sizeof(len))) 
			return false;  
		// string can't fit in what's left
		if (len > payloadRemaining) 
			return false;   
		// check we can actually read the bits
		out.assign((size_t)len, '\0');
		if (len > 0 && !crcRead(&out[0], (size_t)len)) 
			return false;
		return true;
	}

	// ReadNodes
	// reconstruct all nodes from disk. All reads go through crcRead / ReadString,
	// which fold bytes into the CRC and reject implausible lengths before they can
	// cause a huge allocation.
	bool ReadNodes(Graph& graph)
	{
		// clear previous lookup
		loadedNodes.clear();

		// read every node
		for (size_t i = 0; i < nodeCount; ++i)
		{
			// read label (length-prefixed, bounds-checked)
			std::string label;
			if (!ReadString(label)) return false;

			// read property count, then bounds-check it: each property needs at
			// least 16 bytes (two 8-byte length prefixes), so a propertyCount larger
			// than payloadRemaining/16 is impossible and rejected before we loop.
			uint64_t propertyCount;
			if (!crcRead(&propertyCount, sizeof(propertyCount))) return false;
			if (propertyCount > payloadRemaining / 16) return false;

			propertiesMap properties;
			for (uint64_t j = 0; j < propertyCount; ++j)
			{
				std::string key, value;
				// validate properties
				if (!ReadString(key)) 
					return false;
				if (!ReadString(value)) 
					return false;
				properties[key] = value;
			}

			// create node
			NodeId id = graph.CreateNode(label, std::move(properties));

			// remember generated id
			loadedNodes.push_back(id);
		}

		return graphFile.good();
	}

	// ReadEdges
	// reconstruct every edge from the graph file. Reads via crcRead / ReadString
	// (CRC + bounds-checked); the endpoint indices are additionally checked against
	// the number of nodes actually loaded, exactly as before.
	bool ReadEdges(Graph& graph)
	{
		// warning from Graph operations
		int warning = operationSuccessful;

		// reconstruct every edge
		for (size_t i = 0; i < edgeCount; i++)
		{
			// read source + destination node indices (CRC + bounds-checked reads)
			size_t fromIndex, toIndex;
			if (!crcRead(&fromIndex, sizeof(fromIndex))) return false;
			if (!crcRead(&toIndex, sizeof(toIndex))) return false;

			// read edge label (length-prefixed, bounds-checked before allocation)
			std::string label;
			if (!ReadString(label)) return false;

			// ensure indices reference nodes that were actually loaded
			if (fromIndex >= loadedNodes.size() ||
				toIndex >= loadedNodes.size())
			{
				return false;
			}

			// reconstruct edge using the newly-created NodeIds
			graph.CreateEdge(
				loadedNodes[fromIndex],
				loadedNodes[toIndex],
				label,
				warning);

			// graph rejected the edge
			if (warning != operationSuccessful)
				return false;
		}

		// ensure the stream is still valid
		return graphFile.good();
	}

	// WriteEdges
	// writes every edge in the graph
	bool WriteEdges(const Graph& graph)
	{
		// Build a deterministic mapping from NodeId -> insertion index.
		// When edges are written to the binary file we store node references
		// as indices (size_t) into the node insertion order. This keeps the
		// on-disk format compact and independent of ephemeral NodeId values.
		std::vector<NodeId> nodeIds;
		graph.GetNodeIdOrder(nodeIds);

		// map NodeIds to their insertion index
		std::unordered_map<NodeId, size_t> nodeIndex;

		for (size_t i = 0; i < nodeIds.size(); i++)
			nodeIndex[nodeIds[i]] = i;

		// retrieve all edges and write each as:
		//   [fromIndex:size_t][toIndex:size_t][labelLength:uint64][label:bytes]
		std::vector<EdgeId> edgeIds;
		graph.GetAllEdgeIds(edgeIds);

		// write every edge (all through crcWrite to fold into the payload CRC)
		for (EdgeId edgeId : edgeIds)
		{
			Edge edge;

			if (!graph.GetEdge(edgeId, edge))
				return false;
			// convert NodeIds to their stored insertion indices.
			// Note: .at() will throw if a NodeId is unexpectedly missing which
			// would indicate a logic error in the Graph instance.
			size_t fromIndex = nodeIndex.at(edge.from);
			size_t toIndex = nodeIndex.at(edge.to);

			// write indices
			if (!crcWrite(&fromIndex, sizeof(fromIndex))) return false;
			if (!crcWrite(&toIndex, sizeof(toIndex))) return false;

			// write label length + label contents
			uint64_t labelLength = edge.label.size();
			if (!crcWrite(&labelLength, sizeof(labelLength))) return false;
			if (!crcWrite(edge.label.data(), labelLength)) return false;
		}

		return graphFile.good();
	}

	// ImportNodes
	// Expects CSV Format: Label,key=value,key=value,key=value
	// E.g. Person,name=Oscar,age=22
	// Is:	City, name = Sydney
	bool ImportNodes(Graph& graph)
	{
		// Clear any previous loaded node list used to map CSV indices -> NodeId
		loadedNodes.clear();

		std::string line;

		// Read each line from the CSV input stream. Each line represents one node.
		// Blank line terminates node import (existing behavior in the code).
		while (std::getline(csvFile, line))
		{
			if (line.empty())
				break;

			std::stringstream stream(line);

			std::string token;

			// First token is the node label
			std::getline(stream, token, ',');

			std::string label = token;

			propertiesMap properties;

			// Remaining tokens are properties in the form key=value.
			// Malformed tokens without '=' are ignored.
			while (std::getline(stream, token, ','))
			{
				size_t equals = token.find('=');

				if (equals == std::string::npos)
					continue;

				properties[token.substr(0, equals)] =
					token.substr(equals + 1);
			}

			// Create the node and remember the resulting NodeId so edge imports
			// can reference nodes by their CSV index (0-based).
			loadedNodes.push_back(
				graph.CreateNode(label, properties));
		}

		return true;
	}

	// ImportEdges
	// Expects CSV Format: 0,1,FRIENDS
	//					   2, 4, WORKS_AT
	// Each line uses numeric indices that reference the order of nodes
	// produced during ImportNodes (i.e. indices into loadedNodes).
	bool ImportEdges(Graph& graph)
	{
		std::string line;

		int warning;

		// Read each edge line: fromIndex,toIndex,label
		while (std::getline(csvFile, line))
		{
			if (line.empty())
				continue;

			std::stringstream stream(line);

			std::string token;

			// parse from index (CSV index -> loadedNodes index)
			std::getline(stream, token, ',');
			size_t from = std::stoull(token);

			// parse to index
			std::getline(stream, token, ',');
			size_t to = std::stoull(token);

			// remaining token is the label (may contain commas if not strictly CSV-escaped)
			std::getline(stream, token);

			// validate indices against loadedNodes which should have been filled by ImportNodes
			if (from >= loadedNodes.size())
				return false;

			if (to >= loadedNodes.size())
				return false;

			// Create the edge using the NodeIds resolved from the previously-created nodes.
			// Graph::CreateEdge reports status in 'warning' which we check for success.
			graph.CreateEdge(
				loadedNodes[from],
				loadedNodes[to],
				token,
				warning);

			// propagate any graph-level rejection as failure of the import
			if (warning != operationSuccessful)
				return false;
		}

		return true;
	}

	// ExportNodes
	// Writes nodes in insertion order to the currently-open csvFile stream.
	// Output format per-line: Label,key=value,key=value,...
	bool ExportNodes(const Graph& graph)
	{
		std::vector<NodeId> nodeIds;

		// preserve insertion order for deterministic output
		graph.GetNodeIdOrder(nodeIds);

		for (NodeId nodeId : nodeIds)
		{
			Node node;

			if (!graph.GetNode(nodeId, node))
				return false;

			// start line with the node label
			csvFile << node.label;

			// append each property as ,key=value
			for (const auto& property : node.properties)
			{
				csvFile << ","
					<< property.first
					<< "="
					<< property.second;
			}

			// terminate line
			csvFile << "\n";
		}

		// Write a blank line to separate the node block from the edge block.
		// ImportNodes() reads node lines until it encounters an empty line,
		// at which point ImportEdges() takes over. Without this separator the
		// edge lines would be (mis)parsed as nodes and no edges would import.
		csvFile << "\n";

		// return whether the underlying stream is still OK
		return csvFile.good();
	}

	// ExportEdges
	bool ExportEdges(const Graph& graph)
	{
		// build NodeId -> insertion index map so we can export edges
		// using numeric indices consistent with ImportEdges format.
		std::vector<NodeId> nodeIds;
		graph.GetNodeIdOrder(nodeIds);

		std::unordered_map<NodeId, size_t> nodeIndex;

		for (size_t i = 0; i < nodeIds.size(); i++)
			nodeIndex[nodeIds[i]] = i;

		// export every edge in the format: fromIndex,toIndex,label\n
		std::vector<EdgeId> edgeIds;
		graph.GetAllEdgeIds(edgeIds);
		// Write to the csv file each edge in the expected format
		for (EdgeId edgeId : edgeIds)
		{
			Edge edge;

			if (!graph.GetEdge(edgeId, edge))
				return false;

			// write indices corresponding to the insertion order of nodes
			csvFile
				<< nodeIndex.at(edge.from)
				<< ","
				<< nodeIndex.at(edge.to)
				<< ","
				<< edge.label
				<< "\n";
		}

		return csvFile.good();
	}

public:
	// constructor
	StorageEngine(std::string path);

	// load graph (from the path fixed at construction)
	bool Load(Graph& graph);

	// save graph to binary (to the path fixed at construction)
	bool Save(const Graph& graph);

	// Load/Save the live graph to an EXPLICIT path chosen at runtime (e.g. a
	// file the interactive user picked from a dialog). These retarget the
	// engine's current file path, then reuse the same Load/Save machinery, so
	// the on-disk format is identical to the fixed-path versions. After the
	// call the engine's current path is the one just used, so a later
	// bare Save()/Load() targets that same file.
	bool Load(Graph& graph, const std::string& path);
	bool Save(const Graph& graph, const std::string& path);

	// convert CSV to Graph
	bool ImportCSV(Graph& graph, const std::string& csvFilePath);

	// export CSV to Graph
	bool ExportCSV(const Graph& graph, const std::string& csvFilePath);

	// validation that the produced graph matches the stored graph
	bool ValidateGraph(const Graph& graph);

	// Save a CSR snapshot to disk
	bool SaveSnapshot(const CSR_Representation& snapshot, const std::string& snapshotFilePath);

	// Load a CSR snapshot from disk
	bool LoadSnapshot(CSR_Representation& snapshot, const std::string& snapshotFilePath);
};