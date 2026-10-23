package hub

import (
	"bytes"
	"io"
	"log"
	"os"
	"runtime"
	"sort"
	"strconv"
	"sync"
	"testing"
	"time"

	"ttt2-server/internal/gin"
	"ttt2-server/internal/wire"
)

// TestScale: thousands of games at once - log in, host, list, ask GIN, leave - with the answer times measured, and
// the server ends empty with no goroutines left behind. HUB_SCALE_PLAYERS sets the count (default 1000; the server
// is meant for thousands - run it with 5000).
func TestScale(t *testing.T) {
	if testing.Short() {
		t.Skip("scale test")
	}
	n := 1000
	if v, err := strconv.Atoi(os.Getenv("HUB_SCALE_PLAYERS")); err == nil && v > 0 {
		n = v
	}
	cfg := trusting()
	cfg.MaxPlayers = n
	cfg.MaxConnsPerIP = n
	s := startSetup(t, cfg, nil, func(h *Hub) {
		h.SetGIN(gin.New(&ginStore{}, func() gin.Settings { return gin.Settings{MOTD: "m"} }, log.New(io.Discard, "", 0)))
	})
	before := runtime.NumGoroutine()
	var memBefore runtime.MemStats
	runtime.GC()
	runtime.ReadMemStats(&memBefore)

	clients := make([]*client, n)
	var wg sync.WaitGroup
	slots := make(chan struct{}, maxPendingPrefix) // one address: at most 4 handshakes at a time
	start := time.Now()
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			slots <- struct{}{}
			clients[i] = s.login(t, 0x0110000100010000+uint64(i), "P"+strconv.Itoa(i%1000))
			<-slots
		}(i)
	}
	wg.Wait()
	logins := time.Since(start)
	var memAfter runtime.MemStats
	runtime.GC()
	runtime.ReadMemStats(&memAfter)
	if st := s.h.Stats(); st.Online != n {
		t.Fatalf("%d online", st.Online)
	}
	t.Logf("%d logins in %s (%.0f per second); heap %+.1f MB (%.1f KB a player, test clients included)", n, logins.Round(time.Millisecond),
		float64(n)/logins.Seconds(), float64(memAfter.HeapAlloc-memBefore.HeapAlloc)/1e6, float64(memAfter.HeapAlloc-memBefore.HeapAlloc)/1e3/float64(n))

	// half of them host a session
	for i, c := range clients {
		if i%2 == 0 {
			c.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(makeRecord(0x8000000000100000+uint64(i), c.peer)) })
		}
	}
	for _, c := range clients {
		c.sync()
	}
	if st := s.h.Stats(); st.Sessions != (n+1)/2 {
		t.Fatalf("%d sessions", st.Sessions)
	}
	time.Sleep(listCacheTime)

	// everyone searches and asks GIN at once; every answer is timed
	lat := make([]time.Duration, 0, 2*n)
	var mu sync.Mutex
	errs := make(chan string, n)
	start = time.Now()
	for i, c := range clients {
		wg.Add(1)
		go func(i int, c *client) {
			defer wg.Done()
			t0 := time.Now()
			c.send(wire.SessionList, func(w *wire.Writer) { w.U32(7); w.U32(maxListResults) })
			c.send(wire.GinRequest, func(w *wire.Writer) { w.U32(8); w.Blob(settingsRequest) })
			var listed int
			for got := 0; got < 2; {
				c.c.SetReadDeadline(time.Now().Add(20 * time.Second))
				typ, payload, err := wire.ReadFrame(c.c)
				if err != nil {
					errs <- err.Error()
					return
				}
				r := wire.Reader{B: payload}
				switch typ {
				case wire.ListResult:
					r.U32()
					listed = int(r.U32())
				case wire.GinResponse:
					r.U32()
					if !bytes.HasPrefix(r.Blob(), []byte("HTTP/1.1 200")) {
						errs <- "GIN answer"
						return
					}
				default:
					continue
				}
				got++
				mu.Lock()
				lat = append(lat, time.Since(t0))
				mu.Unlock()
			}
			if listed != min(maxListResults, (n+1)/2) {
				errs <- "listed " + strconv.Itoa(listed)
			}
		}(i, c)
	}
	wg.Wait()
	close(errs)
	for e := range errs {
		t.Fatal(e)
	}
	sort.Slice(lat, func(i, j int) bool { return lat[i] < lat[j] })
	t.Logf("%d lists + %d GIN answers in %s; answer time median %s, 99%% %s, max %s", n, n, time.Since(start).Round(time.Millisecond),
		lat[len(lat)/2].Round(time.Microsecond), lat[len(lat)*99/100].Round(time.Microsecond), lat[len(lat)-1].Round(time.Microsecond))

	for _, c := range clients {
		c.c.Close()
	}
	deadline := time.Now().Add(10 * time.Second)
	for {
		st := s.h.Stats()
		// Serve's own goroutines (UDP, INFO worker, ranked-results sweeper) may start after `before` was taken
		if st.Online == 0 && st.Sessions == 0 && runtime.NumGoroutine() <= before+3 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("after everyone left: %+v, goroutines %d (before %d)", st, runtime.NumGoroutine(), before)
		}
		time.Sleep(50 * time.Millisecond)
	}
	s.h.mu.Lock()
	leftover := len(s.h.perPrefix) + len(s.h.pendPrefix) + s.h.pending
	s.h.mu.Unlock()
	if leftover != 0 {
		t.Fatalf("connection accounting leaked: %d", leftover)
	}
}
