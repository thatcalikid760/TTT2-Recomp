package hub

import (
	"encoding/binary"
	"path/filepath"
	"testing"
	"time"

	"ttt2-server/internal/stats"
	"ttt2-server/internal/store"
	"ttt2-server/internal/wire"
)

// startHistory: a server keeping statistics and the match history in a fresh database.
func startHistory(t *testing.T) (*server, *store.Store) {
	t.Helper()
	db, err := store.Open(filepath.Join(t.TempDir(), "server.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	store.SetBoardFreshness(0)
	cfg := trusting()
	cfg.MaxConnsPerIP = 64 // every test player comes from 127.0.0.1
	s := startSetup(t, cfg, nil, func(h *Hub) {
		h.SetRecords(db)
		h.SetStats(db)
		h.SetMatches(db)
	})
	return s, db
}

// withMatchTimes sets the match history's times for one test.
func withMatchTimes(t *testing.T, rematch, shortest time.Duration) {
	oldRematch, oldShortest := matchRematch, matchShortest
	matchRematch, matchShortest = rematch, shortest
	t.Cleanup(func() { matchRematch, matchShortest = oldRematch, oldShortest })
}

func (cl *client) matchEvent(session uint64, event, local uint8, peers ...uint64) {
	cl.send(wire.MatchEvent, func(w *wire.Writer) {
		w.U64(session)
		w.U8(event)
		w.U8(local)
		w.U8(uint8(len(peers)))
		for _, p := range peers {
			w.U64(p)
		}
	})
	cl.sync()
}

// history is the recorded matches (once the queued writes are done), newest first.
func history(t *testing.T, s *server, db *store.Store, who string) []store.Match {
	t.Helper()
	s.h.FlushMatches()
	ms, err := db.RecentMatches(who, 200)
	if err != nil {
		t.Fatal(err)
	}
	return ms
}

func TestMatchStartJoinEnd(t *testing.T) {
	withMatchTimes(t, 20*time.Second, 0)
	s, db := startHistory(t)
	flags, a := welcomeFlags(t, s, peerA)
	if flags&8 == 0 {
		t.Fatal("WELCOME must say matches are recorded")
	}
	b := s.login(t, peerB, "GUEST B")
	const room = 0x8000000000DD0001
	a.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(makeRecord(room, peerA)) })
	a.sync()
	// A's game names B; B's own report says two players sit at its PC (pair play)
	a.matchEvent(room, matchStarted, 1, peerB, peerC) // C is no player here: left out
	b.matchEvent(room, matchStarted, 2, peerA)
	a.matchEvent(room, matchStarted, 1, peerB) // a resend changes nothing
	if st := s.h.Stats(); st.InMatches != 2 {
		t.Fatalf("%d in matches", st.InMatches)
	}
	ms := history(t, s, db, "")
	if len(ms) != 1 || ms[0].Kind != store.KindPlayer || !ms[0].Ended.IsZero() || len(ms[0].Players) != 2 ||
		ms[0].Players[0].SteamID != peerA || ms[0].Players[0].Name != "G" || ms[0].Players[0].LocalPlayers != 1 ||
		ms[0].Players[1].SteamID != peerB || ms[0].Players[1].LocalPlayers != 2 {
		t.Fatalf("%+v", ms)
	}
	// an END from either player closes it; the other's END finds nothing open
	b.matchEvent(room, matchEnded, 2)
	a.matchEvent(room, matchEnded, 1)
	if st := s.h.Stats(); st.InMatches != 0 {
		t.Fatalf("%d in matches after the end", st.InMatches)
	}
	if ms = history(t, s, db, ""); len(ms) != 1 || ms[0].Result != "ended" || ms[0].Ended.IsZero() {
		t.Fatalf("%+v", ms)
	}
	// the next match in the room: a new row
	a.matchEvent(room, matchStarted, 1, peerB)
	if ms = history(t, s, db, ""); len(ms) != 2 || !ms[0].Ended.IsZero() {
		t.Fatalf("%+v", ms)
	}
	// an END for a match only just opened is the previous one's, arriving late: ignored
	matchShortest = time.Minute
	b.matchEvent(room, matchEnded, 2)
	if ms = history(t, s, db, ""); !ms[0].Ended.IsZero() {
		t.Fatal("a late END closed the next match")
	}
	// someone not in the match cannot end it
	matchShortest = 0
	c := s.login(t, peerC, "THIRD C")
	c.matchEvent(room, matchEnded, 1)
	if ms = history(t, s, db, ""); !ms[0].Ended.IsZero() {
		t.Fatal("an outsider ended a match")
	}
	if ms = history(t, s, db, "76561198000000000"); len(ms) != 0 {
		t.Fatal("matches of nobody")
	}
}

