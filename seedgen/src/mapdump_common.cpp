#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "mapdump_common.h"
#include "levels_db.h"
#include "objects_db.h"
#include "monstats_db.h"
#include "stringtbl.h"
const ActId kAllActs[5] = {
    ActId::Act1, ActId::Act2, ActId::Act3, ActId::Act4, ActId::Act5,
};
const LevelId kTownLevels[]    = { LevelId::RogueCamp, LevelId::LutGholein,    LevelId::KurastDocks,       LevelId::PandemoniumFortress, LevelId::Harrogath };
const LevelId kActFirstLevel[] = { LevelId::RogueCamp, LevelId::LutGholein,    LevelId::KurastDocks,       LevelId::PandemoniumFortress, LevelId::Harrogath };
const LevelId kActLastLevel[]  = { LevelId::CowLevel,        LevelId::ArcaneSanctuary, LevelId::DuranceOfHateLevel3, LevelId::ChaosSanctuary,    LevelId::TheWorldstoneChamber };

struct LevelName { uint32_t id; const char* name; };
static const LevelName kLevelNames[] = {
    {1,"RogueCamp"},{2,"BloodMoor"},{3,"ColdPlains"},{4,"StonyField"},
    {5,"DarkWood"},{6,"BlackMarsh"},{7,"TamoeHighland"},{8,"DenOfEvil"},
    {9,"CaveLevel1"},{10,"UndergroundPassageLevel1"},{11,"HoleLevel1"},
    {12,"PitLevel1"},{13,"CaveLevel2"},{14,"UndergroundPassageLevel2"},
    {15,"HoleLevel2"},{16,"PitLevel2"},{17,"BurialGrounds"},{18,"Crypt"},
    {19,"Mausoleum"},{20,"ForgottenTower"},{21,"TowerCellarLevel1"},
    {22,"TowerCellarLevel2"},{23,"TowerCellarLevel3"},{24,"TowerCellarLevel4"},
    {25,"TowerCellarLevel5"},{26,"MonasteryGate"},{27,"OuterCloister"},
    {28,"Barracks"},{29,"JailLevel1"},{30,"JailLevel2"},{31,"JailLevel3"},
    {32,"InnerCloister"},{33,"Cathedral"},{34,"CatacombsLevel1"},
    {35,"CatacombsLevel2"},{36,"CatacombsLevel3"},{37,"CatacombsLevel4"},
    {38,"Tristram"},{39,"CowLevel"},
    {40,"LutGholein"},{41,"RockyWaste"},{42,"DryHills"},{43,"FarOasis"},
    {44,"LostCity"},{45,"ValleyOfSnakes"},{46,"CanyonOfTheMagi"},
    {47,"SewersLevel1Act2"},{48,"SewersLevel2Act2"},{49,"SewersLevel3Act2"},
    {50,"HaremLevel1"},{51,"HaremLevel2"},{52,"PalaceCellarLevel1"},
    {53,"PalaceCellarLevel2"},{54,"PalaceCellarLevel3"},{55,"StonyTombLevel1"},
    {56,"HallsOfTheDeadLevel1"},{57,"HallsOfTheDeadLevel2"},
    {58,"ClawViperTempleLevel1"},{59,"StonyTombLevel2"},
    {60,"HallsOfTheDeadLevel3"},{61,"ClawViperTempleLevel2"},
    {62,"MaggotLairLevel1"},{63,"MaggotLairLevel2"},{64,"MaggotLairLevel3"},
    {65,"AncientTunnels"},{66,"TalRashasTomb1"},{67,"TalRashasTomb2"},
    {68,"TalRashasTomb3"},{69,"TalRashasTomb4"},{70,"TalRashasTomb5"},
    {71,"TalRashasTomb6"},{72,"TalRashasTomb7"},{73,"DurielsLair"},
    {74,"ArcaneSanctuary"},
    {75,"KurastDocks"},{76,"SpiderForest"},{77,"GreatMarsh"},{78,"FlayerJungle"},
    {79,"LowerKurast"},{80,"KurastBazaar"},{81,"UpperKurast"},
    {82,"KurastCauseway"},{83,"Travincal"},{84,"SpiderCave"},{85,"SpiderCavern"},
    {86,"SwampyPitLevel1"},{87,"SwampyPitLevel2"},{88,"FlayerDungeonLevel1"},
    {89,"FlayerDungeonLevel2"},{90,"SwampyPitLevel3"},{91,"FlayerDungeonLevel3"},
    {92,"Sewers2Level1"},{93,"Sewers2Level2"},{94,"RuinedTemple"},
    {95,"DisusedFane"},{96,"ForgottenReliquary"},{97,"ForgottenTemple"},
    {98,"RuinedFane"},{99,"DisusedReliquary"},{100,"DuranceOfHateLevel1"},
    {101,"DuranceOfHateLevel2"},{102,"DuranceOfHateLevel3"},
    {103,"PandemoniumFortress"},{104,"OuterSteppes"},{105,"PlainsOfDespair"},
    {106,"CityOfTheDamned"},{107,"RiverOfFlame"},{108,"ChaosSanctuary"},
    {109,"Harrogath"},{110,"BloodyFoothills"},{111,"FrigidHighlands"},
    {112,"ArreatPlateau"},{113,"CrystalizedCavernLevel1"},{114,"CellarOfPity"},
    {115,"CrystalizedCavernLevel2"},{116,"EchoChamber"},{117,"FrozenTundra"},
    {118,"CrystallinePassage"},{119,"GlacialTrail"},{120,"TheAncientsWay"},
    {121,"NihlathaksTemple"},{122,"HallsOfAnguish"},{123,"HallsOfPain"},
    {124,"HallsOfVaught"},{125,"Hell1"},{126,"Hell2"},{127,"Hell3"},
    {128,"WorldstoneKeepLevel1"},{129,"WorldstoneKeepLevel2"},
    {130,"WorldstoneKeepLevel3"},{131,"ThroneOfDestruction"},
    {132,"TheWorldstoneChamber"},
};

