#pragma once
#include <cstdint>
#include <string>
#include "mapdump_common.h"
#include "levelgraph_format.h"

// "levelgraph" subcommand — export the per-seed room layout of one tower
// cellar level as a packed binary stream, one fixed-size record per seed.
//
// Per-record layout (24 bytes): 8 grave-room slots × 3 bytes each.
//   byte 0:      position = (cell_y << 4) | cell_x (cells are 40 tiles).
//                Unused slots: 0xFF (cells never exceed 7 on either axis).
//   bytes 1..2:  TowerRoom::encode() little-endian. Unused slots: 0xFFFF.
//
// Rooms inside a record are sorted by (cell_y, cell_x) so the order is
// deterministic. Across the 30k+ tower-level instances we have, no level
// has ever held more than 8 rooms, so the 8-slot cap is exact.

struct LevelGraphArgs : CommonArgs {
    uint32_t    levelId = 0;          // 21..24 (or any single level id)
    std::string outFile = "";         // explicit output path; defaults to
                                      // <outDir>/level<id>_<start>_<end>.bin
};

bool ParseLevelGraphArgs(int argc, char** argv, LevelGraphArgs& out);
void PrintLevelGraphUsage();
int  RunLevelGraph(const LevelGraphArgs& args);

// Decode one record at byte offset (seedOffset * 25) of `file` and print
// every room (cell, encoded value, asText) plus the implied connections.
int RunGraphDecode(const char* file, uint32_t seedOffset);
void PrintGraphDecodeUsage();
