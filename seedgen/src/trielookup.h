#pragma once
#include <cstdint>
#include <string>

// `trielookup.exe buildindex` / `trielookup.exe lookup`
//
// Persistent sequence index for fast seed lookup from level-graph .bin
// files (produced by `mapdump.exe levelgraph`).
//
// Per-seed pipeline:
//   1. Decode the 24-byte levelgraph record into a list of (cell_x, cell_y,
//      encoded) entries.
//   2. Find the StairsUp room (encoded value in {0x2700, 0x2800, 0x2900,
//      0x2A00}). BFS over actual shape-based connectivity, tie-breaking by
//      N→E→S→W priority. Sequence = encoded values in visit order.
//   3. Emit a fixed 20-byte index record: 8 uint16 sequence slots
//      (0xFFFF for unused) + uint32 seed.
//
// All 20-byte records are externally sorted by sequence so a player prefix
// becomes a contiguous byte range; binary search finds it in O(log N).

struct BuildIndexArgs {
    uint32_t    levelId    = 0;
    // Glob pattern for input level<id>_*_*.bin files (Win32 FindFirstFile
    // semantics — '*' and '?' wildcards work in the filename component).
    // Accepts a single file path with no wildcards too.
    std::string input;
    std::string outputFile;
    // Soft cap on per-chunk memory during external sort. Default 1 GiB.
    uint64_t    chunkBytes = 1ull << 30;
};

struct LookupArgs {
    std::string indexFile;
    // Prefix as comma-separated 4-char hex uint16 encoded values. Each
    // nibble may be replaced with '?' to mark 4 unknown bits, e.g.
    //   --prefix 2700,11??,2E00
    // means "StairsUp W, then NW/Empty/DoorNorth/<any graves>, then N/StairsDown".
    std::string prefix;
    int         listLimit       = 0;  // 0 = count only; >0 = print up to N seeds
    int         maxUnknownBits  = 4;  // safety cap; query fans out to 2^N lookups
    bool        json            = false;  // emit machine-readable output
    // Parity filter, applied during the linear scan over hit ranges so that
    // both `matchCountFiltered` and the emitted seed list reflect only
    // parity-matching candidates. 0 = no filter, 1 = even (seed&1 == 0),
    // 2 = odd (seed&1 == 1). Models the Tower L5 "Countess north/west"
    // tell, which deterministically splits seeds by parity.
    int         parity          = 0;
};

struct CollisionsArgs {
    std::string indexFile;
    int         topGroups   = 10;  // dump the N largest collision groups
    int         seedsPerTop = 5;   // up to this many seeds per top group
    // If true, treat (sequence, seed & 1) as the bucket key — i.e. seeds with
    // different parity are not considered colliding. Models the "level 5 gives
    // us parity for free" case.
    bool        parity      = false;
    // Restrict the bucket key to the first `prefixLen` sequence slots
    // (default kIndexSeqSlots = full sequence).
    int         prefixLen   = 8;
    // If true, mask the low byte (graves bitmask) of every key element so
    // seeds that only differ in grave layout collide.
    bool        stripGraves = false;
    // If set, also export every seed belonging to a bucket of size in
    // [exportMin, exportMax] as packed little-endian uint32, sorted ascending.
    // 0 means unset (no bound) — exportFile must be non-empty to enable.
    uint32_t    exportMin   = 0;
    uint32_t    exportMax   = 0;
    std::string exportFile;
};

bool ParseBuildIndexArgs(int argc, char** argv, BuildIndexArgs& out);
void PrintBuildIndexUsage();
int  RunBuildIndex(const BuildIndexArgs& args);

bool ParseLookupArgs(int argc, char** argv, LookupArgs& out);
void PrintLookupUsage();
int  RunLookup(const LookupArgs& args);

bool ParseCollisionsArgs(int argc, char** argv, CollisionsArgs& out);
void PrintCollisionsUsage();
int  RunCollisions(const CollisionsArgs& args);

// Decode a comma-separated list of 4-char hex encoded TowerRoom values
// (the same values stored in a trie sequence) via the C++ TowerRoom table.
// Useful for reading back a trie sequence in human terms without going
// to the source .bin files.
int  RunDecode(int argc, char** argv);
void PrintDecodeUsage();

// Record layout: 8 uint16 sequence entries + uint32 seed. No padding
// (uint16 × 8 = 16 bytes, then a 4-aligned uint32).
struct IndexRecord {
    uint16_t seq[8];
    uint32_t seed;
};
static_assert(sizeof(IndexRecord) == 20, "IndexRecord must be packed at 20 bytes");

constexpr int kIndexSeqSlots   = 8;
constexpr int kIndexRecordSize = 20;
