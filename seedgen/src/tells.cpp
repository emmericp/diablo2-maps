#include "tells.h"
#include "act1_wild_rooms.h"
#include "d2npcid.h"
#include "extractor.h"
#include "objects_db.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

namespace MapData {

const LevelMap& TellContext::GetLevel(LevelId levelNo) {
    auto it = cache_.find(levelNo);
    if (it != cache_.end()) return it->second;
    LevelMap m = ExtractLevel(pAct_, actNo_, LvlId(levelNo));
    auto [ins, inserted] = cache_.emplace(levelNo, std::move(m));
    (void)inserted;
    return ins->second;
}

// Room-based tell compute functions live in tells_rooms.cpp. Each returns
// the unified Act1WildRoom classification (HouseBedSouth / StonesCircle /
// Lake / Other / ...). Shrine tells return None / Well / Shrine{2,81,83,84}
// / Multiple — wells are bucketed under the same tell since they're the same
// kind of "click-to-activate world feature" for our purpose.
// Town-front rooms (3 cells one row north of the camp gate). Per-position
// shrine variants were dropped — use the level-wide BloodMoorShrineN
// tells instead.
TellResult ComputeBloodMoorTownStraight  (TellContext& ctx);
TellResult ComputeBloodMoorTownLeft      (TellContext& ctx);
TellResult ComputeBloodMoorTownRight     (TellContext& ctx);

TellResult ComputeBloodMoorSWCorner      (TellContext& ctx);
TellResult ComputeBloodMoorSWCornerNorth (TellContext& ctx);
TellResult ComputeBloodMoorSWCornerEast  (TellContext& ctx);
TellResult ComputeBloodMoorNWCorner      (TellContext& ctx);
TellResult ComputeBloodMoorNWCornerSouth (TellContext& ctx);
TellResult ComputeBloodMoorNWCornerEast  (TellContext& ctx);

// Cold Plains exit POV: just three player-relative cells. Straight skips
// the exit cell itself and looks at the room two cells forward (the first
// room a player sees once they're past the boundary). Per-position shrine
// variants dropped in favor of level-wide BloodMoorShrineN.
TellResult ComputeBloodMoorColdPlainsStraight (TellContext& ctx);
TellResult ComputeBloodMoorColdPlainsLeft     (TellContext& ctx);
TellResult ComputeBloodMoorColdPlainsRight    (TellContext& ctx);
TellResult ComputeColdPlainsWaypointSide                (TellContext& ctx);
TellResult ComputeColdPlainsWaypointForward             (TellContext& ctx);
TellResult ComputeColdPlainsWaypointForwardLeft         (TellContext& ctx);
TellResult ComputeColdPlainsWaypointForwardRight        (TellContext& ctx);
TellResult ComputeColdPlainsWaypointLeft                (TellContext& ctx);
TellResult ComputeColdPlainsWaypointRight               (TellContext& ctx);
TellResult ComputeColdPlainsCaveLevel1Entrance                (TellContext& ctx);

// Shared helpers in tells_rooms.cpp (also used by Den of Evil and
// Underground Passage entrance tells defined here).
const char* StairsStyleName(int txtFileNo);
bool        FindStairsTo(const LevelMap& m, LevelId destLevel,
                         int& outTxt, int& outX, int& outY);

// Camp gate reference in Blood Moor local coords — the leftmost (west-most)
// tile of the gate the player exits Rogue Encampment through. Used to
// anchor every "near the town exit" tell (trees flanking the gate, the
// row of rooms one cell north of it, the chest/bed scan radius...).
//
// With CampExit=N (the focus subset) two Blood Moor layouts exist:
//   BloodMoorExit=W: (344, 279) — Blood Moor extends west toward Cold Plains.
//   BloodMoorExit=N: (144, 479) — Blood Moor extends north toward Cold Plains.
// Other directions don't occur (river on the east, Rogue Encampment to the
// south). In both layouts the gate is the camp's north wall, so the
// player-facing orientation around it is the same (Straight=N, Left=W,
// Right=E). Returns false if the layout matches neither — callers are
// expected to gate on kReqNCampGate but the check is defensive.
//
// Defined at MapData level (not anonymous) so tells_rooms.cpp can call it.
bool BloodMoorCampGate(TellContext& ctx, int& gx, int& gy);

// Cold Plains → Stony Field exit POV (3 cells, player POV — same shape
// as the BloodMoor → Cold Plains tells). Straight skips the exit cell.
TellResult ComputeColdPlainsStonyFieldStraight (TellContext& ctx);
TellResult ComputeColdPlainsStonyFieldLeft     (TellContext& ctx);
TellResult ComputeColdPlainsStonyFieldRight    (TellContext& ctx);

// Cave Level 1 entrance 8-cell compass surround.
TellResult ComputeColdPlainsCaveLevel1N  (TellContext& ctx);
TellResult ComputeColdPlainsCaveLevel1NE (TellContext& ctx);
TellResult ComputeColdPlainsCaveLevel1E  (TellContext& ctx);
TellResult ComputeColdPlainsCaveLevel1SE (TellContext& ctx);
TellResult ComputeColdPlainsCaveLevel1S  (TellContext& ctx);
TellResult ComputeColdPlainsCaveLevel1SW (TellContext& ctx);
TellResult ComputeColdPlainsCaveLevel1W  (TellContext& ctx);
TellResult ComputeColdPlainsCaveLevel1NW (TellContext& ctx);
// Den of Evil entrance 8-cell compass surround. Per-position shrine
// variants were dropped; the room classifier now embeds shrine info as
// enum values (Shrine2, Well, ...) and the per-template count tells
// below cover level-wide shrine totals.
TellResult ComputeBloodMoorDenOfEvilN  (TellContext& ctx);
TellResult ComputeBloodMoorDenOfEvilNE (TellContext& ctx);
TellResult ComputeBloodMoorDenOfEvilE  (TellContext& ctx);
TellResult ComputeBloodMoorDenOfEvilSE (TellContext& ctx);
TellResult ComputeBloodMoorDenOfEvilS  (TellContext& ctx);
TellResult ComputeBloodMoorDenOfEvilSW (TellContext& ctx);
TellResult ComputeBloodMoorDenOfEvilW  (TellContext& ctx);
TellResult ComputeBloodMoorDenOfEvilNW (TellContext& ctx);

// Per-template room-count tells for the Act 1 wilderness levels —
// defined in tells_rooms.cpp via the ACT1_WILD_ROOMS X-macro.
#define X(NAME) \
    TellResult ComputeBloodMoor##NAME(TellContext& ctx); \
    TellResult ComputeColdPlains##NAME(TellContext& ctx); \
    TellResult ComputeStonyField##NAME(TellContext& ctx);
ACT1_WILD_ROOMS
#undef X

namespace {

// Compass direction from a level's center to the centroid of its bridge
// rooms connecting to a specific neighboring level. Returns "ERROR" if the
// adjacency is missing — the caller knows the bridge must exist.
const char* DirectionTo(const LevelMap& m, LevelId toLevelNo) {
    int sumX = 0, sumY = 0, count = 0;
    for (const auto& a : m.adjacents) {
        if (a.levelNo == LvlId(toLevelNo)) {
            sumX += a.bridgeX; sumY += a.bridgeY; ++count;
        }
    }
    if (count == 0) return "ERROR";
    const int cx = sumX / count - m.sizeX / 2;
    const int cy = sumY / count - m.sizeY / 2;
    return (abs(cx) > abs(cy)) ? (cx > 0 ? "E" : "W")
                                : (cy > 0 ? "S" : "N");
}

// Mirror a compass letter (N↔S, E↔W). Passes "ERROR" / "?" through unchanged
// so it's safe to wrap DirectionTo() results.
const char* Opposite(const char* dir) {
    if (!dir) return dir;
    switch (dir[0]) {
        case 'N': return "S";
        case 'S': return "N";
        case 'E': return "W";
        case 'W': return "E";
        default:  return dir;
    }
}

// Centroid of all bridge points toward a neighboring level, in level-local tiles.
// Returns false if the neighbor isn't adjacent.
bool BridgeCentroid(const LevelMap& m, LevelId toLevelNo, int& outX, int& outY) {
    int sumX = 0, sumY = 0, count = 0;
    for (const auto& a : m.adjacents) {
        if (a.levelNo == LvlId(toLevelNo)) {
            sumX += a.bridgeX; sumY += a.bridgeY; ++count;
        }
    }
    if (count == 0) return false;
    outX = sumX / count;
    outY = sumY / count;
    return true;
}

// --- Tell implementations ---

// Renders a tell at the bridge centroid of `m` toward `toLevelNo`. Used by
// the *Exit direction tells, whose value is a compass letter — the centroid
// is the natural place to draw it.
TellLocation BridgeLocation(const LevelMap& m, LevelId selfLevel, LevelId toLevelNo) {
    int bx = 0, by = 0;
    BridgeCentroid(m, toLevelNo, bx, by);
    return { selfLevel, bx, by, 0, 0 };
}

// Act 1 wilderness exit direction (Rogue Encampment → Blood Moor).
TellResult ComputeCampExit(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::RogueCamp);
    if (m.empty()) return "ERROR";
    return { DirectionTo(m, LevelId::BloodMoor),
             BridgeLocation(m, LevelId::RogueCamp, LevelId::BloodMoor) };
}

// Camp-exit direction in player-walk terms ("which way did you walk out of
// camp"), computed from BloodMoor's adjacency list so prereqs can gate on
// the camp-gate orientation without forcing a RogueCamp extraction
// (RogueCamp load is too expensive for performance-critical paths).
//
// DirectionTo returns the direction from BloodMoor's center to its bridge
// to camp — which is the mirror of the player's walk direction (if you
// walk N out of camp, the camp-bridge sits on BloodMoor's S side). We
// flip it so this tell and CampExit agree by convention. Equals "N" for
// the focus subset.
TellResult ComputeBloodMoorCampExit(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::BloodMoor);
    if (m.empty()) return "ERROR";
    return { Opposite(DirectionTo(m, LevelId::RogueCamp)),
             BridgeLocation(m, LevelId::BloodMoor, LevelId::RogueCamp) };
}

