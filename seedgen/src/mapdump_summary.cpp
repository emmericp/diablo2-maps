#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include "mapdump_summary.h"

void PrintSummaryUsage() {
    printf("Usage: mapdump.exe summary <startSeed> <endSeed> [options]\n");
    printf("  Computes tells for each seed and writes a CSV.\n");
    printf("Options:\n");
    printf("  --game <dir>      D2 install dir (default: C:\\Program Files (x86)\\Diablo II)\n");
    printf("  --out  <dir>      output root (default: ./out)\n");
    printf("  --filter T=V[,V]  skip seeds whose tell T doesn't match value V\n");
    printf("  --seed-filter F   binary file of uint32 LE seeds; only these are processed\n");
    printf("  --tells T1,T2,... tells to include as CSV columns (default: all)\n");
    printf("  --tell-levels L1,L2,... restrict tells to those touching any of these level ids\n");
    printf("Output: <outDir>/tells.csv\n");
    printf("Registered tells:");
    for (const auto* t : MapData::AllTells()) printf(" %s", t->name);
    printf("\n");
}

bool ParseSummaryArgs(int argc, char** argv, SummaryArgs& out) {
    if (argc < 4) return false;
    if (!ParseSeedRange(argc, argv, 2, out)) return false;
    for (int i = 4; i < argc; ++i) {
        bool ok = true;
        if (TryParseCommonOption(i, argc, argv, out, ok)) {
            if (!ok) return false;
        } else if (strcmp(argv[i], "--tells") == 0 && i + 1 < argc) {
            if (!ParseTellList(argv[++i], out.tells)) return false;
        } else if (strcmp(argv[i], "--tell-levels") == 0 && i + 1 < argc) {
            if (!ParseLevelList(argv[++i], out.tellLevels)) {
                fprintf(stderr, "Bad --tell-levels list\n"); return false;
            }
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]); return false;
        }
    }
    return true;
}

int RunSummary(const SummaryArgs& args) {
    if (!InitD2(args)) return 1;

    std::vector<const MapData::Tell*> exportTells =
        args.tells.empty() ? MapData::AllTells() : args.tells;
    if (!args.tellLevels.empty()) {
        std::vector<const MapData::Tell*> filtered;
        for (const auto* t : exportTells) {
            for (const auto lvl : t->levels) {
                if (args.tellLevels.count(LvlId(lvl))) { filtered.push_back(t); break; }
            }
        }
        exportTells = std::move(filtered);
        if (exportTells.empty()) {
            fprintf(stderr, "No tells touch the requested levels\n");
            return 1;
        }
    }

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\tells.csv", args.outDir.c_str());
    FILE* csv = fopen(path, "w");
    if (!csv) {
        fprintf(stderr, "Cannot open %s for writing\n", path);
        return 1;
    }

    fprintf(csv, "Seed");
    for (auto* t : exportTells) fprintf(csv, ",%s", t->name);
    fprintf(csv, "\n");
    printf("Export: writing %s with %zu tell column%s\n",
           path, exportTells.size(), exportTells.size() == 1 ? "" : "s");

    uint32_t skipped = 0;
    ProgressReporter progress(args.startSeed, args.endSeed);

    for (uint32_t seed = args.startSeed; seed <= args.endSeed; ++seed) {
        if (!SeedAllowed(args, seed)) { ++skipped; progress.update(seed); continue; }
        EvalResult ev = EvalTells(seed, args.filter, exportTells);
        if (!ev.pass) { ++skipped; progress.update(seed); continue; }

        fprintf(csv, "%u", seed);
        for (auto* t : exportTells) {
            auto it = ev.values.find(t);
            fprintf(csv, ",%s", (it != ev.values.end()) ? it->second.value.c_str() : "?");
        }
        fprintf(csv, "\n");
        progress.update(seed);
    }
    fclose(csv);

    const double   elapsed = progress.elapsedSeconds();
    const uint32_t total   = args.endSeed - args.startSeed + 1;
    const uint32_t matched = total - skipped;
    printf("\nDone. %u seed%s exported in %.1f seconds (%.0f seeds/second)",
           matched, matched == 1 ? "" : "s",
           elapsed, elapsed > 0 ? total / elapsed : 0.0);
    if (skipped) printf(", %u skipped by filter", skipped);
    printf(".\n");
    return 0;
}