const char* GetLevelName(uint32_t id) {
    for (const auto& n : kLevelNames)
        if (n.id == id) return n.name;
    return "Unknown";
}

const char* GetLevelDisplayName(uint32_t id) {
    return MapData::LevelDisplayName(id).c_str();
}

Act* SafeLoadAct(ActId actNo, uint32_t seed, uint32_t difficulty, LevelId townLevel) {
    Act* a = nullptr;
    __try {
        a = D2_LoadAct(ActNo(actNo), seed, difficulty, LvlId(townLevel));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        a = nullptr;
    }
    return a;
}

EvalResult EvalTells(uint32_t seed,
                     const FilterMap& filter,
                     const std::vector<const MapData::Tell*>& extra) {
    EvalResult r;

    // Closure: input tells plus all transitive prereqs.
    auto addClosure = [](const MapData::Tell* t,
                         std::unordered_set<const MapData::Tell*>& out,
                         auto& self) -> void {
        if (!out.insert(t).second) return;
        for (const auto& req : t->prereqs)
            if (const auto* dep = MapData::ResolvePrereq(req)) self(dep, out, self);
    };

    std::unordered_set<const MapData::Tell*> filterCl, extraCl;
    for (const auto& kv : filter)       addClosure(kv.first, filterCl, addClosure);
    for (const auto* t  : extra)        addClosure(t,        extraCl,  addClosure);

    // Per-act, topo-sorted: filter closure first, then extras not already
    // covered by the filter closure. Keeps phase-1 short-circuit fast.
    std::vector<const MapData::Tell*> filterByAct[5];
    std::vector<const MapData::Tell*> extraByAct[5];
    bool anyFilterInAct[5] = {};

    for (const auto* t : MapData::AllTellsTopoSorted()) {
        const uint32_t i = ActNo(t->actNo);
        if (i >= 5) continue;
        if (filterCl.count(t)) {
            filterByAct[i].push_back(t);
            anyFilterInAct[i] = true;
        } else if (extraCl.count(t)) {
            extraByAct[i].push_back(t);
        }
    }

    // Evaluate one tell. Prereqs are already in r.values (topo order).
    //   any prereq value == "ERROR" -> "ERROR" (chain is broken)
    //   any prereq value not in allowed -> "?"  (this tell is invalid)
    //   otherwise -> compute
    auto evalOne = [&](const MapData::Tell* t, MapData::TellContext& ctx) {
        bool sawError = false;
        bool invalid  = false;
        for (const auto& req : t->prereqs) {
            const auto* dep = MapData::ResolvePrereq(req);
            auto it = r.values.find(dep);
            if (it == r.values.end()) { sawError = true; continue; }  // topo bug
            if (it->second.value == "ERROR") { sawError = true; continue; }
            bool matched = false;
            for (const auto& v : req.allowedValues)
                if (it->second.value == v) { matched = true; break; }
            if (!matched) { invalid = true; break; }
        }
        if      (sawError) r.values[t] = "ERROR";
        else if (invalid)  r.values[t] = "?";
        else               r.values[t] = t->compute(ctx);
    };

    auto checkFilter = [&](const MapData::Tell* t) {
        auto it = filter.find(t);
        if (it == filter.end()) return true;
        return it->second.count(r.values[t].value) != 0;
    };

    // Phase 1: acts that carry at least one filter tell. Compute filter closure
    // first (short-circuiting on mismatch), then extras in the same load.
    for (ActId a : kAllActs) {
        const uint32_t i = ActNo(a);
        if (!r.pass) break;
        if (!anyFilterInAct[i]) continue;

        Act* pAct = SafeLoadAct(a, seed, /*difficulty=*/0, kTownLevels[i]);
        if (!pAct) {
            for (auto* t : filterByAct[i]) {
                r.values[t] = "ERROR";
                if (!checkFilter(t)) { r.pass = false; break; }
            }
            if (r.pass) {
                for (auto* t : extraByAct[i]) r.values[t] = "ERROR";
                extraByAct[i].clear();
            }
            continue;
        }
        MapData::TellContext ctx(pAct, a, seed);

        for (auto* t : filterByAct[i]) {
            evalOne(t, ctx);
            if (!checkFilter(t)) { r.pass = false; break; }
        }
        if (r.pass) {
            for (auto* t : extraByAct[i]) evalOne(t, ctx);
            extraByAct[i].clear();
        }

        D2_UnloadAct(pAct);
    }

    if (!r.pass) return r;

    // Phase 2: acts with only extras (no filter touched them).
    for (ActId a : kAllActs) {
        const uint32_t i = ActNo(a);
        if (extraByAct[i].empty()) continue;
        Act* pAct = SafeLoadAct(a, seed, /*difficulty=*/0, kTownLevels[i]);
        if (!pAct) {
            for (auto* t : extraByAct[i]) r.values[t] = "ERROR";
            continue;
        }
        MapData::TellContext ctx(pAct, a, seed);
        for (auto* t : extraByAct[i]) evalOne(t, ctx);
        D2_UnloadAct(pAct);
    }
    return r;
}

