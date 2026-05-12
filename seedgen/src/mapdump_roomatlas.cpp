#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "mapdump_roomatlas.h"
#include "extractor.h"
#include "tower_rooms.h"
#include "miniz.h"

namespace {

constexpr int kRoomTileSize = 40;          // every tower room is 40 × 40 tiles
constexpr int kRoomTileCount = kRoomTileSize * kRoomTileSize;

// (roomId, variant) packed into one uint16 for hash keys: variant in bit 15,
// roomId in bits 0..14. Mirrors but isn't identical to the on-disk encoding,
// which uses bit 7 of the high byte for variant.
inline uint32_t ComboHash(uint32_t roomId, uint8_t variant) {
    return (roomId & 0x7FFFu) | (uint32_t(variant) << 15);
}

const char* kB64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = uint32_t(data[i]) << 16;
        if (i + 1 < len) b |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < len) b |= uint32_t(data[i + 2]);
        out.push_back(kB64[(b >> 18) & 0x3F]);
        out.push_back(kB64[(b >> 12) & 0x3F]);
        out.push_back(i + 1 < len ? kB64[(b >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < len ? kB64[(b >> 0) & 0x3F] : '=');
    }
    return out;
}

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

struct Collected {
    uint32_t                roomId   = 0;
    uint8_t                 variant  = 0;
    std::string             shape;
    std::string             theme;
    std::string             variantName;
    uint32_t                seed     = 0;
    uint32_t                levelId  = 0;
    int                     roomX    = 0;
    int                     roomY    = 0;
    std::vector<uint16_t>   tiles;       // length kRoomTileCount, row-major
};

bool ExtractRoomTiles(
    const MapData::LevelMap& m, const MapData::Room& r,
    std::vector<uint16_t>& out,
    bool& clipped)
{
    out.assign(kRoomTileCount, MapData::kNoData);
    clipped = false;
    if (r.sizeX < kRoomTileSize || r.sizeY < kRoomTileSize) {
        // Tower rooms should always be 40×40. Smaller rooms aren't ours.
        return false;
    }
    for (int dy = 0; dy < kRoomTileSize; ++dy) {
        const int sy = r.y + dy;
        if (sy < 0 || sy >= m.sizeY) { clipped = true; continue; }
        for (int dx = 0; dx < kRoomTileSize; ++dx) {
            const int sx = r.x + dx;
            if (sx < 0 || sx >= m.sizeX) { clipped = true; continue; }
            out[dy * kRoomTileSize + dx] = m.coll[sy * m.sizeX + sx];
        }
    }
    return true;
}

std::string DeflateB64(const std::vector<uint16_t>& tiles) {
    const size_t srcLen = tiles.size() * sizeof(uint16_t);
    mz_ulong dstLen = static_cast<mz_ulong>(mz_compressBound(static_cast<mz_ulong>(srcLen)));
    std::vector<uint8_t> z(dstLen);
    const int rc = mz_compress2(
        z.data(), &dstLen,
        reinterpret_cast<const uint8_t*>(tiles.data()),
        static_cast<mz_ulong>(srcLen),
        MZ_BEST_COMPRESSION);
    if (rc != MZ_OK) return "";
    return Base64Encode(z.data(), dstLen);
}

void WriteAtlasJson(
    const std::vector<Collected>& items,
    const std::vector<MapData::TowerRoom::ComboKey>& expectedCombos,
    const std::string& outFile)
{
    std::unordered_map<uint32_t, const Collected*> byHash;
    for (const auto& c : items) byHash[ComboHash(c.roomId, c.variant)] = &c;

    FILE* f = fopen(outFile.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "Cannot write %s\n", outFile.c_str());
        return;
    }
    fprintf(f, "{\n");
    fprintf(f, "  \"version\":  1,\n");
    fprintf(f, "  \"tileSize\": %d,\n", kRoomTileSize);
    fprintf(f, "  \"rooms\":    [\n");
    bool first = true;
    for (const auto& key : expectedCombos) {
        const auto it = byHash.find(ComboHash(key.roomId, key.variant));
        if (it == byHash.end()) continue;
        const Collected& c = *it->second;
        if (!first) fprintf(f, ",\n");
        first = false;
        const std::string b64 = DeflateB64(c.tiles);
        fprintf(f,
            "    {"
            "\"roomId\": %u, "
            "\"variant\": %u, "
            "\"shape\": \"%s\", "
            "\"theme\": \"%s\", "
            "\"variantName\": \"%s\", "
            "\"exampleSeed\": %u, "
            "\"exampleLevelId\": %u, "
            "\"roomX\": %d, "
            "\"roomY\": %d, "
            "\"collDeflateB64\": \"%s\""
            "}",
            c.roomId, c.variant,
            c.shape.c_str(), c.theme.c_str(), c.variantName.c_str(),
            c.seed, c.levelId, c.roomX, c.roomY,
            b64.c_str());
    }
    fprintf(f, "\n  ]\n}\n");
    fclose(f);
}

} // namespace

// ---------------------------------------------------------------------------

