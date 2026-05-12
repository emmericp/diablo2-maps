package main

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"log"
	"math/rand"
	"os"
	"os/exec"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// Pool keeps `size` mapdump.exe child processes alive in "server" mode and
// dispatches one render request to one worker at a time. Workers are recycled
// after maxRequests because LoadAct leaks memory inside the D2 DLLs, and
// replaced indefinitely on respawn failure (exponential backoff).
type Pool struct {
	mapdumpPath string
	gameDir     string
	runner      string // optional command prefix, e.g. "wine" on Linux
	maxRequests int
	size        int

	idle    chan *worker  // available workers (cap == size)
	closeCh chan struct{} // closed by Shutdown to wake sleeping respawn loops
	alive   atomic.Int32  // workers whose process is running (idle or checked out)

	mu     sync.Mutex
	closed bool
}

type worker struct {
	id          int
	cmd         *exec.Cmd
	stdin       io.WriteCloser
	stdout      *bufio.Reader
	served      int
	maxRequests int // pool.maxRequests jittered by ±5% so workers don't all retire at once
}

func NewPool(mapdumpPath, gameDir, runner string, size, maxRequests int) (*Pool, error) {
	if size < 1 {
		return nil, errors.New("pool size must be >= 1")
	}
	if maxRequests < 1 {
		return nil, errors.New("maxRequests must be >= 1")
	}
	p := &Pool{
		mapdumpPath: mapdumpPath,
		gameDir:     gameDir,
		runner:      runner,
		maxRequests: maxRequests,
		size:        size,
		idle:        make(chan *worker, size),
		closeCh:     make(chan struct{}),
	}
	for i := 0; i < size; i++ {
		w, err := p.spawn(i)
		if err != nil {
			p.Shutdown()
			return nil, fmt.Errorf("spawn worker %d: %w", i, err)
		}
		p.idle <- w
	}
	return p, nil
}

func (p *Pool) spawn(id int) (*worker, error) {
	args := []string{p.mapdumpPath, "server", "--game", p.gameDir}
	var prog string
	var argv []string
	if tokens := strings.Fields(p.runner); len(tokens) > 0 {
		// e.g. --runner "wine"  ->  exec("wine", "mapdump.exe", "server", ...)
		prog = tokens[0]
		argv = append(tokens[1:], args...)
	} else {
		prog = args[0]
		argv = args[1:]
	}
	cmd := exec.Command(prog, argv...)
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, err
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	cmd.Stderr = &prefixWriter{prefix: fmt.Sprintf("[worker %d] ", id), w: os.Stderr}
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	pid := cmd.Process.Pid
	p.alive.Add(1)
	log.Printf("[pool] spawned worker %d (pid=%d, alive=%d/%d)",
		id, pid, p.alive.Load(), p.size)

	// The monitor owns cmd.Wait() and decrements alive when the process exits
	// for ANY reason: a Pool.kill, an internal crash, an external kill, etc.
	// This keeps /healthz honest without proactive polling.
	go func() {
		_ = cmd.Wait()
		p.alive.Add(-1)
		log.Printf("[worker %d] process exited (pid=%d, alive=%d/%d)",
			id, pid, p.alive.Load(), p.size)
	}()

	return &worker{
		id:          id,
		cmd:         cmd,
		stdin:       stdin,
		stdout:      bufio.NewReaderSize(stdout, 64*1024),
		maxRequests: jitterMaxRequests(p.maxRequests),
	}, nil
}

// Spread retirement so a fleet doesn't synchronize a respawn storm.
// ±5% uniform, with a floor of 1.
func jitterMaxRequests(base int) int {
	span := base / 10 // 10% wide window = ±5%
	if span < 2 {
		return base
	}
	n := base - span/2 + rand.Intn(span+1)
	if n < 1 {
		n = 1
	}
	return n
}

