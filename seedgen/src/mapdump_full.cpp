#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "mapdump_full.h"
#include "extractor.h"
#include "objects_db.h"
#include "monstats_db.h"
#include "tells.h"
#include "miniz.h"

void PrintDumpUsage() {
    printf("Usage: mapdump.exe dump <startSeed> <endSeed> [options]\n");
    printf("   or: mapdump.exe dump --seed-csv <file> [options]\n");
    printf("Options:\n");
    printf("  --maps 1,2,...     dump only these level IDs (default: all)\n");
    printf("  --game <dir>       D2 install dir (default: C:\\Program Files (x86)\\Diablo II)\n");
    printf("  --out  <dir>       output root (default: ./out)\n");
    printf("  --filter T=V[,V]   skip seeds whose tell T doesn't match value V\n");
    printf("  --seed-filter F    binary file of uint32 LE seeds; only these are processed\n");
    printf("  --seed-csv    F    dump exactly the seeds in column 1 of CSV F (header ok)\n");
    printf("Output: <outDir>/seed_<DECIMAL>.json\n");
}

bool ParseDumpArgs(int argc, char** argv, DumpArgs& out) {
    if (argc < 3) return false;

    // Positional <startSeed> <endSeed> is optional: if argv[2] starts with a
    // digit, treat it as the range; otherwise expect --seed-csv among options.
    int firstOpt = 2;
    if (argv[2][0] >= '0' && argv[2][0] <= '9') {
        if (!ParseSeedRange(argc, argv, 2, out)) return false;
        firstOpt = 4;
    }

    for (int i = firstOpt; i < argc; ++i) {
        bool ok = true;
        if (TryParseCommonOption(i, argc, argv, out, ok)) {
            if (!ok) return false;
        } else if (strcmp(argv[i], "--maps") == 0 && i + 1 < argc) {
            if (!ParseLevelList(argv[++i], out.levelFilter)) {
                fprintf(stderr, "Bad --maps list\n"); return false;
            }
        } else if (strcmp(argv[i], "--seed-csv") == 0 && i + 1 < argc) {
            if (!ReadSeedsFromCsv(argv[++i], out.explicitSeeds)) return false;
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]); return false;
        }
    }

    if (out.explicitSeeds.empty() && firstOpt == 2) {
        fprintf(stderr, "Need either <startSeed> <endSeed> or --seed-csv <file>\n");
        return false;
    }
    return true;
}

// --- JSON helpers ---

static const char kB64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string Base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = (uint32_t)data[i] << 16;
        if (i + 1 < len) b |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) b |= (uint32_t)data[i + 2];
        out.push_back(kB64Chars[(b >> 18) & 0x3F]);
        out.push_back(kB64Chars[(b >> 12) & 0x3F]);
        out.push_back(i + 1 < len ? kB64Chars[(b >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < len ? kB64Chars[(b >> 0) & 0x3F] : '=');
    }
    return out;
}

static std::string JsonEscape(const char* s) {
    std::string out;
    for (; *s; ++s) {
        if (*s == '"')       { out += "\\\""; }
        else if (*s == '\\') { out += "\\\\"; }
        else if (*s == '\n') { out += "\\n";  }
        else if (*s == '\r') { out += "\\r";  }
        else if (*s == '\t') { out += "\\t";  }
        else                 { out.push_back(*s); }
    }
    return out;
}

// Tells whose locations include this level's id. Tells without locations
// don't get emitted under a level (they describe the whole seed, not a place).
struct LevelTellEntry {
    const MapData::Tell*       tell;
    const MapData::TellResult* result;
    // Subset of result->locations that live on this level.
    std::vector<const MapData::TellLocation*> locs;
};

