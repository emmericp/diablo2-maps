package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"time"
)

// /api/filter — multi-stage seed identification.
//
// Combines the trielookup pass, the seed-parity halving (level 5 north/east
// vs west/south stairs-up is encoded by seed&1: north == even, west == odd),
// the L1/L2/L3 stairs-up cardinal-direction filter (Tower1/2/3StairsUp
// tells), and the town exit filter (BloodMoorCampExit tell). All of it
// happens inside this handler — the client supplies player answers as
// directions, the server maps to tell values and string-compares against
// what mapdump emits.
//
// Why this lives server-side: the trielookup result set after the L4 lookup
// can be in the thousands. Sending that to a browser and looping per-seed
// /api/render calls is exactly the round-trip storm we want to avoid, plus
// it would force the client to know the names of every tell. Centralizing
// here keeps the only client contract "direction × direction × direction",
// and lets us cap the result list to a small N (default 64) regardless of
// how many seeds the lookup produced.
//
// Filter ordering matches the user's stated efficiency preference:
//   1. trielookup with parity baked in (cheap, O(log N) probe + O(matches)
//      walk only when --parity is supplied). Parity is pushed all the way
//      down to the trielookup binary so that the filterMaxLookup cap is
//      compared against the parity-filtered candidate count and so the
//      seed buffer never wastes a slot on a wrong-parity candidate.
//   2. stairs L1/L2/L3 (one render per candidate; usually narrows by ~4×)
//   3. town exit (one render per remaining candidate; tiebreaker)
//
// Request — GET with these query params (all but `prefix` optional):
//   prefix=<hex,hex,...>   same syntax as /api/lookup (4-char hex, ?
//                          wildcards, /UMASK bit-level masks)
//   parity=even|odd        keep seeds whose &1 matches; "" = no parity filter
//   stairs1=N|E|S|W        keep seeds whose Tower1StairsUp tell is StairsUp<X>
//   stairs2, stairs3       same idea for level 22 / 23
//   townexit=N|E|S|W       keep seeds whose BloodMoorCampExit tell == <X>
//   limit=N                cap on returned `seeds[]` (default 64, ceiling 256)
//
// Response (JSON, one line):
//   {
//     "lookup": <int>,                  raw count from trielookup
//     "after_parity": <int>,            only present when parity= was sent
//     "after_filters": <int>,           after stairs + townexit (if any)
//     "final_count": <int>,             == after_filters when filters fired
//     "seeds": [<uint32>, ...],         up to `limit` survivors
//     "truncated": <bool>,              true iff final_count > len(seeds)
//     "warning": "<msg>"                set when we refused to filter the
//                                       full set (matchCount too large)
//   }

const (
	// Buffer cap on the seed array we read back from trielookup. Big enough
	// that we always see the *full* parity-filtered candidate set whenever
	// the render gate (below) would let us proceed.
	filterMaxLookup = 4096
	// Per-seed render budget. These mirror FILTER_RENDER_CAP_NO_PARITY /
	// FILTER_RENDER_CAP_WITH_PARITY in renderer/src/lookup/lookup-mock.ts
	// — bump both together so the UI gate and the server gate stay in
	// sync. Without parity, /api/filter refuses past 100 candidates; with
	// parity (Countess direction → seed&1), up to 200, because the trie
	// halves them before the renderer pool sees them.
	filterRenderCapNoParity   = 100
	filterRenderCapWithParity = 200
	filterDefaultLim          = 64
	filterMaxLim              = 256
)

// Cardinal directions encode the tell suffix. "" means "don't apply".
var dirToStairsUpValue = map[string]string{
	"N": "StairsUpN", "E": "StairsUpE", "S": "StairsUpS", "W": "StairsUpW",
}

type filterRequest struct {
	prefix   string
	parity   string   // "", "even", "odd"
	stairs   [3]string
	townExit string
	limit    int
}

type filterResponse struct {
	Lookup       int      `json:"lookup"`
	AfterParity  *int     `json:"after_parity,omitempty"`
	AfterFilters *int     `json:"after_filters,omitempty"`
	FinalCount   int      `json:"final_count"`
	Seeds        []uint32 `json:"seeds"`
	Truncated    bool     `json:"truncated,omitempty"`
	Warning      string   `json:"warning,omitempty"`
}

type renderJSON struct {
	Levels []struct {
		LevelNo int `json:"levelNo"`
		Tells   []struct {
			Name  string `json:"name"`
			Value string `json:"value"`
		} `json:"tells"`
	} `json:"levels"`
}

