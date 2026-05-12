#pragma once
#include <cstdint>
#include <string>
#include "mapdata.h"

// =============================================================================
// objects.txt-backed lookup. Loaded once after D2_Initialize succeeds.
// Maps PresetUnit.dwTxtFileNo (row index in objects.txt) → name + ObjectKind.
//
// Names are matched case-insensitively against the Name column to derive
// category (waypoint / shrine / well / chest / door / stairs / quest).
// =============================================================================

namespace MapData {

// Parses objects.txt out of the MPQ via Storm. Returns false if extraction
// fails — categorization will then fall back to ObjectKind::Generic for all.
// When verbose=true, prints a one-line summary of the parsed table to stdout.
bool LoadObjectsDb(bool verbose = false);

// Returns row count (0 if not loaded).
size_t ObjectsDbSize();

// Look up the Name column for an object txtFileNo. Returns "" if out of range.
// This is the raw key from objects.txt (e.g. "magic shrine") — tells.cpp
// matches on the raw value, so this stays as-is.
const std::string& ObjectName(uint32_t txtFileNo);

// Localized display name, resolved by feeding ObjectName(txtFileNo) into
// D2Lang's string table (e.g. "magic shrine" -> "Magic Shrine"). Returns ""
// if the row has no name, the lookup failed, or D2_LookupString isn't ready.
const std::string& ObjectDisplayName(uint32_t txtFileNo);

// Look up the Description column (column 1) for an object txtFileNo. The header
// labels it "description - not loaded", meaning the game runtime ignores it,
// but it's still authored data and useful for identifying objects (e.g.
// "Door Gate Left" vs "Andariel's Door"). Returns "" if out of range or blank.
const std::string& ObjectDescription(uint32_t txtFileNo);

// Category lookup. Returns ObjectKind::Generic for unknown / out-of-range.
ObjectKind ObjectKindFor(uint32_t txtFileNo);

// Human-readable kind name.
const char* KindName(ObjectKind k);

} // namespace MapData
