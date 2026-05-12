#pragma once
#include <cstdint>
#include <string>

// =============================================================================
// In-process loader for D2's localized string tables. Bypasses D2Lang.dll —
// we read string.tbl / expansionstring.tbl / patchstring.tbl out of the MPQ
// chain via Storm and parse the format ourselves (struct layout from plugy's
// d2StringTblStruct.h). One combined key->value map covers all three.
// All values are UTF-8.
//
// Lookups are case-insensitive on the key, matching D2's behavior — the
// objects.txt Name column is lowercase ("magic shrine") but string.tbl keys
// can be either case.
// =============================================================================

namespace MapData {

// Load all three .tbl files from the MPQ. Safe to call multiple times.
// Returns true if at least one .tbl loaded successfully.
bool LoadStringTbl(bool verbose = false);

// Resolve `key` to its localized display string (UTF-8). Returns "" if the
// key isn't in any loaded table, or if LoadStringTbl hasn't been called.
const std::string& LookupString(const char* key);

size_t StringTblSize();

} // namespace MapData