// Blood Moor exit direction toward Cold Plains.
TellResult ComputeBloodMoorExit(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::BloodMoor);
    if (m.empty()) return "ERROR";
    return { DirectionTo(m, LevelId::ColdPlains),
             BridgeLocation(m, LevelId::BloodMoor, LevelId::ColdPlains) };
}

// Cold Plains exit direction toward Stony Field — the next area in the
// wilderness chain. Same compass-letter scheme as BloodMoorExit.
TellResult ComputeColdPlainsExit(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::ColdPlains);
    if (m.empty()) return "ERROR";
    return { DirectionTo(m, LevelId::StonyField),
             BridgeLocation(m, LevelId::ColdPlains, LevelId::StonyField) };
}

// L/R position of the west exit (where Navi stands). Precondition: BloodMoorExit=W.
// Two possible positions: Y≈100 (R, right/north) and Y≈180 (L, left/south).
TellResult ComputeBloodMoorColdPlainsPos(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::BloodMoor);
    if (m.empty()) return "ERROR";
    for (const auto& p : m.presets) {
        if (p.type == 1 && p.txtFileNo == NpcIdx(NpcId::Navi))
            return { p.y < 140 ? "R" : "L",
                     TellLocation{ LevelId::BloodMoor, p.x, p.y, 0, 0 } };
    }
    return "ERROR";
}

// Cold Plains waypoint sub-tell. The waypoint always sits in the room the
// player enters first (the room with a bridge to Blood Moor). We use the
// adjacency list to find that room, derive the entry direction from the
// level-global direction to Blood Moor, then report whether the waypoint
// sits to the player's left or right inside the room.
TellResult ComputeColdPlainsWaypointSide(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::ColdPlains);
    if (m.empty()) return "ERROR";

    const Preset* wp = nullptr;
    for (const auto& p : m.presets)
        if (p.kind == ObjectKind::Waypoint) { wp = &p; break; }
    if (!wp) return "ERROR";

    const Room* wpRoom = nullptr;
    for (const auto& r : m.rooms) {
        if (wp->x >= r.x && wp->x < r.x + r.sizeX &&
            wp->y >= r.y && wp->y < r.y + r.sizeY) {
            wpRoom = &r;
            break;
        }
    }
    if (!wpRoom) return "ERROR";

    // Direction Blood Moor lies in from Cold Plains' center = direction the
    // player crossed in from. Player faces the opposite direction inside the
    // room. (BloodMoor is adjacent via the wall column, not the waypoint
    // room itself — so we don't try to confirm the waypoint room has a
    // direct adjacency entry; we just trust the level-level direction.)
    const int rcx = wpRoom->x + wpRoom->sizeX / 2;
    const int rcy = wpRoom->y + wpRoom->sizeY / 2;
    const char* enterFrom = DirectionTo(m, LevelId::BloodMoor);
    if (!enterFrom || !enterFrom[0]) return "ERROR";

    const int dx = wp->x - rcx;
    const int dy = wp->y - rcy;
    bool right;
    switch (enterFrom[0]) {
        case 'E': right = (dy < 0); break;  // facing W: right = north
        case 'W': right = (dy > 0); break;  // facing E: right = south
        case 'N': right = (dx < 0); break;  // facing S: right = west
        case 'S': right = (dx > 0); break;  // facing N: right = east
        default: return "ERROR";
    }

    return { right ? "Right" : "Left",
             TellLocation{ LevelId::ColdPlains, wp->x, wp->y, 0, 0 } };
}

// Compass direction along the axis with the largest absolute delta. Useful
// when one preset (the Summoner, a quest object) lies at the far end of one
// of four axis-extreme arms.
const char* AxisExtremeDir(int dx, int dy) {
    if (dx == 0 && dy == 0) return "?";
    return (abs(dx) > abs(dy)) ? (dx > 0 ? "E" : "W")
                                : (dy > 0 ? "S" : "N");
}

// All four "object near camp gate" tells share this scan. Precondition
// guaranteed by the runner: CampExit=N AND BloodMoorExit=W — so the Rogue
// Encampment bridge always exists. Returns "ERROR" if the level didn't load
// or the bridge centroid can't be computed (would indicate broken extraction).
template <typename Pred>
TellResult ScanNearCampGate(TellContext& ctx, Pred isMatch) {
    const LevelMap& m = ctx.GetLevel(LevelId::BloodMoor);
    if (m.empty()) return "ERROR";
    int bx, by;
    if (!BridgeCentroid(m, LevelId::RogueCamp, bx, by)) return "ERROR";
    constexpr int kR2 = 60 * 60;
    for (const auto& p : m.presets) {
        if (p.type != 2) continue;
        const int dx = p.x - bx, dy = p.y - by;
        if (dx*dx + dy*dy > kR2) continue;
        if (isMatch(p))
            return { "yes", TellLocation{ LevelId::BloodMoor, p.x, p.y, 0, 0 } };
    }
    return "no";
}

TellResult ComputeBloodMoorChest(TellContext& ctx) {
    return ScanNearCampGate(ctx, [](const Preset& p) {
        return p.kind == ObjectKind::Chest || p.kind == ObjectKind::SuperChest;
    });
}

TellResult ComputeBloodMoorBed(TellContext& ctx) {
    return ScanNearCampGate(ctx, [](const Preset& p) {
        const std::string& name = ObjectName(p.txtFileNo);
        return name.size() >= 3
            && (name[0]|32)=='b' && (name[1]|32)=='e' && (name[2]|32)=='d';
    });
}

// Den of Evil entrance sprite facing (S or E). txtFileNo 2 = east-facing
// (common), txtFileNo 3 = south-facing (rare, ~15% of seeds).
// Den of Evil entrance style (shared with CaveLevel1Stairs* and the new
// StonyFieldUndergroundPassageLevel1Entrance via StairsStyleName).
TellResult ComputeBloodMoorDenOfEvilEntrance(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::BloodMoor);
    if (m.empty()) return "ERROR";
    int txt, x, y;
    if (!FindStairsTo(m, LevelId::DenOfEvil, txt, x, y)) return "ERROR";
    return { StairsStyleName(txt), TellLocation{ LevelId::BloodMoor, x, y, 0, 0 } };
}

// Underground Passage Level 1 entrance style in Stony Field — same
// vocabulary as BloodMoorDenOfEvilEntrance.
TellResult ComputeStonyFieldUndergroundPassageLevel1Entrance(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::StonyField);
    if (m.empty()) return "ERROR";
    int txt, x, y;
    if (!FindStairsTo(m, LevelId::UndergroundPassageLevel1, txt, x, y)) return "ERROR";
    return { StairsStyleName(txt), TellLocation{ LevelId::StonyField, x, y, 0, 0 } };
}

// Den of Evil entrance position relative to the two natural anchors a
// player can identify visually: the camp gate (centroid of the bridge to
// Rogue Encampment) and the Navi NPC at the Cold Plains boundary. Returns
// "Town" or "Exit" depending on which the DOE entrance sits closer to
// (squared-distance comparison in level-local tile coords). Works for any
// Blood Moor layout since both anchors are derived from the level's own
// adjacency / preset lists.
TellResult ComputeDenOfEvilPos(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::BloodMoor);
    if (m.empty()) return "ERROR";

    const Preset* doe = nullptr;
    for (const auto& p : m.presets) {
        if (p.type != uint16_t(PresetType::Exit)) continue;
        if (p.destLevelNo != uint32_t(LvlId(LevelId::DenOfEvil))) continue;
        doe = &p; break;
    }
    if (!doe) return "ERROR";

    int gx, gy;
    if (!BridgeCentroid(m, LevelId::RogueCamp, gx, gy)) return "ERROR";

    const Preset* navi = nullptr;
    for (const auto& p : m.presets) {
        if (p.type == 1 && p.txtFileNo == NpcIdx(NpcId::Navi)) {
            navi = &p; break;
        }
    }
    if (!navi) return "ERROR";

    const long dTownSq = (long)(doe->x - gx) * (doe->x - gx)
                       + (long)(doe->y - gy) * (doe->y - gy);
    const long dExitSq = (long)(doe->x - navi->x) * (doe->x - navi->x)
                       + (long)(doe->y - navi->y) * (doe->y - navi->y);
    return { dTownSq < dExitSq ? "Town" : "Exit",
             TellLocation{ LevelId::BloodMoor, doe->x, doe->y, 0, 0 } };
}

