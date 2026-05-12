#include "tells.h"
#include "act1_wild_rooms.h"
#include <cstdint>
#include <cstdlib>

// =============================================================================
// Room-based tells. Wilderness levels are tiled into rooms; each room cell
// carries a `roomNumber` (rooms.txt row index) that fixes the visual
// template painted there. The unified entry point is `ClassifyAct1WildRoom`:
// given a Room and its LevelMap, it returns an `Act1WildRoom` enum value
// bundling the template AND its variant (e.g. HouseBedSouth, StonesKidney).
//
// One enum covers Act 1 wilderness (BloodMoor, ColdPlains, StonyField, ...)
// since rooms.txt is global. Future acts get their own enums.
//
// Compute functions are thin wrappers: resolve (x, y) → Room → classify →
// stringify. Shrine detection (which also covers wells — they're treated
// as another shrine variant) stays separate since it applies to *any* room
// template.
// =============================================================================
//
// --- Act 1 wilderness room templates ---
//
//   roomNo  feature                          variants
//   ------  -------------------------------  ------------------------------------
//   29      stone formation                  Circle / Cross / Kidney  (by AltTile area)
//   44      large fallen camp                —
//   46      small lake with tree reflection  —
//   47      house                            BedSouth / BedEastNoPorch / BedEastPorch
//                                              / Small / Cow / Fallens (by preset signature)
//   48      house (variants TBD)             —
//   49      burning/overrun house            —
//   30      visually ambiguous               folded into Other
//
//   Stairs preset → destLevelNo decides; e.g. CaveLevel1 → CaveLevel1Stairs{A,B}
//   (overrides whatever roomNo classification would have said). The A/B split
//   on the cave entrance comes from the exit preset's txtFileNo (2 vs 3),
//   which selects between two visually distinct stair tiles.

