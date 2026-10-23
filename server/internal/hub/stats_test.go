package hub

import (
	"bytes"
	"path/filepath"
	"testing"
	"time"

	"ttt2-server/internal/record"
	"ttt2-server/internal/stats"
	"ttt2-server/internal/store"
	"ttt2-server/internal/wire"
)

const (
	peerA, peerB, peerC  = 0x0110000100000A01, 0x0110000100000B02, 0x0110000100000C03
	xuidA, xuidB, xuidC  = 0x0009000000000A01, 0x0009000000000B02, 0x0009000000000C03
	bpView, survivalView = 9, 230
)

func startStats(t *testing.T) (*server, *store.Store) {
	t.Helper()
	db, err := store.Open(filepath.Join(t.TempDir(), "server.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	store.SetBoardFreshness(0) // the tests read a board right after writing it
	s := startSetup(t, trusting(), nil, func(h *Hub) {
		h.SetRecords(db)
		h.SetStats(db)
	})
	return s, db
}

// a ranked session record (XSESSION_CREATE flags of the game's ranked match, MEASURED: 0x73F)
func rankedRecord(id, host uint64) []byte {
	rec := makeRecord(id, host)
	rec[4+8+8+8] = 0x3F
	rec[4+8+8+8+1] = 0x07
	return rec
}

func result(rating int64, wins, losses int32) []stats.Write {
	return []stats.Write{{View: bpView, Props: []stats.Property{
		{ID: 0x20000001, Value: stats.Value{Type: stats.TypeInt64, Bits: uint64(rating)}},
		{ID: 0x10000010, Value: stats.Value{Type: stats.TypeInt32, Bits: uint64(int64(wins))}},
		{ID: 0x10000011, Value: stats.Value{Type: stats.TypeInt32, Bits: uint64(int64(losses))}},
	}}}
}

func (cl *client) writeStats(session, own, subject uint64, writes []stats.Write) {
	cl.send(wire.StatsWrite, func(w *wire.Writer) {
		w.U64(session)
		w.U64(own)
		w.U64(subject)
		w.U8(uint8(len(writes)))
		for _, v := range writes {
			w.U32(v.View)
			w.U8(uint8(len(v.Props)))
			for _, p := range v.Props {
				w.U32(p.ID)
				w.U8(p.Value.Type)
				w.U64(p.Value.Bits)
			}
		}
	})
	cl.sync()
}

func (cl *client) register(session, own, opponent uint64) {
	cl.send(wire.ArbRegister, func(w *wire.Writer) { w.U64(session); w.U64(own); w.U64(opponent) })
	cl.sync()
}

type statRow struct {
	xuid   uint64
	rank   uint32
	rating int64
	name   string
	cols   map[uint16]stats.Value
}

func readResult(t *testing.T, cl *client, request uint32) (bool, uint32, []statRow) {
	t.Helper()
	typ, r := cl.next()
	if typ != wire.StatsResult || r.U32() != request {
		t.Fatalf("STATS_RESULT expected, got %d", typ)
	}
	ok := r.U8() == 1
	total := r.U32()
	var rows []statRow
	for n := r.U16(); n > 0; n-- {
		row := statRow{xuid: r.U64(), rank: r.U32(), rating: int64(r.U64()), name: r.Str(), cols: map[uint16]stats.Value{}}
		for c := r.U8(); c > 0; c-- {
			id := r.U16()
			row.cols[id] = stats.Value{Type: r.U8(), Bits: r.U64()}
		}
		rows = append(rows, row)
	}
	if r.Err() != nil || len(r.B) != 0 {
		t.Fatal("malformed STATS_RESULT")
	}
	return ok, total, rows
}

func (cl *client) board(view, start, count uint32, pivot uint64) (bool, uint32, []statRow) {
	cl.send(wire.StatsBoard, func(w *wire.Writer) {
		w.U32(21)
		w.U32(view)
		w.U32(start)
		w.U32(count)
		w.U64(pivot)
		w.U8(3)
		w.U16(5)
		w.U16(6)
		w.U16(1)
	})
	return readResult(cl.t, cl, 21)
}

func (cl *client) players(view uint32, xuids ...uint64) (bool, uint32, []statRow) {
	cl.send(wire.StatsPlayers, func(w *wire.Writer) {
		w.U32(22)
		w.U32(view)
		w.U8(2)
		w.U16(5)
		w.U16(6)
		w.U8(uint8(len(xuids)))
		for _, x := range xuids {
			w.U64(x)
		}
	})
	return readResult(cl.t, cl, 22)
}

func TestStatisticsOffered(t *testing.T) {
	s, _ := startStats(t)
	if flags, _ := welcomeFlags(t, s, peerA); flags&4 == 0 {
		t.Fatal("WELCOME must offer statistics")
	}
	plain := start(t, trusting(), nil)
	flags, cl := welcomeFlags(t, plain, peerB)
	if flags&4 != 0 {
		t.Fatal("a server without a database offers no statistics")
	}
	cl.send(wire.StatsBoard, func(w *wire.Writer) {
		w.U32(1)
		w.U32(9)
		w.U32(1)
		w.U32(10)
		w.U64(0)
		w.U8(0)
	})
	if !closedSoon(cl.c, 3*time.Second) {
		t.Fatal("statistics frames to a server without them must close the connection")
	}
}

func TestRankedResultsCountWhenBothAgree(t *testing.T) {
	s, _ := startStats(t)
	a, b, c := s.login(t, peerA, "HOST A"), s.login(t, peerB, "GUEST B"), s.login(t, peerC, "THIRD C")
	const match = 0x8000000000AB0001
	a.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(rankedRecord(match, peerA)) })
	a.sync()
	// a third player registers first, naming the host: the host never names it, so it is never the opponent
	c.register(match, xuidC, peerA)
	a.register(match, xuidA, peerB)
	b.register(match, xuidB, peerA)

	// each game writes both players' results; the third player's report is dropped
	a.writeStats(match, xuidA, xuidA, result(600, 1, 0))
	c.writeStats(match, xuidC, xuidA, result(99999, 50, 0))
	if _, total, _ := a.board(bpView, 1, 10, 0); total != 0 {
		t.Fatal("one game's report must not count yet")
	}
	a.writeStats(match, xuidA, xuidB, result(400, 0, 1))
	b.writeStats(match, xuidB, xuidB, result(400, 0, 1))
	b.writeStats(match, xuidB, xuidA, result(600, 1, 0))
	ok, total, rows := b.board(bpView, 1, 10, 0)
	if !ok || total != 2 || len(rows) != 2 || rows[0].xuid != xuidA || rows[0].rating != 600 || rows[0].name != "HOST A" ||
		rows[0].rank != 1 || rows[1].xuid != xuidB || rows[1].rank != 2 || rows[1].cols[6].Bits != 1 || rows[1].cols[1].Type != stats.TypeNull {
		t.Fatalf("%v %d %+v", ok, total, rows)
	}
	// a resend after it counted changes nothing
	a.writeStats(match, xuidA, xuidA, result(700, 2, 0))
	if _, _, rows := a.board(bpView, 1, 1, 0); rows[0].rating != 600 {
		t.Fatal("a settled result changed")
	}

	// the next match: the games disagree about A -> discarded
	const second = 0x8000000000AB0002
	a.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(rankedRecord(second, peerA)) })
	a.sync()
	a.register(second, xuidA, peerB)
	b.register(second, xuidB, peerA)
	a.writeStats(second, xuidA, xuidA, result(5000, 2, 0))
	b.writeStats(second, xuidB, xuidA, result(650, 2, 0))
	if _, _, rows := a.players(bpView, xuidA); len(rows) != 1 || rows[0].rating != 600 {
		t.Fatalf("disagreeing reports must not count: %+v", rows)
	}

	// results for a session the server never saw
	b.writeStats(0x8000000000FFFFFF, xuidB, xuidB, result(9000, 9, 0))
	if _, _, rows := a.players(bpView, xuidB); rows[0].rating != 400 {
		t.Fatal("results for an unknown session counted")
	}
	// a connection's XUID is fixed: another one in a write is refused
	b.writeStats(second, 0x0009000000000EEE, xuidB, result(9000, 9, 0))
	if _, _, rows := a.players(bpView, xuidB); rows[0].rating != 400 {
		t.Fatal("a write with another XUID counted")
	}
}

