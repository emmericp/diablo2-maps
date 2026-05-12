#pragma once
#include <cstdint>
#include <string>

// =============================================================================
// Levels.txt-backed lookup. Loaded once after D2_Initialize succeeds.
// Maps level id (Levels.txt's "Id" column) to its in-game display name
// (the "LevelName" column, e.g. "Rogue Encampment" / "Blood Moor").
//
// Levels.txt's LevelName column is the English in-game string verbatim, so no
// D2Lang lookup is needed here — see objects_db / monstats_db for the
// key-based pattern that does need the string table.
// =============================================================================

namespace MapData {

bool   LoadLevelsDb(bool verbose = false);
size_t LevelsDbSize();

// Display name for a level id, e.g. id=1 -> "Rogue Encampment".
// Returns "" if id is unknown or DB not loaded.
const std::string& LevelDisplayName(uint32_t id);

} // namespace MapData
