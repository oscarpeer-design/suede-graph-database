#pragma once
// used to run tests

int test_graph();

// Exercises the entire query layer (Query.cpp): tokenizer, clause parsers
// (SELECT / INSERT / DELETE / MATCH / WHERE), and every execution path
// (node/edge selects, inserts, deletes, and BFS-backed traversals).
int test_query_layer();

// Exercises the entire StorageEngine (StorageEngine.cpp): binary Save/Load
// round-trips, ValidateGraph integrity checks, and CSV import/export.
int test_storage_engine();

int run_tests();