namespace MapData {

enum class Act1WildRoom : uint8_t {
    Other = 0,
    HouseBedSouth,
    HouseBedEastNoPorch,
    HouseBedEastPorch,
    HouseSmall,
    HouseCow,
    HouseFallens,
    HouseUnknown,        // roomNo 47 but no recognised preset signature
    StonesCircle,
    StonesCross,
    StonesKidney,
    StonesOther,         // roomNo 29 but tile count outside known bands
    Lake,
    LargeFallenCamp,
    // roomNo 48 — a smaller house template than 47, with its own preset
    // signature taxonomy. Same bed-x-position split as roomNo 47.
    House48BedSouth,     // bed 247 at rel x < 21
    House48BedEast,      // bed 247 at rel x >= 21
    House48Rogue,        // npc 817 + RogueCorpse (obj 55) — abandoned/overrun
    House48Stable,       // chest 240, no bed — stable variant
    House48Empty,        // no presets at all — small empty house
    House48Other,        // roomNo 48 with unrecognised preset signature
    HouseBurning,
    // Cave Level 1 stairs cell, sub-typed by the stairs preset's txtFileNo
    // using the same vocabulary as DenOfEvilEntrance and the new
    // UndergroundPassageEntrance — see StairsStyleName.
    CaveLevel1StairsInWall,   // txtFileNo == 1
    CaveLevel1StairsEast,     // txtFileNo == 2
    CaveLevel1StairsSouth,    // txtFileNo == 3
    CaveLevel1StairsOther,    // anything else
    // Wilderness shrine cells. Empirically (across 2348 BloodMoor seeds,
    // 11,740 shrine/well instances) shrines only spawn in roomNo 0 — never
    // inside any classified template — so they fold cleanly into the
    // existing room enum and players can spot one without exploring the
    // rest of the level. Sub-typed by the shrine preset's txtFileNo
    // (matches the ShrineAt helper's vocabulary).
    Shrine2,                  // txtFileNo == 2
    Shrine81,                 // txtFileNo == 81
    Shrine83,                 // txtFileNo == 83
    Shrine84,                 // txtFileNo == 84
    ShrineOther,              // unknown shrine txtFileNo
    Well,                     // ObjectKind::Well (txtFileNo typically 130)
};

// Stairs/exit-preset style shared by every wilderness→dungeon entrance.
// The exit preset's txtFileNo selects which visual tile renders the stairs:
//   1 → in-wall stairs (set into a wall face)
//   2 → east-facing stairs (open ground, opening points east)
//   3 → south-facing stairs (open ground, opening points south)
// Anything else folds to "Other".
const char* StairsStyleName(int txtFileNo);

// Look up the first map-tile-exit preset in `m` whose destination is
// `destLevel`. Returns false if none. On success, fills outTxt with the
// preset's txtFileNo (input to StairsStyleName) and outX/outY with its
// level-local position.
bool FindStairsTo(const LevelMap& m, LevelId destLevel,
                  int& outTxt, int& outX, int& outY);

// Leftmost gate tile in Blood Moor local coords (see definition in
// tells.cpp). Used by the town-front room tells below to anchor their
// sample points to the gate regardless of which Blood Moor layout (W or N
// exit toward Cold Plains) the seed produced.
bool BloodMoorCampGate(TellContext& ctx, int& gx, int& gy);

namespace {

constexpr int kRoomSize = 40;

// Find the room enclosing level-local (x, y). Returns nullptr if no room
// covers that point — typical for "missing" corner cells.
const Room* FindRoomAt(const LevelMap& m, int x, int y) {
    for (const auto& r : m.rooms)
        if (x >= r.x && x < r.x + r.sizeX
            && y >= r.y && y < r.y + r.sizeY)
            return &r;
    return nullptr;
}

// Preset signature inside a House (roomNo 47). objects.txt rows 247 and 248
// are both "bed"; bed 248 is exclusive to BedEastPorch. The other variants
// either have bed 247 (split by its relative x) or have no bed and a
// distinctive other preset.
struct HousePresets {
    bool hasBed247   = false;
    bool hasBed248   = false;
    bool hasChest    = false;   // obj 241
    bool hasCow      = false;   // npc 179
    bool hasFallens  = false;   // npc 817
    int  bed247xRel  = 0;       // bed 247 x coord, relative to room origin
};

HousePresets ScanHousePresets(const LevelMap& m, const Room& r) {
    HousePresets h{};
    for (const auto& p : m.presets) {
        if (p.x < r.x || p.x >= r.x + r.sizeX) continue;
        if (p.y < r.y || p.y >= r.y + r.sizeY) continue;
        if (p.type == uint16_t(PresetType::Object)) {
            switch (p.txtFileNo) {
                case 247: h.hasBed247 = true; h.bed247xRel = p.x - r.x; break;
                case 248: h.hasBed248 = true;                           break;
                case 241: h.hasChest  = true;                           break;
            }
        } else if (p.type == uint16_t(PresetType::NPC)) {
            if      (p.txtFileNo == 179) h.hasCow     = true;
            else if (p.txtFileNo == 817) h.hasFallens = true;
        }
    }
    return h;
}

// Splits BedSouth (rel x=19) from BedEastNoPorch (rel x=23). Room-relative.
constexpr int kBedEastRelX = 21;

// AltTile-flagged tiles in the room. Stones rooms (roomNo 29) are paved
// with these and the count cleanly distinguishes Circle (~550) / Cross
// (~825) / Kidney (~1025) from a 791-seed sample.
int CountAltTileInRoom(const LevelMap& m, const Room& r) {
    int cnt = 0;
    for (int y = r.y; y < r.y + r.sizeY; ++y) {
        if (y < 0 || y >= m.sizeY) continue;
        for (int x = r.x; x < r.x + r.sizeX; ++x) {
            if (x < 0 || x >= m.sizeX) continue;
            if (m.coll[y * m.sizeX + x] & AlternateTile) ++cnt;
        }
    }
    return cnt;
}

bool RoomHasStairsTo(const LevelMap& m, const Room& r, LevelId destLevel) {
    for (const auto& p : m.presets) {
        if (p.x < r.x || p.x >= r.x + r.sizeX) continue;
        if (p.y < r.y || p.y >= r.y + r.sizeY) continue;
        if (p.kind == ObjectKind::Stairs
            && p.destLevelNo == uint32_t(LvlId(destLevel))) return true;
    }
    return false;
}

// Returns the txtFileNo of the first Stairs preset in `r` going to `destLevel`,
// or -1 if no such preset exists. For Cave entrances this is 2 or 3 in the
// data we've seen, encoding the visual variant of the stair tile.
int RoomStairsTxtFileNoTo(const LevelMap& m, const Room& r, LevelId destLevel) {
    for (const auto& p : m.presets) {
        if (p.x < r.x || p.x >= r.x + r.sizeX) continue;
        if (p.y < r.y || p.y >= r.y + r.sizeY) continue;
        if (p.kind == ObjectKind::Stairs
            && p.destLevelNo == uint32_t(LvlId(destLevel)))
            return static_cast<int>(p.txtFileNo);
    }
    return -1;
}

} // anonymous

Act1WildRoom ClassifyAct1WildRoom(const Room& r, const LevelMap& m) {
    // Cave-exit cells are identified by their Stairs preset's destLevelNo,
    // not by roomNo — the stairs are the ground truth even if the
    // surrounding template changes. The txtFileNo selects which of three
    // visual stair tiles renders (see StairsStyleName).
    const int caveStairsTxt = RoomStairsTxtFileNoTo(m, r, LevelId::CaveLevel1);
    if (caveStairsTxt >= 0) {
        switch (caveStairsTxt) {
            case 1: return Act1WildRoom::CaveLevel1StairsInWall;
            case 2: return Act1WildRoom::CaveLevel1StairsEast;
            case 3: return Act1WildRoom::CaveLevel1StairsSouth;
            default: return Act1WildRoom::CaveLevel1StairsOther;
        }
    }

    switch (r.roomNumber) {
        case 47: {
            const HousePresets h = ScanHousePresets(m, r);
            if (h.hasBed248) return Act1WildRoom::HouseBedEastPorch;
            if (h.hasBed247) return h.bed247xRel < kBedEastRelX
                ? Act1WildRoom::HouseBedSouth
                : Act1WildRoom::HouseBedEastNoPorch;
            if (h.hasFallens) return Act1WildRoom::HouseFallens;
            if (h.hasCow)     return Act1WildRoom::HouseCow;
            if (h.hasChest)   return Act1WildRoom::HouseSmall;
            return Act1WildRoom::HouseUnknown;
        }
        case 29: {
            const int alt = CountAltTileInRoom(m, r);
            constexpr int kTol = 100;
            if (std::abs(alt - 550)  <= kTol) return Act1WildRoom::StonesCircle;
            if (std::abs(alt - 825)  <= kTol) return Act1WildRoom::StonesCross;
            if (std::abs(alt - 1025) <= kTol) return Act1WildRoom::StonesKidney;
            return Act1WildRoom::StonesOther;
        }
        case 46: return Act1WildRoom::Lake;
        case 44: return Act1WildRoom::LargeFallenCamp;
        case 48: {
            // Same bed-x-position split as roomNo 47. Other variants are
            // identified by their distinctive presets:
            //   bed 247       → House48Bed{South,East}
            //   npc 817       → House48Rogue (with RogueCorpse obj 55)
            //   chest 240     → House48Stable (chest as the only preset)
            //   no presets    → House48Empty
            bool hasBed = false, hasRogue = false, hasChest = false;
            bool hasAny = false;
            int  bedXRel = 0;
            for (const auto& p : m.presets) {
                if (p.x < r.x || p.x >= r.x + r.sizeX) continue;
                if (p.y < r.y || p.y >= r.y + r.sizeY) continue;
                hasAny = true;
                if (p.type == uint16_t(PresetType::Object)) {
                    if      (p.txtFileNo == 247) { hasBed = true; bedXRel = p.x - r.x; }
                    else if (p.txtFileNo == 240) { hasChest = true; }
                } else if (p.type == uint16_t(PresetType::NPC)) {
                    if (p.txtFileNo == 817) hasRogue = true;
                }
            }
            if (hasBed)   return bedXRel < kBedEastRelX
                ? Act1WildRoom::House48BedSouth
                : Act1WildRoom::House48BedEast;
            if (hasRogue) return Act1WildRoom::House48Rogue;
            if (hasChest) return Act1WildRoom::House48Stable;
            if (!hasAny)  return Act1WildRoom::House48Empty;
            return Act1WildRoom::House48Other;
        }
        case 49: return Act1WildRoom::HouseBurning;
        default: {
            // Wilderness filler (typically roomNo 0). Check for a shrine
            // or well inside — players spot them without exploring the
            // rest of the level, and they're mutually exclusive with all
            // the named templates above (verified empirically).
            for (const auto& p : m.presets) {
                if (p.x < r.x || p.x >= r.x + r.sizeX) continue;
                if (p.y < r.y || p.y >= r.y + r.sizeY) continue;
                if (p.kind == ObjectKind::Well)   return Act1WildRoom::Well;
                if (p.kind != ObjectKind::Shrine) continue;
                switch (p.txtFileNo) {
                    case 2:  return Act1WildRoom::Shrine2;
                    case 81: return Act1WildRoom::Shrine81;
                    case 83: return Act1WildRoom::Shrine83;
                    case 84: return Act1WildRoom::Shrine84;
                    default: return Act1WildRoom::ShrineOther;
                }
            }
            return Act1WildRoom::Other;
        }
    }
}

const char* Act1WildRoomName(Act1WildRoom r) {
    switch (r) {
        case Act1WildRoom::Other:               return "Other";
        case Act1WildRoom::HouseBedSouth:       return "HouseBedSouth";
        case Act1WildRoom::HouseBedEastNoPorch: return "HouseBedEastNoPorch";
        case Act1WildRoom::HouseBedEastPorch:   return "HouseBedEastPorch";
        case Act1WildRoom::HouseSmall:          return "HouseSmall";
        case Act1WildRoom::HouseCow:            return "HouseCow";
        case Act1WildRoom::HouseFallens:        return "HouseFallens";
        case Act1WildRoom::HouseUnknown:        return "HouseUnknown";
        case Act1WildRoom::StonesCircle:        return "StonesCircle";
        case Act1WildRoom::StonesCross:         return "StonesCross";
        case Act1WildRoom::StonesKidney:        return "StonesKidney";
        case Act1WildRoom::StonesOther:         return "StonesOther";
        case Act1WildRoom::Lake:                return "Lake";
        case Act1WildRoom::LargeFallenCamp:     return "LargeFallenCamp";
        case Act1WildRoom::House48BedSouth:     return "House48BedSouth";
        case Act1WildRoom::House48BedEast:      return "House48BedEast";
        case Act1WildRoom::House48Rogue:        return "House48Rogue";
        case Act1WildRoom::House48Stable:       return "House48Stable";
        case Act1WildRoom::House48Empty:        return "House48Empty";
        case Act1WildRoom::House48Other:        return "House48Other";
        case Act1WildRoom::HouseBurning:          return "HouseBurning";
        case Act1WildRoom::CaveLevel1StairsInWall: return "CaveLevel1StairsInWall";
        case Act1WildRoom::CaveLevel1StairsEast:   return "CaveLevel1StairsEast";
        case Act1WildRoom::CaveLevel1StairsSouth:  return "CaveLevel1StairsSouth";
        case Act1WildRoom::CaveLevel1StairsOther:  return "CaveLevel1StairsOther";
        case Act1WildRoom::Shrine2:                return "Shrine2";
        case Act1WildRoom::Shrine81:               return "Shrine81";
        case Act1WildRoom::Shrine83:               return "Shrine83";
        case Act1WildRoom::Shrine84:               return "Shrine84";
        case Act1WildRoom::ShrineOther:            return "ShrineOther";
        case Act1WildRoom::Well:                   return "Well";
    }
    return "?";
}

const char* StairsStyleName(int txtFileNo) {
    switch (txtFileNo) {
        case 1: return "InWall";
        case 2: return "East";
        case 3: return "South";
        default: return "Other";
    }
}

bool FindStairsTo(const LevelMap& m, LevelId destLevel,
                  int& outTxt, int& outX, int& outY) {
    for (const auto& p : m.presets) {
        if (p.kind != ObjectKind::Stairs) continue;
        if (p.destLevelNo != uint32_t(LvlId(destLevel))) continue;
        outTxt = static_cast<int>(p.txtFileNo);
        outX   = p.x;
        outY   = p.y;
        return true;
    }
    return false;
}

// --- Compute helpers ---

namespace {

// Resolve (x, y) → Room → Act1WildRoom string. Returns "Other" with the
// queried cell as the location when no room covers the point.
TellResult ClassifyRoomAt(const LevelMap& m, LevelId levelId, int x, int y) {
    const Room* r = FindRoomAt(m, x, y);
    if (!r) return { "Other",
                     TellLocation{ levelId, x, y, kRoomSize, kRoomSize } };
    return { Act1WildRoomName(ClassifyAct1WildRoom(*r, m)),
             TellLocation{ levelId, r->x, r->y, r->sizeX, r->sizeY } };
}

// Shrine detector — room-template-agnostic. Wells are treated as another
// shrine variant. Returns one of:
//   None, Well, Shrine2, Shrine81, Shrine83, Shrine84, Multiple
// Shrine{N} carries the shrine's objects.txt row (txtFileNo) as the visual
// variant. Multiple = >1 shrine in the room (wells counted), or a shrine
// with an unrecognised txtFileNo (defensive — empirically all are 2/81/83/84).
TellResult ShrineAt(const LevelMap& m, LevelId levelId, int x, int y) {
    const Room* r = FindRoomAt(m, x, y);
    if (!r) return "None";
    int shrineCount = 0;
    int loneTxt = -1;          // txtFileNo of the sole shrine, if exactly one
    bool loneIsWell = false;
    int  loneX = 0, loneY = 0;
    for (const auto& p : m.presets) {
        if (p.x < r->x || p.x >= r->x + r->sizeX) continue;
        if (p.y < r->y || p.y >= r->y + r->sizeY) continue;
        const bool isShrine = (p.kind == ObjectKind::Shrine);
        const bool isWell   = (p.kind == ObjectKind::Well);
        if (!isShrine && !isWell) continue;
        ++shrineCount;
        if (shrineCount == 1) {
            loneTxt    = static_cast<int>(p.txtFileNo);
            loneIsWell = isWell;
            loneX = p.x; loneY = p.y;
        }
    }
    if (shrineCount == 0) return "None";
    if (shrineCount > 1)
        return { "Multiple", TellLocation{ levelId, r->x, r->y, r->sizeX, r->sizeY } };
    const TellLocation loc{ levelId, loneX, loneY, 0, 0 };
    if (loneIsWell) return { "Well", loc };
    switch (loneTxt) {
        case 2:  return { "Shrine2",  loc };
        case 81: return { "Shrine81", loc };
        case 83: return { "Shrine83", loc };
        case 84: return { "Shrine84", loc };
        default: return { "Multiple", loc };  // unknown shrine variant
    }
}

TellResult BloodMoorClassify(TellContext& ctx, int x, int y) {
    const LevelMap& m = ctx.GetLevel(LevelId::BloodMoor);
    if (m.empty()) return "ERROR";
    return ClassifyRoomAt(m, LevelId::BloodMoor, x, y);
}

TellResult BloodMoorShrine(TellContext& ctx, int x, int y) {
    const LevelMap& m = ctx.GetLevel(LevelId::BloodMoor);
    if (m.empty()) return "ERROR";
    return ShrineAt(m, LevelId::BloodMoor, x, y);
}

} // anonymous

// --- Town-front tells: 3 rooms one cell north of the camp gate. ---
//
// Sample points are anchored to the gate reference (BloodMoorCampGate):
//   Straight: room directly north of the gate room.
//   Left:     one room cell to the west of Straight.
//   Right:    one room cell to the east of Straight.
// The (dx, dy) offsets are the same in both W and N Blood Moor layouts —
// the player exits Rogue Encampment northward in either case.
namespace {

struct GateRoomOffset { int dx, dy; };
constexpr GateRoomOffset kTownStraight { -24, -79 };
constexpr GateRoomOffset kTownLeft     { -64, -79 };
constexpr GateRoomOffset kTownRight    { +16, -79 };

TellResult TownClassifyAt(TellContext& ctx, GateRoomOffset o) {
    int gx, gy;
    if (!BloodMoorCampGate(ctx, gx, gy)) return "ERROR";
    return BloodMoorClassify(ctx, gx + o.dx, gy + o.dy);
}

} // anonymous

TellResult ComputeBloodMoorTownStraight (TellContext& ctx) { return TownClassifyAt(ctx, kTownStraight); }
TellResult ComputeBloodMoorTownLeft     (TellContext& ctx) { return TownClassifyAt(ctx, kTownLeft); }
TellResult ComputeBloodMoorTownRight    (TellContext& ctx) { return TownClassifyAt(ctx, kTownRight); }

// --- West-exit tells: 3 rooms relative to where Navi stands. ---
//
// The west wall is at x=0; first interior column is x=40. Exit y is
// dynamic — Navi is the anchor. Left = south (player's left facing west),
// Right = north, Straight = same row but one cell *east* of the literal
// exit cell (which is always empty).

// (BloodMoor west-exit compute fns moved further down — they need
// ExitNeighborAt which is defined alongside the other boundary-exit
// helpers.)

// --- Corner-shape tells: a "missing" corner is a hole in the room grid. ---

namespace {

TellResult RoomPresenceResult(const LevelMap& m, int x, int y,
                              const char* presentVal, const char* missingVal) {
    bool present = false;
    for (const auto& r : m.rooms)
        if (r.x == x && r.y == y) { present = true; break; }
    return { present ? presentVal : missingVal,
             TellLocation{ LevelId::BloodMoor, x, y, kRoomSize, kRoomSize } };
}

} // anonymous

TellResult ComputeBloodMoorSWCorner(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::BloodMoor);
    if (m.empty()) return "ERROR";
    return RoomPresenceResult(m, 0, m.sizeY - kRoomSize, "present", "missing");
}
TellResult ComputeBloodMoorSWCornerNorth(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::BloodMoor);
    if (m.empty()) return "ERROR";
    return RoomPresenceResult(m, 0, m.sizeY - 2 * kRoomSize, "no", "yes");
}
TellResult ComputeBloodMoorSWCornerEast(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::BloodMoor);
    if (m.empty()) return "ERROR";
    return RoomPresenceResult(m, kRoomSize, m.sizeY - kRoomSize, "no", "yes");
}
TellResult ComputeBloodMoorNWCorner(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::BloodMoor);
    if (m.empty()) return "ERROR";
    return RoomPresenceResult(m, 0, 0, "present", "missing");
}
TellResult ComputeBloodMoorNWCornerSouth(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::BloodMoor);
    if (m.empty()) return "ERROR";
    return RoomPresenceResult(m, 0, kRoomSize, "no", "yes");
}
TellResult ComputeBloodMoorNWCornerEast(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::BloodMoor);
    if (m.empty()) return "ERROR";
    return RoomPresenceResult(m, kRoomSize, 0, "no", "yes");
}

