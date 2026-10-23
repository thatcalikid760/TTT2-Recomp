package hub

import (
	"encoding/binary"
	"testing"
	"time"

	"ttt2-server/internal/record"
	"ttt2-server/internal/wire"
)

// recordWith: a session record with GAME_TYPE (0x800A) and its public slots.
func recordWith(id, host uint64, gameType uint32, filled, max uint32) []byte {
	var w wire.Writer
	w.U32(record.Magic)
	w.U64(id)
	w.U64(host)
	w.U64(0x0009000000000001)
	w.U32(0x32F)
	w.U32(max)
	w.U32(0)
	w.U32(filled)
	w.U32(0)
	info := make([]byte, 0x3C)
	binary.BigEndian.PutUint64(info[0:], id)
	binary.BigEndian.PutUint64(info[8+0x18:], host)
	w.Raw(info)
	w.U32(2)
	w.U32(0x800A)
	w.U32(gameType)
	w.U32(0x800B)
	w.U32(0)
	w.U32(0)
	w.U32(0x80000001)
	return w.B
}

func (cl *client) search(filters map[uint32]uint32, openOnly bool) []uint64 {
	cl.send(wire.SessionList, func(w *wire.Writer) {
		w.U32(5)
		w.U32(64)
		w.U8(uint8(len(filters)))
		for id, v := range filters {
			w.U32(id)
			w.U32(v)
		}
		if openOnly {
			w.U8(1)
		} else {
			w.U8(0)
		}
	})
	typ, r := cl.next()
	if typ != wire.ListResult || r.U32() != 5 {
		cl.t.Fatal("LIST_RESULT")
	}
	var ids []uint64
	for n := r.U32(); n > 0; n-- {
		rec := r.Blob()
		ids = append(ids, binary.LittleEndian.Uint64(rec[4:12]))
	}
	return ids
}

func TestSearchFindsRankedAmongMany(t *testing.T) {
	cfg := trusting()
	cfg.MaxConnsPerIP = 128
	cfg.MaxPlayers = 200
	s := start(t, cfg, nil)
	ranked := s.login(t, 0x0110000100005000, "RANKED HOST")
	const rankedID = 0x8000000000500000
	ranked.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(recordWith(rankedID, ranked.peer, 0, 0, 1)) })
	ranked.sync()
	// then 70 newer Player Match rooms of 70 hosts (the first one full), and a host with 4 rooms
	for h := 0; h < 70; h++ {
		c := s.login(t, 0x0110000100006000+uint64(h), "PM")
		id := 0x8000000000600000 + uint64(h)
		filled := uint32(0)
		if h == 0 {
			filled = 4
		}
		c.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(recordWith(id, c.peer, 1, filled, 4)) })
		c.sync()
	}
	many := s.login(t, 0x0110000100008000, "MANY ROOMS")
	for k := 0; k < 4; k++ {
		id := 0x8000000000800000 + uint64(k)
		many.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(recordWith(id, many.peer, 1, 0, 4)) })
	}
	many.sync()
	s.h.mu.Lock()
	s.h.lists.byProto = nil // a fresh snapshot
	s.h.mu.Unlock()
	searcher := s.login(t, 0x0110000100007000, "SEARCHER")
	all := searcher.search(nil, false) // an old game's list: no filters, the 64 newest rooms
	for _, id := range all {
		if id == rankedID {
			t.Fatal("the unfiltered list holds only the 64 newest rooms")
		}
	}
	if len(all) != 64 {
		t.Fatalf("%d", len(all))
	}
	if got := searcher.search(map[uint32]uint32{0x800A: 0}, true); len(got) != 1 || got[0] != rankedID {
		t.Fatalf("a ranked search must find the ranked room: %x", got)
	}
	open := searcher.search(map[uint32]uint32{0x800A: 1}, true)
	for _, id := range open {
		if id == 0x8000000000600000 {
			t.Fatal("a full room in an open-slots search")
		}
	}
	if len(open) != 64 {
		t.Fatalf("%d open Player Match rooms", len(open))
	}
	mine := 0
	for _, id := range open {
		if id >= 0x8000000000800000 && id < 0x8000000000800004 {
			mine++
		}
	}
	if mine != 1 {
		t.Fatalf("a host's rooms in one answer: %d (one per host)", mine)
	}
	// a session without the asked context does not match
	if got := searcher.search(map[uint32]uint32{0x1234: 1}, false); len(got) != 0 {
		t.Fatalf("%d rooms without the context", len(got))
	}
	// a malformed filter closes the connection
	searcher.send(wire.SessionList, func(w *wire.Writer) { w.U32(1); w.U32(1); w.U8(200) })
	if !closedSoon(searcher.c, 3e9) {
		t.Fatal("bad filters must close the connection")
	}
}

func (cl *client) peerCheck(peer uint64) uint8 {
	cl.send(wire.PeerCheck, func(w *wire.Writer) { w.U32(77); w.U64(peer) })
	typ, r := cl.next()
	if typ != wire.PeerResult || r.U32() != 77 {
		cl.t.Fatal("PEER_RESULT")
	}
	return r.U8()
}

func TestPeerCheck(t *testing.T) {
	s := start(t, trusting(), nil)
	a := s.login(t, 0x0110000100009001, "A")
	b := s.login(t, 0x0110000100009002, "B")
	if a.peerCheck(b.peer) != 1 || b.peerCheck(a.peer) != 1 {
		t.Fatal("players of this server")
	}
	if a.peerCheck(0x0110000100009FFF) != 0 || a.peerCheck(a.peer) != 0 {
		t.Fatal("someone not here (or oneself)")
	}
	b.c.Close()
	deadline := time.Now().Add(3 * time.Second)
	for a.peerCheck(b.peer) != 0 {
		if time.Now().After(deadline) {
			t.Fatal("a player who left must not count")
		}
		time.Sleep(20 * time.Millisecond)
	}
}

func TestXuidNames(t *testing.T) {
	s, db := startStats(t)
	a := s.login(t, peerA, "Alice")
	s.login(t, peerB, "Bob")
	if err := db.SetXUID(peerA, xuidA); err != nil {
		t.Fatal(err)
	}
	if err := db.SetXUID(peerB, xuidB); err != nil {
		t.Fatal(err)
	}
	ask := func(xuids ...uint64) map[uint64]string {
		a.send(wire.XuidNames, func(w *wire.Writer) {
			w.U32(5)
			w.U8(uint8(len(xuids)))
			for _, x := range xuids {
				w.U64(x)
			}
		})
		typ, r := a.next()
		if typ != wire.NamesResult || r.U32() != 5 {
			t.Fatal("XUID_NAMES_RESULT")
		}
		out := map[uint64]string{}
		for n := int(r.U8()); n > 0; n-- {
			x := r.U64()
			out[x] = r.Str()
		}
		if r.Err() != nil {
			t.Fatal(r.Err())
		}
		return out
	}
	got := ask(xuidA, xuidB, xuidC)
	if len(got) != 2 || got[xuidA] != "Alice" || got[xuidB] != "Bob" {
		t.Fatalf("%v", got)
	}
	// more than 16 XUIDs: a malformed frame
	a.send(wire.XuidNames, func(w *wire.Writer) { w.U32(1); w.U8(17) })
	if !closedSoon(a.c, 3e9) {
		t.Fatal("too many XUIDs must close the connection")
	}
}
