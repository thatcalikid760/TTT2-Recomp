package hub

import (
	"bytes"
	"io"
	"log"
	"testing"
	"time"
	"ttt2-server/internal/store"

	"ttt2-server/internal/config"
	"ttt2-server/internal/gin"
	"ttt2-server/internal/wire"
)

type ginStore struct{ profiles int }

func (g *ginStore) SetXUID(uint64, uint64) error                   { return nil }
func (g *ginStore) SteamIDsOf([]uint64) (map[uint64]uint64, error) { return map[uint64]uint64{}, nil }
func (g *ginStore) TeamOf(uint64) (store.Team, bool, error)        { return store.Team{}, false, nil }
func (g *ginStore) SaveUpload(uint64, string, []byte, int, time.Time) error {
	return nil
}
func (g *ginStore) SaveGhost(uint64, int, string, []byte, time.Time) error   { return nil }
func (g *ginStore) Ghosts(int, string, uint64, int) ([]store.Ghost, error)   { return nil, nil }
func (g *ginStore) GhostOf(uint64, int, string) ([]byte, bool, error)        { return nil, false, nil }
func (g *ginStore) SaveReplay(uint64, store.Replay, time.Time) (bool, error) { return true, nil }
func (g *ginStore) Replays(store.ReplayQuery) ([]store.Replay, error)        { return nil, nil }
func (g *ginStore) SaveProfile(uint64, []byte, time.Time) error {
	g.profiles++
	return nil
}

// welcomeFlags logs in and returns the WELCOME's flags byte (and the client).
func welcomeFlags(t *testing.T, s *server, peer uint64) (uint8, *client) {
	t.Helper()
	c, typ, r := s.handshake(t, hello, auth(peer, "G", nil, ""))
	if typ != wire.Welcome {
		t.Fatalf("WELCOME expected, got %d", typ)
	}
	r.U64()
	return r.U8(), &client{t: t, s: s, c: c, peer: peer}
}

var settingsRequest = []byte("GET /2.2/ttt2/xbox/settings HTTP/1.1\r\nHost: x\r\n\r\n")

// The game's GIN requests travel over the control connection when the server says it answers them.
func TestGINOverControl(t *testing.T) {
	st := &ginStore{}
	s := startSetup(t, config.Default(), nil, func(h *Hub) {
		h.SetGIN(gin.New(st, func() gin.Settings { return gin.Settings{News: []string{"HELLO NEWS"}} }, log.New(io.Discard, "", 0)))
	})
	flags, cl := welcomeFlags(t, s, 0x0110000100000201)
	if flags&2 == 0 {
		t.Fatal("WELCOME must say GIN is answered")
	}
	cl.send(wire.GinRequest, func(w *wire.Writer) { w.U32(7); w.Blob(settingsRequest) })
	for {
		typ, r := cl.next()
		if typ != wire.GinResponse {
			continue
		}
		if r.U32() != 7 {
			t.Fatal("the response must carry its request number")
		}
		body := r.Blob()
		if !bytes.HasPrefix(body, []byte("HTTP/1.1 200")) || !bytes.Contains(body, []byte("HELLO NEWS")) {
			t.Fatalf("response %q", body)
		}
		break
	}
	// a game asking too fast gets "busy" answers, not a disconnect
	for i := 0; i < 60; i++ {
		cl.send(wire.GinRequest, func(w *wire.Writer) { w.U32(uint32(100 + i)); w.Blob(settingsRequest) })
	}
	busy := 0
	for got := 0; got < 60; {
		typ, r := cl.next()
		if typ != wire.GinResponse {
			continue
		}
		got++
		r.U32()
		if bytes.HasPrefix(r.Blob(), []byte("HTTP/1.1 503")) {
			busy++
		}
	}
	if busy == 0 || busy == 60 {
		t.Fatalf("%d of 60 busy: the limit is a burst, then a rate", busy)
	}
}

func TestGINNotOffered(t *testing.T) {
	s := start(t, config.Default(), nil)
	flags, cl := welcomeFlags(t, s, 0x0110000100000202)
	if flags&2 != 0 {
		t.Fatal("a server without GIN must not offer it")
	}
	cl.send(wire.GinRequest, func(w *wire.Writer) { w.U32(1); w.Blob(settingsRequest) })
	if !closedSoon(cl.c, 3*time.Second) {
		t.Fatal("a GIN request to a server that did not offer it is a protocol error")
	}
}