// --- Den of Evil interior tells ---
//
// Load DenOfEvil itself. The entrance is the Stairs preset going back to
// BloodMoor; the room containing it has one of four rooms.txt templates:
//   roomNo 85 / 86 → "Narrow" entrance
//   roomNo 83 / 84 → "Wide"   entrance
// The sub-variants (85 vs 86, 83 vs 84) aren't visually distinguishable to
// a player, so we collapse them — the boss-direction tell carries the
// remaining bit.

struct DenOfEvilEntrance {
    bool     valid      = false;
    uint32_t roomNumber = 0;
    int      x          = 0;
    int      y          = 0;
};

DenOfEvilEntrance FindDenOfEvilEntrance(const LevelMap& m) {
    DenOfEvilEntrance e{};
    for (const auto& p : m.presets) {
        if (p.kind != ObjectKind::Stairs) continue;
        if (p.destLevelNo != uint32_t(LvlId(LevelId::BloodMoor))) continue;
        for (const auto& r : m.rooms) {
            if (p.x >= r.x && p.x < r.x + r.sizeX
                && p.y >= r.y && p.y < r.y + r.sizeY) {
                e.valid      = true;
                e.roomNumber = r.roomNumber;
                e.x          = p.x;
                e.y          = p.y;
                return e;
            }
        }
    }
    return e;
}

// pointless: 100% predicted by boss/entrance position
TellResult ComputeDenOfEvilEntranceRoom(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::DenOfEvil);
    if (m.empty()) return "ERROR";
    const auto e = FindDenOfEvilEntrance(m);
    if (!e.valid) return "ERROR";
    const TellLocation loc{ LevelId::DenOfEvil, e.x, e.y, 0, 0 };
    switch (e.roomNumber) {
        case 83: case 84: return { "Wide",   loc };
        case 85: case 86: return { "Narrow", loc };
        default:          return { "Other",  loc };
    }
}

// Corpsefire (npc 774) position relative to the entrance, projected from
// iso to screen-cardinal. On D2's iso projection screen N is up-right and
// screen E is down-right, so the four iso-cardinal corners collapse to
// screen cardinals:
//   NE (+x, -y) → Right
//   NW (-x, -y) → Up
//   SE (+x, +y) → Down
//   SW (-x, +y) → Left
TellResult ComputeDenOfEvilBoss(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::DenOfEvil);
    if (m.empty()) return "ERROR";
    const auto e = FindDenOfEvilEntrance(m);
    if (!e.valid) return "ERROR";

    constexpr uint32_t kCorpsefireTxt = 774;
    const Preset* boss = nullptr;
    for (const auto& p : m.presets) {
        if (p.type == uint16_t(PresetType::NPC) && p.txtFileNo == kCorpsefireTxt) {
            boss = &p;
            break;
        }
    }
    if (!boss) return "ERROR";

    const int dx = boss->x - e.x;
    const int dy = boss->y - e.y;
    const char* dir;
    if (dx >= 0 && dy <  0)      dir = "Right"; // NE
    else if (dx <  0 && dy <= 0) dir = "Up";    // NW
    else if (dx >= 0 && dy >= 0) dir = "Down";  // SE
    else                          dir = "Left";  // SW
    return { dir, TellLocation{ LevelId::DenOfEvil, boss->x, boss->y, 0, 0 } };
}

// Arcane Sanctuary exit wing direction. The Summoner (NpcId::Summoner, hcIdx
// 250) is preset at the far end of exactly one of the four axis-aligned arms.
TellResult ComputeArcaneExit(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::ArcaneSanctuary);
    if (m.empty()) return "ERROR";

    const int cx = m.sizeX / 2;
    const int cy = m.sizeY / 2;
    for (const auto& p : m.presets) {
        if (p.type == 1 && p.txtFileNo == NpcIdx(NpcId::Summoner))
            return { AxisExtremeDir(p.x - cx, p.y - cy),
                     TellLocation{ LevelId::ArcaneSanctuary, p.x, p.y, 0, 0 } };
    }
    return "ERROR";
}

// Count interior islands (BlockWalk-connected components that don't touch
// the level border, ≥500 tiles). We deliberately don't expose per-island
// locations — in BloodMoor the layout is just the mirror of the Den of
// Evil position, and in later areas the island layout is too messy to be
// useful as a renderer overlay.
int CountIslands(const LevelMap& m) {
    const int W = m.sizeX, H = m.sizeY;
    const int N = W * H;

    std::vector<bool> blocked(N);
    for (int i = 0; i < N; ++i)
        blocked[i] = (m.coll[i] != kNoData) && (m.coll[i] & BlockWalk);

    std::vector<bool> visited(N, false);
    std::vector<int>  queue;
    queue.reserve(N / 4);

    constexpr int kMinIslandTiles = 500;
    int count = 0;

    for (int start = 0; start < N; ++start) {
        if (!blocked[start] || visited[start]) continue;

        queue.clear();
        queue.push_back(start);
        visited[start] = true;

        bool touchesBorder = false;
        int  compSize = 0;

        for (int qi = 0; qi < (int)queue.size(); ++qi) {
            const int cur = queue[qi];
            const int cx  = cur % W, cy = cur / W;
            ++compSize;
            if (cx == 0 || cx == W-1 || cy == 0 || cy == H-1)
                touchesBorder = true;

            const int nbrs[4][2] = {{cx-1,cy},{cx+1,cy},{cx,cy-1},{cx,cy+1}};
            for (auto& nb : nbrs) {
                const int nx = nb[0], ny = nb[1];
                if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                const int ni = ny * W + nx;
                if (!visited[ni] && blocked[ni]) { visited[ni] = true; queue.push_back(ni); }
            }
        }

        if (!touchesBorder && compSize >= kMinIslandTiles) ++count;
    }

    return count;
}

TellResult ComputeNumIslandsAt(TellContext& ctx, LevelId levelId) {
    const LevelMap& m = ctx.GetLevel(levelId);
    if (m.empty()) return "ERROR";
    return std::to_string(CountIslands(m));
}

TellResult ComputeBloodMoorNumIslands (TellContext& ctx) { return ComputeNumIslandsAt(ctx, LevelId::BloodMoor); }
TellResult ComputeColdPlainsNumIslands(TellContext& ctx) { return ComputeNumIslandsAt(ctx, LevelId::ColdPlains); }
TellResult ComputeStonyFieldNumIslands(TellContext& ctx) { return ComputeNumIslandsAt(ctx, LevelId::StonyField); }

// Classify the shape of one island using a 3x3 bounding-box fill grid.
// Each cell is "filled" if >45% of its tiles are blocked.
// Returns one of: Square, Rectangle, L, C, Upside_down_L, Upside_down_C, Other.
const char* ClassifyIslandShape(const std::vector<int>& cells,
                                const std::vector<bool>& inIsland,
                                int W) {
    int minX = W, maxX = 0, minY = INT_MAX, maxY = 0;
    for (int idx : cells) {
        int x = idx % W, y = idx / W;
        if (x < minX) minX = x; if (x > maxX) maxX = x;
        if (y < minY) minY = y; if (y > maxY) maxY = y;
    }
    const int bw = maxX - minX + 1, bh = maxY - minY + 1;

    bool f[3][3] = {};
    for (int gy = 0; gy < 3; ++gy) {
        for (int gx = 0; gx < 3; ++gx) {
            const int xa = minX + bw*gx/3, xb = minX + bw*(gx+1)/3;
            const int ya = minY + bh*gy/3, yb = minY + bh*(gy+1)/3;
            const int total = (xb - xa) * (yb - ya);
            if (total == 0) continue;
            int filled = 0;
            for (int ry = ya; ry < yb; ++ry)
                for (int rx = xa; rx < xb; ++rx)
                    filled += inIsland[ry * W + rx] ? 1 : 0;
            f[gy][gx] = (double)filled / total > 0.45;
        }
    }

    const bool TL=f[0][0], TC=f[0][1], TR=f[0][2];
    const bool ML=f[1][0], MC=f[1][1], MR=f[1][2];
    const bool BL=f[2][0], BC=f[2][1], BR=f[2][2];

    if (TL&&TC&&TR&&ML&&MC&&MR&&BL&&BC&&BR) {
        const float ar = (float)std::max(bw,bh) / (float)std::min(bw,bh);
        return ar < 1.3f ? "Square" : "Rectangle";
    }
    if (!TC && TL&&TR&&ML&&MC&&MR&&BL&&BC&&BR)          return "C";
    if (!BC && TL&&TC&&TR&&ML&&MC&&MR&&BL&&BR)          return "Upside_down_C";
    if (!TL&&!TC && TR&&ML&&MC&&MR&&BL&&BC&&BR)         return "L";
    if (!BC&&!BR && TL&&TC&&TR&&ML&&MC&&MR&&BL)         return "Upside_down_L";
    if (!BL&&!BR && BC&&TL&&TC&&TR&&ML&&MC&&MR)         return "Left_T";
    if (!TL&&!TR && TC&&ML&&MC&&MR&&BL&&BC&&BR)         return "Right_T";
    return "Other";
}

