package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"
)

// Hard cap on the request line we forward to mapdump. mapdump's stdin fgets
// buffer is 2048 bytes — anything longer would desync the protocol because the
// tail of the line gets read as a second request.
const maxRequestLine = 1800

func main() {
	addr := flag.String("addr", ":8080", "listen address")
	mapdumpPath := flag.String("mapdump", `seedgen\build\Release\mapdump.exe`,
		"path to mapdump.exe (must support `server` subcommand)")
	gameDir := flag.String("game", `C:\Program Files (x86)\Diablo II`,
		"Diablo II install directory passed to each worker")
	staticDir := flag.String("static-dir", `renderer\dist`,
		"directory served at /")
	poolSize := flag.Int("pool", 2*runtime.NumCPU(),
		"number of mapdump workers (default 2 * NumCPU)")
	maxReq := flag.Int("max-requests", 100,
		"recycle each worker after this many renders (LoadAct leaks memory)")
	timeout := flag.Duration("timeout", 60*time.Second,
		"per-request timeout; on expiry the worker is force-killed and replaced")
	minHealthy := flag.Int("min-healthy", -1,
		"/healthz reports unhealthy when fewer than this many workers are alive (default: half the pool)")
	runner := flag.String("runner", "",
		"optional command prefix for spawning mapdump (e.g. 'wine' on Linux)")
	cacheSize := flag.Int("cache-size", 128,
		"max number of (seed, acts, difficulty) responses kept in the LRU cache (0 disables)")
	cacheAdmit := flag.Int("cache-admit", 3,
		"a (seed, acts, difficulty) tuple must be requested this many times before it enters the cache")
	trielookupPath := flag.String("trielookup", "",
		"path to the trielookup binary (enables /api/lookup); leave blank to disable")
	trielookupIndex := flag.String("trielookup-index", "",
		"path to the trielookup index (.idx) file consulted by /api/lookup")
	lookupTimeout := flag.Int("lookup-timeout", 10,
		"per-/api/lookup timeout in seconds")
	screenshotsDir := flag.String("screenshots-dir", "",
		"directory of full-size tower screenshots; thumbnails are generated on demand. "+
			"Default: <static-dir>/tower-screenshots")
	flag.Parse()

	if *minHealthy < 0 {
		*minHealthy = *poolSize / 2
		if *minHealthy < 1 {
			*minHealthy = 1
		}
	}

	if *minHealthy > *poolSize {
		log.Printf("warning: --min-healthy %d > --pool %d; /healthz will always be unhealthy",
			*minHealthy, *poolSize)
	}

	pool, err := NewPool(*mapdumpPath, *gameDir, *runner, *poolSize, *maxReq)
	if err != nil {
		log.Fatalf("pool: %v", err)
	}

	cache := NewCache(*cacheSize, *cacheAdmit)
	if *cacheSize > 0 {
		go warmCache(pool, cache, *timeout)
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/api/render", renderHandler(pool, cache, *timeout))
	mux.HandleFunc("/healthz", healthHandler(pool, cache, *minHealthy))

	switch {
	case *trielookupPath != "" && *trielookupIndex != "":
		mux.HandleFunc("/api/lookup", lookupHandler(*trielookupPath, *trielookupIndex, *lookupTimeout))
		mux.HandleFunc("/api/filter", filterHandler(*trielookupPath, *trielookupIndex, pool, 0))
		log.Printf("/api/lookup + /api/filter enabled (trielookup=%q index=%q)", *trielookupPath, *trielookupIndex)
	case *trielookupPath != "" || *trielookupIndex != "":
		log.Printf("warning: /api/lookup + /api/filter disabled: --trielookup and --trielookup-index must both be set")
	}

	{
		sd := *screenshotsDir
		if sd == "" {
			sd = filepath.Join(*staticDir, "tower-screenshots")
		}
		if fi, err := os.Stat(sd); err == nil && fi.IsDir() {
			mux.Handle("/tower-screenshots/",
				http.StripPrefix("/tower-screenshots/", screenshotsHandler(sd)))
			log.Printf("/tower-screenshots/ serving %q (thumbs cached on demand)", sd)
		} else {
			log.Printf("warning: --screenshots-dir %q not a directory; /tower-screenshots/ falls back to static-dir", sd)
		}
	}

	mux.Handle("/", http.FileServer(http.Dir(*staticDir)))

	srv := &http.Server{
		Addr:         *addr,
		Handler:      logRequests(mux),
		ReadTimeout:  10 * time.Second,
		WriteTimeout: *timeout + 5*time.Second,
	}

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-stop
		log.Println("shutting down...")
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		_ = srv.Shutdown(ctx)
		pool.Shutdown()
	}()

	log.Printf("mapserver listening on %s (pool=%d, max-requests=%d, min-healthy=%d, cache=%d/admit=%d, static=%q)",
		*addr, *poolSize, *maxReq, *minHealthy, *cacheSize, *cacheAdmit, *staticDir)
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("server: %v", err)
	}
	pool.Shutdown()
}