// --- Cold Plains: 5 tiles around the waypoint, named from the player's POV ---
//
// The waypoint always sits in the first room the player walks into. The
// surrounding cells are useful tells; we expose the 5 in front of (and
// around) the player at entry:
//
//   Forward         one cell ahead
//   ForwardLeft     diagonally forward-left
//   ForwardRight    diagonally forward-right
//   Left            one cell to the player's left
//   Right           one cell to the player's right
//
// All offsets are room-relative; the rotation from player-(forward, right)
// to map-(dx, dy) depends on which side of Cold Plains the source level
// sits on (= direction the player came from). Works for all 4 entry
// directions even though current data only has BloodMoor east of CP.

namespace {

// Direction of the source-level bridge centroid from Cold Plains' center.
// 'E'/'W'/'N'/'S', or 0 on missing adjacency.
char SourceDirection(const LevelMap& m, LevelId source) {
    int sumX = 0, sumY = 0, count = 0;
    for (const auto& a : m.adjacents) {
        if (a.levelNo == uint32_t(LvlId(source))) {
            sumX += a.bridgeX; sumY += a.bridgeY; ++count;
        }
    }
    if (count == 0) return 0;
    const int cx = sumX / count - m.sizeX / 2;
    const int cy = sumY / count - m.sizeY / 2;
    if (std::abs(cx) > std::abs(cy)) return cx > 0 ? 'E' : 'W';
    return cy > 0 ? 'S' : 'N';
}

// Player-relative (forward, right) → map-relative (dx, dy), given the
// direction the player came FROM. Game coords have +y = south.
//   came-from E (faces W): forward = -x, right = -y (north)
//   came-from W (faces E): forward = +x, right = +y (south)
//   came-from N (faces S): forward = +y, right = -x (west)
//   came-from S (faces N): forward = -y, right = +x (east)
void PlayerToMap(char cameFrom, int forward, int right, int& dx, int& dy) {
    switch (cameFrom) {
        case 'E': dx = -forward; dy = -right;   break;
        case 'W': dx = +forward; dy = +right;   break;
        case 'N': dx = -right;   dy = +forward; break;
        case 'S': dx = +right;   dy = -forward; break;
        default:  dx = 0;        dy = 0;        break;
    }
}

TellResult ColdPlainsWaypointNeighbor(TellContext& ctx, int forward, int right) {
    const LevelMap& m = ctx.GetLevel(LevelId::ColdPlains);
    if (m.empty()) return "ERROR";
    const Preset* wp = nullptr;
    for (const auto& p : m.presets)
        if (p.kind == ObjectKind::Waypoint) { wp = &p; break; }
    if (!wp) return "ERROR";
    const Room* wpRoom = FindRoomAt(m, wp->x, wp->y);
    if (!wpRoom) return "ERROR";
    const char cameFrom = SourceDirection(m, LevelId::BloodMoor);
    if (!cameFrom) return "ERROR";
    int dx, dy;
    PlayerToMap(cameFrom, forward, right, dx, dy);
    const int tx = wpRoom->x + dx * kRoomSize;
    const int ty = wpRoom->y + dy * kRoomSize;
    return ClassifyRoomAt(m, LevelId::ColdPlains,
                          tx + kRoomSize / 2, ty + kRoomSize / 2);
}

} // anonymous