void PrintRoomAtlasUsage() {
    printf("Usage: mapdump.exe roomatlas <startSeed> <endSeed> [options]\n");
    printf("  Scans the given seed range looking for canonical examples of every\n");
    printf("  (roomId, variant) combo whose graves bitmask == 0. For each combo\n");
    printf("  it captures the room's 40x40 collision tiles into atlas.json.\n");
    printf("Options:\n");
    printf("  --level <id>        scan only this tower level (repeatable). Default: 21,22,23,24\n");
    printf("  --out <file>        output JSON (default: atlas.json)\n");
    printf("  --game <dir>        D2 install dir\n");
    printf("  --filter T=V[,V]    only consider seeds matching tell T\n");
    printf("  --seed-filter F     binary file of uint32 LE seeds; only these are processed\n");
}

bool ParseRoomAtlasArgs(int argc, char** argv, RoomAtlasArgs& out) {
    if (argc < 4) return false;
    if (!ParseSeedRange(argc, argv, 2, out)) return false;
    for (int i = 4; i < argc; ++i) {
        bool ok = true;
        if (TryParseCommonOption(i, argc, argv, out, ok)) {
            if (!ok) return false;
        } else if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
            out.levelIds.push_back(static_cast<uint32_t>(atoi(argv[++i])));
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out.outFile = argv[++i];
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return false;
        }
    }
    if (out.levelIds.empty()) out.levelIds = {21, 22, 23, 24};
    return true;
}

int RunRoomAtlas(const RoomAtlasArgs& args) {
    if (!InitD2(args)) return 1;

    const auto expected = MapData::TowerRoom::AllKnownCombos();
    std::unordered_map<uint32_t, Collected> collected;

    printf("Target combos: %zu, scanning levels:", expected.size());
    for (const auto id : args.levelIds) printf(" %u", id);
    printf("\n");

    ProgressReporter progress(args.startSeed, args.endSeed);
    uint32_t scanned = 0;

    for (uint32_t seed = args.startSeed; seed <= args.endSeed; ++seed) {
        if (!SeedAllowed(args, seed)) {
            progress.update(seed);
            continue;
        }
        bool allFound = collected.size() == expected.size();
        if (allFound) break;

        // One Act load per seed. Different tower levels share Act 1 so we
        // can scan them all from a single load.
        ActId  act   = ActForLevel(args.levelIds[0]);
        LevelId town = TownForLevel(args.levelIds[0]);
        Act* pAct = SafeLoadAct(act, seed, 0, town);
        if (!pAct) { progress.update(seed); continue; }

        for (const uint32_t lvlId : args.levelIds) {
            MapData::LevelMap m = MapData::ExtractLevel(pAct, act, lvlId);
            if (m.empty()) continue;

            for (const auto& r : m.rooms) {
                if (!MapData::TowerRoom::IsKnownRoomId(r.roomNumber)) continue;
                const MapData::TowerRoom tr = MapData::TowerRoom::FromGame(m, r);
                if (!tr.valid()) continue;
                if (tr.graves() != 0) continue;          // graves==0 only
                const uint32_t key = ComboHash(tr.roomId(), tr.variant());
                if (collected.count(key)) continue;       // first-come wins

                Collected c;
                c.roomId      = tr.roomId();
                c.variant     = tr.variant();
                c.shape       = tr.shape();
                c.theme       = tr.theme();
                c.variantName = tr.variantName();
                c.seed        = seed;
                c.levelId     = lvlId;
                c.roomX       = r.x;
                c.roomY       = r.y;
                bool clipped  = false;
                if (!ExtractRoomTiles(m, r, c.tiles, clipped)) continue;
                collected.emplace(key, std::move(c));
                printf("  + %u/%u: roomId=%u variant=%u (seed %u lvl %u) %s/%s%s%s\n",
                       static_cast<unsigned>(collected.size()),
                       static_cast<unsigned>(expected.size()),
                       tr.roomId(), tr.variant(), seed, lvlId,
                       tr.shape(), tr.theme(),
                       *tr.variantName() ? "/" : "", tr.variantName());
            }
        }
        D2_UnloadAct(pAct);
        ++scanned;
        progress.update(seed);
        if (collected.size() == expected.size()) break;
    }

    // Sort collected into expected order for stable JSON.
    std::vector<Collected> items;
    items.reserve(collected.size());
    for (const auto& k : expected) {
        auto it = collected.find(ComboHash(k.roomId, k.variant));
        if (it != collected.end()) items.push_back(it->second);
    }

    WriteAtlasJson(items, expected, args.outFile);

    printf("\nDone. Scanned %u seeds. Collected %zu/%zu combos. Wrote %s\n",
           scanned, items.size(), expected.size(), args.outFile.c_str());
    if (items.size() < expected.size()) {
        printf("Missing:\n");
        for (const auto& k : expected) {
            if (collected.count(ComboHash(k.roomId, k.variant))) continue;
            printf("  - roomId=%u variant=%u\n", k.roomId, k.variant);
        }
    }
    return 0;
}
