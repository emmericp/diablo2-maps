#include "levels_db.h"
#include "d2loader.h"
#include "stringtbl.h"
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace MapData {
namespace {

std::unordered_map<uint32_t, std::string> g_levels;
const std::string g_empty;

// Return the 0-based tab-separated column `colIdx` of [lineStart, lineEnd).
// Empty string if the line doesn't have that many columns.
std::string GetCol(const char* lineStart, const char* lineEnd, int colIdx) {
    const char* p   = lineStart;
    int         cur = 0;
    while (p < lineEnd && cur < colIdx) {
        if (*p == '\t') ++cur;
        ++p;
    }
    if (cur < colIdx) return "";
    const char* e = p;
    while (e < lineEnd && *e != '\t') ++e;
    return std::string(p, e);
}

} // anonymous

bool LoadLevelsDb(bool verbose) {
    g_levels.clear();

    std::vector<uint8_t> raw;
    if (!D2_ReadMpqFile("data\\global\\excel\\Levels.txt", raw)) {
        fprintf(stderr, "LoadLevelsDb: failed to read Levels.txt from MPQ\n");
        return false;
    }

    const char* p   = reinterpret_cast<const char*>(raw.data());
    const char* end = p + raw.size();
    bool        headerSeen = false;
    int         rowsParsed = 0;

    // Levels.txt columns (0-based): 0=Name, 1=Id, 120=LevelName.
    constexpr int kIdCol        = 1;
    constexpr int kLevelNameCol = 120;

    while (p < end) {
        const char* lineStart = p;
        while (p < end && *p != '\n') ++p;
        const char* lineEnd = p;
        if (p < end) ++p;
        if (lineEnd > lineStart && *(lineEnd - 1) == '\r') --lineEnd;
        if (!headerSeen) { headerSeen = true; continue; }
        if (lineEnd == lineStart) continue;

        std::string idStr = GetCol(lineStart, lineEnd, kIdCol);
        if (idStr.empty()) continue;
        char*         idEnd = nullptr;
        unsigned long id    = strtoul(idStr.c_str(), &idEnd, 10);
        if (idEnd == idStr.c_str() || id == 0) continue;

        std::string key = GetCol(lineStart, lineEnd, kLevelNameCol);
        if (key.empty()) continue;

        // The LevelName column is itself a string-table key (e.g. "Rigid
        // Highlands" -> expansionstring.tbl -> "Frigid Highlands"). Resolve
        // it. If the key isn't in any .tbl, fall back to the raw column —
        // most pre-expansion act-1..4 names are already the displayed text.
        const std::string& resolved = LookupString(key.c_str());
        g_levels[static_cast<uint32_t>(id)] = resolved.empty() ? std::move(key) : resolved;
        ++rowsParsed;
    }

    if (verbose) printf("LoadLevelsDb: parsed %d levels\n", rowsParsed);
    return rowsParsed > 0;
}

size_t LevelsDbSize() { return g_levels.size(); }

const std::string& LevelDisplayName(uint32_t id) {
    auto it = g_levels.find(id);
    return (it == g_levels.end()) ? g_empty : it->second;
}

} // namespace MapData
