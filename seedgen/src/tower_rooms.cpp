#include "tower_rooms.h"

namespace MapData {

namespace {

constexpr uint16_t kGraveTile  = BlockWalk | AlternateTile;            // 0x0011
constexpr uint16_t kPillar25   = BlockWalk | Wall | Blank;             // 0x0025
constexpr uint16_t kPillar07   = BlockWalk | BlockLineOfSight | Wall;  // 0x0007

struct Slot        { int dx, dy, w, h; };
struct PillarProbe { int dx, dy; uint16_t value; };

// One visual variant of a room: a name, the canonical pillar probe that
// distinguishes it from the room's other variant, and its ordered list of
// grave slots. For single-variant rooms the probe is ignored and the name
// is empty.
struct VariantDef {
    const char* name;
    PillarProbe probe;
    int         slotCount;
    Slot        slots[TowerRoom::kMaxSlots];
};

struct RoomDef {
    uint32_t    roomId;
    const char* shape;     // "W", "ESW", "NESW", etc.
    const char* theme;     // "Empty", "StairsUp", "Caskets", ...
    int         numVariants;
    VariantDef  variants[2];
};

// Shortcut for themed rooms with no variants and no graves.
#define EMPTY_VARIANT {"", {0, 0, 0}, 0, {}}

// All known tower-cellar room templates. Variant 0 is listed first; the
// 1-bit variant field in the encoded form selects between [0] and [1].
// Slots are listed in the order they were defined when we reverse-engineered
// each room — that ordering is part of the on-disk format, do not reshuffle.
static const RoomDef kRooms[] = {
    // -------- Dead-end family (1 exit each).
    //   Empty (grave-bearing) rooms have wide/narrow variants. The themed,
    //   stair, and shrine rooms in each shape have no variants and no graves.
    {109, "W", "Empty",   2, {
        {"Wide",   {10, 10, kPillar25}, 6, {
            {16, 10, 2, 4}, {21, 10, 2, 4}, {25, 16, 4, 2},
            {25, 21, 4, 2}, {21, 25, 2, 4}, {16, 25, 2, 4},
        }},
        {"Narrow", {10, 15, kPillar25}, 1, {
            { 5, 21, 4, 2},
        }},
    }},
    {110, "E", "Empty",   2, {
        {"Wide",   {10, 10, kPillar25}, 6, {
            {21, 25, 2, 4}, {16, 25, 2, 4}, {10, 21, 4, 2},
            {10, 16, 4, 2}, {16, 10, 2, 4}, {21, 10, 2, 4},
        }},
        {"Narrow", {35, 15, kPillar25}, 1, {
            {35, 21, 4, 2},
        }},
    }},
    {112, "S", "Empty",   2, {
        {"Wide",   {10, 10, kPillar25}, 6, {
            {10, 21, 4, 2}, {10, 16, 4, 2}, {16, 10, 2, 4},
            {21, 10, 2, 4}, {25, 16, 4, 2}, {25, 21, 4, 2},
        }},
        {"Narrow", {15, 30, kPillar25}, 1, {
            {21, 30, 2, 4},
        }},
    }},
    {116, "N", "Empty",   2, {
        {"Wide",   {10, 10, kPillar25}, 6, {
            {25, 16, 4, 2}, {25, 21, 4, 2}, {21, 25, 2, 4},
            {16, 25, 2, 4}, {10, 21, 4, 2}, {10, 16, 4, 2},
        }},
        {"Narrow", {15, 10, kPillar25}, 2, {
            {21,  5, 2, 4}, {16,  5, 2, 4},
        }},
    }},
    {124, "W", "Caskets",      1, {EMPTY_VARIANT, EMPTY_VARIANT}},
    {125, "E", "Candles",      1, {EMPTY_VARIANT, EMPTY_VARIANT}},
    {127, "S", "SmallCandles", 1, {EMPTY_VARIANT, EMPTY_VARIANT}},
    {131, "N", "Braziers",     1, {EMPTY_VARIANT, EMPTY_VARIANT}},
    {139, "W", "StairsUp",     1, {EMPTY_VARIANT, EMPTY_VARIANT}},
    {140, "E", "StairsUp",     1, {EMPTY_VARIANT, EMPTY_VARIANT}},
    {141, "S", "StairsUp",     1, {EMPTY_VARIANT, EMPTY_VARIANT}},
    {142, "N", "StairsUp",     1, {EMPTY_VARIANT, EMPTY_VARIANT}},
    {143, "W", "StairsDown",   1, {EMPTY_VARIANT, EMPTY_VARIANT}},
    {144, "E", "StairsDown",   1, {EMPTY_VARIANT, EMPTY_VARIANT}},
    {145, "S", "StairsDown",   1, {EMPTY_VARIANT, EMPTY_VARIANT}},
    {146, "N", "StairsDown",   1, {EMPTY_VARIANT, EMPTY_VARIANT}},

    // -------- Corridor pair: 111 (EW), 120 (NS). Both Empty templates share
    //          the same grave row across their two pillar-count variants.
    //          The themed E-W corridor is 126 (Torches); 135 is the N-S
    //          corridor with the shrine (no graves, fixed shape).
    {111, "EW", "Empty",  2, {
        {"Wide",   {15, 10, kPillar25}, 4, {
            { 6, 15, 2, 4}, {16, 15, 2, 4}, {26, 15, 2, 4}, {36, 15, 2, 4},
        }},
        {"Narrow", {10, 15, kPillar25}, 4, {
            { 6, 15, 2, 4}, {16, 15, 2, 4}, {26, 15, 2, 4}, {36, 15, 2, 4},
        }},
    }},
    {120, "NS", "Empty",  2, {
        {"Wide",   {10, 15, kPillar25}, 4, {
            {15, 36, 4, 2}, {15, 26, 4, 2}, {15, 16, 4, 2}, {15,  6, 4, 2},
        }},
        // Narrow only ever has graves at the south and north ends; the middle
        // two row positions of Wide are absent here.
        {"Narrow", {15, 10, kPillar25}, 2, {
            {15, 36, 4, 2}, {15,  6, 4, 2},
        }},
    }},
    {126, "EW", "Torches", 1, {EMPTY_VARIANT, EMPTY_VARIANT}},
    {135, "NS", "Shrine",  1, {EMPTY_VARIANT, EMPTY_VARIANT}},

    // -------- Corner-piece family: 113 (SW), 114 (SE), 117 (NW), 118 (NE).
    //          Each Empty template has two 2x2 pillars and the same four
    //          grave slots across variants. Themed counterparts (128/129/
    //          132/133) take the same shape.
    {113, "SW", "Empty", 2, {
        {"DoorWest",  {15, 30, kPillar25}, 4, {
            {20, 36, 4, 2}, {20, 21, 4, 2}, {16, 15, 2, 4}, { 6, 15, 2, 4},
        }},
        {"DoorSouth", {10, 15, kPillar25}, 4, {
            {20, 36, 4, 2}, {20, 21, 4, 2}, {16, 15, 2, 4}, { 6, 15, 2, 4},
        }},
    }},
    {114, "SE", "Empty", 2, {
        {"DoorEast",  {15, 30, kPillar25}, 4, {
            {15, 36, 4, 2}, {15, 21, 4, 2}, {21, 15, 2, 4}, {36, 15, 2, 4},
        }},
        {"DoorSouth", {30, 15, kPillar25}, 4, {
            {15, 36, 4, 2}, {15, 21, 4, 2}, {21, 15, 2, 4}, {36, 15, 2, 4},
        }},
    }},
    {117, "NW", "Empty", 2, {
        {"DoorNorth", {10, 15, kPillar25}, 4, {
            { 6, 20, 2, 4}, {16, 20, 2, 4}, {20, 16, 4, 2}, {20,  6, 4, 2},
        }},
        {"DoorWest",  {15, 10, kPillar25}, 4, {
            { 6, 20, 2, 4}, {16, 20, 2, 4}, {20, 16, 4, 2}, {20,  6, 4, 2},
        }},
    }},
    {118, "NE", "Empty", 2, {
        {"DoorNorth", {30, 15, kPillar25}, 4, {
            {15,  6, 4, 2}, {15, 16, 4, 2}, {21, 20, 2, 4}, {36, 15, 2, 4},
        }},
        {"DoorEast",  {15, 10, kPillar25}, 4, {
            {15,  6, 4, 2}, {15, 16, 4, 2}, {21, 20, 2, 4}, {36, 15, 2, 4},
        }},
    }},
    {128, "SW", "Holes",            1, {EMPTY_VARIANT, EMPTY_VARIANT}},
    {129, "SE", "Armory",           1, {EMPTY_VARIANT, EMPTY_VARIANT}},
    {132, "NW", "SingleDeadRogue",  1, {EMPTY_VARIANT, EMPTY_VARIANT}},
    {133, "NE", "ArmoryLight",      1, {{"", {0, 0, 0}, 2, {
            {15,  6, 4, 2}, {36, 15, 2, 4},
        }}, EMPTY_VARIANT}},

    // -------- T-intersection family: 115 (ESW), 119 (NEW), 121 (NSW),
    //          122 (NES). Empty templates have variants distinguishing 3- vs
    //          4-pillar layouts; the row picks up an extra grave in the
    //          4-pillar variant.
    {115, "ESW", "Empty", 2, {
        {"doors-west-east", {15, 10, kPillar25}, 4, {
            {15, 36, 4, 2}, { 6, 15, 2, 4}, {15, 16, 4, 2}, {36, 15, 2, 4},
        }},
        {"door-south",      {10, 15, kPillar25}, 5, {
            {15, 36, 4, 2}, { 6, 15, 2, 4}, {16, 15, 2, 4}, {21, 15, 2, 4}, {36, 15, 2, 4},
        }},
    }},
    {119, "NEW", "Empty", 2, {
        // door-north uses 0x0007 pillars rather than 0x0025.
        {"DoorsWestEast", {15, 30, kPillar25}, 3, {
            { 6, 15, 2, 4}, {15,  6, 4, 2}, {36, 15, 2, 4},
        }},
        {"DoorNorth",      {10, 15, kPillar07}, 5, {
            {15,  6, 4, 2}, { 6, 15, 2, 4}, {16, 15, 2, 4}, {21, 15, 2, 4}, {36, 15, 2, 4},
        }},
    }},
    {121, "NSW", "Empty", 2, {
        {"DoorWest",         {15, 10, kPillar25}, 5, {
            { 6, 15, 2, 4}, {15, 36, 4, 2}, {15, 21, 4, 2}, {15, 16, 4, 2}, {15,  6, 4, 2},
        }},
        {"DoorsNorthSouth", {30, 15, kPillar25}, 5, {
            { 6, 15, 2, 4}, {15, 36, 4, 2}, {15, 21, 4, 2}, {15, 16, 4, 2}, {15,  6, 4, 2},
        }},
    }},
    {122, "NES", "Empty", 2, {
        {"DoorEast",         {15, 10, kPillar25}, 5, {
            {36, 15, 2, 4}, {15, 36, 4, 2}, {15, 21, 4, 2}, {15, 16, 4, 2}, {15,  6, 4, 2},
        }},
        {"DoorsNorthSouth", {10, 15, kPillar25}, 5, {
            {36, 15, 2, 4}, {15, 36, 4, 2}, {15, 21, 4, 2}, {15, 16, 4, 2}, {15,  6, 4, 2},
        }},
    }},
    {130, "ESW", "FirePentagram",         1, {EMPTY_VARIANT, EMPTY_VARIANT}},
    {134, "NEW", "DoubleDeadRogueFire",   1, {{"", {0, 0, 0}, 1, {
            {15,  6, 4, 2},
        }}, EMPTY_VARIANT}},
    {136, "NSW", "Candles",               1, {{"", {0, 0, 0}, 2, {
            {15, 36, 4, 2}, {15,  6, 4, 2},
        }}, EMPTY_VARIANT}},
    {137, "NES", "IntersectBraziers",     1, {{"", {0, 0, 0}, 3, {
            {15, 36, 4, 2}, {36, 15, 2, 4}, {15,  6, 4, 2},
        }}, EMPTY_VARIANT}},

    // -------- Crossing room: 123 only.
    {123, "NESW", "Empty", 2, {
        {"DoorsWestEast",   {15, 10, kPillar25}, 4, {
            {15, 36, 4, 2}, { 6, 15, 2, 4}, {15,  6, 4, 2}, {36, 15, 2, 4},
        }},
        {"DoorsNorthSouth", {10, 15, kPillar25}, 4, {
            {15, 36, 4, 2}, { 6, 15, 2, 4}, {15,  6, 4, 2}, {36, 15, 2, 4},
        }},
    }},
};

#undef EMPTY_VARIANT

const RoomDef* FindRoom(uint32_t roomId) {
    for (const auto& r : kRooms) {
        if (r.roomId == roomId) return &r;
    }
    return nullptr;
}

int HitCount(const LevelMap& m, int bx, int by, int bw, int bh, uint16_t match) {
    int hits = 0;
    for (int y = by; y < by + bh; ++y) {
        if (y < 0 || y >= m.sizeY) continue;
        for (int x = bx; x < bx + bw; ++x) {
            if (x < 0 || x >= m.sizeX) continue;
            if (m.coll[y * m.sizeX + x] == match) ++hits;
        }
    }
    return hits;
}

bool GraveAt(const LevelMap& m, int bx, int by, int bw, int bh) {
    return HitCount(m, bx, by, bw, bh, kGraveTile) >= 2;
}

bool PillarAt(const LevelMap& m, const PillarProbe& p, int rx, int ry) {
    return HitCount(m, rx + p.dx, ry + p.dy, 2, 2, p.value) >= 2;
}

// Pick which variant index (0 or 1) this room sits in. Returns -1 when
// neither variant probe matches (or both do, which would be an ambiguous
// layout we don't expect to see).
int DetectVariant(const LevelMap& m, const Room& r, const RoomDef& def) {
    if (def.numVariants == 1) return 0;
    const bool v0 = PillarAt(m, def.variants[0].probe, r.x, r.y);
    const bool v1 = PillarAt(m, def.variants[1].probe, r.x, r.y);
    if (v0 && !v1) return 0;
    if (v1 && !v0) return 1;
    return -1;
}

uint8_t ComputeGraves(const LevelMap& m, const Room& r, const VariantDef& v) {
    uint8_t out = 0;
    for (int i = 0; i < v.slotCount; ++i) {
        const Slot& s = v.slots[i];
        if (GraveAt(m, r.x + s.dx, r.y + s.dy, s.w, s.h)) out |= uint8_t(1u << i);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------

bool TowerRoom::IsKnownRoomId(uint32_t roomId) {
    return FindRoom(roomId) != nullptr;
}

std::vector<TowerRoom::ComboKey> TowerRoom::AllKnownCombos() {
    std::vector<ComboKey> out;
    for (const auto& r : kRooms) {
        for (int v = 0; v < r.numVariants; ++v) {
            out.push_back({r.roomId, static_cast<uint8_t>(v)});
        }
    }
    return out;
}

TowerRoom TowerRoom::FromGame(const LevelMap& m, const Room& r) {
    const RoomDef* def = FindRoom(r.roomNumber);
    if (!def) return TowerRoom{};
    const int variant = DetectVariant(m, r, *def);
    if (variant < 0) return TowerRoom{};
    const uint8_t graves = ComputeGraves(m, r, def->variants[variant]);
    return TowerRoom(r.roomNumber, static_cast<uint8_t>(variant), graves);
}

const char* TowerRoom::shape() const {
    const RoomDef* def = FindRoom(roomId_);
    return def ? def->shape : "";
}

const char* TowerRoom::theme() const {
    const RoomDef* def = FindRoom(roomId_);
    return def ? def->theme : "";
}

const char* TowerRoom::variantName() const {
    const RoomDef* def = FindRoom(roomId_);
    if (!def) return "";
    return def->variants[variant_].name;
}

int TowerRoom::graveSlotCount() const {
    const RoomDef* def = FindRoom(roomId_);
    if (!def) return 0;
    return def->variants[variant_].slotCount;
}

bool TowerRoom::hasGrave(int slot1based) const {
    if (slot1based < 1 || slot1based > kMaxSlots) return false;
    return ((graves_ >> (slot1based - 1)) & 1u) != 0;
}

std::string TowerRoom::gravesMaskString() const {
    const int n = graveSlotCount();
    std::string s(n, '0');
    for (int i = 0; i < n; ++i) {
        if ((graves_ >> i) & 1u) s[i] = '1';
    }
    return s;
}

std::string TowerRoom::asText() const {
    if (!valid()) return "?";
    std::string out = shape();
    out += '/';
    out += theme();
    const char* vname = variantName();
    if (vname && *vname) {
        out += '/';
        out += vname;
    }
    if (graveSlotCount() > 0) {
        out += '/';
        out += gravesMaskString();
    }
    return out;
}

uint16_t TowerRoom::encode() const {
    const uint16_t hi = uint16_t((uint16_t(variant_ & 1u) << 7)
                                 | uint16_t((roomId_ - 100) & 0x7Fu));
    return uint16_t(graves_) | uint16_t(hi << 8);
}

TowerRoom TowerRoom::Decode(uint16_t v) {
    const uint8_t  lo      = uint8_t(v & 0xFFu);
    const uint8_t  hi      = uint8_t((v >> 8) & 0xFFu);
    const uint8_t  variant = uint8_t((hi >> 7) & 1u);
    const uint32_t roomId  = uint32_t(hi & 0x7Fu) + 100u;
    return TowerRoom(roomId, variant, lo);
}

} // namespace MapData
