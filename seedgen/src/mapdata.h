#pragma once
#include <cstdint>
#include <vector>
#include "d2levels.h"

// =============================================================================
// Internal map representation — produced by the extractor, consumed by the
// renderer. Intentionally has no D2 dependency so it can be serialized to a
// database / file format in the future without dragging in the game DLLs.
// =============================================================================

namespace MapData {

enum class PresetType : uint16_t {
    NPC    = 1,
    Object = 2,
    Exit   = 5,
};

// Coarse categorization of objects, resolved from objects.txt at extraction
// time. Stored on Preset so LevelMap stays self-contained (no need to keep
// objects.txt around when rendering or serializing).
enum class ObjectKind : uint8_t {
    Generic = 0,
    Waypoint,
    Shrine,
    Well,
    SuperChest,
    Chest,
    Door,
    Stairs,     // map-tile exits (PresetType::Exit) — also flagged here
    Quest,
};

struct Preset {
    int16_t    x = 0, y = 0;       // tile coords, level-local
    uint16_t   type = 0;           // PresetType enum value
    ObjectKind kind = ObjectKind::Generic;  // populated for type==Object/Exit
    uint32_t   txtFileNo = 0;      // npcs.txt / objects.txt row index
    uint32_t   destLevelNo = 0;    // exits (type=5) only; 0 otherwise
};

// One entry per Room2-to-Room2 connection that crosses a level boundary.
// Discovered from Room2->pRoom2Near. Multiple entries can target the same
// levelNo when several of our rooms touch their level (e.g. a wilderness
// border can span many rooms). bridgeX/Y is the center of our Room2 in
// level-local tile coords — averaging these gives the connection's centroid.
struct AdjacentArea {
    uint32_t levelNo  = 0;
    int16_t  bridgeX  = 0;
    int16_t  bridgeY  = 0;
};

// Sentinel for "no collision data at this tile" — distinguishable from any
// real collision flag value (real flags fit in lower bits).
constexpr uint16_t kNoData = 0xFFFF;

// Collision WORD bit flags. Names and values from d2inject's LevelMap.h.
//  Plain enum so values implicitly convert to uint16_t for bitwise ops
//  on `coll[]`.
enum CollisionFlag : uint16_t {
    BlockWalk        = 0x0001,
    BlockLineOfSight = 0x0002,
    Wall             = 0x0004,
    BlockPlayer      = 0x0008,
    AlternateTile    = 0x0010,
    Blank            = 0x0020,
    Missile          = 0x0040,
    Player           = 0x0080,
    NPCLocation      = 0x0100,
    Item             = 0x0200,
    Object           = 0x0400,
    ClosedDoor       = 0x0800,
    NPCCollision     = 0x1000,
};

struct Room {
    int32_t  x = 0, y = 0;        // tile coords, level-local
    int32_t  sizeX = 0, sizeY = 0;
    uint32_t roomNumber = 0;       // rooms.txt row index
    uint32_t subNumber  = 0;       // variant index within that row
};

struct LevelMap {
    uint32_t levelNo = 0;
    ActId    actNo   = ActId::Act1;

    // Level origin and dimensions in global tile coords (each = dwPosX*5 etc.)
    int32_t  originX = 0, originY = 0;
    int32_t  sizeX   = 0, sizeY   = 0;

    // Tight bbox of actually-painted collision tiles, level-local.
    // Set to {0,0,0,0} if no tiles painted.
    int32_t  tightMinX = 0, tightMinY = 0;
    int32_t  tightMaxX = 0, tightMaxY = 0;

    // Collision grid, row-major: coll[y * sizeX + x]. kNoData = unpainted.
    std::vector<uint16_t> coll;

    // NPCs, objects, exits.
    std::vector<Preset> presets;

    std::vector<AdjacentArea> adjacents;
    std::vector<Room>         rooms;

    bool empty() const { return coll.empty(); }
};

} // namespace MapData
