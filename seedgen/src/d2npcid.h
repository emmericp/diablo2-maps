#pragma once
#include <cstdint>

// Subset of monstats.txt hcIdx values used by our tells.
enum class NpcId : uint32_t {
    Navi            = 266,  // rogue scout guarding Blood Moor → Cold Plains exit
    Summoner        = 250,  // Arcane Sanctuary super-unique; always alone on one arm
};

constexpr uint32_t NpcIdx(NpcId n) { return static_cast<uint32_t>(n); }
