#pragma once
#include <cstdint>

// Struct layouts verified against d2mapapi D2Structs.h and C# Structs.cs
// (StructLayout.Explicit field offsets).

struct CollMap {
    uint32_t  dwPosGameX;   // 0x00  tile coords (already multiplied by 5)
    uint32_t  dwPosGameY;   // 0x04
    uint32_t  dwSizeGameX;  // 0x08
    uint32_t  dwSizeGameY;  // 0x0C
    uint32_t  dwPosRoomX;   // 0x10
    uint32_t  dwPosRoomY;   // 0x14
    uint32_t  dwSizeRoomX;  // 0x18
    uint32_t  dwSizeRoomY;  // 0x1C
    uint16_t* pMapStart;    // 0x20  row-major WORD array, each encodes walkability flags
    uint16_t* pMapEnd;      // 0x24
};

// Forward declarations
struct Room2;
struct Room1;
struct Level;
struct ActMisc;
struct Act;

struct RoomTile {
    Room2*    pRoom2;  // 0x00  destination room/level
    RoomTile* pNext;   // 0x04
    uint32_t  _1[2];   // 0x08
    uint32_t* nNum;    // 0x10  tile number; matched against PresetUnit.dwTxtFileNo for exits
};

struct PresetUnit {
    uint32_t    _1;           // 0x00
    uint32_t    dwTxtFileNo;  // 0x04  row in npcs.txt or objects.txt
    uint32_t    dwPosX;       // 0x08  position relative to Room2 origin (tiles)
    PresetUnit* pPresetNext;  // 0x0C
    uint32_t    _2;           // 0x10
    uint32_t    dwType;       // 0x14  1=NPC  2=Object  5=Tile/Exit
    uint32_t    dwPosY;       // 0x18
};

struct Room1 {
    Room1**  pRoomsNear;   // 0x00
    uint32_t _1[3];        // 0x04
    Room2*   pRoom2;       // 0x10
    uint32_t _2[3];        // 0x14
    CollMap* Coll;         // 0x20
    uint32_t numRoomsNear; // 0x24
    uint32_t _3[9];        // 0x28
    uint32_t dwPosX;       // 0x4C
    uint32_t dwPosY;       // 0x50
    uint32_t dwSizeX;      // 0x54
    uint32_t dwSizeY;      // 0x58
    uint32_t _4[6];        // 0x5C
    void*    pUnitFirst;   // 0x74
    uint32_t _5;           // 0x78
    Room1*   pRoomNext;    // 0x7C
};

struct RoomType2Info {
    uint32_t  dwRoomNumber;  // 0x00  row in rooms.txt
    uint32_t  _1;            // 0x04
    uint32_t* pdwSubNumber;  // 0x08  → variant index (selects File1/File2/… in rooms.txt)
};

struct Room2 {
    uint32_t       _1[2];        // 0x00
    Room2**        pRoom2Near;   // 0x08
    uint32_t       _2[5];        // 0x0C
    RoomType2Info* pType2Info;   // 0x20  room type / DS1 variant info
    Room2*         pRoom2Next;   // 0x24
    uint32_t       dwRoomFlags;  // 0x28
    uint32_t       dwRoomsNear;  // 0x2C
    Room1*         pRoom1;       // 0x30
    uint32_t       dwPosX;       // 0x34  game units (×5 for tile coords)
    uint32_t       dwPosY;       // 0x38
    uint32_t       dwSizeX;      // 0x3C
    uint32_t       dwSizeY;      // 0x40
    uint32_t       _3;           // 0x44
    uint32_t       dwPresetType; // 0x48
    RoomTile*      pRoomTiles;   // 0x4C
    uint32_t       _4[2];        // 0x50
    Level*         pLevel;       // 0x58
    PresetUnit*    pPreset;      // 0x5C
};

struct Level {
    uint32_t  _1[4];        // 0x00
    Room2*    pRoom2First;  // 0x10
    uint32_t  _2[2];        // 0x14
    uint32_t  dwPosX;       // 0x1C  game units (×5 for tile coords)
    uint32_t  dwPosY;       // 0x20
    uint32_t  dwSizeX;      // 0x24
    uint32_t  dwSizeY;      // 0x28
    uint32_t  _3[96];       // 0x2C
    Level*    pNextLevel;   // 0x1AC
    uint32_t  _4;           // 0x1B0
    ActMisc*  pMisc;        // 0x1B4
    uint32_t  _5[6];        // 0x1B8  seed1 at 0x1C4, seed2 at 0x1C8 (plugy D2UnitStruct.h)
    uint32_t  dwLevelNo;    // 0x1D0
};

struct ActMisc {
    uint32_t  _1[37];           // 0x00
    uint32_t  dwStaffTombLevel; // 0x94  Act 2: index of the real staff tomb (0–6)
    uint32_t  _2[245];          // 0x98
    Act*      pAct;             // 0x46C
    uint32_t  _3[3];            // 0x470
    Level*    pLevelFirst;      // 0x47C
};

struct Act {
    uint32_t  _1[3];      // 0x00
    uint32_t  dwMapSeed;  // 0x0C
    Room1*    pRoom1;     // 0x10
    uint32_t  dwAct;      // 0x14
    uint32_t  _2[12];     // 0x18
    ActMisc*  pMisc;      // 0x48
};

// Callback struct passed to D2WIN_10005 and referenced by D2CLIENT_InitGameMisc.
// Layout from d2mapapi D2Structs.h — note the explicit pack(1): without it the
// compiler pads to align fpInit at 0x210 instead of the packed 0x20D, and D2
// code reads/writes the wrong offset.
#pragma pack(push, 1)
struct D2ClientStruct {
    uint32_t  dwInit;         // 0x000  set to 1
    uint8_t   _1[0x209];      // 0x004
    uint32_t (*fpInit)();     // 0x20D  D2 engine calls this to retrieve dwInit
};
#pragma pack(pop)