// Tree tiles are single-tile obstacles with collision value 0x25
// (Blank | Wall | BlockWalk). Count them inside a level-local rectangle
// [xMin,xMax) × [yMin,yMax); ranges are clamped to the map.
int CountTreesInRect(const LevelMap& m, int xMin, int xMax, int yMin, int yMax) {
    constexpr uint16_t kTreeColl = Blank | Wall | BlockWalk;  // 0x25
    const int x0 = std::max(0, xMin), x1 = std::min(m.sizeX, xMax);
    const int y0 = std::max(0, yMin), y1 = std::min(m.sizeY, yMax);
    int count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (m.coll[y * m.sizeX + x] == kTreeColl) ++count;
        }
    }
    return count;
}

// Tree counts in the four wall-segment rectangles flanking the camp gate,
// parameterised by the gate reference point (BloodMoorCampGate). Y range
// covers the 31 tiles north of the gate row (up to the camp's north wall);
// X ranges straddle the gate inclusively to either side:
//   L2: x∈[gx-40, gx-14) — far west of gate
//   L1: x∈[gx-14, gx+ 1) — near west of gate
//   R1: x∈[gx+15, gx+31) — near east of gate
//   R2: x∈[gx+31, gx+42) — far east of gate
// The player-facing orientation (Straight=N, Left=W, Right=E) is the same
// in both W and N layouts because the gate is the camp's north wall in
// either case — only its position in BloodMoor-local coords moves.
struct GateTreeRect { int dxLo, dxHi; };
constexpr GateTreeRect kTreeRectL2 { -40, -14 };
constexpr GateTreeRect kTreeRectL1 { -14, +1 };
constexpr GateTreeRect kTreeRectR1 { +15, +31 };
constexpr GateTreeRect kTreeRectR2 { +31, +42 };
constexpr int          kTreeRectDyLo = -31;
constexpr int          kTreeRectDyHi = 0;

TellResult ComputeTreeRectAt(TellContext& ctx, GateTreeRect rect) {
    int gx, gy;
    if (!BloodMoorCampGate(ctx, gx, gy)) return "ERROR";
    const LevelMap& m = ctx.GetLevel(LevelId::BloodMoor);
    if (m.empty()) return "ERROR";
    const int x0 = gx + rect.dxLo, x1 = gx + rect.dxHi;
    const int y0 = gy + kTreeRectDyLo, y1 = gy + kTreeRectDyHi;
    return { std::to_string(CountTreesInRect(m, x0, x1, y0, y1)),
             TellLocation{ LevelId::BloodMoor, x0, y0, x1 - x0, y1 - y0 } };
}

TellResult ComputeBloodMoorTreesL2(TellContext& ctx) { return ComputeTreeRectAt(ctx, kTreeRectL2); }
TellResult ComputeBloodMoorTreesL1(TellContext& ctx) { return ComputeTreeRectAt(ctx, kTreeRectL1); }
TellResult ComputeBloodMoorTreesR1(TellContext& ctx) { return ComputeTreeRectAt(ctx, kTreeRectR1); }
TellResult ComputeBloodMoorTreesR2(TellContext& ctx) { return ComputeTreeRectAt(ctx, kTreeRectR2); }

TellResult ComputeBloodMoorIslandShape(TellContext& ctx) {
    const LevelMap& m = ctx.GetLevel(LevelId::BloodMoor);
    if (m.empty()) return "ERROR";

    const int W = m.sizeX, H = m.sizeY;
    const int N = W * H;

    std::vector<bool> blocked(N);
    for (int i = 0; i < N; ++i)
        blocked[i] = (m.coll[i] != kNoData) && (m.coll[i] & BlockWalk);

    std::vector<bool> visited(N, false);
    std::vector<bool> inIsland(N, false);
    std::vector<int>  queue;
    queue.reserve(N / 4);

    constexpr int kMinIslandTiles = 500;
    std::string               sharedShape;
    std::vector<TellLocation> locs;

    for (int start = 0; start < N; ++start) {
        if (!blocked[start] || visited[start]) continue;

        queue.clear();
        queue.push_back(start);
        visited[start] = true;

        bool touchesBorder = false;
        int  minX = W, maxX = 0, minY = H, maxY = 0;

        for (int qi = 0; qi < (int)queue.size(); ++qi) {
            const int cur = queue[qi];
            const int cx  = cur % W, cy = cur / W;
            if (cx < minX) minX = cx;
            if (cx > maxX) maxX = cx;
            if (cy < minY) minY = cy;
            if (cy > maxY) maxY = cy;
            if (cx == 0 || cx == W-1 || cy == 0 || cy == H-1) touchesBorder = true;
            const int nbrs[4][2] = {{cx-1,cy},{cx+1,cy},{cx,cy-1},{cx,cy+1}};
            for (auto& nb : nbrs) {
                const int nx = nb[0], ny = nb[1];
                if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                const int ni = ny * W + nx;
                if (!visited[ni] && blocked[ni]) { visited[ni] = true; queue.push_back(ni); }
            }
        }

        if (touchesBorder || (int)queue.size() < kMinIslandTiles) continue;

        locs.push_back({ LevelId::BloodMoor, minX, minY,
                         maxX - minX + 1, maxY - minY + 1 });

        // Mark island for 3x3 grid lookup, classify, then unmark.
        for (int idx : queue) inIsland[idx] = true;
        const char* shape = ClassifyIslandShape(queue, inIsland, W);
        for (int idx : queue) inIsland[idx] = false;

        if (sharedShape.empty()) {
            sharedShape = shape;
        } else if (sharedShape != shape) {
            return { "Other", std::move(locs) };
        }
    }

    if (sharedShape.empty()) return "none";
    return { sharedShape, std::move(locs) };
}

// --- Object-count tells ---
//
// Level-wide scans by objects.txt row index. Used by torch / chest / gold /
// shrine count tells across the Act 1 wilderness chain. Cheap: one pass over
// `m.presets` per tell, and the level is already cached by TellContext.

// Count of type=Object presets with the given txtFileNo. One TellLocation
// per hit so the renderer marks each one.
TellResult ObjectCountAt(TellContext& ctx, LevelId levelId, uint32_t txt) {
    const LevelMap& m = ctx.GetLevel(levelId);
    if (m.empty()) return "ERROR";
    std::vector<TellLocation> locs;
    int count = 0;
    for (const auto& p : m.presets) {
        if (p.type == uint16_t(PresetType::Object) && p.txtFileNo == txt) {
            ++count;
            locs.push_back({ levelId, p.x, p.y, 0, 0 });
        }
    }
    return { std::to_string(count), std::move(locs) };
}

// Binary "yes"/"no" presence. Location = first match.
TellResult ObjectPresentAt(TellContext& ctx, LevelId levelId, uint32_t txt) {
    const LevelMap& m = ctx.GetLevel(levelId);
    if (m.empty()) return "ERROR";
    for (const auto& p : m.presets) {
        if (p.type == uint16_t(PresetType::Object) && p.txtFileNo == txt) {
            return { "yes", TellLocation{ levelId, p.x, p.y, 0, 0 } };
        }
    }
    return "no";
}

// Count of rooms that contain at least one object with txtA AND one with txtB.
// One TellLocation per matching room (the room rect).
TellResult RoomsWithBothObjectsAt(TellContext& ctx, LevelId levelId,
                                  uint32_t txtA, uint32_t txtB) {
    const LevelMap& m = ctx.GetLevel(levelId);
    if (m.empty()) return "ERROR";
    std::vector<TellLocation> locs;
    int count = 0;
    for (const auto& r : m.rooms) {
        bool hasA = false, hasB = false;
        for (const auto& p : m.presets) {
            if (p.type != uint16_t(PresetType::Object)) continue;
            if (p.x < r.x || p.x >= r.x + r.sizeX) continue;
            if (p.y < r.y || p.y >= r.y + r.sizeY) continue;
            if      (p.txtFileNo == txtA) hasA = true;
            else if (p.txtFileNo == txtB) hasB = true;
            if (hasA && hasB) break;
        }
        if (hasA && hasB) {
            ++count;
            locs.push_back({ levelId, r.x, r.y, r.sizeX, r.sizeY });
        }
    }
    return { std::to_string(count), std::move(locs) };
}

