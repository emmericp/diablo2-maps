#pragma once

// Wire format for the per-seed levelgraph .bin files emitted by
// `mapdump.exe levelgraph` and consumed by `trielookup.exe buildindex`.
//
// One fixed-size record per seed, packed contiguously. No header. No
// embedded level id (the file's name carries that).
//
// 8 grave-room slots × 3 bytes each = 24 bytes:
//   byte 0:      position = (cell_y << 4) | cell_x. Unused slots: 0xFF.
//   bytes 1..2:  TowerRoom::encode() little-endian. Unused slots: 0xFFFF.
//
// Records hold rooms in (cell_y, cell_x) order so the layout is
// deterministic across producers.

constexpr int kLevelGraphMaxRooms   = 8;
constexpr int kLevelGraphRecordSize = kLevelGraphMaxRooms * 3;  // 24