bool ParseSeedRange(int argc, char** argv, int seedIdx, CommonArgs& out) {
    if (seedIdx + 1 >= argc) {
        fprintf(stderr, "Expected <startSeed> <endSeed>\n");
        return false;
    }
    out.startSeed = static_cast<uint32_t>(strtoul(argv[seedIdx],     nullptr, 0));
    out.endSeed   = static_cast<uint32_t>(strtoul(argv[seedIdx + 1], nullptr, 0));
    if (out.endSeed < out.startSeed) {
        fprintf(stderr, "endSeed (%u) must be >= startSeed (%u)\n",
                out.endSeed, out.startSeed);
        return false;
    }
    return true;
}

bool ParseLevelList(const char* s, std::unordered_set<uint32_t>& out) {
    while (*s) {
        char* end;
        unsigned long v = strtoul(s, &end, 0);
        if (end == s) return false;
        out.insert(static_cast<uint32_t>(v));
        s = end;
        while (*s == ',' || *s == ' ') ++s;
    }
    return !out.empty();
}

bool ParseFilterArg(const char* s, FilterMap& filter) {
    // Supports two forms, freely mixed with commas:
    //   "Tell=V1,V2"           — one tell, OR-values
    //   "Tell1=V1,Tell2=V2"    — multiple tells in one arg
    // A token is a new tell when it contains '='; otherwise it's an OR-value
    // for the most-recently seen tell.
    const MapData::Tell* curTell = nullptr;
    std::unordered_set<std::string>* curDest = nullptr;

    const char* p = s;
    while (*p) {
        while (*p == ',' || *p == ' ') ++p;
        if (!*p) break;

        const char* tokStart = p;
        while (*p && *p != ',' && *p != ' ') ++p;
        std::string tok(tokStart, p);

        const auto eqPos = tok.find('=');
        if (eqPos != std::string::npos && eqPos > 0) {
            std::string key = tok.substr(0, eqPos);
            std::string val = tok.substr(eqPos + 1);
            curTell = MapData::FindTell(key);
            if (!curTell) { fprintf(stderr, "Unknown tell name in --filter: %s\n", key.c_str()); return false; }
            curDest = &filter[curTell];
            if (!val.empty()) curDest->insert(val);
        } else {
            if (!curTell) { fprintf(stderr, "Expected Tell=Value in --filter: %s\n", s); return false; }
            if (!tok.empty()) curDest->insert(tok);
        }
    }
    return !filter.empty();
}

