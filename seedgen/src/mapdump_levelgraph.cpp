#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "mapdump_levelgraph.h"
#include "extractor.h"
#include "tower_rooms.h"

namespace {

ActId ActForLevel(uint32_t levelId) {
    for (uint32_t i = 0; i < 5; ++i) {
        if (levelId >= LvlId(kActFirstLevel[i]) && levelId <= LvlId(kActLastLevel[i])) {
            return kAllActs[i];
        }
    }
    return ActId::Act1;
}

LevelId TownForLevel(uint32_t levelId) {
    for (uint32_t i = 0; i < 5; ++i) {
        if (levelId >= LvlId(kActFirstLevel[i]) && levelId <= LvlId(kActLastLevel[i])) {
            return kTownLevels[i];
        }
    }
    return LevelId::RogueCamp;
}

void EmptyRecord(uint8_t (&rec)[kLevelGraphRecordSize]) {
    memset(rec, 0xFF, kLevelGraphRecordSize);
}

void PackRecord(const MapData::LevelMap& m, uint8_t (&rec)[kLevelGraphRecordSize]) {
    EmptyRecord(rec);
    struct Entry { uint8_t cellX, cellY; uint16_t encoded; };
    std::vector<Entry> entries;
    entries.reserve(m.rooms.size());
    for (const auto& r : m.rooms) {
        if (!MapData::TowerRoom::IsKnownRoomId(r.roomNumber)) continue;
        const MapData::TowerRoom tr = MapData::TowerRoom::FromGame(m, r);
        if (!tr.valid()) continue;
        const int cx = r.x / 40;
        const int cy = r.y / 40;
        if (cx < 0 || cx > 14 || cy < 0 || cy > 14) continue;  // 0xFF reserved
        entries.push_back({static_cast<uint8_t>(cx),
                           static_cast<uint8_t>(cy),
                           tr.encode()});
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) {
                  if (a.cellY != b.cellY) return a.cellY < b.cellY;
                  return a.cellX < b.cellX;
              });
    const size_t n = std::min<size_t>(entries.size(), kLevelGraphMaxRooms);
    for (size_t i = 0; i < n; ++i) {
        const Entry& e = entries[i];
        rec[i * 3 + 0] = static_cast<uint8_t>((e.cellY << 4) | (e.cellX & 0x0F));
        rec[i * 3 + 1] = static_cast<uint8_t>(e.encoded & 0xFF);
        rec[i * 3 + 2] = static_cast<uint8_t>((e.encoded >> 8) & 0xFF);
    }
}

} // namespace

void PrintLevelGraphUsage() {
    printf("Usage: mapdump.exe levelgraph <startSeed> <endSeed> --level <id> [options]\n");
    printf("  Writes one packed binary record per seed (25 bytes) describing the\n");
    printf("  TowerRoom layout of the given level. Levels are exported independently,\n");
    printf("  one file per level.\n");
    printf("Options:\n");
    printf("  --level <id>      level id to export (e.g. 21..24 for tower cellar)\n");
    printf("  --game  <dir>     D2 install dir (default: C:\\Program Files (x86)\\Diablo II)\n");
    printf("  --outfile <path>  explicit output file path (default: <outDir>/level<id>_<start>_<end>.bin)\n");
    printf("  --filter T=V[,V]  skip seeds whose tell T doesn't match value V\n");
    printf("  --seed-filter F   binary file of uint32 LE seeds; only these are processed\n");
    printf("Output: a fixed-size %d-byte record per seed, packed contiguously.\n",
           kLevelGraphRecordSize);
}

bool ParseLevelGraphArgs(int argc, char** argv, LevelGraphArgs& out) {
    if (argc < 4) return false;
    if (!ParseSeedRange(argc, argv, 2, out)) return false;
    for (int i = 4; i < argc; ++i) {
        bool ok = true;
        if (TryParseCommonOption(i, argc, argv, out, ok)) {
            if (!ok) return false;
        } else if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
            out.levelId = static_cast<uint32_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--outfile") == 0 && i + 1 < argc) {
            out.outFile = argv[++i];
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]); return false;
        }
    }
    if (out.levelId == 0) {
        fprintf(stderr, "--level <id> is required\n");
        return false;
    }
    return true;
}

