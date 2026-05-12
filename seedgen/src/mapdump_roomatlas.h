#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "mapdump_common.h"

// "roomatlas" subcommand — scan a seed range over one or more tower levels,
// classify each room (TowerRoom::FromGame), and collect a canonical example
// of every (roomId, variant) combo whose graves bitmask == 0. For each
// collected example we serialize the room's 40×40 collision tiles (zlib-
// deflated, base64-encoded — same encoding as `mapdump dump`) into a JSON
// atlas the lookup UI can render.
//
// Stops early as soon as every known (roomId, variant) has been collected.

struct RoomAtlasArgs : CommonArgs {
    // One or more tower level ids to scan. Default: 21..24.
    std::vector<uint32_t> levelIds;
    std::string           outFile = "atlas.json";
};

bool ParseRoomAtlasArgs(int argc, char** argv, RoomAtlasArgs& out);
void PrintRoomAtlasUsage();
int  RunRoomAtlas(const RoomAtlasArgs& args);
