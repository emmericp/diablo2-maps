package main

import (
	"container/list"
	"sync"
)

// Cache is a thread-safe LRU response cache with an admission filter and a
// set of pinned, never-evicted entries.
//
// Admission filter: a key must be observed at least admitThreshold times
// before its response is admitted to the LRU. That keeps one-off seed
// lookups from churning the cache — only seeds visited repeatedly (the
// default seed 0, or a seed in a shared link people open more than once)
// take up space.
//
// Pinned entries are inserted by Pin and never evicted; they don't count
// against maxSize. Used to pre-warm seed 0 at startup.
type Cache struct {
	mu             sync.Mutex
	maxSize        int
	admitThreshold int

	entries map[string]*list.Element // key -> *list.Element holding *entry
	order   *list.List               // LRU order, front = most recently used

	pinned map[string][]byte

	// Bounded admission counter. Tracks recent miss counts so a seed seen
	// admitThreshold times graduates into the main LRU.
	seen      map[string]*list.Element // key -> *list.Element holding *seenEntry
	seenOrder *list.List
	seenCap   int
}

type entry struct {
	key  string
	body []byte
}

type seenEntry struct {
	key   string
	count int
}

func NewCache(maxSize, admitThreshold int) *Cache {
	if maxSize < 1 {
		maxSize = 1
	}
	if admitThreshold < 1 {
		admitThreshold = 1
	}
	seenCap := maxSize * 64
	return &Cache{
		maxSize:        maxSize,
		admitThreshold: admitThreshold,
		entries:        make(map[string]*list.Element),
		order:          list.New(),
		pinned:         make(map[string][]byte),
		seen:           make(map[string]*list.Element),
		seenOrder:      list.New(),
		seenCap:        seenCap,
	}
}

// Get returns a cached response body, or nil if the key is not cached.
func (c *Cache) Get(key string) []byte {
	c.mu.Lock()
	defer c.mu.Unlock()
	if b, ok := c.pinned[key]; ok {
		return b
	}
	if el, ok := c.entries[key]; ok {
		c.order.MoveToFront(el)
		return el.Value.(*entry).body
	}
	return nil
}

// Observe records a miss and admits the body to the LRU once the key has
// been seen admitThreshold times. Calling Observe for an already-cached or
// pinned key is a no-op.
func (c *Cache) Observe(key string, body []byte) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if _, ok := c.pinned[key]; ok {
		return
	}
	if _, ok := c.entries[key]; ok {
		return
	}

	var count int
	if el, ok := c.seen[key]; ok {
		c.seenOrder.MoveToFront(el)
		s := el.Value.(*seenEntry)
		s.count++
		count = s.count
	} else {
		s := &seenEntry{key: key, count: 1}
		el := c.seenOrder.PushFront(s)
		c.seen[key] = el
		count = 1
		for c.seenOrder.Len() > c.seenCap {
			old := c.seenOrder.Back()
			if old == nil {
				break
			}
			c.seenOrder.Remove(old)
			delete(c.seen, old.Value.(*seenEntry).key)
		}
	}

	if count < c.admitThreshold || c.maxSize == 0 {
		return
	}
	c.admitLocked(key, body)

	// Once admitted, the seen counter is no longer needed.
	if el, ok := c.seen[key]; ok {
		c.seenOrder.Remove(el)
		delete(c.seen, key)
	}
}

// Pin stores a body that is never evicted and does not count against
// maxSize. Re-pinning a key replaces the body.
func (c *Cache) Pin(key string, body []byte) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.pinned[key] = body
	if el, ok := c.entries[key]; ok {
		c.order.Remove(el)
		delete(c.entries, key)
	}
	if el, ok := c.seen[key]; ok {
		c.seenOrder.Remove(el)
		delete(c.seen, key)
	}
}

func (c *Cache) admitLocked(key string, body []byte) {
	e := &entry{key: key, body: body}
	el := c.order.PushFront(e)
	c.entries[key] = el
	for c.order.Len() > c.maxSize {
		old := c.order.Back()
		if old == nil {
			break
		}
		c.order.Remove(old)
		delete(c.entries, old.Value.(*entry).key)
	}
}

// Stats reports current cache occupancy. lru is the LRU size, pinned the
// pre-warmed entry count.
func (c *Cache) Stats() (lru, pinned int) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.order.Len(), len(c.pinned)
}