func TestRankedNeedsMutualNaming(t *testing.T) {
	s, _ := startStats(t)
	a, b := s.login(t, peerA, "HOST A"), s.login(t, peerB, "GUEST B")
	const match = 0x8000000000AB0009
	a.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(rankedRecord(match, peerA)) })
	a.sync()
	// the host's game names someone else (it is connected to another opponent): B is not the match
	a.register(match, xuidA, peerC)
	b.register(match, xuidB, peerA)
	a.writeStats(match, xuidA, xuidB, result(1, 0, 1))
	b.writeStats(match, xuidB, xuidB, result(1, 0, 1))
	if _, total, _ := a.board(bpView, 1, 10, 0); total != 0 {
		t.Fatal("results of an unpaired match counted")
	}
	// a session that is not ranked takes no registrations
	const plain = 0x8000000000AB000A
	a.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(makeRecord(plain, peerA)) })
	a.sync()
	a.register(plain, xuidA, peerB)
	b.register(plain, xuidB, peerA)
	a.writeStats(plain, xuidA, xuidB, result(1, 0, 1))
	b.writeStats(plain, xuidB, xuidB, result(1, 0, 1))
	if _, total, _ := a.board(bpView, 1, 10, 0); total != 0 {
		t.Fatal("results of a Player Match counted")
	}
}