// Den of Evil object tells.
TellResult ComputeDenOfEvilTorches   (TellContext& ctx) { return ObjectCountAt  (ctx, LevelId::DenOfEvil, 37);  }
TellResult ComputeDenOfEvilChest     (TellContext& ctx) { return ObjectPresentAt(ctx, LevelId::DenOfEvil, 240); }
TellResult ComputeDenOfEvilGold      (TellContext& ctx) { return ObjectPresentAt(ctx, LevelId::DenOfEvil, 269); }
TellResult ComputeDenOfEvilShrine2   (TellContext& ctx) { return ObjectCountAt  (ctx, LevelId::DenOfEvil,   2); }
TellResult ComputeDenOfEvilShrine81  (TellContext& ctx) { return ObjectCountAt  (ctx, LevelId::DenOfEvil,  81); }
TellResult ComputeDenOfEvilShrine83  (TellContext& ctx) { return ObjectCountAt  (ctx, LevelId::DenOfEvil,  83); }
TellResult ComputeDenOfEvilShrine84  (TellContext& ctx) { return ObjectCountAt  (ctx, LevelId::DenOfEvil,  84); }
TellResult ComputeDenOfEvilShrine130 (TellContext& ctx) { return ObjectCountAt  (ctx, LevelId::DenOfEvil, 130); }

// Wilderness object tells. Per-shrine count tells moved to the room-based
// ACT1_WILD_ROOMS X-macro (defined in tells_rooms.cpp) so shrine values
// come from the room classifier and stay consistent with per-position
// tells.
TellResult ComputeBloodMoorTorchChest  (TellContext& ctx) { return RoomsWithBothObjectsAt(ctx, LevelId::BloodMoor,  37, 240); }
TellResult ComputeColdPlainsTorchChest (TellContext& ctx) { return RoomsWithBothObjectsAt(ctx, LevelId::ColdPlains, 37, 240); }
TellResult ComputeStonyFieldGold       (TellContext& ctx) { return ObjectCountAt(ctx, LevelId::StonyField, 269); }

// --- Shared prereq lists ---
//
// kReqNWGate: the focus subset of the project — camp exit on the north side
// of Rogue Encampment, Blood Moor exit toward Cold Plains on the west. Many
// Blood Moor tells use hard-coded coordinate windows that only make sense in
// this layout (see CLAUDE.md).
// We gate the camp orientation on BloodMoorCampExit (read from BloodMoor's
// adjacency list) rather than CampExit (read from RogueCamp) so prereq
// chains that only need BloodMoor don't drag in a RogueCamp extraction.
// BloodMoorCampExit is in player-walk terms (matches CampExit by convention),
// so the focus subset is "N", not the BloodMoor-centric mirror.
const std::vector<Requirement> kReqNWGate = {
    { "BloodMoorCampExit", {"N"} },
    { "BloodMoorExit",     {"W"} },
};

// kReqNCampGate: a strict superset of kReqNWGate that also covers the
// BloodMoorExit=N layout. Used by tells anchored to the camp gate
// reference (BloodMoorCampGate) — trees, town-front rooms, chest/bed
// proximity — which work identically in both layouts once the reference
// point is parameterised.
const std::vector<Requirement> kReqNCampGate = {
    { "BloodMoorCampExit", {"N"} },
    { "BloodMoorExit",     {"W", "N"} },
};

// Tells that only make sense once we know the SW corner is missing.
// Transitively inherits kReqNWGate via BloodMoorSWCorner.
const std::vector<Requirement> kReqSWMissing = {
    { "BloodMoorSWCorner", {"missing"} },
};

// Symmetric NW-corner sub-tell prereq.
const std::vector<Requirement> kReqNWCornerMissing = {
    { "BloodMoorNWCorner", {"missing"} },
};

// Tower Cellar exit position relative to the entrance, in renderer-iso
// screen space. Inside each tower level the player arrives at one staircase
// (up to the previous level) and leaves at another (down to the next).
// Compass directions are awkward here because the rooms are tight and a
// player rarely sees both stairs at once — but they can usually judge which
// is higher on the minimap and which is further left, *if* the offset is
// large enough.
//
// Renderer iso (projection.ts):
//   screen_x = 2 * (x - y)   smaller = further left
//   screen_y =      x + y    smaller = higher up on screen
//
// `ExitAbove` = exit is above the entrance on screen (smaller screen_y).
// `ExitLeft`  = exit is left of the entrance on screen (smaller screen_x).
// The two axes are independent — any combination is possible.
//
// We emit a strict yes/no even when the screen-axis delta is tiny; the
// lookup UI is responsible for letting the player answer "uncertain".
TellResult ComputeTowerExitAxis(TellContext& ctx, LevelId self,
                                LevelId entranceDest, LevelId exitDest,
                                bool wantAbove) {
    const LevelMap& m = ctx.GetLevel(self);
    if (m.empty()) return "ERROR";
    int t, entX, entY, exX, exY;
    if (!FindStairsTo(m, entranceDest, t, entX, entY)) return "ERROR";
    if (!FindStairsTo(m, exitDest,     t, exX,  exY))  return "ERROR";

    const std::vector<TellLocation> locs{
        { self, entX, entY, 0, 0 },
        { self, exX,  exY,  0, 0 },
    };
    // The factor of 2 on screen_x drops out of a sign comparison, so we
    // compare (x - y) directly.
    const bool flag = wantAbove
        ? (exX + exY) < (entX + entY)
        : (exX - exY) < (entX - entY);
    return { flag ? "yes" : "no", locs };
}

TellResult ComputeTowerExit1Above(TellContext& ctx) {
    return ComputeTowerExitAxis(ctx, LevelId::TowerCellarLevel1,
        LevelId::ForgottenTower,     LevelId::TowerCellarLevel2, true);
}
TellResult ComputeTowerExit1Left(TellContext& ctx) {
    return ComputeTowerExitAxis(ctx, LevelId::TowerCellarLevel1,
        LevelId::ForgottenTower,     LevelId::TowerCellarLevel2, false);
}
TellResult ComputeTowerExit2Above(TellContext& ctx) {
    return ComputeTowerExitAxis(ctx, LevelId::TowerCellarLevel2,
        LevelId::TowerCellarLevel1,  LevelId::TowerCellarLevel3, true);
}
TellResult ComputeTowerExit2Left(TellContext& ctx) {
    return ComputeTowerExitAxis(ctx, LevelId::TowerCellarLevel2,
        LevelId::TowerCellarLevel1,  LevelId::TowerCellarLevel3, false);
}
TellResult ComputeTowerExit3Above(TellContext& ctx) {
    return ComputeTowerExitAxis(ctx, LevelId::TowerCellarLevel3,
        LevelId::TowerCellarLevel2,  LevelId::TowerCellarLevel4, true);
}
TellResult ComputeTowerExit3Left(TellContext& ctx) {
    return ComputeTowerExitAxis(ctx, LevelId::TowerCellarLevel3,
        LevelId::TowerCellarLevel2,  LevelId::TowerCellarLevel4, false);
}
TellResult ComputeTowerExit4Above(TellContext& ctx) {
    return ComputeTowerExitAxis(ctx, LevelId::TowerCellarLevel4,
        LevelId::TowerCellarLevel3,  LevelId::TowerCellarLevel5, true);
}
TellResult ComputeTowerExit4Left(TellContext& ctx) {
    return ComputeTowerExitAxis(ctx, LevelId::TowerCellarLevel4,
        LevelId::TowerCellarLevel3,  LevelId::TowerCellarLevel5, false);
}

