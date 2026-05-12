#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "d2loader.h"
#include "mapdump_full.h"
#include "mapdump_levelgraph.h"
#include "mapdump_roomatlas.h"
#include "mapdump_server.h"
#include "mapdump_summary.h"

static const char* kDefaultGameDir = "C:\\Program Files (x86)\\Diablo II";

static void PrintUsage() {
    printf("Usage: mapdump.exe <command> [options]\n\n");
    printf("Commands:\n");
    printf("  dump       <startSeed> <endSeed>  Dump per-seed level data as JSON\n");
    printf("  summary    <startSeed> <endSeed>  Export per-seed tells as a CSV\n");
    printf("  levelgraph <startSeed> <endSeed>  Export one level's room layout as packed binary\n");
    printf("  roomatlas  <startSeed> <endSeed>  Capture canonical tile examples per (roomId,variant)\n");
    printf("  graphdecode <file> <seed-offset>  Decode one record from a levelgraph .bin file\n");
    printf("  server                            Serve seeds from stdin -> stdout\n");
    printf("  extract    <mpq-path>             Dump a file from the D2 MPQ\n\n");
    printf("Run 'mapdump.exe <command>' with no seeds for command-specific help.\n");
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc < 2) { PrintUsage(); return 1; }

    if (strcmp(argv[1], "dump") == 0) {
        DumpArgs args;
        if (!ParseDumpArgs(argc, argv, args)) { PrintDumpUsage(); return 1; }
        return RunDump(args);
    }
    if (strcmp(argv[1], "summary") == 0) {
        SummaryArgs args;
        if (!ParseSummaryArgs(argc, argv, args)) { PrintSummaryUsage(); return 1; }
        return RunSummary(args);
    }
    if (strcmp(argv[1], "levelgraph") == 0) {
        LevelGraphArgs args;
        if (!ParseLevelGraphArgs(argc, argv, args)) { PrintLevelGraphUsage(); return 1; }
        return RunLevelGraph(args);
    }
    if (strcmp(argv[1], "roomatlas") == 0) {
        RoomAtlasArgs args;
        if (!ParseRoomAtlasArgs(argc, argv, args)) { PrintRoomAtlasUsage(); return 1; }
        return RunRoomAtlas(args);
    }
    if (strcmp(argv[1], "graphdecode") == 0) {
        if (argc < 4) { PrintGraphDecodeUsage(); return 1; }
        const char* file = argv[2];
        const uint32_t off = static_cast<uint32_t>(atoi(argv[3]));
        return RunGraphDecode(file, off);
    }
    if (strcmp(argv[1], "server") == 0) {
        return RunServer(argc, argv);
    }
    if (strcmp(argv[1], "extract") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: mapdump.exe extract <mpq-path> [--game <dir>] [--out <file>]\n");
            return 1;
        }
        const char* mpqPath  = argv[2];
        std::string gameDir  = kDefaultGameDir;
        std::string outFile;

        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
                gameDir = argv[++i];
            } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
                outFile = argv[++i];
            } else {
                fprintf(stderr, "Unknown arg: %s\n", argv[i]); return 1;
            }
        }

        // Default output: basename of the MPQ-internal path in the cwd.
        if (outFile.empty()) {
            const char* slash = strrchr(mpqPath, '\\');
            if (!slash) slash = strrchr(mpqPath, '/');
            outFile = slash ? slash + 1 : mpqPath;
        }

        if (!D2_Initialize(gameDir.c_str(), /*verbose=*/true)) {
            fprintf(stderr, "D2_Initialize failed\n"); return 1;
        }
        std::vector<uint8_t> data;
        if (!D2_ReadMpqFile(mpqPath, data)) {
            fprintf(stderr, "D2_ReadMpqFile failed for: %s\n", mpqPath); return 1;
        }
        FILE* f = fopen(outFile.c_str(), "wb");
        if (!f) { fprintf(stderr, "Cannot write: %s\n", outFile.c_str()); return 1; }
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
        fprintf(stderr, "Wrote %zu bytes to %s\n", data.size(), outFile.c_str());
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n\n", argv[1]);
    PrintUsage();
    return 1;
}