func renderHandler(pool *Pool, cache *Cache, timeout time.Duration) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		q := r.URL.Query()
		seedStr := strings.TrimSpace(q.Get("seed"))
		acts := strings.TrimSpace(q.Get("acts"))
		levels := strings.TrimSpace(q.Get("levels"))
		diffStr := strings.TrimSpace(q.Get("difficulty"))
		if diffStr == "" {
			diffStr = "0"
		}

		if _, err := strconv.ParseUint(seedStr, 10, 32); err != nil {
			http.Error(w, "bad seed (uint32 expected)", http.StatusBadRequest)
			return
		}

		// `levels=` wins when both are set. The protocol token sent to mapdump
		// is `L:21,22,23` for level ids, else the acts string ("ALL" or "1,2").
		var filterTok string
		switch {
		case levels != "":
			if !validLevels(levels) {
				http.Error(w, "bad levels (comma-separated integers 1..255)",
					http.StatusBadRequest)
				return
			}
			filterTok = "L:" + levels
		case acts != "":
			if !validActs(acts) {
				http.Error(w, "bad acts (use ALL or comma-separated integers 1..5)",
					http.StatusBadRequest)
				return
			}
			filterTok = acts
		default:
			filterTok = "ALL"
		}
		if d, err := strconv.ParseUint(diffStr, 10, 32); err != nil || d > 2 {
			http.Error(w, "bad difficulty (0, 1, or 2)", http.StatusBadRequest)
			return
		}

		reqLine := seedStr + " " + filterTok + " " + diffStr
		if len(reqLine) > maxRequestLine {
			http.Error(w, fmt.Sprintf("request too long (%d > %d bytes)",
				len(reqLine), maxRequestLine), http.StatusBadRequest)
			return
		}

		if body := cache.Get(reqLine); body != nil {
			w.Header().Set("Content-Type", "application/json")
			w.Header().Set("Cache-Control", "no-store")
			_, _ = w.Write(body)
			return
		}

		ctx, cancel := context.WithTimeout(r.Context(), timeout)
		defer cancel()
		body, err := pool.Render(ctx, reqLine)
		if err != nil {
			switch {
			case errors.Is(err, context.DeadlineExceeded):
				http.Error(w, "render timed out", http.StatusGatewayTimeout)
			case errors.Is(err, context.Canceled):
				// Client disconnected. Don't bother writing a response.
				return
			default:
				http.Error(w, "render failed: "+err.Error(), http.StatusBadGateway)
			}
			return
		}
		cache.Observe(reqLine, body)
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		_, _ = w.Write(body)
	}
}

// warmCache pre-renders seed 0 (the default seed) and pins it. Runs in a
// goroutine after the pool is up; the server starts listening immediately
// and the first uncached request just waits in the pool queue as usual.
func warmCache(pool *Pool, cache *Cache, timeout time.Duration) {
	const key = "0 1 0"
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	body, err := pool.Render(ctx, key)
	if err != nil {
		log.Printf("[cache] warm %q failed: %v", key, err)
		return
	}
	cache.Pin(key, body)
	log.Printf("[cache] warmed %q (%d bytes)", key, len(body))
}

func healthHandler(pool *Pool, cache *Cache, minHealthy int) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		alive := pool.Alive()
		size := pool.Size()
		cacheLRU, cachePinned := cache.Stats()
		healthy := alive >= minHealthy
		status := http.StatusOK
		if !healthy {
			status = http.StatusServiceUnavailable
		}
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.WriteHeader(status)
		fmt.Fprintf(w, `{"healthy":%t,"alive":%d,"size":%d,"min_healthy":%d,"cache_lru":%d,"cache_pinned":%d}`+"\n",
			healthy, alive, size, minHealthy, cacheLRU, cachePinned)
	}
}

func validActs(s string) bool {
	if s == "ALL" || s == "all" {
		return true
	}
	if s == "" {
		return false
	}
	for _, part := range strings.Split(s, ",") {
		part = strings.TrimSpace(part)
		n, err := strconv.Atoi(part)
		if err != nil || n < 1 || n > 5 {
			return false
		}
	}
	return true
}

func validLevels(s string) bool {
	if s == "" {
		return false
	}
	for _, part := range strings.Split(s, ",") {
		part = strings.TrimSpace(part)
		n, err := strconv.Atoi(part)
		if err != nil || n < 1 || n > 255 {
			return false
		}
	}
	return true
}

func logRequests(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		lw := &loggingWriter{ResponseWriter: w, status: 200}
		next.ServeHTTP(lw, r)
		log.Printf("%s %s -> %d (%d B, %s)",
			r.Method, r.URL.RequestURI(), lw.status, lw.bytes,
			time.Since(start).Round(time.Millisecond))
	})
}

type loggingWriter struct {
	http.ResponseWriter
	status int
	bytes  int
}

func (l *loggingWriter) WriteHeader(code int) {
	l.status = code
	l.ResponseWriter.WriteHeader(code)
}
func (l *loggingWriter) Write(b []byte) (int, error) {
	n, err := l.ResponseWriter.Write(b)
	l.bytes += n
	return n, err
}
