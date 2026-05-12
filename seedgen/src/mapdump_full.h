#pragma once
#include <cstdio>
#include <unordered_set>
#include "mapdump_common.h"
#include "mapdata.h"

struct DumpArgs : CommonArgs {
    std::unordered_set<uint32_t> levelFilter;
    // If non-empty, dump exactly these seeds (in given order) and ignore the
    // startSeed/endSeed range. Populated from --seed-csv.
    std::vector<uint32_t>        explicitSeeds;
};

bool ParseDumpArgs(int argc, char** argv, DumpArgs& out);
void PrintDumpUsage();
int  RunDump(const DumpArgs& args);

// Emit the full per-seed JSON document to `f`. Does not close `f`. Used by the
// per-seed file dump path and the stdin/stdout server.
void WriteJsonSeedToFile(FILE* f, uint32_t seed,
                         const std::vector<MapData::LevelMap>& levels,
                         const EvalResult& ev);
