#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "mapdata.h"

// =============================================================================
// TowerRoom — a compact classification of a single tower-cellar room.
//
// Every tower-cellar (Act 1, LevelId 21..24) room template carries three
// pieces of metadata we care about:
//
//   - shape: the interconnect signature — which sides have exits.
//     Values: "W","E","S","N" (dead-end), "EW","NS","NE","NW","SE","SW"
//     (2-way corridor / corner), "ESW","NEW","NSW","NES" (T), "NESW" (cross).
//     Listed in the user's room table.
//
//   - theme: visual / functional theme baked into the template:
//     "Empty" for the plain (grave-bearing) templates, or a name like
//     "Caskets", "FirePentagram", "Shrine", "StairsUp", "StairsDown", ...
//
//   - variant: a secondary classification within a single (shape, theme)
//     pair. Used by the "Empty" templates to distinguish door / pillar
//     layouts (e.g. "wide" vs "narrow", "South" vs "East"). At most two
//     variants per room, fitting in a single bit.
//
//   - graves: 0..8 fixed grave slots, each "present" iff the canonical
//     bounding box holds at least two tiles of collision value 0x0011
//     (BlockWalk | AlternateTile — the open-grave signature).
//
// All metadata lives in a single table inside tower_rooms.cpp; this header
// just exposes the lookup + the 2-byte packed encoding used for bulk export.
//
// Binary encoding (16 bits, byte order = LE-friendly):
//   byte 0 (low):  graves bitmask. bit i (LSB-first) set iff slot i+1 is
//                  present. Slot count is variant-specific.
//   byte 1 (high): bit 7   = variant flag (0 = primary, 1 = alternate)
//                  bits 0..6 = roomId - 100 (room IDs we use sit in [109,146])
// Total: 2 bytes per classified room.
// =============================================================================

namespace MapData {

class TowerRoom {
public:
    static constexpr int kMaxSlots = 8;

    // True for room IDs known to the metadata table.
    static bool IsKnownRoomId(uint32_t roomId);

    // One (roomId, variant) combo from the metadata table.
    struct ComboKey { uint32_t roomId; uint8_t variant; };

    // Every (roomId, variant) the table knows about, in declaration order
    // (StairsDown-N is the last one). Used by the atlas extractor to know
    // when it has collected a canonical example of every combination.
    static std::vector<ComboKey> AllKnownCombos();

    // Build a TowerRoom by reading the level's collision grid at the room's
    // position. Returns an invalid TowerRoom (valid()==false) if the room id
    // isn't recognised or the variant can't be determined.
    static TowerRoom FromGame(const LevelMap& m, const Room& r);

    // Decode the 2-byte packed form. Decoded value's validity follows
    // IsKnownRoomId — decoding arbitrary bytes does NOT silently produce a
    // valid TowerRoom.
    static TowerRoom Decode(uint16_t v);

    TowerRoom() = default;

    bool        valid() const { return IsKnownRoomId(roomId_); }
    uint32_t    roomId() const { return roomId_; }
    uint8_t     variant() const { return variant_; }      // 0 or 1

    // Room-level metadata. Empty string for invalid rooms.
    const char* shape() const;        // "W", "ESW", "NESW", ...
    const char* theme() const;        // "Empty", "Caskets", "StairsUp", ...

    // Variant name (empty for single-variant rooms).
    const char* variantName() const;

    // Grave info (slot-bit ordering matches the table's slots[] order).
    int         graveSlotCount() const;
    uint8_t     graves() const { return graves_; }
    bool        hasGrave(int slot1based) const;
    std::string gravesMaskString() const;   // length == graveSlotCount()

    // Compact human-readable form:
    //   "<shape>/<theme>[/<variantName>][/<gravesMask>]"
    // The variant component is omitted for single-variant rooms and the
    // graves component is omitted when the room has no grave slots.
    // Invalid TowerRooms render as "?".
    std::string asText() const;

    uint16_t encode() const;

private:
    TowerRoom(uint32_t roomId, uint8_t variant, uint8_t graves)
        : roomId_(roomId), variant_(variant), graves_(graves) {}

    uint32_t roomId_  = 0;
    uint8_t  variant_ = 0;
    uint8_t  graves_  = 0;
};

} // namespace MapData
