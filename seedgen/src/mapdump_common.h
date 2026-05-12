#pragma once
#include <cstdint>
#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "d2loader.h"
#include "d2levels.h"
#include "tells.h"

extern const ActId   kAllActs[5];
extern const LevelId kTownLevels[5];
extern const LevelId kActFirstLevel[5];
extern const LevelId kActLastLevel[5];

const char* GetLevelName(uint32_t id);

// In-game display name from Levels.txt LevelName (e.g. id=1 -> "Rogue
// Encampment"). Returns "" if LoadLevelsDb hasn't populated this id.
const char* GetLevelDisplayName(uint32_t id);

Act* SafeLoadAct(ActId actNo, uint32_t seed, uint32_t difficulty, LevelId townLevel);

using FilterMap = std::unordered_map<const MapData::Tell*, std::unordered_set<std::string>>;

struct EvalResult {
    bool pass = true;
    // Each tell maps to its full TellResult (string value + optional positions).
    // CSV/filter paths read .value; JSON dump emits .locations as well.
    std::unordered_map<const MapData::Tell*, MapData::TellResult> values;
};

EvalResult EvalTells(uint32_t seed,
                     const FilterMap& filter,
                     const std::vector<const MapData::Tell*>& extra);

struct CommonArgs {
    uint32_t    startSeed = 0;
    uint32_t    endSeed   = 0;
    std::string gameDir   = "C:\\Program Files (x86)\\Diablo II";
    std::string outDir    = "./out";
    FilterMap   filter;
    // Bitset over [startSeed, endSeed]: true = seed allowed. Empty = no filter.
    std::vector<bool> seedFilter;
};

inline bool SeedAllowed(const CommonArgs& a, uint32_t seed) {
    return a.seedFilter.empty() || a.seedFilter[seed - a.startSeed];
}

// Parse startSeed/endSeed from argv[seedIdx] and argv[seedIdx+1].
bool ParseSeedRange(int argc, char** argv, int seedIdx, CommonArgs& out);
bool ParseLevelList(const char* s, std::unordered_set<uint32_t>& out);
bool ParseFilterArg(const char* s, FilterMap& filter);
bool ParseTellList(const char* s, std::vector<const MapData::Tell*>& out);

// Read seeds from a CSV file in the format produced by `mapdump summary`:
// first column = seed, header row optional. Non-numeric / blank lines are
// silently skipped. Order is preserved; duplicates are kept as-is.
bool ReadSeedsFromCsv(const char* path, std::vector<uint32_t>& out);

// Try to handle one common flag at argv[i]. Advances i if a value arg was consumed.
// Returns true if the flag was recognized; sets ok=false and returns true on parse error.
bool TryParseCommonOption(int& i, int argc, char** argv, CommonArgs& out, bool& ok);

// Load D2 DLLs and DBs, create output directory, print banner. Returns false on failure.
bool InitD2(const CommonArgs& args);

struct ProgressReporter {
    static constexpr uint32_t kBatch = 500;
    uint32_t startSeed, endSeed;
    std::chrono::steady_clock::time_point t0, tBatch;
    uint32_t batchBase;

    ProgressReporter(uint32_t start, uint32_t end);
    void   update(uint32_t seed);
    double elapsedSeconds() const;
};
