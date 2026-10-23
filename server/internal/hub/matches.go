package hub

import (
	"errors"
	"sync"
	"sync/atomic"
	"time"

	"ttt2-server/internal/store"
	"ttt2-server/internal/wire"
)

// MatchStore keeps the match history (internal/store). Called on the hub's match writer goroutine only, in the order
// the events came - never on a player's goroutine or under a lock, so a slow database delays the history, not the
// players.
type MatchStore interface {
	OpenMatch(session uint64, kind string, at time.Time) (int64, error)
	AddMatchPlayer(match int64, steamID uint64, name string, localPlayers int, own bool) error
	EndMatch(match int64, at time.Time, result string) error
	SettleRanked(r store.RankedResult) error
	PruneMatches(before time.Time) (int64, error)
}

// The match history (PROTOCOL.md "Match history"). Each game says when a match starts (MATCH_EVENT 1: the room's
// session, the players at its PC, the Steam IDs of the other PCs) and when it ends (2). A match is its session's while
// it is open: the first STARTED opens it, the other games' join it, an END from any of its players closes it. The
// server sees no match traffic, so a match is what the games say: a modified game can put itself (or the players it
// names) in a room's match, never change anyone's ranked results.
var (
	matchRematch   = 20 * time.Second // a STARTED from a player of the session's match this long after it opened: a new match began without an END
	matchShortest  = 10 * time.Second // an END for a match this young is the previous match's, arriving late
	matchLongest   = time.Hour        // a match still open this long is closed ("unfinished")
	matchAbandoned = time.Minute      // ... as is one none of whose players has been online in it this long
	matchSweepTime = 30 * time.Second
	matchPruneTime = 24 * time.Hour // matches older than keep_matches_days are forgotten this often
)

const (
	matchStarted  = 1
	matchEnded    = 2
	maxMatchPeers = 7
	matchQueue    = 4096 // database writes waiting for the writer; beyond that they are dropped (and counted)
)

// matchState: the open matches. h.mu, when it is needed too, is taken first.
type matchState struct {
	mu        sync.Mutex
	bySession map[uint64]*openMatch
	current   map[uint64]*openMatch       // Steam ID -> the open match it joined last
	queue     chan func(MatchStore) error // the writer's work, queued under mu (the events' order)
	dropped   atomic.Int64                // writes dropped on a full queue (logged by the sweeper)
}

type openMatch struct {
	row     *matchRow
	session uint64
	ranked  bool
	started time.Time
	seen    time.Time      // the last sweep that found one of its players online in it
	players map[uint64]int // Steam ID -> the players at that PC its own game reported (0: only named by another game)
}

// matchRow is a match's database row: its id is set (by the insert) and read on the writer goroutine only.
type matchRow struct{ id int64 }

type namedPeer struct {
	id   uint64
	name string
}

type matchEventFrame struct {
	session uint64
	event   uint8
	local   int      // players at the sender's PC (1..4; 2 for pair play)
	peers   []uint64 // the other PCs in the match
}

// SetMatches sets where the match history is kept (before Serve); nil: matches are not recorded (WELCOME says so, and
// a MATCH_EVENT closes the connection).
func (h *Hub) SetMatches(db MatchStore) {
	h.matchDB = db
	h.matches.bySession = map[uint64]*openMatch{}
	h.matches.current = map[uint64]*openMatch{}
	h.matches.queue = make(chan func(MatchStore) error, matchQueue)
}

// readMatchEvent reads a MATCH_EVENT: u64 session, u8 event, u8 local players, u8 peer count + u64 peers.
func readMatchEvent(r *wire.Reader) (matchEventFrame, error) {
	ev := matchEventFrame{session: r.U64(), event: r.U8(), local: int(r.U8())}
	n := int(r.U8())
	if r.Err() != nil || n > maxMatchPeers {
		return ev, errors.New("bad MATCH_EVENT")
	}
	for i := 0; i < n; i++ {
		ev.peers = append(ev.peers, r.U64())
	}
	if r.Err() != nil || len(r.B) != 0 || ev.session == 0 || (ev.event != matchStarted && ev.event != matchEnded) ||
		ev.local < 1 || ev.local > 4 {
		return ev, errors.New("bad MATCH_EVENT")
	}
	return ev, nil
}

