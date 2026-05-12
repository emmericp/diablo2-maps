#include "monstats_db.h"
#include "d2loader.h"
#include "stringtbl.h"
#include <vector>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>

namespace MapData {
namespace {

struct Entry {
    std::string id;        // col 0 — internal token like "summoner"
    std::string nameStr;   // col 5 — display string key, usually English already
    std::string display;   // localized via D2Lang lookup of nameStr, or ""
};

std::unordered_map<uint32_t, Entry> g_monsters;
const std::string g_empty;

// Capture pointers to the start of the first N tab-separated columns of a
// line. Returns the number captured. Does not copy strings — pointers are
// valid as long as the source buffer outlives use.
int ColumnStarts(const char* lineStart, const char* lineEnd,
                 const char** out, int maxCols) {
    int n = 0;
    if (n < maxCols) out[n++] = lineStart;
    for (const char* p = lineStart; p < lineEnd && n < maxCols; ++p) {
        if (*p == '\t') out[n++] = p + 1;
    }
    return n;
}

std::string Slice(const char* a, const char* b) { return std::string(a, b - a); }

} // anonymous

bool LoadMonStatsDb(bool verbose) {
    g_monsters.clear();

    std::vector<uint8_t> raw;
    if (!D2_ReadMpqFile("data\\global\\excel\\monstats.txt", raw)) {
        fprintf(stderr, "LoadMonStatsDb: failed to read monstats.txt from MPQ\n");
        return false;
    }

    const char* p   = reinterpret_cast<const char*>(raw.data());
    const char* end = p + raw.size();
    bool headerSeen = false;
    int rowsParsed  = 0;

    while (p < end) {
        const char* lineStart = p;
        while (p < end && *p != '\n') ++p;
        const char* lineEnd = p;
        if (p < end) ++p;
        if (lineEnd > lineStart && *(lineEnd - 1) == '\r') --lineEnd;
        if (!headerSeen) { headerSeen = true; continue; }
        if (lineEnd == lineStart) continue;

        // We only need columns 0 (Id), 1 (hcIdx), 5 (NameStr). Capture the
        // starts of cols 0..6 — col 6's start tells us where col 5 ends.
        const char* col[7] = {};
        const int n = ColumnStarts(lineStart, lineEnd, col, 7);
        if (n < 6) continue;
        const char* col5End = (n >= 7) ? (col[6] - 1) : lineEnd;
        const char* col0End = (n >= 2) ? (col[1] - 1) : lineEnd;
        const char* col1End = (n >= 3) ? (col[2] - 1) : lineEnd;

        // Parse hcIdx (col 1). Blank rows / comment rows are skipped.
        if (col1End <= col[1]) continue;
        char* endptr;
        std::string hcStr = Slice(col[1], col1End);
        unsigned long hcIdx = strtoul(hcStr.c_str(), &endptr, 10);
        if (endptr == hcStr.c_str()) continue;

        Entry e;
        e.id      = Slice(col[0], col0End);
        e.nameStr = Slice(col[5], col5End);
        if (!e.nameStr.empty()) e.display = LookupString(e.nameStr.c_str());
        g_monsters[static_cast<uint32_t>(hcIdx)] = std::move(e);
        ++rowsParsed;
    }

    int localized = 0;
    for (const auto& kv : g_monsters)
        if (!kv.second.display.empty()) ++localized;

    if (verbose) printf("LoadMonStatsDb: parsed %d monsters (localized=%d)\n",
                        rowsParsed, localized);
    return rowsParsed > 0;
}

size_t MonStatsDbSize() { return g_monsters.size(); }

const std::string& MonsterName(uint32_t hcIdx) {
    auto it = g_monsters.find(hcIdx);
    return (it == g_monsters.end()) ? g_empty : it->second.nameStr;
}

const std::string& MonsterDisplayName(uint32_t hcIdx) {
    auto it = g_monsters.find(hcIdx);
    return (it == g_monsters.end()) ? g_empty : it->second.display;
}

const std::string& MonsterId(uint32_t hcIdx) {
    auto it = g_monsters.find(hcIdx);
    return (it == g_monsters.end()) ? g_empty : it->second.id;
}

} // namespace MapData
