#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>
#include "mapdump_server.h"
#include "mapdump_common.h"
#include "mapdump_full.h"
#include "extractor.h"
#include "levels_db.h"
#include "objects_db.h"
#include "monstats_db.h"
#include "stringtbl.h"
#include "tells.h"

static const char* kDefaultGameDir = "C:\\Program Files (x86)\\Diablo II";

void PrintServerUsage() {
    fprintf(stderr,
        "Usage: mapdump.exe server [--game <dir>]\n"
        "Reads requests from stdin, writes one-line JSON to stdout.\n"
        "Request format (one per line):\n"
        "  <seed> <acts|ALL|L:<levels>> <difficulty>\n"
        "  e.g.  12345 1 0           // all of Act 1\n"
        "        12345 1,2 0         // acts 1 and 2\n"
        "        12345 ALL 0         // every act\n"
        "        12345 L:21,22,23 0  // exact level ids only\n"
        "Close stdin (EOF) to exit.\n");
}

// Parse the second token. Three forms:
//   - "ALL"           → act filter empty, level filter empty (everything)
//   - "1,2,3"         → act filter = those acts
//   - "L:21,22,23"    → level filter = those specific level ids
// Returns false on any unparseable / out-of-range token.
static bool ParseActsOrLevelsArg(const char* s,
                                 std::unordered_set<uint32_t>& actFilter,
                                 std::unordered_set<uint32_t>& levelFilter) {
    actFilter.clear();
    levelFilter.clear();
    if (_stricmp(s, "ALL") == 0) return true;
    if ((s[0] == 'L' || s[0] == 'l') && s[1] == ':') {
        s += 2;
        while (*s) {
            while (*s == ',' || *s == ' ') ++s;
            if (!*s) break;
            char* end = nullptr;
            unsigned long v = strtoul(s, &end, 10);
            if (end == s || v < 1 || v > 255) return false;
            levelFilter.insert(static_cast<uint32_t>(v));
            s = end;
        }
        return !levelFilter.empty();
    }
    while (*s) {
        while (*s == ',' || *s == ' ') ++s;
        if (!*s) break;
        char* end = nullptr;
        unsigned long v = strtoul(s, &end, 10);
        if (end == s || v < 1 || v > 5) return false;
        actFilter.insert(static_cast<uint32_t>(v));
        s = end;
    }
    return true;
}

static void WriteErrorJson(uint32_t seed, const char* msg) {
    // Escape just the chars JsonEscape covers in mapdump_full; the message is
    // ours so we keep it ASCII and quote-free.
    fprintf(stdout, "{\"seed\":%u,\"error\":\"%s\"}\n", seed, msg);
    fflush(stdout);
}

static void EmitOneLineJson(FILE* scratch, uint32_t seed,
                            const std::vector<MapData::LevelMap>& levels,
                            const EvalResult& ev) {
    rewind(scratch);
    // Truncate the scratch file so successive responses don't pile up.
    if (_chsize_s(_fileno(scratch), 0) != 0) {
        fprintf(stderr, "[server] _chsize_s failed\n");
    }
    WriteJsonSeedToFile(scratch, seed, levels, ev);
    fflush(scratch);

    const long n = ftell(scratch);
    rewind(scratch);
    std::string buf;
    if (n > 0) {
        buf.resize(static_cast<size_t>(n));
        const size_t got = fread(buf.data(), 1, buf.size(), scratch);
        buf.resize(got);
    }

    // Strip raw newlines. JSON escapes embedded newlines to "\\n", so the only
    // '\n' / '\r' bytes in `buf` are formatting whitespace from WriteJson*.
    std::string oneLine;
    oneLine.reserve(buf.size() + 1);
    for (char c : buf) {
        if (c == '\n' || c == '\r') continue;
        oneLine.push_back(c);
    }
    oneLine.push_back('\n');
    fwrite(oneLine.data(), 1, oneLine.size(), stdout);
    fflush(stdout);
}

