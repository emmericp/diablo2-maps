#pragma once

// X-macro listing every Act1WildRoom template with a per-level room-count
// tell. Each entry expands to one tell per Act 1 wilderness level
// (BloodMoor / ColdPlains / StonyField), named `<Level><Template>` —
// e.g. `BloodMoorShrine2`, `ColdPlainsCaveLevel1StairsSouth`.
//
// Single source of truth: tells.cpp uses it for forward declarations and
// registry entries; tells_rooms.cpp uses it to generate the actual
// compute-fn definitions. Add a row here once and the tell shows up on
// all three levels.
//
// "Other" and the *Other / *Unknown fallbacks are deliberately omitted —
// they're background filler or unrecognised sub-variants a player can't
// identify in-game. Some (template, level) combinations always return 0
// (Lake only spawns in BloodMoor, House48 only in ColdPlains, etc.); we
// keep them for symmetry — the per-seed cost is one extra loop iteration.
#define ACT1_WILD_ROOMS \
    X(Shrine2) \
    X(Shrine81) \
    X(Shrine83) \
    X(Shrine84) \
    X(Well) \
    X(HouseBedSouth) \
    X(HouseBedEastNoPorch) \
    X(HouseBedEastPorch) \
    X(HouseSmall) \
    X(HouseCow) \
    X(HouseFallens) \
    X(HouseBurning) \
    X(House48BedSouth) \
    X(House48BedEast) \
    X(House48Rogue) \
    X(House48Stable) \
    X(House48Empty) \
    X(StonesCircle) \
    X(StonesCross) \
    X(StonesKidney) \
    X(Lake) \
    X(LargeFallenCamp) \
    X(CaveLevel1StairsInWall) \
    X(CaveLevel1StairsEast) \
    X(CaveLevel1StairsSouth)
