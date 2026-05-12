#pragma once
#include <cstdint>
#include <string>

// =============================================================================
// MonStats.txt-backed lookup. Loaded once after D2_Initialize succeeds.
// Maps PresetUnit.dwTxtFileNo (== hcIdx column in monstats.txt) → display name
// (NameStr column) and internal token (Id column).
// =============================================================================

namespace MapData {

// When verbose=true, prints a one-line parsed-row count to stdout.
bool   LoadMonStatsDb(bool verbose = false);
size_t MonStatsDbSize();

// Raw NameStr key from monstats.txt (e.g. "Summoner"). Usually already
// readable English, but for some rows it's a string-table key (e.g.
// "andariel" -> resolves to "Andariel"). "" if unknown.
const std::string& MonsterName(uint32_t hcIdx);

// Localized display name. Resolves MonsterName(hcIdx) through D2Lang and
// falls back to "" if the lookup yields nothing. "" if unknown.
const std::string& MonsterDisplayName(uint32_t hcIdx);

// Internal token from the Id column (e.g. "summoner"). "" if unknown.
const std::string& MonsterId(uint32_t hcIdx);

} // namespace MapData