func TestMatchRematchAndSweep(t *testing.T) {
	withMatchTimes(t, 50*time.Millisecond, 0)
	s, db := startHistory(t)
	a, b := s.login(t, peerA, "HOST A"), s.login(t, peerB, "GUEST B")
	const room = 0x8000000000DD0002
	a.matchEvent(room, matchStarted, 1, peerB)
	b.matchEvent(room, matchStarted, 1, peerA) // B was named: its own STARTED joins, it starts nothing new
	time.Sleep(60 * time.Millisecond)
	// A starts again without an END: the first match was over
	a.matchEvent(room, matchStarted, 1, peerB)
	ms := history(t, s, db, "")
	if len(ms) != 2 || !ms[0].Ended.IsZero() || ms[1].Result != "unfinished" || ms[1].Ended.IsZero() || len(ms[1].Players) != 2 {
		t.Fatalf("%+v", ms)
	}
	// everyone leaves: the sweep closes it once nobody has been in it for a while
	a.c.Close()
	b.c.Close()
	deadline := time.Now().Add(3 * time.Second)
	for s.h.Stats().Online != 0 {
		if time.Now().After(deadline) {
			t.Fatal("players did not leave")
		}
		time.Sleep(20 * time.Millisecond)
	}
	s.h.sweepMatches(time.Now())
	if ms = history(t, s, db, ""); !ms[0].Ended.IsZero() {
		t.Fatal("closed at once: a player may be reconnecting")
	}
	s.h.sweepMatches(time.Now().Add(matchAbandoned + time.Second))
	if ms = history(t, s, db, ""); ms[0].Result != "unfinished" || ms[0].Ended.After(time.Now()) {
		t.Fatalf("an abandoned match: %+v", ms[0])
	}
	// a match nobody ends is closed after an hour, its players online or not
	c := s.login(t, peerC, "THIRD C")
	c.matchEvent(0x8000000000DD0003, matchStarted, 1)
	later := time.Now().Add(matchLongest + time.Second)
	s.h.sweepMatches(later)
	if ms = history(t, s, db, ""); ms[0].Session != 0x8000000000DD0003 || ms[0].Result != "unfinished" || ms[0].Ended.Unix() != later.Unix() {
		t.Fatalf("%+v", ms[0])
	}
	if s.h.Stats().InMatches != 0 {
		t.Fatal("a closed match still counts")
	}
}

func TestMatchEventLimits(t *testing.T) {
	withMatchTimes(t, 20*time.Second, 0)
	s, db := startHistory(t)
	// a burst beyond 4 events is ignored: the END of this one never lands
	a := s.login(t, peerA, "A")
	for i := 0; i < 6; i++ {
		a.matchEvent(0x8000000000DD0010, matchStarted, 1)
	}
	a.matchEvent(0x8000000000DD0010, matchEnded, 1)
	if ms := history(t, s, db, ""); len(ms) != 1 || !ms[0].Ended.IsZero() {
		t.Fatalf("%+v", ms)
	}
	// a player opens at most 3 matches in a burst
	b := s.login(t, peerB, "B")
	for i := uint64(0); i < 4; i++ {
		b.matchEvent(0x8000000000DD0020+i, matchStarted, 1)
	}
	if ms := history(t, s, db, "B"); len(ms) != 3 {
		t.Fatalf("%d matches opened", len(ms))
	}
	// malformed events close the connection (error 8)
	for i, bad := range []func(w *wire.Writer){
		func(w *wire.Writer) { w.U64(1); w.U8(3); w.U8(1); w.U8(0) },                          // no such event
		func(w *wire.Writer) { w.U64(1); w.U8(1); w.U8(0); w.U8(0) },                          // nobody at the PC
		func(w *wire.Writer) { w.U64(1); w.U8(1); w.U8(5); w.U8(0) },                          // five players at one PC
		func(w *wire.Writer) { w.U64(0); w.U8(1); w.U8(1); w.U8(0) },                          // no session
		func(w *wire.Writer) { w.U64(1); w.U8(1); w.U8(1); w.U8(8); w.Raw(make([]byte, 64)) }, // eight peers
		func(w *wire.Writer) { w.U64(1); w.U8(1); w.U8(1); w.U8(1) },                          // a peer missing
		func(w *wire.Writer) { w.U64(1); w.U8(2); w.U8(1); w.U8(0); w.U8(0) },                 // a byte too many
	} {
		cl := s.login(t, 0x0110000100000F00+uint64(i), "BAD")
		cl.send(wire.MatchEvent, bad)
		for {
			typ, r := cl.next()
			if typ == wire.Error {
				if code := binary.LittleEndian.Uint32(r.B); code != wire.ErrMalformed {
					t.Fatalf("case %d: error %d", i, code)
				}
				break
			}
		}
	}
	// a server that records no matches never asked for them: the frame closes the connection
	plain := start(t, trusting(), nil)
	flags, cl := welcomeFlags(t, plain, peerC)
	if flags&8 != 0 {
		t.Fatal("a server without a database records no matches")
	}
	cl.send(wire.MatchEvent, func(w *wire.Writer) { w.U64(0x8000000000DD0030); w.U8(matchStarted); w.U8(1); w.U8(0) })
	if !closedSoon(cl.c, 3*time.Second) {
		t.Fatal("MATCH_EVENT to a server without match history must close the connection")
	}
}

