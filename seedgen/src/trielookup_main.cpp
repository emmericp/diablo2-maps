#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstring>
#include "trielookup.h"

static void PrintUsage() {
    printf("Usage: trielookup.exe <command> [options]\n\n");
    printf("Commands:\n");
    printf("  buildindex   Build a sorted (sequence, seed) index file\n");
    printf("  lookup       Query a prefix in a buildindex output file\n");
    printf("  collisions   Scan an index for runs of identical sequences\n");
    printf("  decode       Decode raw uint16 encoded values via the TowerRoom table\n\n");
    printf("Run 'trielookup.exe <command>' with no further args for command-specific help.\n");
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { PrintUsage(); return 1; }

    if (strcmp(argv[1], "buildindex") == 0) {
        BuildIndexArgs args;
        if (!ParseBuildIndexArgs(argc, argv, args)) { PrintBuildIndexUsage(); return 1; }
        return RunBuildIndex(args);
    }
    if (strcmp(argv[1], "lookup") == 0) {
        LookupArgs args;
        if (!ParseLookupArgs(argc, argv, args)) { PrintLookupUsage(); return 1; }
        return RunLookup(args);
    }
    if (strcmp(argv[1], "collisions") == 0) {
        CollisionsArgs args;
        if (!ParseCollisionsArgs(argc, argv, args)) { PrintCollisionsUsage(); return 1; }
        return RunCollisions(args);
    }
    if (strcmp(argv[1], "decode") == 0) {
        return RunDecode(argc, argv);
    }

    fprintf(stderr, "Unknown command: %s\n\n", argv[1]);
    PrintUsage();
    return 1;
}