int RunServer(int argc, char** argv) {
    std::string gameDir = kDefaultGameDir;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
            gameDir = argv[++i];
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            PrintServerUsage();
            return 1;
        }
    }

    // Binary mode so we don't sprinkle CRLF into the JSON stream.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin),  _O_BINARY);

    if (!D2_Initialize(gameDir.c_str())) {
        fprintf(stderr, "D2_Initialize failed\n");
        return 1;
    }
    MapData::LoadStringTbl();
    MapData::LoadLevelsDb();
    MapData::LoadObjectsDb();
    MapData::LoadMonStatsDb();

    FILE* scratch = tmpfile();
    if (!scratch) {
        fprintf(stderr, "tmpfile() failed\n");
        return 1;
    }

    const std::vector<const MapData::Tell*> allTells = MapData::AllTells();
    fprintf(stderr, "mapdump server ready (game=%s)\n", gameDir.c_str());

    char line[2048];
    while (fgets(line, sizeof(line), stdin)) {
        size_t L = strlen(line);
        while (L > 0 && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';
        if (L == 0) continue;

        char seedStr[64] = {}, actStr[64] = {}, diffStr[16] = {};
        const int parsed = sscanf(line, "%63s %63s %15s", seedStr, actStr, diffStr);
        if (parsed < 3) {
            WriteErrorJson(0, "expected: <seed> <acts|ALL> <difficulty>");
            continue;
        }

        char* end = nullptr;
        const uint32_t seed = static_cast<uint32_t>(strtoul(seedStr, &end, 10));
        if (end == seedStr || *end != '\0') { WriteErrorJson(0, "bad seed"); continue; }

        const uint32_t difficulty = static_cast<uint32_t>(strtoul(diffStr, &end, 10));
        if (end == diffStr || *end != '\0' || difficulty > 2) {
            WriteErrorJson(seed, "bad difficulty");
            continue;
        }

        std::unordered_set<uint32_t> actFilter;   // empty = all acts
        std::unordered_set<uint32_t> levelFilter; // empty = no per-level filter
        if (!ParseActsOrLevelsArg(actStr, actFilter, levelFilter)) {
            WriteErrorJson(seed, "bad acts/levels");
            continue;
        }

        // If a level filter is set, derive which acts we need to load (each
        // act covers a contiguous range of level ids). Otherwise honor the
        // act filter as before. Empty `actFilter` AND empty `levelFilter`
        // means "all acts, all levels".
        auto actHasAny = [&](ActId act) {
            const uint32_t i = ActNo(act);
            if (!levelFilter.empty()) {
                for (uint32_t lvl = LvlId(kActFirstLevel[i]);
                              lvl <= LvlId(kActLastLevel[i]); ++lvl) {
                    if (levelFilter.count(lvl)) return true;
                }
                return false;
            }
            return actFilter.empty() || actFilter.count(ActNo(act) + 1) != 0;
        };

        auto levelWanted = [&](uint32_t lvl) {
            return levelFilter.empty() || levelFilter.count(lvl) != 0;
        };

        // Tells run unfiltered so the JSON carries every tell's locations,
        // matching the `dump` command's output shape.
        FilterMap noFilter;
        EvalResult ev = EvalTells(seed, noFilter, allTells);

        std::vector<MapData::LevelMap> levels;
        for (ActId act : kAllActs) {
            if (!actHasAny(act)) continue;
            const uint32_t i = ActNo(act);
            Act* pAct = SafeLoadAct(act, seed, difficulty, kTownLevels[i]);
            if (!pAct) continue;
            for (uint32_t lvl = LvlId(kActFirstLevel[i]);
                          lvl <= LvlId(kActLastLevel[i]); ++lvl) {
                if (!levelWanted(lvl)) continue;
                MapData::LevelMap m = MapData::ExtractLevel(pAct, act, lvl);
                if (!m.empty()) levels.push_back(std::move(m));
            }
            D2_UnloadAct(pAct);
        }

        EmitOneLineJson(scratch, seed, levels, ev);
    }

    fclose(scratch);
    return 0;
}