TellResult ComputeColdPlainsWaypointForward      (TellContext& ctx) { return ColdPlainsWaypointNeighbor(ctx, +1,  0); }
TellResult ComputeColdPlainsWaypointForwardLeft  (TellContext& ctx) { return ColdPlainsWaypointNeighbor(ctx, +1, -1); }
TellResult ComputeColdPlainsWaypointForwardRight (TellContext& ctx) { return ColdPlainsWaypointNeighbor(ctx, +1, +1); }
TellResult ComputeColdPlainsWaypointLeft         (TellContext& ctx) { return ColdPlainsWaypointNeighbor(ctx,  0, -1); }
TellResult ComputeColdPlainsWaypointRight        (TellContext& ctx) { return ColdPlainsWaypointNeighbor(ctx,  0, +1); }

// Cave (CaveLevel1) entrance style: locate the Stairs preset in Cold Plains
// that leads to CaveLevel1 and return its visual variant via the same
// Act1WildRoom classifier (CaveLevel1StairsA / B / Other).
TellResult ComputeColdPlainsCaveLevel1Entrance(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::ColdPlains);
    if (m.empty()) return "ERROR";
    const Preset* stairs = nullptr;
    for (const auto& p : m.presets) {
        if (p.kind == ObjectKind::Stairs
            && p.destLevelNo == uint32_t(LvlId(LevelId::CaveLevel1))) {
            stairs = &p; break;
        }
    }
    if (!stairs) return "ERROR";
    return ClassifyRoomAt(m, LevelId::ColdPlains, stairs->x, stairs->y);
}