func filterHandler(binPath, indexPath string, pool *Pool, perRequestSec int) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		req, err := parseFilterRequest(r)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}

		ctx, cancel := context.WithTimeout(r.Context(), filterTimeout(perRequestSec))
		defer cancel()

		// Stage 1: trielookup, with parity pushed down so the seed buffer
		// (capped at filterMaxLookup) only ever holds parity-matching
		// candidates. `effectiveCount` is what later cap-checks use — when
		// the user picked a Countess direction it's already-halved.
		seeds, totalLookup, afterParity, err := runTrielookup(
			ctx, binPath, indexPath, req.prefix, req.parity, filterMaxLookup)
		if err != nil {
			if errors.Is(ctx.Err(), context.DeadlineExceeded) {
				http.Error(w, "lookup timed out", http.StatusGatewayTimeout)
				return
			}
			http.Error(w, "lookup failed: "+err.Error(), http.StatusBadGateway)
			return
		}
		resp := filterResponse{Lookup: totalLookup}
		effectiveCount := totalLookup
		if req.parity != "" {
			n := afterParity
			resp.AfterParity = &n
			effectiveCount = afterParity
		}

		// Per-seed render budget. The cap depends on whether parity was
		// supplied: with parity the trie already halved the candidate set,
		// so we can let twice as many through to the renderer pool. The
		// gate is enforced server-side (here) AND mirrored in the UI so
		// the user doesn't waste a click — but the server is the source
		// of truth: it returns 400 on overflow no matter what the client
		// says, which is the load limit the box actually needs.
		renderCap := filterRenderCapNoParity
		if req.parity != "" {
			renderCap = filterRenderCapWithParity
		}
		if effectiveCount > renderCap {
			hint := "narrow the L4 layout further"
			if req.parity == "" {
				hint = "answer the Countess (L5) direction or narrow the L4 layout further"
			}
			http.Error(w, fmt.Sprintf(
				"%d candidates exceeds the %d-seed cap; %s",
				effectiveCount, renderCap, hint),
				http.StatusBadRequest)
			return
		}

		// Stage 3+4: per-seed renders, applying stairs + townexit filters.
		// Skipped when the caller didn't supply any tell answers — the
		// pipeline still progresses on parity alone.
		if req.stairs[0] != "" || req.stairs[1] != "" || req.stairs[2] != "" || req.townExit != "" {
			seeds, err = applyTellFilters(ctx, pool, seeds, req)
			if err != nil {
				if errors.Is(ctx.Err(), context.DeadlineExceeded) {
					http.Error(w, "filter timed out", http.StatusGatewayTimeout)
					return
				}
				http.Error(w, "filter failed: "+err.Error(), http.StatusBadGateway)
				return
			}
			n := len(seeds)
			resp.AfterFilters = &n
		}

		resp.FinalCount = len(seeds)
		if req.limit > 0 && len(seeds) > req.limit {
			resp.Seeds = seeds[:req.limit]
			resp.Truncated = true
		} else {
			resp.Seeds = seeds
		}

		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		_ = json.NewEncoder(w).Encode(&resp)
	}
}

func parseFilterRequest(r *http.Request) (filterRequest, error) {
	q := r.URL.Query()
	out := filterRequest{
		prefix:   strings.TrimSpace(q.Get("prefix")),
		parity:   strings.ToLower(strings.TrimSpace(q.Get("parity"))),
		townExit: strings.ToUpper(strings.TrimSpace(q.Get("townexit"))),
		limit:    filterDefaultLim,
	}
	out.stairs[0] = strings.ToUpper(strings.TrimSpace(q.Get("stairs1")))
	out.stairs[1] = strings.ToUpper(strings.TrimSpace(q.Get("stairs2")))
	out.stairs[2] = strings.ToUpper(strings.TrimSpace(q.Get("stairs3")))

	if out.prefix == "" {
		return out, errors.New("missing prefix")
	}
	if len(out.prefix) > maxLookupPrefixLen {
		return out, errors.New("prefix too long")
	}
	if err := validateLookupPrefix(out.prefix); err != nil {
		return out, fmt.Errorf("bad prefix: %w", err)
	}
	if out.parity != "" && out.parity != "even" && out.parity != "odd" {
		return out, errors.New("bad parity (even|odd)")
	}
	for i, s := range out.stairs {
		if s != "" && !validDirection(s) {
			return out, fmt.Errorf("bad stairs%d (N|E|S|W)", i+1)
		}
	}
	if out.townExit != "" && !validDirection(out.townExit) {
		return out, errors.New("bad townexit (N|E|S|W)")
	}
	if v := strings.TrimSpace(q.Get("limit")); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 1 || n > filterMaxLim {
			return out, fmt.Errorf("bad limit (1..%d)", filterMaxLim)
		}
		out.limit = n
	}
	return out, nil
}

func validDirection(s string) bool {
	return s == "N" || s == "E" || s == "S" || s == "W"
}