// Tower Cellar room-template classifier. The Forgotten Tower's four
// navigable cellar levels (L1-L4) share an identical room generator —
// same template set 109-146 minus 138, verified across 2348 seeds on
// every level — so a single classifier serves all of them.
//
// Names follow a "<Shape><Feature><Direction>" convention where the
// direction is the player-facing orientation seen in renderer iso (the
// direction a player has to walk out of the room to reach the rest of
// the level).
//
// NOTE — empirical cross-tell correlation between the up and down stairs:
//   The up-stair and down-stair roomNumbers are 100% correlated on every
//   tower level: only 4 of the 16 combinations occur (139/146, 141/143,
//   142/144, 140/145). A consequence is that pairs A (139/146) and C
//   (142/144) are both 1-torch on each stair and only distinguishable by
//   template shape. We deliberately don't encode that correlation here —
//   the extractor stays seed-agnostic; a later UI / index optimizer pass
//   can mark impossible answer combinations as "uncertain" instead.
const char* TowerRoomName(uint32_t roomNo) {
    switch (roomNo) {
        // Stair rooms — one of each per seed; up vs down are 100% correlated.
        case 139: return "StairsUpW";
        case 140: return "StairsUpE";
        case 141: return "StairsUpS";
        case 142: return "StairsUpN";
        case 143: return "StairsDownW";
        case 144: return "StairsDownE";
        case 145: return "StairsDownS";
        case 146: return "StairsDownN";

        // Themed rooms — at most one cell per seed.
        // Two torches frame the entrance corridor; the caskets are interior.
        case 124: return "DeadEndCasketsW";
        case 125: return "DeadEndCandlesE";
        case 126: return "CorridorTorchesWE";
        // TODO: identify obj 580 visually in the renderer — it's the
        // distinctive non-candle feature here but unnamed in objects.txt.
        case 127: return "DeadEndSmallCandlesS";
        // TODO: better visual identification of the objects in this room
        // (holes + flies signature, but the tileset deserves a second look).
        case 128: return "CorridorHolesSW";
        case 129: return "CorridorArmorySE";
        // 29× fire-small + 16× fire-medium laid out as a pentagram —
        // the most striking room in the level.
        case 130: return "FirePentagram";
        case 131: return "DeadEndBraziersN";
        case 132: return "CorridorSingleDeadRogueNW";
        case 133: return "CorridorArmoryLightNE";
        case 134: return "IntersectDoubleDeadRogueFire";
        case 137: return "IntersectBraziers";

        // "DeadEndEmpty<D>" rooms have no presets — players identify them
        // by tile shape and orientation alone.
        case 109: return "DeadEndEmptyW";
        case 110: return "DeadEndEmptyE";
        case 112: return "DeadEndEmptyS";
        case 116: return "DeadEndEmptyN";

        // Room 135 (only shrine-bearing room) and room 136 (candle+casket,
        // hard to identify visually) deliberately not enumerated here.
        // Room 135 is covered by the level-wide shrine count tell.
        default:  return "Other";
    }
}

// Count of rooms with the given rooms.txt row index on `selfLevel`.
// Empirically the themed rooms never duplicate per seed but the four
// DeadEndEmpty* rooms do (up to 3× observed in the 78k sample), so a
// count is the right shape regardless of template.
TellResult ComputeTowerRoomCount(TellContext& ctx, LevelId selfLevel, uint32_t roomNo) {
    const LevelMap& m = ctx.GetLevel(selfLevel);
    if (m.empty()) return "ERROR";
    std::vector<TellLocation> locs;
    int count = 0;
    for (const auto& r : m.rooms) {
        if (r.roomNumber == roomNo) {
            ++count;
            locs.push_back({ selfLevel, r.x, r.y, r.sizeX, r.sizeY });
        }
    }
    return { std::to_string(count), std::move(locs) };
}

// Level-wide shrine count. The tower only ever spawns one shrine variant
// (txt 77, "healthorama") in a single room template (135); a count
// generalises better than a per-room tell and gives the player an easy
// yes/no-style question.
TellResult ComputeTowerShrines(TellContext& ctx, LevelId selfLevel) {
    const LevelMap& m = ctx.GetLevel(selfLevel);
    if (m.empty()) return "ERROR";
    std::vector<TellLocation> locs;
    int count = 0;
    for (const auto& p : m.presets) {
        if (p.type == uint16_t(PresetType::Object)
            && p.kind == ObjectKind::Shrine) {
            ++count;
            locs.push_back({ selfLevel, p.x, p.y, 0, 0 });
        }
    }
    return { std::to_string(count), std::move(locs) };
}

// TowerRoomName of the room containing the stair preset going to
// `destLevel` on `selfLevel`. The four stair-up rooms (139-142) and four
// stair-down rooms (143-146) each map to a "StairsUp<D>" / "StairsDown<D>"
// directional name.
TellResult ComputeTowerStairRoom(TellContext& ctx, LevelId selfLevel, LevelId destLevel) {
    const LevelMap& m = ctx.GetLevel(selfLevel);
    if (m.empty()) return "ERROR";
    int t, sx, sy;
    if (!FindStairsTo(m, destLevel, t, sx, sy)) return "ERROR";
    for (const auto& r : m.rooms) {
        if (sx >= r.x && sx < r.x + r.sizeX
            && sy >= r.y && sy < r.y + r.sizeY) {
            return { TowerRoomName(r.roomNumber),
                     TellLocation{ selfLevel, r.x, r.y, r.sizeX, r.sizeY } };
        }
    }
    return "ERROR";
}

// "Room immediately adjacent to the stair room", decoded from the stair
// room's name suffix. The suffix is the direction the player walks out of
// the stair room to reach the rest of the level; we step one room cell in
// that direction (game tile coords: W = -X, E = +X, N = -Y, S = +Y — same
// convention as DirectionTo above) and return that room's TowerRoomName.
TellResult ComputeTowerStairAdjacent(TellContext& ctx, LevelId selfLevel, LevelId destLevel) {
    const LevelMap& m = ctx.GetLevel(selfLevel);
    if (m.empty()) return "ERROR";
    int t, sx, sy;
    if (!FindStairsTo(m, destLevel, t, sx, sy)) return "ERROR";

    const Room* stair = nullptr;
    for (const auto& r : m.rooms) {
        if (sx >= r.x && sx < r.x + r.sizeX
            && sy >= r.y && sy < r.y + r.sizeY) {
            stair = &r; break;
        }
    }
    if (!stair) return "ERROR";

    const std::string name = TowerRoomName(stair->roomNumber);
    if (name.empty()) return "ERROR";
    int dx = 0, dy = 0;
    switch (name.back()) {
        case 'W': dx = -stair->sizeX; break;
        case 'E': dx = +stair->sizeX; break;
        case 'N': dy = -stair->sizeY; break;
        case 'S': dy = +stair->sizeY; break;
        default:  return "ERROR";
    }
    const int nx = stair->x + dx, ny = stair->y + dy;
    for (const auto& r : m.rooms) {
        if (r.x == nx && r.y == ny) {
            return { TowerRoomName(r.roomNumber),
                     TellLocation{ selfLevel, r.x, r.y, r.sizeX, r.sizeY } };
        }
    }
    return "ERROR";
}

// Per-level compute-fn instantiation for the four navigable tower levels.
// L5 is the bottom (Countess room) and isn't enumerated — its layout is
// special and doesn't have a "down" exit. Each TOWER_LEVEL row generates
// 5 compute fns (StairsUp/Down, StairsUp/DownAdjacent, Shrines).
#define TOWER_LEVEL(N, SELF, UP, DOWN) \
    TellResult ComputeTower##N##StairsUp(TellContext& ctx) { \
        return ComputeTowerStairRoom(ctx, LevelId::SELF, LevelId::UP); \
    } \
    TellResult ComputeTower##N##StairsDown(TellContext& ctx) { \
        return ComputeTowerStairRoom(ctx, LevelId::SELF, LevelId::DOWN); \
    } \
    TellResult ComputeTower##N##StairsUpAdjacent(TellContext& ctx) { \
        return ComputeTowerStairAdjacent(ctx, LevelId::SELF, LevelId::UP); \
    } \
    TellResult ComputeTower##N##StairsDownAdjacent(TellContext& ctx) { \
        return ComputeTowerStairAdjacent(ctx, LevelId::SELF, LevelId::DOWN); \
    } \
    TellResult ComputeTower##N##Shrines(TellContext& ctx) { \
        return ComputeTowerShrines(ctx, LevelId::SELF); \
    }
TOWER_LEVEL(1, TowerCellarLevel1, ForgottenTower,    TowerCellarLevel2)
TOWER_LEVEL(2, TowerCellarLevel2, TowerCellarLevel1, TowerCellarLevel3)
TOWER_LEVEL(3, TowerCellarLevel3, TowerCellarLevel2, TowerCellarLevel4)
TOWER_LEVEL(4, TowerCellarLevel4, TowerCellarLevel3, TowerCellarLevel5)
#undef TOWER_LEVEL

// X-macro list of classified tower room templates. Reused below to
// generate registry entries — add a row here and the count tell appears
// on all four levels automatically.
#define TOWER_ROOMS \
    X(DeadEndCasketsW,              124) \
    X(DeadEndCandlesE,              125) \
    X(CorridorTorchesWE,            126) \
    X(DeadEndSmallCandlesS,         127) \
    X(CorridorHolesSW,              128) \
    X(CorridorArmorySE,             129) \
    X(FirePentagram,                130) \
    X(DeadEndBraziersN,             131) \
    X(CorridorSingleDeadRogueNW,    132) \
    X(CorridorArmoryLightNE,        133) \
    X(IntersectDoubleDeadRogueFire, 134) \
    X(IntersectBraziers,            137) \
    X(DeadEndEmptyW,                109) \
    X(DeadEndEmptyE,                110) \
    X(DeadEndEmptyS,                112) \
    X(DeadEndEmptyN,                116)