// --- Boundary-exit neighborhoods (5 player-POV cells) and dungeon-entrance
// surrounds (8 absolute-compass cells). Generic helpers below; per-exit
// compute functions follow. ---

namespace {

// Direction (E/W/N/S) → unit room-cell offset.
void DirCellOffset(char dir, int& dxCells, int& dyCells) {
    switch (dir) {
        case 'E': dxCells = +1; dyCells = 0;  break;
        case 'W': dxCells = -1; dyCells = 0;  break;
        case 'N': dxCells = 0;  dyCells = -1; break;
        case 'S': dxCells = 0;  dyCells = +1; break;
        default:  dxCells = 0;  dyCells = 0;  break;
    }
}

char OppositeDir(char dir) {
    switch (dir) {
        case 'E': return 'W';
        case 'W': return 'E';
        case 'N': return 'S';
        case 'S': return 'N';
    }
    return 0;
}

// Find the txt-37 Dummy preset in `m` most in the direction of adjacency to
// `destLevel` (highest projection onto level-center → bridge-centroid).
// Snaps to room grid. Returns false on missing adjacency or no Dummy 37.
bool FindDummy37Exit(const LevelMap& m, LevelId destLevel, int& outX, int& outY) {
    int sumX = 0, sumY = 0, count = 0;
    for (const auto& a : m.adjacents) {
        if (a.levelNo == uint32_t(LvlId(destLevel))) {
            sumX += a.bridgeX; sumY += a.bridgeY; ++count;
        }
    }
    if (count == 0) return false;
    const int adjX = sumX / count - m.sizeX / 2;
    const int adjY = sumY / count - m.sizeY / 2;

    const Preset* best = nullptr;
    long bestScore = LONG_MIN;
    for (const auto& p : m.presets) {
        if (p.type != uint16_t(PresetType::Object) || p.txtFileNo != 37) continue;
        const long px = p.x - m.sizeX / 2;
        const long py = p.y - m.sizeY / 2;
        const long score = px * adjX + py * adjY;
        if (score > bestScore) { bestScore = score; best = &p; }
    }
    if (!best) return false;
    outX = (best->x / kRoomSize) * kRoomSize;
    outY = (best->y / kRoomSize) * kRoomSize;
    return true;
}

// 5-tile player-POV exit neighborhood. Exit cell = (forward=+1, right=0);
// anchor = one cell behind exit toward level center; cells follow
// PlayerToMap rotation based on the player's approach direction.
TellResult ExitNeighborAt(TellContext& ctx, LevelId level, LevelId destLevel,
                          int forward, int right, bool feature) {
    const LevelMap& m = ctx.GetLevel(level);
    if (m.empty()) return "ERROR";
    int exitX, exitY;
    if (!FindDummy37Exit(m, destLevel, exitX, exitY)) return "ERROR";
    const char dir = SourceDirection(m, destLevel);
    if (!dir) return "ERROR";
    int dxDir, dyDir;
    DirCellOffset(dir, dxDir, dyDir);
    const int anchorX = exitX - dxDir * kRoomSize;
    const int anchorY = exitY - dyDir * kRoomSize;
    int dx, dy;
    PlayerToMap(OppositeDir(dir), forward, right, dx, dy);
    const int tx = anchorX + dx * kRoomSize;
    const int ty = anchorY + dy * kRoomSize;
    if (feature) return ShrineAt(m, level, tx + kRoomSize/2, ty + kRoomSize/2);
    return ClassifyRoomAt(m, level, tx + kRoomSize/2, ty + kRoomSize/2);
}

// 8-tile absolute-compass dungeon surround. Center = the room containing
// the Stairs preset to destLevel; we sample the 8 neighbour cells at
// fixed (dxCells, dyCells) offsets.
TellResult DungeonNeighborAt(TellContext& ctx, LevelId level, LevelId destLevel,
                             int dxCells, int dyCells, bool feature) {
    const LevelMap& m = ctx.GetLevel(level);
    if (m.empty()) return "ERROR";
    int txt, sx, sy;
    if (!FindStairsTo(m, destLevel, txt, sx, sy)) return "ERROR";
    const int stairX = (sx / kRoomSize) * kRoomSize;
    const int stairY = (sy / kRoomSize) * kRoomSize;
    const int tx = stairX + dxCells * kRoomSize;
    const int ty = stairY + dyCells * kRoomSize;
    if (feature) return ShrineAt(m, level, tx + kRoomSize/2, ty + kRoomSize/2);
    return ClassifyRoomAt(m, level, tx + kRoomSize/2, ty + kRoomSize/2);
}

} // anonymous