// Shell out to trielookup --json and parse the result. `--list=cap` bounds
// the seeds[] array. `matchCount` is the raw trie-hit count; when a parity
// is supplied trielookup also emits `matchCountFiltered` (post-parity
// candidates) and seeds[] holds only parity-matching survivors — no Go-side
// parity pass needed. afterParity == totalCount when parity == "".
func runTrielookup(
	ctx context.Context, binPath, indexPath, prefix, parity string, cap int,
) (seeds []uint32, totalCount int, afterParity int, err error) {
	args := []string{
		"lookup",
		"--index", indexPath,
		"--prefix", prefix,
		"--json",
		"--list", strconv.Itoa(cap),
	}
	if parity != "" {
		args = append(args, "--parity", parity)
	}
	cmd := exec.CommandContext(ctx, binPath, args...)
	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		return nil, 0, 0, fmt.Errorf("%w: %s", err, strings.TrimSpace(stderr.String()))
	}
	var j struct {
		MatchCount         int      `json:"matchCount"`
		MatchCountFiltered *int     `json:"matchCountFiltered,omitempty"`
		Seeds              []uint32 `json:"seeds"`
	}
	if err := json.Unmarshal(stdout.Bytes(), &j); err != nil {
		return nil, 0, 0, fmt.Errorf("decoding trielookup output: %w", err)
	}
	filtered := j.MatchCount
	if j.MatchCountFiltered != nil {
		filtered = *j.MatchCountFiltered
	}
	return j.Seeds, j.MatchCount, filtered, nil
}

// Per-seed render-based filter. Spawns at most pool.Size() concurrent
// workers; each one pulls the next candidate off the work channel, renders
// the levels we need, and votes the seed in or out. Order in the output is
// ascending so the UI gets stable rendering.
func applyTellFilters(
	ctx context.Context, pool *Pool, candidates []uint32, req filterRequest,
) ([]uint32, error) {
	// Build the levels token (`L:21,22,23,2` or subset).
	levels := []int{}
	if req.stairs[0] != "" {
		levels = append(levels, 21)
	}
	if req.stairs[1] != "" {
		levels = append(levels, 22)
	}
	if req.stairs[2] != "" {
		levels = append(levels, 23)
	}
	if req.townExit != "" {
		levels = append(levels, 2)
	}
	levelTok := "L:" + joinInts(levels)

	// Worker pool sized to mapserver's worker pool — going wider just
	// queues on pool.idle.
	conc := pool.Size()
	if conc < 1 {
		conc = 1
	}
	type passed struct {
		idx  int
		ok   bool
		seed uint32
	}
	work := make(chan int)
	resCh := make(chan passed, len(candidates))
	var wg sync.WaitGroup

	for i := 0; i < conc; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for idx := range work {
				if ctx.Err() != nil {
					return
				}
				seed := candidates[idx]
				ok, _ := evalSeedTells(ctx, pool, seed, levelTok, req)
				resCh <- passed{idx: idx, ok: ok, seed: seed}
			}
		}()
	}
	for i := range candidates {
		select {
		case work <- i:
		case <-ctx.Done():
			close(work)
			wg.Wait()
			return nil, ctx.Err()
		}
	}
	close(work)
	wg.Wait()
	close(resCh)

	keep := make([]bool, len(candidates))
	for r := range resCh {
		keep[r.idx] = r.ok
	}
	out := make([]uint32, 0, len(candidates))
	for i, s := range candidates {
		if keep[i] {
			out = append(out, s)
		}
	}
	return out, nil
}

func joinInts(xs []int) string {
	parts := make([]string, len(xs))
	for i, v := range xs {
		parts[i] = strconv.Itoa(v)
	}
	return strings.Join(parts, ",")
}

// Render one seed at the requested levels and run the tell predicates. We
// never short-circuit between filters — the cost is the network/render
// round-trip and that's fixed regardless of how many checks we do.
func evalSeedTells(
	ctx context.Context, pool *Pool, seed uint32, levelTok string, req filterRequest,
) (bool, error) {
	body, err := pool.Render(ctx, fmt.Sprintf("%d %s 0", seed, levelTok))
	if err != nil {
		return false, err
	}
	var rj renderJSON
	if err := json.Unmarshal(body, &rj); err != nil {
		return false, err
	}
	check := func(levelNo int, name, want string) bool {
		for _, l := range rj.Levels {
			if l.LevelNo != levelNo {
				continue
			}
			for _, t := range l.Tells {
				if t.Name == name {
					return t.Value == want
				}
			}
			return false
		}
		return false
	}
	if req.stairs[0] != "" && !check(21, "Tower1StairsUp", dirToStairsUpValue[req.stairs[0]]) {
		return false, nil
	}
	if req.stairs[1] != "" && !check(22, "Tower2StairsUp", dirToStairsUpValue[req.stairs[1]]) {
		return false, nil
	}
	if req.stairs[2] != "" && !check(23, "Tower3StairsUp", dirToStairsUpValue[req.stairs[2]]) {
		return false, nil
	}
	if req.townExit != "" && !check(2, "BloodMoorCampExit", req.townExit) {
		return false, nil
	}
	return true, nil
}

// Allow a longer per-request deadline for /api/filter than /api/lookup: we
// may be running thousands of renders against the worker pool.
func filterTimeout(perRequestSec int) time.Duration {
	if perRequestSec > 0 {
		return time.Duration(perRequestSec) * time.Second
	}
	return 5 * time.Minute
}