#define X(NAME, ID) \
    TellResult ComputeTower1##NAME(TellContext& ctx) { return ComputeTowerRoomCount(ctx, LevelId::TowerCellarLevel1, ID); } \
    TellResult ComputeTower2##NAME(TellContext& ctx) { return ComputeTowerRoomCount(ctx, LevelId::TowerCellarLevel2, ID); } \
    TellResult ComputeTower3##NAME(TellContext& ctx) { return ComputeTowerRoomCount(ctx, LevelId::TowerCellarLevel3, ID); } \
    TellResult ComputeTower4##NAME(TellContext& ctx) { return ComputeTowerRoomCount(ctx, LevelId::TowerCellarLevel4, ID); }
TOWER_ROOMS
#undef X


// --- Registry ---
//
// Tell field order: { name, actNo, levels, prereqs, compute }.

const Tell kAll[] = {
	// Camp
    { "CampExit",                   ActId::Act1, { LevelId::RogueCamp },  {}, ComputeCampExit },

	// Tower Cellar L1-L4 — same room generator across all four levels.
	// TowerExit*Above/Left: down-stair vs up-stair position in renderer-iso
	// screen space (two binary tells per level).
	// Tower<N>Stairs[Up|Down]: TowerRoomName of the stair-room template.
	// Tower<N>Stairs[Up|Down]Adjacent: TowerRoomName of the room one cell
	//   beyond the stair-room, in the direction the stair-name encodes.
	// Tower<N>Shrines: level-wide shrine count.
	// Tower<N><RoomName>: count of each classified room template (X-macro).
    { "TowerExit1Above",         ActId::Act1, { LevelId::TowerCellarLevel1 }, {}, ComputeTowerExit1Above },
    { "TowerExit1Left",          ActId::Act1, { LevelId::TowerCellarLevel1 }, {}, ComputeTowerExit1Left },
    { "TowerExit2Above",         ActId::Act1, { LevelId::TowerCellarLevel2 }, {}, ComputeTowerExit2Above },
    { "TowerExit2Left",          ActId::Act1, { LevelId::TowerCellarLevel2 }, {}, ComputeTowerExit2Left },
    { "TowerExit3Above",         ActId::Act1, { LevelId::TowerCellarLevel3 }, {}, ComputeTowerExit3Above },
    { "TowerExit3Left",          ActId::Act1, { LevelId::TowerCellarLevel3 }, {}, ComputeTowerExit3Left },
    { "TowerExit4Above",         ActId::Act1, { LevelId::TowerCellarLevel4 }, {}, ComputeTowerExit4Above },
    { "TowerExit4Left",          ActId::Act1, { LevelId::TowerCellarLevel4 }, {}, ComputeTowerExit4Left },

#define TOWER_LEVEL_TELLS(N, SELF) \
    { "Tower" #N "StairsUp",           ActId::Act1, { LevelId::SELF }, {}, ComputeTower##N##StairsUp }, \
    { "Tower" #N "StairsDown",         ActId::Act1, { LevelId::SELF }, {}, ComputeTower##N##StairsDown }, \
    { "Tower" #N "StairsUpAdjacent",   ActId::Act1, { LevelId::SELF }, {}, ComputeTower##N##StairsUpAdjacent }, \
    { "Tower" #N "StairsDownAdjacent", ActId::Act1, { LevelId::SELF }, {}, ComputeTower##N##StairsDownAdjacent }, \
    { "Tower" #N "Shrines",            ActId::Act1, { LevelId::SELF }, {}, ComputeTower##N##Shrines },
    TOWER_LEVEL_TELLS(1, TowerCellarLevel1)
    TOWER_LEVEL_TELLS(2, TowerCellarLevel2)
    TOWER_LEVEL_TELLS(3, TowerCellarLevel3)
    TOWER_LEVEL_TELLS(4, TowerCellarLevel4)
#undef TOWER_LEVEL_TELLS

#define X(NAME, ID) \
    { "Tower1" #NAME, ActId::Act1, { LevelId::TowerCellarLevel1 }, {}, ComputeTower1##NAME }, \
    { "Tower2" #NAME, ActId::Act1, { LevelId::TowerCellarLevel2 }, {}, ComputeTower2##NAME }, \
    { "Tower3" #NAME, ActId::Act1, { LevelId::TowerCellarLevel3 }, {}, ComputeTower3##NAME }, \
    { "Tower4" #NAME, ActId::Act1, { LevelId::TowerCellarLevel4 }, {}, ComputeTower4##NAME },
    TOWER_ROOMS
#undef X
#undef TOWER_ROOMS


	// Blood Moor
    { "BloodMoorExit",              ActId::Act1, { LevelId::BloodMoor },  {}, ComputeBloodMoorExit },
    { "BloodMoorCampExit",          ActId::Act1, { LevelId::BloodMoor },  {}, ComputeBloodMoorCampExit },
    { "BloodMoorDenOfEvilEntrance", ActId::Act1, { LevelId::BloodMoor },  {}, ComputeBloodMoorDenOfEvilEntrance },
    { "BloodMoorNumIslands",        ActId::Act1, { LevelId::BloodMoor },  {}, ComputeBloodMoorNumIslands },
    { "BloodMoorColdPlainsPos", ActId::Act1, { LevelId::BloodMoor }, kReqNWGate, ComputeBloodMoorColdPlainsPos },
    { "BloodMoorChest",         ActId::Act1, { LevelId::BloodMoor }, kReqNCampGate, ComputeBloodMoorChest },
    { "BloodMoorBed",           ActId::Act1, { LevelId::BloodMoor }, kReqNCampGate, ComputeBloodMoorBed },
    { "DenOfEvilPos",         ActId::Act1, { LevelId::BloodMoor }, {},            ComputeDenOfEvilPos },
    { "BloodMoorIslandShape", ActId::Act1, { LevelId::BloodMoor }, {}, ComputeBloodMoorIslandShape },
    { "BloodMoorTreesL2",     ActId::Act1, { LevelId::BloodMoor }, kReqNCampGate, ComputeBloodMoorTreesL2 },
    { "BloodMoorTreesL1",     ActId::Act1, { LevelId::BloodMoor }, kReqNCampGate, ComputeBloodMoorTreesL1 },
    { "BloodMoorTreesR1",     ActId::Act1, { LevelId::BloodMoor }, kReqNCampGate, ComputeBloodMoorTreesR1 },
    { "BloodMoorTreesR2",     ActId::Act1, { LevelId::BloodMoor }, kReqNCampGate, ComputeBloodMoorTreesR2 },
    { "BloodMoorSWCorner",      ActId::Act1, { LevelId::BloodMoor }, kReqNWGate,          ComputeBloodMoorSWCorner },
    { "BloodMoorSWCornerNorth", ActId::Act1, { LevelId::BloodMoor }, kReqSWMissing,       ComputeBloodMoorSWCornerNorth },
    { "BloodMoorSWCornerEast",  ActId::Act1, { LevelId::BloodMoor }, kReqSWMissing,       ComputeBloodMoorSWCornerEast },
    { "BloodMoorNWCorner",      ActId::Act1, { LevelId::BloodMoor }, kReqNWGate,          ComputeBloodMoorNWCorner },
    { "BloodMoorNWCornerSouth", ActId::Act1, { LevelId::BloodMoor }, kReqNWCornerMissing, ComputeBloodMoorNWCornerSouth },
    { "BloodMoorNWCornerEast",  ActId::Act1, { LevelId::BloodMoor }, kReqNWCornerMissing, ComputeBloodMoorNWCornerEast },
    // Town-front rooms (3 cells one row N of the camp gate).
    { "BloodMoorTownStraight",   ActId::Act1, { LevelId::BloodMoor }, kReqNCampGate, ComputeBloodMoorTownStraight },
    { "BloodMoorTownLeft",       ActId::Act1, { LevelId::BloodMoor }, kReqNCampGate, ComputeBloodMoorTownLeft },
    { "BloodMoorTownRight",      ActId::Act1, { LevelId::BloodMoor }, kReqNCampGate, ComputeBloodMoorTownRight },

    // Cold Plains exit POV (3 cells: immediate L/R + Straight skipping the
    // exit cell itself). Works for both W and N exit layouts since
    // ExitNeighborAt rotates by SourceDirection.
    { "BloodMoorColdPlainsStraight", ActId::Act1, { LevelId::BloodMoor }, {}, ComputeBloodMoorColdPlainsStraight },
    { "BloodMoorColdPlainsLeft",     ActId::Act1, { LevelId::BloodMoor }, {}, ComputeBloodMoorColdPlainsLeft },
    { "BloodMoorColdPlainsRight",    ActId::Act1, { LevelId::BloodMoor }, {}, ComputeBloodMoorColdPlainsRight },

    // Den of Evil entrance 8-cell compass surround.
    { "BloodMoorDenOfEvilN",  ActId::Act1, { LevelId::BloodMoor }, {}, ComputeBloodMoorDenOfEvilN },
    { "BloodMoorDenOfEvilNE", ActId::Act1, { LevelId::BloodMoor }, {}, ComputeBloodMoorDenOfEvilNE },
    { "BloodMoorDenOfEvilE",  ActId::Act1, { LevelId::BloodMoor }, {}, ComputeBloodMoorDenOfEvilE },
    { "BloodMoorDenOfEvilSE", ActId::Act1, { LevelId::BloodMoor }, {}, ComputeBloodMoorDenOfEvilSE },
    { "BloodMoorDenOfEvilS",  ActId::Act1, { LevelId::BloodMoor }, {}, ComputeBloodMoorDenOfEvilS },
    { "BloodMoorDenOfEvilSW", ActId::Act1, { LevelId::BloodMoor }, {}, ComputeBloodMoorDenOfEvilSW },
    { "BloodMoorDenOfEvilW",  ActId::Act1, { LevelId::BloodMoor }, {}, ComputeBloodMoorDenOfEvilW },
    { "BloodMoorDenOfEvilNW", ActId::Act1, { LevelId::BloodMoor }, {}, ComputeBloodMoorDenOfEvilNW },

    { "BloodMoorTorchChest",     ActId::Act1, { LevelId::BloodMoor },  {}, ComputeBloodMoorTorchChest },

	// Den of Evil
    { "DenOfEvilBoss",           ActId::Act1, { LevelId::DenOfEvil }, {}, ComputeDenOfEvilBoss },
    { "DenOfEvilTorches",        ActId::Act1, { LevelId::DenOfEvil }, {}, ComputeDenOfEvilTorches },
    { "DenOfEvilChest",          ActId::Act1, { LevelId::DenOfEvil }, {}, ComputeDenOfEvilChest },
    { "DenOfEvilGold",           ActId::Act1, { LevelId::DenOfEvil }, {}, ComputeDenOfEvilGold },

	// Cold Plains
    { "ColdPlainsExit",             ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsExit },
    { "ColdPlainsNumIslands",       ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsNumIslands },

    { "ColdPlainsWaypointSide",                ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsWaypointSide },
    { "ColdPlainsWaypointForward",             ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsWaypointForward },
    { "ColdPlainsWaypointForwardLeft",         ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsWaypointForwardLeft },
    { "ColdPlainsWaypointForwardRight",        ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsWaypointForwardRight },
    { "ColdPlainsWaypointLeft",                ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsWaypointLeft },
    { "ColdPlainsWaypointRight",               ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsWaypointRight },
    { "ColdPlainsCaveLevel1Entrance",                ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsCaveLevel1Entrance },

    // Stony Field exit POV (3 cells, same shape as the BloodMoor exit
    // tells). Straight skips the exit cell.
    { "ColdPlainsStonyFieldStraight", ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsStonyFieldStraight },
    { "ColdPlainsStonyFieldLeft",     ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsStonyFieldLeft },
    { "ColdPlainsStonyFieldRight",    ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsStonyFieldRight },

    // Cave Level 1 entrance 8-cell compass surround.
    { "ColdPlainsCaveLevel1N",  ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsCaveLevel1N },
    { "ColdPlainsCaveLevel1NE", ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsCaveLevel1NE },
    { "ColdPlainsCaveLevel1E",  ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsCaveLevel1E },
    { "ColdPlainsCaveLevel1SE", ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsCaveLevel1SE },
    { "ColdPlainsCaveLevel1S",  ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsCaveLevel1S },
    { "ColdPlainsCaveLevel1SW", ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsCaveLevel1SW },
    { "ColdPlainsCaveLevel1W",  ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsCaveLevel1W },
    { "ColdPlainsCaveLevel1NW", ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsCaveLevel1NW },

    { "ColdPlainsTorchChest",    ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlainsTorchChest },

	// Stony Field
    { "StonyFieldGold",          ActId::Act1, { LevelId::StonyField }, {}, ComputeStonyFieldGold },
    { "StonyFieldUndergroundPassageLevel1Entrance", ActId::Act1, { LevelId::StonyField }, {}, ComputeStonyFieldUndergroundPassageLevel1Entrance },
    { "StonyFieldNumIslands",       ActId::Act1, { LevelId::StonyField }, {}, ComputeStonyFieldNumIslands },

    // Per-template room counts for the three Act 1 wilderness levels.
    // Generated from ACT1_WILD_ROOMS — each row produces three entries
    // (BloodMoor<NAME>, ColdPlains<NAME>, StonyField<NAME>).
#define X(NAME) \
    { "BloodMoor" #NAME,  ActId::Act1, { LevelId::BloodMoor },  {}, ComputeBloodMoor##NAME }, \
    { "ColdPlains" #NAME, ActId::Act1, { LevelId::ColdPlains }, {}, ComputeColdPlains##NAME }, \
    { "StonyField" #NAME, ActId::Act1, { LevelId::StonyField }, {}, ComputeStonyField##NAME },
    ACT1_WILD_ROOMS
#undef X

	// Arcane Sanctuary
    { "ArcaneExit", ActId::Act2, { LevelId::ArcaneSanctuary }, {}, ComputeArcaneExit },
};
constexpr size_t kNumTells = sizeof(kAll) / sizeof(kAll[0]);

std::vector<const Tell*> g_allList;
std::vector<const Tell*> g_topoList;
bool                     g_topoBuilt = false;

// Topologically sort g_allList so prereqs come before dependents. Cycles fire
// an assert (registry bug, not a runtime concern).
void BuildTopoList() {
    g_topoList.clear();
    g_topoList.reserve(kNumTells);

    enum class Mark { Unvisited, Visiting, Done };
    std::unordered_map<const Tell*, Mark> mark;
    for (size_t i = 0; i < kNumTells; ++i) mark[&kAll[i]] = Mark::Unvisited;

    auto visit = [&](const Tell* t, auto& self) -> void {
        Mark& m = mark[t];
        if (m == Mark::Done) return;
        if (m == Mark::Visiting) {
            fprintf(stderr, "tells.cpp: prereq cycle through %s\n", t->name);
            abort();
        }
        m = Mark::Visiting;
        for (const auto& r : t->prereqs) {
            const Tell* dep = FindTell(r.tellName);
            if (!dep) {
                fprintf(stderr, "tells.cpp: %s depends on unknown tell %s\n",
                        t->name, r.tellName);
                abort();
            }
            if (dep->actNo != t->actNo) {
                fprintf(stderr, "tells.cpp: %s (act %u) depends on %s (act %u);"
                        " cross-act prereqs not supported\n",
                        t->name, ActNo(t->actNo) + 1,
                        dep->name, ActNo(dep->actNo) + 1);
                abort();
            }
            self(dep, self);
        }
        m = Mark::Done;
        g_topoList.push_back(t);
    };

    for (size_t i = 0; i < kNumTells; ++i) visit(&kAll[i], visit);
    g_topoBuilt = true;
}

} // anonymous

// See declaration near the top of this file for context. Implementation
// here (rather than in the anonymous namespace) because tells_rooms.cpp
// needs to call it for the town-front room tells.
bool BloodMoorCampGate(TellContext& ctx, int& gx, int& gy) {
    const LevelMap& m = ctx.GetLevel(LevelId::BloodMoor);
    if (m.empty()) return false;
    int sumX = 0, sumY = 0, count = 0;
    for (const auto& a : m.adjacents) {
        if (a.levelNo == LvlId(LevelId::ColdPlains)) {
            sumX += a.bridgeX; sumY += a.bridgeY; ++count;
        }
    }
    if (count == 0) return false;
    const int cx = sumX / count - m.sizeX / 2;
    const int cy = sumY / count - m.sizeY / 2;
    if (std::abs(cx) > std::abs(cy)) {
        if (cx < 0) { gx = 344; gy = 279; return true; }  // BloodMoorExit=W
    } else {
        if (cy < 0) { gx = 144; gy = 479; return true; }  // BloodMoorExit=N
    }
    return false;
}

const Tell* FindTell(const std::string& name) {
    for (size_t i = 0; i < kNumTells; ++i)
        if (name == kAll[i].name) return &kAll[i];
    return nullptr;
}

const std::vector<const Tell*>& AllTells() {
    if (g_allList.size() != kNumTells) {
        g_allList.clear();
        for (size_t i = 0; i < kNumTells; ++i) g_allList.push_back(&kAll[i]);
    }
    return g_allList;
}

const std::vector<const Tell*>& AllTellsTopoSorted() {
    if (!g_topoBuilt) BuildTopoList();
    return g_topoList;
}

const Tell* ResolvePrereq(const Requirement& r) {
    return FindTell(r.tellName);
}

} // namespace MapData