// --- Blood Moor → Cold Plains exit POV (3 cells, player POV) ---
//
// Anchored on the Dummy-37 nearest the Cold Plains boundary, rotated by
// the player's approach direction (SourceDirection). Works for both W and
// N exit layouts because the rotation is dynamic.
//   Left:     immediate left at the anchor cell.
//   Right:    immediate right at the anchor cell.
//   Straight: cell two forward from the anchor, skipping the exit cell
//             itself (forward=+1 is the boundary cell which is usually
//             empty/transitional).
// Per-position shrine variants were dropped — use BloodMoorShrineN
// (level-wide counts) instead.
TellResult ComputeBloodMoorColdPlainsStraight (TellContext& ctx) { return ExitNeighborAt(ctx, LevelId::BloodMoor, LevelId::ColdPlains, +2,  0, false); }
TellResult ComputeBloodMoorColdPlainsLeft     (TellContext& ctx) { return ExitNeighborAt(ctx, LevelId::BloodMoor, LevelId::ColdPlains,  0, -1, false); }
TellResult ComputeBloodMoorColdPlainsRight    (TellContext& ctx) { return ExitNeighborAt(ctx, LevelId::BloodMoor, LevelId::ColdPlains,  0, +1, false); }

// --- Cold Plains → Stony Field exit POV (3 cells, player POV) ---
// Same shape as the BloodMoor → Cold Plains tells: immediate L/R plus
// Straight (forward+2, skipping the exit cell itself).
TellResult ComputeColdPlainsStonyFieldStraight (TellContext& ctx) { return ExitNeighborAt(ctx, LevelId::ColdPlains, LevelId::StonyField, +2,  0, false); }
TellResult ComputeColdPlainsStonyFieldLeft     (TellContext& ctx) { return ExitNeighborAt(ctx, LevelId::ColdPlains, LevelId::StonyField,  0, -1, false); }
TellResult ComputeColdPlainsStonyFieldRight    (TellContext& ctx) { return ExitNeighborAt(ctx, LevelId::ColdPlains, LevelId::StonyField,  0, +1, false); }