// matchOf is the recorded match of a session (the newest).
func matchOf(t *testing.T, s *server, db *store.Store, session uint64) store.Match {
	t.Helper()
	for _, m := range history(t, s, db, "") {
		if m.Session == session {
			return m
		}
	}
	t.Fatalf("no match of %016X", session)
	return store.Match{}
}

// rankedSide is one game of a ranked match.
type rankedSide struct {
	cl         *client
	peer, xuid uint64
}

// report: this game's results about both players (a's and b's).
func (g rankedSide) report(session uint64, a, b rankedSide, ra, rb []stats.Write) {
	g.cl.writeStats(session, g.xuid, a.xuid, ra)
	g.cl.writeStats(session, g.xuid, b.xuid, rb)
}

func TestRankedResultsInHistory(t *testing.T) {
	withMatchTimes(t, 20*time.Second, 0)
	s, db := startHistory(t)
	// each room its own two players (the event and match limits are per player)
	room := func(id uint64, events bool) (rankedSide, rankedSide) {
		n := 2 * (id & 0xFF)
		a := rankedSide{s.login(t, 0x0110000100001000+n, "HOST A"), 0x0110000100001000 + n, 0x0009000000001000 + n}
		b := rankedSide{s.login(t, 0x0110000100001001+n, "GUEST B"), 0x0110000100001001 + n, 0x0009000000001001 + n}
		a.cl.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(rankedRecord(id, a.peer)) })
		a.cl.sync()
		a.cl.register(id, a.xuid, b.peer)
		b.cl.register(id, b.xuid, a.peer)
		if events {
			a.cl.matchEvent(id, matchStarted, 1, b.peer)
			b.cl.matchEvent(id, matchStarted, 1, a.peer)
		}
		return a, b
	}
	// both games agree: the match ended, then its results settled
	const agreed = 0x8000000000DE0001
	a, b := room(agreed, true)
	a.cl.matchEvent(agreed, matchEnded, 1)
	a.report(agreed, a, b, result(600, 1, 0), result(400, 0, 1))
	b.report(agreed, a, b, result(600, 1, 0), result(400, 0, 1))
	if m := matchOf(t, s, db, agreed); m.Kind != store.KindRanked || m.Result != "agreed" || m.Ended.IsZero() || len(m.Players) != 2 ||
		m.Players[0].Won != -1 || m.Players[1].Won != -1 {
		t.Fatalf("%+v", m)
	}
	// they disagree about A (and agree about B): the match is disagreed; it ends after its results settled
	const disagreed = 0x8000000000DE0002
	a, b = room(disagreed, true)
	a.report(disagreed, a, b, result(5000, 2, 0), result(300, 0, 2))
	b.report(disagreed, a, b, result(650, 2, 0), result(300, 0, 2))
	b.cl.matchEvent(disagreed, matchEnded, 1)
	if m := matchOf(t, s, db, disagreed); m.Result != "disagreed" || m.Ended.IsZero() {
		t.Fatalf("%+v", m)
	}
	// games that send no MATCH_EVENT: the match is recorded from its ranked results
	const silent = 0x8000000000DE0003
	a, b = room(silent, false)
	a.report(silent, a, b, result(700, 2, 0), result(200, 0, 3))
	b.report(silent, a, b, result(700, 2, 0), result(200, 0, 3))
	if m := matchOf(t, s, db, silent); m.Result != "agreed" || m.Kind != store.KindRanked || m.Ended.IsZero() || len(m.Players) != 2 ||
		m.Players[0].SteamID != a.peer || m.Players[0].Name != "HOST A" || m.Players[1].Name != "GUEST B" {
		t.Fatalf("%+v", m)
	}
	// only A's game reports: unconfirmed once the other game's time is up (the match is still open)
	const lone = 0x8000000000DE0004
	a, b = room(lone, true)
	a.report(lone, a, b, result(800, 3, 0), result(100, 0, 4))
	later := time.Now().Add(rankedSettle + time.Second)
	s.h.apply(s.h.sweepRanked(later), later)
	if m := matchOf(t, s, db, lone); m.Result != "unconfirmed" || !m.Ended.IsZero() {
		t.Fatalf("%+v", m)
	}
	// neither game reports: unreported
	const mute = 0x8000000000DE0005
	room(mute, true)
	later = time.Now().Add(rankedSilent + time.Second)
	s.h.apply(s.h.sweepRanked(later), later)
	if m := matchOf(t, s, db, mute); m.Result != "unreported" || len(m.Players) != 2 {
		t.Fatalf("%+v", m)
	}
}
