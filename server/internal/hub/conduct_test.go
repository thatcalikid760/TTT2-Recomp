package hub

import (
	"path/filepath"
	"strings"
	"testing"
	"time"

	"ttt2-server/internal/store"
	"ttt2-server/internal/wire"
)

func startConduct(t *testing.T) (*server, *store.Store) {
	t.Helper()
	db, err := store.Open(filepath.Join(t.TempDir(), "server.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	store.SetBoardFreshness(0)
	s := startSetup(t, trusting(), nil, func(h *Hub) {
		h.SetRecords(db)
		h.SetStats(db)
		h.SetConduct(db)
	})
	return s, db
}

func (cl *client) left(session uint64) {
	cl.send(wire.RankedLeft, func(w *wire.Writer) { w.U64(session) })
	cl.sync()
}

func (cl *client) conduct(who uint64) (found uint8, matches, quits uint32, until uint64) {
	cl.send(wire.ConductGet, func(w *wire.Writer) { w.U32(31); w.U64(who) })
	typ, r := cl.next()
	if typ != wire.ConductResult || r.U32() != 31 {
		cl.t.Fatalf("CONDUCT_RESULT expected, got %d", typ)
	}
	found, matches, quits, until = r.U8(), r.U32(), r.U32(), r.U64()
	if r.Err() != nil || len(r.B) != 0 {
		cl.t.Fatal("malformed CONDUCT_RESULT")
	}
	return
}

// a ranked match between host A and guest B in session `id`
func rankedPair(t *testing.T, a, b *client, id uint64) {
	t.Helper()
	a.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(rankedRecord(id, peerA)) })
	a.sync()
	a.register(id, xuidA, peerB)
	b.register(id, xuidB, peerA)
}

func TestConductOffered(t *testing.T) {
	s, _ := startConduct(t)
	if flags, _ := welcomeFlags(t, s, peerA); flags&32 == 0 {
		t.Fatal("WELCOME must offer ranked conduct")
	}
	plain, _ := startStats(t)
	if flags, _ := welcomeFlags(t, plain, peerA); flags&32 != 0 {
		t.Fatal("a server without a conduct store must not offer it")
	}
}

func TestQuitCountsAsLossAndCoolsDown(t *testing.T) {
	s, _ := startConduct(t)
	a, b := s.login(t, peerA, "HOST A"), s.login(t, peerB, "GUEST B")
	const match = 0x8000000000AD0001
	rankedPair(t, a, b, match)
	// B left mid-match: A's game says so and reports both players; B's game says nothing
	a.left(match)
	a.writeStats(match, xuidA, xuidA, result(800, 1, 0))
	a.writeStats(match, xuidA, xuidB, result(300, 0, 1))
	later := time.Now().Add(rankedSettle + time.Second)
	s.h.apply(s.h.sweepRanked(later), later)

	// A's lone report counts for both
	if _, total, rows := a.board(bpView, 1, 10, 0); total != 2 || rows[0].xuid != xuidA || rows[1].rating != 300 {
		t.Fatalf("the remaining game's results must count: %d %+v", total, rows)
	}
	// B is told, and is in a cooldown
	typ, r := b.next()
	if typ != wire.Notice || !strings.Contains(r.Str(), "5 minutes") {
		t.Fatalf("NOTICE about the cooldown expected, got %d", typ)
	}
	if found, matches, quits, until := b.conduct(0); found != 1 || matches != 1 || quits != 1 || until == 0 {
		t.Fatalf("B's own record: %d %d %d %d", found, matches, quits, until)
	}
	if found, matches, quits, until := a.conduct(peerB); found != 1 || matches != 1 || quits != 1 || until != 0 {
		t.Fatalf("B's record as A sees it (no cooldown shown): %d %d %d %d", found, matches, quits, until)
	}
	if _, matches, quits, _ := a.conduct(0); matches != 1 || quits != 0 {
		t.Fatalf("A's record: %d matches, %d quits", matches, quits)
	}

	// during the cooldown B neither finds nor hosts ranked rooms, and cannot register
	const next = 0x8000000000AD0002
	a.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(rankedRecord(next, peerA)) })
	a.sync()
	time.Sleep(listCacheTime) // lists come from a snapshot at most this old
	if len(b.list()) != 0 {
		t.Fatal("a player in a cooldown must not find ranked rooms")
	}
	const own = 0x8000000000BD0001
	b.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(rankedRecord(own, peerB)) })
	b.sync()
	time.Sleep(listCacheTime)
	if len(a.list()) != 0 {
		t.Fatal("a ranked room of a player in a cooldown must not be listed")
	}
	b.register(next, xuidB, peerA)
	s.h.ranked.mu.Lock()
	m := s.h.ranked.matches[next]
	registered := m != nil && m.claims[peerB] != (claim{})
	s.h.ranked.mu.Unlock()
	if registered {
		t.Fatal("a player in a cooldown must not register for a ranked match")
	}
	// a Player Match room is still found
	const plain = 0x8000000000AD0003
	a.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(makeRecord(plain, peerA)) })
	a.sync()
	time.Sleep(listCacheTime)
	if len(b.list()) != 1 {
		t.Fatal("a cooldown is about ranked matches only")
	}
	// the admin's pardon ends it (the lists give one room per host: the Player Match room goes first)
	a.send(wire.SessionDel, func(w *wire.Writer) { w.U64(plain) })
	a.sync()
	if err := s.h.conduct.(*store.Store).ClearCooldown(peerB); err != nil {
		t.Fatal(err)
	}
	s.h.Pardon(peerB)
	time.Sleep(listCacheTime + 500*time.Millisecond) // and the list budget (2 a second)
	if len(b.list()) != 1 {
		t.Fatal("a pardoned player finds ranked rooms again")
	}
}