// Render sends one request line to a worker and returns the one-line JSON
// response (with trailing newline stripped).
//
// The worker is force-killed and replaced if:
//   - ctx fires before the worker responds (handler timeout / client cancel),
//   - the exec call returns an I/O error (subprocess crashed),
//   - the worker hit maxRequests (proactive retirement).
func (p *Pool) Render(ctx context.Context, req string) ([]byte, error) {
	// Wait for an idle worker, but respect ctx so a starving client doesn't
	// queue forever.
	var w *worker
	select {
	case x, ok := <-p.idle:
		if !ok {
			return nil, errors.New("pool is shut down")
		}
		w = x
	case <-ctx.Done():
		return nil, ctx.Err()
	}

	type result struct {
		body []byte
		err  error
	}
	// Buffer 1 so the exec goroutine can always complete its send and exit,
	// even if Render returned early on ctx.Done.
	ch := make(chan result, 1)
	go func() {
		body, err := w.exec(req)
		ch <- result{body: body, err: err}
	}()

	select {
	case res := <-ch:
		if res.err != nil {
			log.Printf("[worker %d] crashed after %d requests: %v",
				w.id, w.served, res.err)
			p.kill(w)
			go p.replace(w.id)
			return nil, res.err
		}
		w.served++
		if w.served >= w.maxRequests {
			log.Printf("[worker %d] retiring after %d requests (target %d)",
				w.id, w.served, w.maxRequests)
			p.kill(w)
			go p.replace(w.id)
		} else if !p.tryReturn(w) {
			p.kill(w)
		}
		return res.body, nil

	case <-ctx.Done():
		log.Printf("[worker %d] killing on ctx (%v) after %d prior requests",
			w.id, ctx.Err(), w.served)
		p.kill(w)
		go p.replace(w.id)
		return nil, ctx.Err()
	}
}

func (w *worker) exec(req string) ([]byte, error) {
	if _, err := io.WriteString(w.stdin, req+"\n"); err != nil {
		return nil, fmt.Errorf("write: %w", err)
	}
	line, err := w.stdout.ReadBytes('\n')
	if err != nil {
		return nil, fmt.Errorf("read: %w", err)
	}
	for len(line) > 0 && (line[len(line)-1] == '\n' || line[len(line)-1] == '\r') {
		line = line[:len(line)-1]
	}
	return line, nil
}

func (p *Pool) kill(w *worker) {
	_ = w.stdin.Close()
	if w.cmd.Process != nil {
		_ = w.cmd.Process.Kill()
	}
	// cmd.Wait() and the alive-- are owned by the monitor goroutine started
	// in spawn(). Calling Wait here too would race (Wait is not safe to call
	// concurrently) and the second decrement would corrupt /healthz.
}

// replace respawns a worker slot, retrying with exponential backoff until it
// succeeds or the pool is closed.
func (p *Pool) replace(id int) {
	const maxBackoff = 30 * time.Second
	backoff := time.Second
	for {
		if p.isClosed() {
			return
		}
		w, err := p.spawn(id)
		if err == nil {
			if !p.tryReturn(w) {
				p.kill(w)
			}
			return
		}
		log.Printf("[pool] respawn %d failed: %v; retrying in %s", id, err, backoff)
		select {
		case <-time.After(backoff):
		case <-p.closeCh:
			return
		}
		backoff *= 2
		if backoff > maxBackoff {
			backoff = maxBackoff
		}
	}
}

func (p *Pool) tryReturn(w *worker) bool {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed {
		return false
	}
	p.idle <- w
	return true
}

func (p *Pool) isClosed() bool {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.closed
}

func (p *Pool) Shutdown() {
	p.mu.Lock()
	if p.closed {
		p.mu.Unlock()
		return
	}
	p.closed = true
	close(p.closeCh)
	close(p.idle)
	p.mu.Unlock()

	for w := range p.idle {
		p.kill(w)
	}
}

// Alive returns the count of worker subprocesses currently running (idle or
// checked out). Replacement goroutines that are mid-backoff count as 0.
func (p *Pool) Alive() int { return int(p.alive.Load()) }
func (p *Pool) Size() int  { return p.size }

// prefixWriter prepends a tag to each line of the child's stderr so a
// multi-worker stderr stream stays readable.
type prefixWriter struct {
	prefix string
	w      io.Writer
	buf    []byte
}

func (p *prefixWriter) Write(b []byte) (int, error) {
	p.buf = append(p.buf, b...)
	for {
		i := indexByte(p.buf, '\n')
		if i < 0 {
			break
		}
		line := p.buf[:i+1]
		_, _ = fmt.Fprintf(p.w, "%s%s", p.prefix, line)
		p.buf = p.buf[i+1:]
	}
	return len(b), nil
}

func indexByte(b []byte, c byte) int {
	for i, x := range b {
		if x == c {
			return i
		}
	}
	return -1
}