int RunLevelGraph(const LevelGraphArgs& args) {
    if (!InitD2(args)) return 1;

    std::string path = args.outFile;
    if (path.empty()) {
        char buf[MAX_PATH];
        snprintf(buf, sizeof(buf), "%s\\level%u_%u_%u.bin",
                 args.outDir.c_str(), args.levelId, args.startSeed, args.endSeed);
        path = buf;
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "Cannot write %s\n", path.c_str());
        return 1;
    }
    // 1 MiB stdio buffer (vs. the ~4 KiB default)
    static char writeBuf[1 << 20];
    setvbuf(f, writeBuf, _IOFBF, sizeof(writeBuf));
    printf("Export: writing %s (level %u, seeds %u..%u, %d bytes/seed)\n",
           path.c_str(), args.levelId, args.startSeed, args.endSeed,
           kLevelGraphRecordSize);

    const ActId    act      = ActForLevel(args.levelId);
    const LevelId  townLvl  = TownForLevel(args.levelId);
    const uint32_t target   = args.levelId;

    uint8_t rec[kLevelGraphRecordSize];
    uint32_t skipped = 0;
    uint32_t emptyLvl = 0;
    ProgressReporter progress(args.startSeed, args.endSeed);

    for (uint32_t seed = args.startSeed; seed <= args.endSeed; ++seed) {
        if (!SeedAllowed(args, seed)) {
            EmptyRecord(rec);
            fwrite(rec, 1, kLevelGraphRecordSize, f);
            ++skipped;
            progress.update(seed);
            continue;
        }

        Act* pAct = SafeLoadAct(act, seed, /*difficulty=*/0, townLvl);
        if (!pAct) {
            EmptyRecord(rec);
            fwrite(rec, 1, kLevelGraphRecordSize, f);
            progress.update(seed);
            continue;
        }
        MapData::LevelMap m = MapData::ExtractLevel(pAct, act, target);
        D2_UnloadAct(pAct);

        if (m.empty()) {
            EmptyRecord(rec);
            ++emptyLvl;
        } else {
            PackRecord(m, rec);
        }
        fwrite(rec, 1, kLevelGraphRecordSize, f);
        progress.update(seed);
    }
    fclose(f);

    const double   elapsed = progress.elapsedSeconds();
    const uint32_t total   = args.endSeed - args.startSeed + 1;
    const uint32_t matched = total - skipped;
    printf("\nDone. %u/%u seeds in %.1f s (%.0f seeds/s)",
           matched, total, elapsed, elapsed > 0 ? total / elapsed : 0.0);
    if (skipped)  printf(", %u skipped by filter", skipped);
    if (emptyLvl) printf(", %u with no rooms extracted", emptyLvl);
    printf(".\n");
    return 0;
}

// ---------------------------------------------------------------------------

void PrintGraphDecodeUsage() {
    printf("Usage: mapdump.exe graphdecode <file> <seed-offset>\n");
    printf("  Reads one packed levelgraph record from <file> at byte offset\n");
    printf("  (seed-offset * %d) and prints every room (cell, encoded value,\n",
           kLevelGraphRecordSize);
    printf("  shape/theme/variant/graves) plus all implied connections.\n");
}

namespace {

struct DecodedRoom {
    int                cellX   = 0;
    int                cellY   = 0;
    uint16_t           encoded = 0;
    MapData::TowerRoom tr;
};

bool ShapeHasExit(const char* shape, char dir) {
    for (const char* p = shape; *p; ++p) if (*p == dir) return true;
    return false;
}

} // namespace

int RunGraphDecode(const char* file, uint32_t seedOffset) {
    FILE* f = fopen(file, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", file);
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
    const long fsize = ftell(f);
    const long totalRecords = fsize / kLevelGraphRecordSize;
    if (static_cast<long>(seedOffset) >= totalRecords) {
        fprintf(stderr,
                "Seed offset %u out of range. File %s holds %ld records "
                "(offsets 0..%ld at this record size).\n",
                seedOffset, file, totalRecords, totalRecords - 1);
        fclose(f);
        return 1;
    }
    if (fseek(f, static_cast<long>(seedOffset) * kLevelGraphRecordSize, SEEK_SET) != 0) {
        fclose(f);
        return 1;
    }
    uint8_t rec[kLevelGraphRecordSize];
    if (fread(rec, 1, kLevelGraphRecordSize, f) != kLevelGraphRecordSize) {
        fprintf(stderr, "Short read\n");
        fclose(f);
        return 1;
    }
    fclose(f);

    std::vector<DecodedRoom> rooms;
    rooms.reserve(kLevelGraphMaxRooms);
    for (int i = 0; i < kLevelGraphMaxRooms; ++i) {
        const uint8_t pos = rec[i * 3 + 0];
        if (pos == 0xFF) continue;  // unused slot
        DecodedRoom r;
        r.cellX   = pos & 0x0F;
        r.cellY   = (pos >> 4) & 0x0F;
        r.encoded = static_cast<uint16_t>(
            rec[i * 3 + 1] | (rec[i * 3 + 2] << 8));
        r.tr      = MapData::TowerRoom::Decode(r.encoded);
        rooms.push_back(r);
    }
    printf("File:        %s\n", file);
    printf("Seed offset: %u  (byte %u)\n", seedOffset,
           seedOffset * kLevelGraphRecordSize);
    printf("Rooms:       %zu\n\n", rooms.size());

    for (const auto& r : rooms) {
        printf("  cell=(%d,%d)  encoded=0x%04X  %s\n",
               r.cellX, r.cellY, r.encoded, r.tr.asText().c_str());
    }

    // Connections: every orthogonally-adjacent pair where both rooms have
    // the appropriate exit toward the shared edge.
    printf("\nConnections:\n");
    int edges = 0;
    for (size_t i = 0; i < rooms.size(); ++i) {
        for (size_t j = i + 1; j < rooms.size(); ++j) {
            const auto& a = rooms[i];
            const auto& b = rooms[j];
            const int dx = b.cellX - a.cellX;
            const int dy = b.cellY - a.cellY;
            if (std::abs(dx) + std::abs(dy) != 1) continue;
            char aDir = '?', bDir = '?';
            if      (dx ==  1) { aDir = 'E'; bDir = 'W'; }
            else if (dx == -1) { aDir = 'W'; bDir = 'E'; }
            else if (dy ==  1) { aDir = 'S'; bDir = 'N'; }
            else               { aDir = 'N'; bDir = 'S'; }
            const char* aShape = a.tr.shape();
            const char* bShape = b.tr.shape();
            if (ShapeHasExit(aShape, aDir) && ShapeHasExit(bShape, bDir)) {
                printf("  (%d,%d) %c <-> %c (%d,%d)\n",
                       a.cellX, a.cellY, aDir, bDir, b.cellX, b.cellY);
                ++edges;
            }
        }
    }
    if (edges == 0) printf("  (none)\n");
    return 0;
}
