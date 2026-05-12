#pragma once
#include "mapdump_common.h"

struct SummaryArgs : CommonArgs {
    std::vector<const MapData::Tell*> tells;
    std::unordered_set<uint32_t>      tellLevels;  // empty = no level filter
};

bool ParseSummaryArgs(int argc, char** argv, SummaryArgs& out);
void PrintSummaryUsage();
int  RunSummary(const SummaryArgs& args);