// --- Dungeon-entrance surrounds (8 cells, absolute compass) ---

// Cave Level 1 entrance in Cold Plains
TellResult ComputeColdPlainsCaveLevel1N  (TellContext& ctx) { return DungeonNeighborAt(ctx, LevelId::ColdPlains, LevelId::CaveLevel1,  0, -1, false); }
TellResult ComputeColdPlainsCaveLevel1NE (TellContext& ctx) { return DungeonNeighborAt(ctx, LevelId::ColdPlains, LevelId::CaveLevel1, +1, -1, false); }
TellResult ComputeColdPlainsCaveLevel1E  (TellContext& ctx) { return DungeonNeighborAt(ctx, LevelId::ColdPlains, LevelId::CaveLevel1, +1,  0, false); }
TellResult ComputeColdPlainsCaveLevel1SE (TellContext& ctx) { return DungeonNeighborAt(ctx, LevelId::ColdPlains, LevelId::CaveLevel1, +1, +1, false); }
TellResult ComputeColdPlainsCaveLevel1S  (TellContext& ctx) { return DungeonNeighborAt(ctx, LevelId::ColdPlains, LevelId::CaveLevel1,  0, +1, false); }
TellResult ComputeColdPlainsCaveLevel1SW (TellContext& ctx) { return DungeonNeighborAt(ctx, LevelId::ColdPlains, LevelId::CaveLevel1, -1, +1, false); }
TellResult ComputeColdPlainsCaveLevel1W  (TellContext& ctx) { return DungeonNeighborAt(ctx, LevelId::ColdPlains, LevelId::CaveLevel1, -1,  0, false); }
TellResult ComputeColdPlainsCaveLevel1NW (TellContext& ctx) { return DungeonNeighborAt(ctx, LevelId::ColdPlains, LevelId::CaveLevel1, -1, -1, false); }

