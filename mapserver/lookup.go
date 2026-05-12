package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"net/http"
	"os/exec"
	"strconv"
	"strings"
	"time"
)

// /api/lookup?prefix=<hex,...>&list=N&max-unknown-bits=K
//
// Shells out to `trielookup lookup --json` against the configured index file
// and forwards the resulting JSON object to the caller. The prefix syntax is
// the same as the CLI: 4-char hex tokens, '?' for 4 unknown bits per nibble,
// optional `/UMASK` suffix for bit-level wildcards.
//
// Validated server-side to keep the user input out of arbitrary argv slots:
// prefix tokens must each match [0-9A-Fa-f?]{4}(/[0-9A-Fa-f]{4})? and the
// overall length is bounded.
func lookupHandler(binPath, indexPath string, timeout int) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		q := r.URL.Query()
		prefix := strings.TrimSpace(q.Get("prefix"))
		listStr := strings.TrimSpace(q.Get("list"))
		maxStr := strings.TrimSpace(q.Get("max-unknown-bits"))

		if prefix == "" {
			http.Error(w, "missing prefix", http.StatusBadRequest)
			return
		}
		if len(prefix) > maxLookupPrefixLen {
			http.Error(w, "prefix too long", http.StatusBadRequest)
			return
		}
		if err := validateLookupPrefix(prefix); err != nil {
			http.Error(w, "bad prefix: "+err.Error(), http.StatusBadRequest)
			return
		}

		listN := 0
		if listStr != "" {
			n, err := strconv.Atoi(listStr)
			if err != nil || n < 0 || n > maxLookupListN {
				http.Error(w, "bad list", http.StatusBadRequest)
				return
			}
			listN = n
		}
		maxBits := -1 // -1 = let trielookup use its default (4)
		if maxStr != "" {
			n, err := strconv.Atoi(maxStr)
			if err != nil || n < 0 || n > 12 {
				http.Error(w, "bad max-unknown-bits", http.StatusBadRequest)
				return
			}
			maxBits = n
		}

		args := []string{
			"lookup",
			"--index", indexPath,
			"--prefix", prefix,
			"--json",
		}
		if listN > 0 {
			args = append(args, "--list", strconv.Itoa(listN))
		}
		if maxBits >= 0 {
			args = append(args, "--max-unknown-bits", strconv.Itoa(maxBits))
		}

		ctx, cancel := context.WithTimeout(r.Context(), trielookupTimeout(timeout))
		defer cancel()

		cmd := exec.CommandContext(ctx, binPath, args...)
		var stdout, stderr bytes.Buffer
		cmd.Stdout = &stdout
		cmd.Stderr = &stderr
		if err := cmd.Run(); err != nil {
			if errors.Is(ctx.Err(), context.DeadlineExceeded) {
				http.Error(w, "lookup timed out", http.StatusGatewayTimeout)
				return
			}
			msg := strings.TrimSpace(stderr.String())
			if msg == "" {
				msg = err.Error()
			}
			http.Error(w, "lookup failed: "+msg, http.StatusBadGateway)
			return
		}

		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		_, _ = w.Write(stdout.Bytes())
	}
}

// Bounds on user input. The index probe itself is O(log N * 2^unknownBits)
// which is fast even at the upper bound, so these are mostly to keep the
// shell-out argv compact.
const (
	maxLookupPrefixLen = 200   // ~8 tokens × 4 chars + `/UMASK` + separators
	maxLookupListN     = 256   // /api/lookup is for live UI counts + a small
	                           // sample. The multi-stage pipeline lives in
	                           // /api/filter, which keeps the larger seed set
	                           // server-side and never ships it over the wire.
)

// Per-call timeout fallback if the caller didn't supply one. Lookups against
// an mmap'd 11 GiB index are sub-second when warm, so 10s is plenty for cold
// + max-fanout edge cases.
func trielookupTimeout(perRequest int) time.Duration {
	if perRequest > 0 {
		return time.Duration(perRequest) * time.Second
	}
	return 10 * time.Second
}

// Validate the prefix string. Each comma-separated token is 4 chars where
// each char is hex or '?', optionally followed by /XXXX where each X is hex.
// Returns an error describing the first invalid token.
func validateLookupPrefix(s string) error {
	if s == "" {
		return errors.New("empty")
	}
	parts := strings.Split(s, ",")
	if len(parts) > 8 {
		return fmt.Errorf("too many tokens (%d > 8)", len(parts))
	}
	for i, t := range parts {
		if err := validateLookupToken(t); err != nil {
			return fmt.Errorf("token %d (%q): %w", i+1, t, err)
		}
	}
	return nil
}

func validateLookupToken(t string) error {
	slash := strings.IndexByte(t, '/')
	base := t
	umask := ""
	if slash >= 0 {
		base = t[:slash]
		umask = t[slash+1:]
	}
	if len(base) != 4 {
		return errors.New("base must be 4 chars")
	}
	for _, c := range base {
		if !isHexOrQuestion(c) {
			return fmt.Errorf("bad char %q in base", c)
		}
	}
	if slash >= 0 {
		if len(umask) != 4 {
			return errors.New("umask must be 4 chars")
		}
		for _, c := range umask {
			if !isHex(c) {
				return fmt.Errorf("bad char %q in umask", c)
			}
		}
	}
	return nil
}

func isHex(c rune) bool {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')
}
func isHexOrQuestion(c rune) bool {
	return c == '?' || isHex(c)
}