func TestRankedOpponentLeft(t *testing.T) {
	s, _ := startStats(t)
	a, b := s.login(t, peerA, "HOST A"), s.login(t, peerB, "GUEST B")
	const match = 0x8000000000AB0003
	a.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(rankedRecord(match, peerA)) })
	a.sync()
	a.register(match, xuidA, peerB)
	b.register(match, xuidB, peerA)
	// B's game left before writing: A reports both - one game alone never writes a result, not even about a leaver
	a.writeStats(match, xuidA, xuidA, result(800, 1, 0))
	a.writeStats(match, xuidA, xuidB, result(300, 0, 1))
	// and a report about someone not in the match
	a.writeStats(match, xuidA, xuidC, result(1, 0, 9))
	now := time.Now()
	later := now.Add(rankedSettle + time.Second)
	s.h.apply(s.h.sweepRanked(later), later)
	if _, total, _ := a.board(bpView, 1, 10, 0); total != 0 {
		t.Fatal("unconfirmed reports must not count")
	}
	s.h.ranked.mu.Lock()
	open := len(s.h.ranked.open)
	m := s.h.ranked.matches[match]
	s.h.ranked.mu.Unlock()
	if open != 0 || m == nil || len(m.reports) != 0 {
		t.Fatalf("a settled match frees its reports and its players' open counts: open %d", open)
	}
	// the match is forgotten after a while
	s.h.sweepRanked(later.Add(rankedKeep + time.Second))
	s.h.ranked.mu.Lock()
	left := len(s.h.ranked.matches)
	s.h.ranked.mu.Unlock()
	if left != 0 {
		t.Fatal("old ranked matches must be forgotten")
	}
}

