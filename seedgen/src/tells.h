#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "d2levels.h"
#include "mapdata.h"
#include "d2types.h"

// =============================================================================
// Tell registry. A "tell" is a string-valued observation about a seed (e.g.
// "Act 1 wilderness exit direction = W"). Each tell declares which act it
// needs and a compute function that derives the value from a TellContext.
//
// The runner groups tells by act so each act is loaded at most once per seed.
// TellContext caches level extractions, so two tells sharing a level only pay
// the extraction cost once.
// =============================================================================

namespace MapData {

class TellContext {
public:
    TellContext(Act* pAct, ActId actNo, uint32_t seed)
        : pAct_(pAct), actNo_(actNo), seed_(seed) {}

    Act*     GetAct() const { return pAct_; }
    ActId    ActNo()  const { return actNo_; }
    uint32_t Seed()   const { return seed_; }

    // Lazily extracts and caches LevelMap for the given level id.
    const LevelMap& GetLevel(LevelId levelNo);

private:
    Act*     pAct_;
    ActId    actNo_;
    uint32_t seed_;
    std::unordered_map<LevelId, LevelMap> cache_;
};

// A single precondition: prerequisite tell must report one of `allowedValues`.
// Multiple Requirements on the same Tell are ANDed together.
struct Requirement {
    const char*              tellName;
    std::vector<std::string> allowedValues;
};

// Where on the map this tell wants to be drawn. Coordinates are level-local
// tiles. A point has w==0 && h==0; otherwise the location is an axis-aligned
// rectangle [x, x+w) × [y, y+h). A single tell may attach multiple locations
// (e.g. one rect per detected island, or a search box + a found-object dot).
struct TellLocation {
    LevelId levelNo = LevelId(0);
    int     x = 0, y = 0;
    int     w = 0, h = 0;
};

// Tell compute output. Implicitly constructible from a string so existing
// compute functions can `return "yes";` unchanged — they just won't carry
// position info. Tells that have a sensible coordinate attach one or more
// TellLocations for the renderer.
struct TellResult {
    std::string               value;
    std::vector<TellLocation> locations;

    TellResult() = default;
    TellResult(const char* v) : value(v) {}
    TellResult(std::string v) : value(std::move(v)) {}
    TellResult(std::string v, TellLocation loc)
        : value(std::move(v)), locations{loc} {}
    TellResult(std::string v, std::vector<TellLocation> locs)
        : value(std::move(v)), locations(std::move(locs)) {}
};

struct Tell {
    const char* name;          // PascalCase identifier (e.g. "A1Town")
    ActId       actNo;         // which act to load
    std::vector<LevelId> levels;   // level IDs this tell reads (informational —
                                   // GetLevel is lazy regardless — but allows
                                   // pre-validation, debug printing, and
                                   // future scheduling decisions).
    std::vector<Requirement> prereqs;  // empty = unconditional. When any prereq
                                       // fails, the runner emits "?" and skips
                                       // compute. Resolved within the tell's
                                       // own act; cross-act prereqs are not
                                       // supported (would defeat the act-load
                                       // grouping in EvalTells).
    TellResult (*compute)(TellContext& ctx);
};

// Sentinel values emitted by the runner / compute fns:
//   "ERROR" — compute failed (level didn't load, expected preset missing, ...)
//   "?"     — prereq not met (this tell is invalid for this seed)

const Tell*                     FindTell(const std::string& name);
const std::vector<const Tell*>& AllTells();              // declaration order
const std::vector<const Tell*>& AllTellsTopoSorted();    // prereqs before dependents

// Returns the resolved prereq target (or nullptr if the name doesn't match
// any registered tell). Cached after first build.
const Tell* ResolvePrereq(const Requirement& r);

} // namespace MapData