static std::vector<LevelTellEntry> LevelTellsFor(
    const EvalResult& ev, LevelId levelNo)
{
    std::vector<LevelTellEntry> out;
    for (const auto* t : MapData::AllTells()) {
        auto it = ev.values.find(t);
        if (it == ev.values.end()) continue;
        const auto& res = it->second;
        if (res.value == "?" || res.locations.empty()) continue;
        LevelTellEntry e{ t, &res, {} };
        for (const auto& loc : res.locations)
            if (loc.levelNo == levelNo) e.locs.push_back(&loc);
        if (!e.locs.empty()) out.push_back(std::move(e));
    }
    return out;
}

static void WriteJsonTells(FILE* f, const std::vector<LevelTellEntry>& tells) {
    fprintf(f, "      \"tells\": [");
    for (size_t i = 0; i < tells.size(); ++i) {
        const auto& e = tells[i];
        if (i) fprintf(f, ",");
        fprintf(f, "\n        {\"name\": \"%s\", \"value\": \"%s\", \"locations\": [",
                e.tell->name, JsonEscape(e.result->value.c_str()).c_str());
        for (size_t j = 0; j < e.locs.size(); ++j) {
            const auto* L = e.locs[j];
            if (j) fprintf(f, ", ");
            fprintf(f, "{\"x\": %d, \"y\": %d, \"w\": %d, \"h\": %d}",
                    L->x, L->y, L->w, L->h);
        }
        fprintf(f, "]}");
    }
    if (!tells.empty()) fprintf(f, "\n      ");
    fprintf(f, "],\n");
}