// Den of Evil entrance in Blood Moor
TellResult ComputeBloodMoorDenOfEvilN  (TellContext& ctx) { return DungeonNeighborAt(ctx, LevelId::BloodMoor, LevelId::DenOfEvil,  0, -1, false); }
TellResult ComputeBloodMoorDenOfEvilNE (TellContext& ctx) { return DungeonNeighborAt(ctx, LevelId::BloodMoor, LevelId::DenOfEvil, +1, -1, false); }
TellResult ComputeBloodMoorDenOfEvilE  (TellContext& ctx) { return DungeonNeighborAt(ctx, LevelId::BloodMoor, LevelId::DenOfEvil, +1,  0, false); }
TellResult ComputeBloodMoorDenOfEvilSE (TellContext& ctx) { return DungeonNeighborAt(ctx, LevelId::BloodMoor, LevelId::DenOfEvil, +1, +1, false); }
TellResult ComputeBloodMoorDenOfEvilS  (TellContext& ctx) { return DungeonNeighborAt(ctx, LevelId::BloodMoor, LevelId::DenOfEvil,  0, +1, false); }
TellResult ComputeBloodMoorDenOfEvilSW (TellContext& ctx) { return DungeonNeighborAt(ctx, LevelId::BloodMoor, LevelId::DenOfEvil, -1, +1, false); }
TellResult ComputeBloodMoorDenOfEvilW  (TellContext& ctx) { return DungeonNeighborAt(ctx, LevelId::BloodMoor, LevelId::DenOfEvil, -1,  0, false); }
TellResult ComputeBloodMoorDenOfEvilNW (TellContext& ctx) { return DungeonNeighborAt(ctx, LevelId::BloodMoor, LevelId::DenOfEvil, -1, -1, false); }

// --- Per-template room counts for the Act 1 wilderness levels ---
//
// For each Act1WildRoom value in the ACT1_WILD_ROOMS macro, generate a
// compute fn per wilderness level. Each tell counts the rooms of that
// template on the level and attaches their rects as TellLocations.
TellResult Act1WildRoomCount(TellContext& ctx, LevelId selfLevel, Act1WildRoom target) {
    const LevelMap& m = ctx.GetLevel(selfLevel);
    if (m.empty()) return "ERROR";
    std::vector<TellLocation> locs;
    int count = 0;
    for (const auto& r : m.rooms) {
        if (ClassifyAct1WildRoom(r, m) == target) {
            ++count;
            locs.push_back({ selfLevel, r.x, r.y, r.sizeX, r.sizeY });
        }
    }
    return { std::to_string(count), std::move(locs) };
}

#define X(NAME) \
    TellResult ComputeBloodMoor##NAME(TellContext& ctx) { return Act1WildRoomCount(ctx, LevelId::BloodMoor,  Act1WildRoom::NAME); } \
    TellResult ComputeColdPlains##NAME(TellContext& ctx) { return Act1WildRoomCount(ctx, LevelId::ColdPlains, Act1WildRoom::NAME); } \
    TellResult ComputeStonyField##NAME(TellContext& ctx) { return Act1WildRoomCount(ctx, LevelId::StonyField, Act1WildRoom::NAME); }
ACT1_WILD_ROOMS
#undef X

} // namespace MapData
