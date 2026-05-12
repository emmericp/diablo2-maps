#include "objects_db.h"
#include "d2loader.h"
#include "stringtbl.h"
#include <vector>
#include <cstdio>
#include <cstring>

namespace MapData {
namespace {

std::vector<std::string> g_names;     // indexed by txtFileNo (raw key)
std::vector<std::string> g_descs;     // indexed by txtFileNo (column 1)
std::vector<std::string> g_display;   // indexed by txtFileNo (localized)
std::vector<ObjectKind>  g_kinds;
const std::string        g_empty;

bool ContainsCI(const std::string& s, const char* needle) {
    const size_t nl = strlen(needle);
    if (nl == 0 || nl > s.size()) return false;
    for (size_t i = 0; i + nl <= s.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < nl; ++j) {
            char a = s[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// Order matters: more specific patterns first. "SuperChest" must beat "Chest".
ObjectKind ClassifyName(const std::string& name) {
    if (ContainsCI(name, "shrine"))      return ObjectKind::Shrine;
    if (ContainsCI(name, "well"))        return ObjectKind::Well;
    if (ContainsCI(name, "waypoint") ||
        ContainsCI(name, " wp")    ||
        ContainsCI(name, "wp "))         return ObjectKind::Waypoint;
    if (ContainsCI(name, "superchest"))  return ObjectKind::SuperChest;
    if (ContainsCI(name, "chest"))       return ObjectKind::Chest;
    if (ContainsCI(name, "door"))        return ObjectKind::Door;
    if (ContainsCI(name, "stair"))       return ObjectKind::Stairs;
    if (ContainsCI(name, "cairn")    ||
        ContainsCI(name, "horadric") ||
        ContainsCI(name, "khalim")   ||
        ContainsCI(name, "compelling orb") ||
        ContainsCI(name, "hellforge"))    return ObjectKind::Quest;
    return ObjectKind::Generic;
}

} // anonymous

bool LoadObjectsDb(bool verbose) {
    g_names.clear();
    g_descs.clear();
    g_display.clear();
    g_kinds.clear();

    std::vector<uint8_t> raw;
    if (!D2_ReadMpqFile("data\\global\\excel\\objects.txt", raw)) {
        fprintf(stderr, "LoadObjectsDb: failed to read data\\global\\excel\\objects.txt from MPQ\n");
        return false;
    }

    // Parse tab-separated. First row is the column header — skip it.
    const char* p   = reinterpret_cast<const char*>(raw.data());
    const char* end = p + raw.size();
    bool seenHeader = false;

    while (p < end) {
        const char* lineStart = p;
        while (p < end && *p != '\n') ++p;
        const char* lineEnd = p;
        if (p < end) ++p;  // skip the '\n'

        // Strip trailing \r
        if (lineEnd > lineStart && *(lineEnd - 1) == '\r') --lineEnd;

        if (!seenHeader) { seenHeader = true; continue; }
        if (lineEnd == lineStart) continue;  // skip blank lines

        // Column 0 = Name, column 1 = description. Both tab-separated.
        const char* tab = lineStart;
        while (tab < lineEnd && *tab != '\t') ++tab;
        std::string name(lineStart, tab);

        std::string desc;
        if (tab < lineEnd) {
            const char* d0 = tab + 1;
            const char* d1 = d0;
            while (d1 < lineEnd && *d1 != '\t') ++d1;
            // Descriptions containing commas are wrapped in CSV-style quotes
            // (e.g. "Door by Dock, Act 2"). Strip matching outer quotes.
            if (d1 - d0 >= 2 && *d0 == '"' && *(d1 - 1) == '"') { ++d0; --d1; }
            desc.assign(d0, d1);
        }

        // D2 marks "expansion-only" or commented rows with "Expansion" header
        // lines mid-file; those are simply additional rows with that token in
        // col 0 — we keep them so the row index stays aligned with txtFileNo.

        g_names.push_back(name);
        g_descs.push_back(desc);
        g_kinds.push_back(ClassifyName(name));
    }

    // Localized display names — resolve each row's Name key through D2Lang.
    // Rows with no entry in the string table get an empty display name; the
    // JSON emit path falls back to the raw key in that case.
    g_display.resize(g_names.size());
    int localized = 0;
    for (size_t i = 0; i < g_names.size(); ++i) {
        if (g_names[i].empty()) continue;
        const std::string& v = LookupString(g_names[i].c_str());
        if (!v.empty()) { g_display[i] = v; ++localized; }
    }

    // Act 5's Objects.txt rows often don't reflect the actual in-game type
    // (a "magic shrine" name on a dummy, a "well" name on a waypoint, etc.).
    // These overrides are verified empirically from the renderer.
    struct Override { uint32_t txtFileNo; ObjectKind kind; };
    static const Override kOverrides[] = {
        // Quest objects whose Objects.txt Name doesn't already match the
        // shrine/waypoint/etc. classifier patterns.
        {   8, ObjectKind::Quest    },  // TowerTome        - Forgotten Tower tome
        {  26, ObjectKind::Quest    },  // Gibbet           - Cain's cage
        {  30, ObjectKind::Quest    },  // Inifuss          - Inifuss tree
        {  61, ObjectKind::Quest    },  // Dummy            - invisible quest marker
        { 149, ObjectKind::Quest    },  // taintedsunaltar
        { 152, ObjectKind::Quest    },  // orifice          - Horadric Staff socket
        { 193, ObjectKind::Quest    },  // LamTome          - Lam Esen's Tome
        { 252, ObjectKind::Quest    },  // gidbinn
        { 298, ObjectKind::Quest    },  // portal           - Arcane Sanctuary portal
        { 357, ObjectKind::Quest    },  // Tome             - act 3 quest tome
        { 367, ObjectKind::Quest    },  // sewer lever      - act 3 sewers
        { 392, ObjectKind::Quest    },  // Seal             - Diablo seal 1
        { 393, ObjectKind::Quest    },  // Seal             - Diablo seal 2
        { 394, ObjectKind::Quest    },  // Seal             - Diablo seal 3
        { 395, ObjectKind::Quest    },  // Seal             - Diablo seal 4
        { 396, ObjectKind::Quest    },  // Seal             - Diablo seal 5
        { 404, ObjectKind::Quest    },  // compellingorb
        { 460, ObjectKind::Quest    },  // dummy            - Anya start marker
        { 462, ObjectKind::Quest    },  // dummy            - Nihlathak start marker
        // Act 5 mislabels — see file comment above.
        { 430, ObjectKind::Generic  },  // Waypoint         - mislabeled (dummy)
        { 473, ObjectKind::Quest    },  // icecaveshrine2
        { 480, ObjectKind::Generic  },  // icecaveshrine2   - mislabeled (dummy)
        { 489, ObjectKind::Generic  },  // magic shrine     - mislabeled (dummy)
        { 494, ObjectKind::Waypoint },  // well             - actually a waypoint
        { 496, ObjectKind::Waypoint },  // magic shrine     - actually a waypoint
        { 504, ObjectKind::Generic  },  // magic shrine     - mislabeled (dummy)
        { 510, ObjectKind::Generic  },  // magic shrine     - mislabeled (dummy)
        { 511, ObjectKind::Waypoint },  // mrpole           - actually a waypoint
        { 523, ObjectKind::Generic  },  // manashrine       - mislabeled (dummy)
    };
    for (const auto& o : kOverrides) {
        if (o.txtFileNo < g_kinds.size()) g_kinds[o.txtFileNo] = o.kind;
    }

    // Tally by kind for sanity.
    int waypoints = 0, shrines = 0, wells = 0, chests = 0, doors = 0;
    for (auto k : g_kinds) {
        switch (k) {
            case ObjectKind::Waypoint: ++waypoints; break;
            case ObjectKind::Shrine:   ++shrines;   break;
            case ObjectKind::Well:     ++wells;     break;
            case ObjectKind::Chest:
            case ObjectKind::SuperChest: ++chests; break;
            case ObjectKind::Door:     ++doors;     break;
            default: break;
        }
    }
    if (verbose) {
        printf("LoadObjectsDb: %zu rows  (waypoints=%d shrines=%d wells=%d chests=%d doors=%d localized=%d)\n",
               g_names.size(), waypoints, shrines, wells, chests, doors, localized);
    }
    return true;
}

size_t ObjectsDbSize() { return g_names.size(); }

const std::string& ObjectName(uint32_t txtFileNo) {
    if (txtFileNo >= g_names.size()) return g_empty;
    return g_names[txtFileNo];
}

const std::string& ObjectDescription(uint32_t txtFileNo) {
    if (txtFileNo >= g_descs.size()) return g_empty;
    return g_descs[txtFileNo];
}

const std::string& ObjectDisplayName(uint32_t txtFileNo) {
    if (txtFileNo >= g_display.size()) return g_empty;
    return g_display[txtFileNo];
}

ObjectKind ObjectKindFor(uint32_t txtFileNo) {
    if (txtFileNo >= g_kinds.size()) return ObjectKind::Generic;
    return g_kinds[txtFileNo];
}

const char* KindName(ObjectKind k) {
    switch (k) {
        case ObjectKind::Waypoint:   return "Waypoint";
        case ObjectKind::Shrine:     return "Shrine";
        case ObjectKind::Well:       return "Well";
        case ObjectKind::SuperChest: return "SuperChest";
        case ObjectKind::Chest:      return "Chest";
        case ObjectKind::Door:       return "Door";
        case ObjectKind::Stairs:     return "Stairs";
        case ObjectKind::Quest:      return "Quest";
        default:                     return "Generic";
    }
}

} // namespace MapData
