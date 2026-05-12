#pragma once
#include "d2types.h"
#include "mapdata.h"

namespace MapData {

// Walks Room2/Room1/CollMap/PresetUnit structures for one level and returns a
// self-contained LevelMap. Returns an empty LevelMap (sizeX == 0) when the
// level has no rooms after InitLevel.
//
// Side effects: AddRoomData/RemoveRoomData are paired correctly — the D2
// state is left as it was found, modulo cached tables in D2Common.
//
LevelMap ExtractLevel(Act* pAct, ActId actNo, uint32_t levelNo);

} // namespace MapData
