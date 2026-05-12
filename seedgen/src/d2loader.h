#pragma once
#include <vector>
#include "d2types.h"

// Returns true if all D2 DLLs loaded and initialized successfully.
// gameDir must be the Diablo II installation directory (contains D2Common.dll etc.)
// When verbose=true, prints the DLL/ordinal/init-sequence banner to stdout.
bool D2_Initialize(const char* gameDir, bool verbose = false);

// Wrappers around the resolved D2COMMON functions.
// All return nullptr / no-op if D2_Initialize has not succeeded.
Act*   D2_LoadAct(uint32_t actNo, uint32_t seed, uint32_t difficulty, uint32_t townLevelId);
void   D2_UnloadAct(Act* pAct);
Level* D2_GetLevel(ActMisc* pMisc, uint32_t levelNo);
void   D2_InitLevel(Level* pLevel);
void   D2_AddRoomData(Act* pAct, int levelNo, int x, int y);
void   D2_RemoveRoomData(Act* pAct, int levelNo, int x, int y);

// Reads a file from the currently-open D2 MPQ priority chain via Storm.dll.
// Path uses backslashes, e.g. "data\\global\\excel\\objects.txt".
// Returns false on any error (file not found, read error, etc.).
bool D2_ReadMpqFile(const char* mpqPath, std::vector<uint8_t>& out);