// matchEvent: MATCH_EVENT. What the server knows of the match is read under the hub's lock (the room's kind, which
// named peers are players here, their names), then the match lock decides.
func (h *Hub) matchEvent(p *Player, ev matchEventFrame, now time.Time) {
	if ev.event == matchEnded {
		h.endMatch(p, ev.session, now)
		return
	}
	h.mu.Lock()
	s := h.sessions[ev.session]
	ranked := s != nil && s.ranked
	var peers []namedPeer
	for _, id := range ev.peers {
		if q := h.players[id]; q != nil && q != p && q.Proto == p.Proto {
			peers = append(peers, namedPeer{id, q.Name})
		}
	}
	most := 16 * h.cfg.Load().MaxPlayers
	h.mu.Unlock()
	h.startMatch(p, ev, ranked, peers, most, now)
}

// startMatch: STARTED - the sender, and the players it names, join the session's open match, or a new one opens.
func (h *Hub) startMatch(p *Player, ev matchEventFrame, ranked bool, peers []namedPeer, most int, now time.Time) {
	ms := &h.matches
	ms.mu.Lock()
	defer ms.mu.Unlock()
	m := ms.bySession[ev.session]
	if m != nil && m.players[p.Peer] > 0 && now.Sub(m.started) >= matchRematch {
		h.closeMatchLocked(m, now, "unfinished") // it started again without an END: the next match in the room
		m = nil
	}
	if m == nil {
		// a player opens at most 3 matches at once, then one every 20 s (a real match lasts longer): the history of a
		// game that floods it stays small
		if len(ms.bySession) >= most || !p.matchOpens.take(1.0/20, 3, now) {
			h.logPlayer(p, "match %016X of %q not recorded: too many", ev.session, p.Name)
			return
		}
		m = h.openMatchLocked(ev.session, ranked, now)
	}
	h.joinMatchLocked(m, p.Peer, p.Name, ev.local, true)
	for _, q := range peers {
		h.joinMatchLocked(m, q.id, q.name, 1, false)
	}
}

// openMatchLocked opens a session's match (h.matches.mu held).
func (h *Hub) openMatchLocked(session uint64, ranked bool, now time.Time) *openMatch {
	m := &openMatch{row: &matchRow{}, session: session, ranked: ranked, started: now, seen: now, players: map[uint64]int{}}
	h.matches.bySession[session] = m
	kind := store.KindPlayer
	if ranked {
		kind = store.KindRanked
	}
	row := m.row
	h.queueMatchWrite(func(db MatchStore) error {
		id, err := db.OpenMatch(session, kind, now)
		row.id = id
		return err
	})
	return m
}

// joinMatchLocked puts a player in a match - own: its own game reported it, with its local players; else another game
// named it - and makes it the player's newest match (h.matches.mu held).
func (h *Hub) joinMatchLocked(m *openMatch, id uint64, name string, local int, own bool) {
	h.matches.current[id] = m
	have, in := m.players[id]
	if in && (!own || have == local) {
		return // known already
	}
	m.players[id] = 0
	if own {
		m.players[id] = local
	}
	row := m.row
	h.queueMatchWrite(func(db MatchStore) error {
		if row.id == 0 {
			return nil // its insert failed (logged)
		}
		return db.AddMatchPlayer(row.id, id, name, local, own)
	})
}

// closeMatchLocked ends an open match (h.matches.mu held): its row gets `result` unless it has one ("" for a ranked
// match: its ranked results give it one).
func (h *Hub) closeMatchLocked(m *openMatch, at time.Time, result string) {
	ms := &h.matches
	if ms.bySession[m.session] == m {
		delete(ms.bySession, m.session)
	}
	for id := range m.players {
		if ms.current[id] == m {
			delete(ms.current, id)
		}
	}
	row := m.row
	h.queueMatchWrite(func(db MatchStore) error {
		if row.id == 0 {
			return nil
		}
		return db.EndMatch(row.id, at, result)
	})
}

// endMatch: ENDED - any of the match's players closes it.
func (h *Hub) endMatch(p *Player, session uint64, now time.Time) {
	ms := &h.matches
	ms.mu.Lock()
	defer ms.mu.Unlock()
	m := ms.bySession[session]
	if m == nil || now.Sub(m.started) < matchShortest {
		return
	}
	if _, in := m.players[p.Peer]; !in {
		return
	}
	result := "ended"
	if m.ranked {
		result = ""
	}
	h.closeMatchLocked(m, now, result)
}