func TestRankedStateIsBounded(t *testing.T) {
	s, _ := startStats(t)
	a := s.login(t, peerA, "HOST A")
	bot := s.login(t, peerC, "BOT")
	// the bot claims a place in many ranked rooms: at most maxOpenPerPlayer of its claims are kept
	for i := 0; i < 4; i++ {
		id := 0x8000000000AC0000 + uint64(i)
		a.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(rankedRecord(id, peerA)) })
	}
	a.sync()
	for round := 0; round < 3; round++ {
		for i := 0; i < 4; i++ {
			bot.send(wire.ArbRegister, func(w *wire.Writer) { w.U64(0x8000000000AC0000 + uint64(i)); w.U64(xuidC); w.U64(peerA) })
		}
		bot.sync()
	}
	s.h.ranked.mu.Lock()
	open := s.h.ranked.open[peerC]
	s.h.ranked.mu.Unlock()
	if open > maxOpenPerPlayer {
		t.Fatalf("%d open claims", open)
	}
	// unpaired claims are forgotten
	s.h.sweepRanked(time.Now().Add(rankedUnpaired + time.Second))
	s.h.ranked.mu.Lock()
	left, still := len(s.h.ranked.matches), s.h.ranked.open[peerC]
	s.h.ranked.mu.Unlock()
	if left != 0 || still != 0 {
		t.Fatalf("%d matches, %d open claims left", left, still)
	}
}

func TestOwnStatisticsAndReads(t *testing.T) {
	s, _ := startStats(t)
	a, b := s.login(t, peerA, "A"), s.login(t, peerB, "B")
	survival := func(rating int64) []stats.Write {
		return []stats.Write{{View: survivalView, Props: []stats.Property{{ID: 0x20000001, Value: stats.Value{Type: stats.TypeInt64, Bits: uint64(rating)}}}}}
	}
	a.writeStats(0, xuidA, xuidA, survival(12))
	b.writeStats(0, xuidB, xuidB, survival(30))
	b.writeStats(0, xuidB, xuidA, survival(9999))              // about another player: dropped
	b.writeStats(0, xuidB, xuidB, []stats.Write{{View: 4242}}) // no such view: dropped
	b.writeStats(0, xuidB, xuidB, []stats.Write{{View: survivalView, Props: []stats.Property{{ID: 0x20000001, Value: stats.Value{Type: 1, Bits: 5}}}}})
	_, total, rows := a.board(survivalView, 1, 10, 0)
	if total != 2 || rows[0].xuid != xuidB || rows[0].rating != 30 || rows[1].rating != 12 {
		t.Fatalf("%d %+v", total, rows)
	}
	// those players' rows; an unknown XUID is left out
	_, _, mine := a.players(survivalView, xuidA, 0x0009FFFFFFFFFFFF)
	if len(mine) != 1 || mine[0].rank != 2 || mine[0].xuid != xuidA {
		t.Fatalf("%+v", mine)
	}
	// the page around a player
	_, _, around := b.board(survivalView, 1, 1, xuidA)
	if len(around) != 1 || around[0].xuid != xuidA {
		t.Fatalf("%+v", around)
	}
	// an unknown view answers "no"
	if ok, _, _ := a.board(4242, 1, 10, 0); ok {
		t.Fatal("unknown view")
	}
	// the same XUID many times is one row
	if _, _, many := a.players(survivalView, xuidA, xuidA, xuidA, xuidB); len(many) != 2 {
		t.Fatalf("%d rows", len(many))
	}
	// a malformed frame closes the connection
	a.send(wire.StatsPlayers, func(w *wire.Writer) { w.U32(1); w.U32(9); w.U8(0); w.U8(200) })
	if !closedSoon(a.c, 3*time.Second) {
		t.Fatal("malformed STATS_PLAYERS must close the connection")
	}
	_ = record.Arbitrated
}