static void WriteJsonLevel(FILE* f, const MapData::LevelMap& m,
                           const std::vector<LevelTellEntry>& tells,
                           bool last) {
    fprintf(f, "    {\n");
    fprintf(f, "      \"levelNo\": %u,\n", m.levelNo);
    fprintf(f, "      \"name\": \"%s\",\n", GetLevelName(m.levelNo));
    if (const char* dn = GetLevelDisplayName(m.levelNo); dn && *dn) {
        fprintf(f, "      \"displayName\": \"%s\",\n", JsonEscape(dn).c_str());
    }
    fprintf(f, "      \"act\": %u,\n", ActNo(m.actNo) + 1);
    fprintf(f, "      \"origin\": [%d, %d],\n", m.originX, m.originY);
    fprintf(f, "      \"size\": [%d, %d],\n", m.sizeX, m.sizeY);

    WriteJsonTells(f, tells);

    fprintf(f, "      \"rooms\": [");
    for (size_t i = 0; i < m.rooms.size(); ++i) {
        const auto& r = m.rooms[i];
        if (i) fprintf(f, ",");
        fprintf(f, "\n        {\"x\": %d, \"y\": %d, \"sizeX\": %d, \"sizeY\": %d, \"roomNo\": %u, \"subNo\": %u}",
                r.x, r.y, r.sizeX, r.sizeY, r.roomNumber, r.subNumber);
    }
    if (!m.rooms.empty()) fprintf(f, "\n      ");
    fprintf(f, "],\n");

    fprintf(f, "      \"adjacents\": [");
    for (size_t i = 0; i < m.adjacents.size(); ++i) {
        const auto& a = m.adjacents[i];
        if (i) fprintf(f, ",");
        fprintf(f, "\n        {\"levelNo\": %u, \"name\": \"%s\"",
                a.levelNo, GetLevelName(a.levelNo));
        const char* adn = GetLevelDisplayName(a.levelNo);
        if (adn && *adn) fprintf(f, ", \"displayName\": \"%s\"", JsonEscape(adn).c_str());
        fprintf(f, ", \"bridgeX\": %d, \"bridgeY\": %d}", a.bridgeX, a.bridgeY);
    }
    if (!m.adjacents.empty()) fprintf(f, "\n      ");
    fprintf(f, "],\n");

    fprintf(f, "      \"presets\": [");
    for (size_t i = 0; i < m.presets.size(); ++i) {
        const auto& p = m.presets[i];
        if (i) fprintf(f, ",");
        const char* tname = p.type == 1 ? "npc" : p.type == 2 ? "obj" : p.type == 5 ? "exit" : "?";
        fprintf(f, "\n        {\"type\": \"%s\", \"txtFileNo\": %u, \"x\": %d, \"y\": %d",
                tname, p.txtFileNo, p.x, p.y);
        if (p.kind != MapData::ObjectKind::Generic)
            fprintf(f, ", \"kind\": \"%s\"", MapData::KindName(p.kind));
        if (p.destLevelNo) {
            fprintf(f, ", \"destLevelNo\": %u, \"destName\": \"%s\"",
                    p.destLevelNo, GetLevelName(p.destLevelNo));
            const char* ddn = GetLevelDisplayName(p.destLevelNo);
            if (ddn && *ddn) fprintf(f, ", \"destDisplayName\": \"%s\"", JsonEscape(ddn).c_str());
        }
        const std::string* name        = nullptr;
        const std::string* displayName = nullptr;
        if (p.type == 1) {
            const auto& n = MapData::MonsterName(p.txtFileNo);
            if (!n.empty()) name = &n;
            const auto& dn = MapData::MonsterDisplayName(p.txtFileNo);
            if (!dn.empty()) displayName = &dn;
        }
        if (p.type == 2) {
            const auto& n = MapData::ObjectName(p.txtFileNo);
            if (!n.empty()) name = &n;
            const auto& dn = MapData::ObjectDisplayName(p.txtFileNo);
            if (!dn.empty()) displayName = &dn;
        }
        if (name) fprintf(f, ", \"name\": \"%s\"", JsonEscape(name->c_str()).c_str());
        if (displayName) fprintf(f, ", \"displayName\": \"%s\"", JsonEscape(displayName->c_str()).c_str());
        if (p.type == 2) {
            const auto& d = MapData::ObjectDescription(p.txtFileNo);
            if (!d.empty()) fprintf(f, ", \"description\": \"%s\"", JsonEscape(d.c_str()).c_str());
        }
        fprintf(f, "}");
    }
    if (!m.presets.empty()) fprintf(f, "\n      ");
    fprintf(f, "],\n");

    // Collision grid: zlib-deflated little-endian uint16 bytes, then base64.
    // Decode on the JS side with DecompressionStream("deflate").
    // 0xFFFF = kNoData (no tile here); other values are D2 walkability flags.
    // Padding around the painted region is mostly 0xFFFF runs, so DEFLATE
    // shrinks this ~15-25x vs the raw base64 we used to emit.
    if (!m.coll.empty()) {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(m.coll.data());
        const mz_ulong srcLen = static_cast<mz_ulong>(m.coll.size() * sizeof(uint16_t));
        mz_ulong dstLen = mz_compressBound(srcLen);
        std::vector<uint8_t> z(dstLen);
        int rc = mz_compress2(z.data(), &dstLen, src, srcLen, MZ_BEST_COMPRESSION);
        if (rc != MZ_OK) {
            fprintf(stderr, "mz_compress2 failed (%d) on level %u; emitting null\n", rc, m.levelNo);
            fprintf(f, "      \"collisionDeflateB64\": null\n");
        } else {
            std::string b64 = Base64Encode(z.data(), dstLen);
            fprintf(f, "      \"collisionWidth\": %d,\n", m.sizeX);
            fprintf(f, "      \"collisionHeight\": %d,\n", m.sizeY);
            fprintf(f, "      \"collisionDeflateB64\": \"%s\"\n", b64.c_str());
        }
    } else {
        fprintf(f, "      \"collisionDeflateB64\": null\n");
    }

    fprintf(f, "    }%s\n", last ? "" : ",");
}

void WriteJsonSeedToFile(FILE* f, uint32_t seed,
                         const std::vector<MapData::LevelMap>& levels,
                         const EvalResult& ev) {
    fprintf(f, "{\n  \"seed\": %u,\n  \"levels\": [\n", seed);
    for (size_t i = 0; i < levels.size(); ++i) {
        const auto tells = LevelTellsFor(ev, LevelId(levels[i].levelNo));
        WriteJsonLevel(f, levels[i], tells, i + 1 == levels.size());
    }
    fprintf(f, "  ]\n}\n");
}