// playing counts the online players whose newest match is open (the caller holds h.mu).
func (ms *matchState) playing(online func(uint64) bool) int {
	ms.mu.Lock()
	defer ms.mu.Unlock()
	n := 0
	for id := range ms.current {
		if online(id) {
			n++
		}
	}
	return n
}

// sweepMatches closes the matches open longer than matchLongest, and those none of whose players has been online in
// them for matchAbandoned (ended when one last was). Every matchSweepTime, from Serve; under h.mu, then the match lock.
func (h *Hub) sweepMatches(now time.Time) {
	h.mu.Lock()
	defer h.mu.Unlock()
	ms := &h.matches
	ms.mu.Lock()
	defer ms.mu.Unlock()
	for _, m := range ms.bySession {
		for id := range m.players {
			if ms.current[id] == m && h.players[id] != nil {
				m.seen = now
				break
			}
		}
		switch {
		case now.Sub(m.started) >= matchLongest:
			h.closeMatchLocked(m, now, "unfinished")
		case now.Sub(m.seen) >= matchAbandoned:
			h.closeMatchLocked(m, m.seen, "unfinished")
		}
	}
}

// recordRanked gives a settled ranked match its result in the history (from apply, outside every lock).
func (h *Hub) recordRanked(o rankedOutcome) {
	if h.matchDB == nil {
		return
	}
	r := store.RankedResult{Session: o.session, Players: [2]uint64{o.host, o.guest}, Registered: o.registered,
		Settled: o.settled, Result: o.result}
	h.mu.Lock()
	for i, id := range r.Players {
		if q := h.players[id]; q != nil {
			r.Names[i] = q.Name
		}
	}
	h.mu.Unlock()
	h.matches.mu.Lock()
	h.queueMatchWrite(func(db MatchStore) error { return db.SettleRanked(r) })
	h.matches.mu.Unlock()
}

// queueMatchWrite hands the writer a database write (h.matches.mu held, so the writes keep the events' order); on a
// full queue it is dropped and counted.
func (h *Hub) queueMatchWrite(w func(MatchStore) error) {
	select {
	case h.matches.queue <- w:
	default:
		h.matches.dropped.Add(1)
	}
}

// matchWriter does the match history's database writes, in order (from Serve).
func (h *Hub) matchWriter(stop <-chan struct{}) {
	var logs bucket
	for {
		select {
		case <-stop:
			return
		case w := <-h.matches.queue:
			if err := w(h.matchDB); err != nil && logs.take(5.0/60, 5, time.Now()) {
				h.log.Printf("database: match history: %v", err)
			}
		}
	}
}

// matchSweeper closes stale matches and, once a day, forgets those older than keep_matches_days (from Serve).
func (h *Hub) matchSweeper(stop <-chan struct{}) {
	t := time.NewTicker(matchSweepTime)
	defer t.Stop()
	var pruned time.Time
	for {
		select {
		case <-stop:
			return
		case now := <-t.C:
			h.sweepMatches(now)
			if n := h.matches.dropped.Swap(0); n > 0 {
				h.log.Printf("match history: %d database writes dropped (the database is too slow)", n)
			}
			if now.Sub(pruned) >= matchPruneTime {
				pruned = now
				h.pruneMatches(now)
			}
		}
	}
}

// pruneMatches forgets the matches older than keep_matches_days (0: they are kept for ever).
func (h *Hub) pruneMatches(now time.Time) {
	days := h.Config().KeepMatchesDays
	if days <= 0 {
		return
	}
	before := now.AddDate(0, 0, -days)
	h.matches.mu.Lock()
	h.queueMatchWrite(func(db MatchStore) error {
		n, err := db.PruneMatches(before)
		if n > 0 {
			h.log.Printf("match history: %d matches older than %d days forgotten", n, days)
		}
		return err
	})
	h.matches.mu.Unlock()
}

// FlushMatches waits (at most a few seconds) until the match history's queued writes are done (a clean stop).
func (h *Hub) FlushMatches() {
	if h.matchDB == nil {
		return
	}
	done := make(chan struct{})
	select {
	case h.matches.queue <- func(MatchStore) error { close(done); return nil }:
	case <-time.After(5 * time.Second):
		return
	}
	select {
	case <-done:
	case <-time.After(5 * time.Second):
	}
}