func TestMutualLeftIsNobodysQuit(t *testing.T) {
	s, db := startConduct(t)
	a, b := s.login(t, peerA, "HOST A"), s.login(t, peerB, "GUEST B")
	const match = 0x8000000000AD0011
	rankedPair(t, a, b, match)
	// the connection broke: each game says the other left, and each reports what it saw
	a.left(match)
	b.left(match)
	a.writeStats(match, xuidA, xuidA, result(800, 1, 0))
	b.writeStats(match, xuidB, xuidB, result(500, 1, 0))
	later := time.Now().Add(rankedSettle + time.Second)
	s.h.apply(s.h.sweepRanked(later), later)
	for _, id := range []uint64{peerA, peerB} {
		c, err := db.Conduct(id)
		if err != nil || c.Quits != 0 || c.Matches != 1 || !c.CooldownUntil.IsZero() {
			t.Fatalf("%d: %+v %v", id, c, err)
		}
	}
	if _, total, _ := a.board(bpView, 1, 10, 0); total != 0 {
		t.Fatal("unconfirmed reports of a broken match must not count")
	}
}

func TestSilentPlayerIsTheQuitter(t *testing.T) {
	s, db := startConduct(t)
	a, b := s.login(t, peerA, "HOST A"), s.login(t, peerB, "GUEST B")
	const match = 0x8000000000AD0021
	rankedPair(t, a, b, match)
	// a game without RANKED_LEFT: only B's game reports
	b.writeStats(match, xuidB, xuidB, result(900, 1, 0))
	b.writeStats(match, xuidB, xuidA, result(100, 0, 1))
	later := time.Now().Add(rankedSettle + time.Second)
	s.h.apply(s.h.sweepRanked(later), later)
	if c, _ := db.Conduct(peerA); c.Quits != 1 {
		t.Fatalf("the silent player is the quitter: %+v", c)
	}
	if _, total, _ := b.board(bpView, 1, 10, 0); total != 2 {
		t.Fatal("the reporting game's results must count")
	}
	// RANKED_LEFT from someone not in the match, or for an unknown match, changes nothing
	c := s.login(t, peerC, "THIRD C")
	c.left(match)
	c.left(0x8000000000FFFFFF)
}

func TestPenaltyLadder(t *testing.T) {
	now := time.Unix(1_800_000_000, 0)
	cases := []struct {
		before   store.Conduct
		strikes  int
		cooldown time.Duration
	}{
		{store.Conduct{}, 1, 5 * time.Minute},
		{store.Conduct{Strikes: 1, LastQuit: now.Add(-time.Hour)}, 2, 15 * time.Minute},
		{store.Conduct{Strikes: 2, LastQuit: now.Add(-time.Hour)}, 3, time.Hour},
		{store.Conduct{Strikes: 7, LastQuit: now.Add(-time.Hour)}, 8, time.Hour},
		{store.Conduct{Strikes: 2, LastQuit: now.Add(-25 * time.Hour)}, 2, 15 * time.Minute}, // one forgiven
		{store.Conduct{Strikes: 2, LastQuit: now.Add(-100 * time.Hour)}, 1, 5 * time.Minute}, // all forgiven
	}
	for _, c := range cases {
		strikes, until := penalty(c.before, now)
		if strikes != c.strikes || until.Sub(now) != c.cooldown {
			t.Errorf("%+v: strikes %d, cooldown %s", c.before, strikes, until.Sub(now))
		}
	}
	if minutesText(5*time.Minute) != "5 minutes" || minutesText(time.Hour) != "1 hour" || minutesText(2*time.Hour) != "2 hours" {
		t.Error("minutesText")
	}
}