static void WriteJsonSeed(const char* path, uint32_t seed,
                          const std::vector<MapData::LevelMap>& levels,
                          const EvalResult& ev) {
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "Cannot write %s\n", path); return; }
    WriteJsonSeedToFile(f, seed, levels, ev);
    fclose(f);
}

// --- RunDump ---

int RunDump(const DumpArgs& args) {
    if (!InitD2(args)) return 1;

    auto inFilter = [&](uint32_t lvl) {
        return args.levelFilter.empty() || args.levelFilter.count(lvl) != 0;
    };
    auto actHasAnyFiltered = [&](ActId act) {
        if (args.levelFilter.empty()) return true;
        const uint32_t i = ActNo(act);
        for (uint32_t lvl : args.levelFilter)
            if (lvl >= LvlId(kActFirstLevel[i]) && lvl <= LvlId(kActLastLevel[i])) return true;
        return false;
    };

    uint32_t skipped = 0;
    const bool     csvMode    = !args.explicitSeeds.empty();
    const uint32_t progStart  = csvMode ? 0u : args.startSeed;
    const uint32_t progEnd    = csvMode ? static_cast<uint32_t>(args.explicitSeeds.size() - 1)
                                        : args.endSeed;
    ProgressReporter progress(progStart, progEnd);

    // Evaluate every registered tell so the JSON dump can attach the locations
    // each tell carries. Cheap relative to the per-level extraction we do next.
    const std::vector<const MapData::Tell*> allTells = MapData::AllTells();

    auto processSeed = [&](uint32_t seed) {
        EvalResult ev = EvalTells(seed, args.filter, allTells);
        if (!ev.pass) { ++skipped; return; }

        // Shared acquisition: extract all matching levels for this seed.
        std::vector<MapData::LevelMap> levels;
        for (ActId act : kAllActs) {
            if (!actHasAnyFiltered(act)) continue;
            const uint32_t i = ActNo(act);
            Act* pAct = SafeLoadAct(act, seed, /*difficulty=*/0, kTownLevels[i]);
            if (!pAct) continue;
            for (uint32_t lvl = LvlId(kActFirstLevel[i]);
                          lvl <= LvlId(kActLastLevel[i]); ++lvl) {
                if (!inFilter(lvl)) continue;
                MapData::LevelMap m = MapData::ExtractLevel(pAct, act, lvl);
                if (!m.empty()) levels.push_back(std::move(m));
            }
            D2_UnloadAct(pAct);
        }
        if (levels.empty()) return;

        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\seed_%010u.json", args.outDir.c_str(), seed);
        WriteJsonSeed(path, seed, levels, ev);
    };

    if (csvMode) {
        for (size_t i = 0; i < args.explicitSeeds.size(); ++i) {
            processSeed(args.explicitSeeds[i]);
            progress.update(static_cast<uint32_t>(i));
        }
    } else {
        for (uint32_t seed = args.startSeed; seed <= args.endSeed; ++seed) {
            if (!SeedAllowed(args, seed)) { ++skipped; progress.update(seed); continue; }
            processSeed(seed);
            progress.update(seed);
        }
    }

    const double   elapsed = progress.elapsedSeconds();
    const uint32_t total   = csvMode
        ? static_cast<uint32_t>(args.explicitSeeds.size())
        : (args.endSeed - args.startSeed + 1);
    const uint32_t matched = total - skipped;
    printf("\nDone. %u/%u seeds in %.1f s (%.0f seeds/s)",
           matched, total, elapsed, elapsed > 0 ? total / elapsed : 0.0);
    if (skipped) printf(", %u skipped by filter", skipped);
    printf(".\n");
    return 0;
}