bool ParseTellList(const char* s, std::vector<const MapData::Tell*>& out) {
    while (*s) {
        while (*s == ',' || *s == ' ') ++s;
        if (!*s) break;
        std::string name;
        while (*s && *s != ',' && *s != ' ') name.push_back(*s++);
        if (name.empty()) continue;
        const MapData::Tell* t = MapData::FindTell(name);
        if (!t) { fprintf(stderr, "Unknown tell: %s\n", name.c_str()); return false; }
        out.push_back(t);
    }
    return !out.empty();
}

static bool LoadSeedFilter(const char* path, CommonArgs& out) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open --seed-filter file: %s\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fileSize < 0 || (fileSize % 4) != 0) {
        fprintf(stderr, "Seed filter size (%ld) is not a multiple of 4\n", fileSize);
        fclose(f);
        return false;
    }
    const size_t total = static_cast<size_t>(fileSize) / 4;

    out.seedFilter.assign(out.endSeed - out.startSeed + 1, false);

    constexpr size_t kBatch = 65536;
    uint32_t buf[kBatch];
    size_t   inRange = 0;
    for (;;) {
        const size_t n = fread(buf, sizeof(uint32_t), kBatch, f);
        if (n == 0) break;
        for (size_t i = 0; i < n; ++i) {
            const uint32_t v = buf[i];
            if (v >= out.startSeed && v <= out.endSeed) {
                out.seedFilter[v - out.startSeed] = true;
                ++inRange;
            }
        }
    }
    fclose(f);

    printf("Seed filter: %zu entries loaded, %zu within [%u, %u]\n",
           total, inRange, out.startSeed, out.endSeed);
    return true;
}

bool TryParseCommonOption(int& i, int argc, char** argv, CommonArgs& out, bool& ok) {
    if (strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
        out.gameDir = argv[++i]; return true;
    }
    if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
        out.outDir = argv[++i]; return true;
    }
    if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
        ok = ParseFilterArg(argv[++i], out.filter);
        return true;
    }
    if (strcmp(argv[i], "--seed-filter") == 0 && i + 1 < argc) {
        ok = LoadSeedFilter(argv[++i], out);
        return true;
    }
    return false;
}

bool InitD2(const CommonArgs& args) {
    printf("D2 Map Dump (1.13c)\n");
    printf("Seeds:    %u .. %u (%u seeds)\n",
           args.startSeed, args.endSeed, args.endSeed - args.startSeed + 1);
    printf("Game dir: %s\n", args.gameDir.c_str());
    printf("Out dir:  %s\n\n", args.outDir.c_str());
    if (!D2_Initialize(args.gameDir.c_str(), /*verbose=*/true)) {
        printf("DLL initialization failed.\n");
        return false;
    }
    MapData::LoadStringTbl(/*verbose=*/true);
    MapData::LoadLevelsDb(/*verbose=*/true);
    MapData::LoadObjectsDb(/*verbose=*/true);
    MapData::LoadMonStatsDb(/*verbose=*/true);
    CreateDirectoryA(args.outDir.c_str(), nullptr);
    return true;
}

bool ReadSeedsFromCsv(const char* path, std::vector<uint32_t>& out) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open --seed-csv file: %s\n", path);
        return false;
    }
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char* end = nullptr;
        const unsigned long v = strtoul(line, &end, 10);
        // Reject lines where the first column isn't a clean integer
        // (covers headers like "Seed,...", blank lines, comments).
        if (end == line) continue;
        if (*end != ',' && *end != '\n' && *end != '\r' && *end != '\0') continue;
        out.push_back(static_cast<uint32_t>(v));
    }
    fclose(f);
    if (out.empty()) {
        fprintf(stderr, "--seed-csv %s contains no parseable seeds\n", path);
        return false;
    }
    return true;
}

ProgressReporter::ProgressReporter(uint32_t start, uint32_t end)
    : startSeed(start), endSeed(end),
      t0(std::chrono::steady_clock::now()),
      tBatch(t0), batchBase(start) {}

void ProgressReporter::update(uint32_t seed) {
    if (((seed - startSeed + 1) % kBatch) != 0 && seed != endSeed) return;
    const auto     now      = std::chrono::steady_clock::now();
    const double   batchSec = std::chrono::duration<double>(now - tBatch).count();
    const double   totalSec = std::chrono::duration<double>(now - t0).count();
    const uint32_t batchN   = seed + 1 - batchBase;
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    const double rssMB = pmc.WorkingSetSize / (1024.0 * 1024.0);
    printf("  [progress] %u seeds, %.1fs total | last %u: %.0f s/s | RSS %.1f MB\n",
           seed - startSeed + 1, totalSec, batchN,
           batchSec > 0 ? batchN / batchSec : 0.0, rssMB);
    tBatch    = now;
    batchBase = seed + 1;
}

double ProgressReporter::elapsedSeconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