// FuzzReadWrites: any STATS_WRITE body - either refused, or every view and value valid, and its canonical form reads
// back to the same writes.
func FuzzReadWrites(f *testing.F) {
	f.Add([]byte{1, 230, 0, 0, 0, 1, 1, 0, 0, 0x20, 2, 5, 0, 0, 0, 0, 0, 0, 0})
	f.Add([]byte{2, 9, 0, 0, 0, 0, 9, 0, 0, 0, 0})
	f.Fuzz(func(t *testing.T, b []byte) {
		r := wire.Reader{B: b}
		writes, err := readWrites(&r)
		if err != nil {
			return
		}
		for _, w := range writes {
			v, ok := stats.Lookup(w.View)
			if !ok {
				t.Fatalf("view %d accepted", w.View)
			}
			if _, err := v.Apply(nil, w.Props); err != nil {
				t.Fatalf("accepted values refused: %v", err)
			}
		}
		c := canonical(writes)
		if !bytes.Equal(canonical(decanonical(c)), c) {
			t.Fatal("canonical form does not read back")
		}
	})
}

func TestRankedRematchInTheSameRoom(t *testing.T) {
	s, _ := startStats(t)
	a, b, c := s.login(t, peerA, "HOST A"), s.login(t, peerB, "GUEST B"), s.login(t, peerC, "THIRD C")
	const room = 0x8000000000AB0010
	a.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(rankedRecord(room, peerA)) })
	a.sync()
	c.register(room, xuidC, peerA) // never named: given back once the pair forms
	a.register(room, xuidA, peerB)
	b.register(room, xuidB, peerA)
	s.h.ranked.mu.Lock()
	openC := s.h.ranked.open[peerC]
	s.h.ranked.mu.Unlock()
	if openC != 0 {
		t.Fatal("a registration nobody named must be given back when the match pairs")
	}
	play := func(ra, rb int64) {
		a.writeStats(room, xuidA, xuidA, result(ra, 1, 0))
		a.writeStats(room, xuidA, xuidB, result(rb, 0, 1))
		b.writeStats(room, xuidB, xuidB, result(rb, 0, 1))
		b.writeStats(room, xuidB, xuidA, result(ra, 1, 0))
	}
	play(600, 400)
	// the next match in the same room: both register again, a new round
	a.register(room, xuidA, peerB)
	b.register(room, xuidB, peerA)
	play(900, 300)
	_, _, rows := a.players(bpView, xuidA, xuidB)
	if len(rows) != 2 || rows[0].rating != 900 {
		t.Fatalf("the second match must count: %+v", rows)
	}
	s.h.ranked.mu.Lock()
	open := len(s.h.ranked.open)
	s.h.ranked.mu.Unlock()
	if open != 0 {
		t.Fatalf("%d players still hold registrations after two settled matches", open)
	}
}

func TestXUIDChangeOnlyWithoutOpenRegistrations(t *testing.T) {
	s, _ := startStats(t)
	a, b := s.login(t, peerA, "HOST A"), s.login(t, peerB, "GUEST B")
	survival := []stats.Write{{View: survivalView, Props: []stats.Property{{ID: 0x20000001, Value: stats.Value{Type: stats.TypeInt64, Bits: 5}}}}}
	a.writeStats(0, xuidA, xuidA, survival)
	// another profile, nothing open: accepted
	const other = 0x0009000000000A99
	a.writeStats(0, other, other, survival)
	if _, _, rows := b.players(survivalView, other); len(rows) != 1 {
		t.Fatal("a profile switch with nothing open must be accepted")
	}
	// with a ranked registration open, the XUID is fixed
	const room = 0x8000000000AB0011
	a.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(rankedRecord(room, peerA)) })
	a.sync()
	a.register(room, other, peerB)
	a.writeStats(0, xuidA, xuidA, []stats.Write{{View: survivalView, Props: []stats.Property{{ID: 0x20000001, Value: stats.Value{Type: stats.TypeInt64, Bits: 50}}}}})
	if _, _, rows := b.players(survivalView, other); len(rows) != 1 || rows[0].rating != 5 {
		t.Fatalf("an XUID change while registered must be refused: %+v", rows)
	}
}
