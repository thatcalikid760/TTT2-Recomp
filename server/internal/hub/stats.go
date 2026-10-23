package hub

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"sort"
	"sync"
	"time"

	"ttt2-server/internal/stats"
	"ttt2-server/internal/wire"
)

// StatsStore keeps the leaderboards (internal/store). Called on the players' connection goroutines and the
// ranked-results sweeper, never under the hub's lock.
type StatsStore interface {
	WriteStats(steamID uint64, at time.Time, writes []stats.Write) error
	Board(view uint32, at time.Time, start, count int) (int64, []stats.Entry, error)
	Entries(view uint32, at time.Time, steamIDs []uint64) (int64, []stats.Entry, error)
	SteamIDsOf(xuids []uint64) (map[uint64]uint64, error)
	SetXUID(steamID, xuid uint64) error
	// NamesOf: the names of the players whose games use these online XUIDs (XUID_NAMES)
	NamesOf(xuids []uint64) (map[uint64]string, error)
}

// Ranked results (PROTOCOL.md "Statistics"). When a ranked match starts each game registers (ARB_REGISTER: the
// session, its own XUID and the Steam ID of the opponent its game is connected to). The host of a live ranked session
// and the player it names, who names the host back, are the match; nobody else's claims or reports count. Each game
// then writes both players' results, and a player's results count only when both games sent the same: a
// disagreement, or a report the other game never confirmed, discards them. A modified game can veto a match, never
// write one. A room can hold one match after another (the host takes the next challenger): a registration in a
// settled match starts the next one.
var (
	rankedSettle   = 2 * time.Minute  // after the first report, the other game's has this long
	rankedUnpaired = 10 * time.Minute // registrations that never became a pair are given back
	rankedSilent   = 30 * time.Minute // a pair that never reported is given back
	rankedKeep     = 10 * time.Minute // a settled match is remembered (resends change nothing) this long
)

const (
	maxOpenPerPlayer = 4  // ranked registrations a player has open at once
	maxStatsPlayers  = 16 // XUIDs in one STATS_PLAYERS
	statReadSlots    = 8  // database reads for statistics at once, whole server
)

type claim struct{ xuid, opponent uint64 }

type rankedMatch struct {
	session uint64
	host    uint64
	created time.Time
	claims  map[uint64]claim             // registrations still counted in ranked.open (Steam ID -> claim)
	paired  uint64                       // the host's opponent once both named each other (0: not yet)
	xuids   map[uint64]uint64            // the pair: Steam ID -> XUID
	reports map[uint64]map[uint64][]byte // subject XUID -> reporter Steam ID -> canonical results
	first   time.Time                    // the first report
	done    map[uint64]bool              // subjects settled
	settled time.Time                    // both subjects settled
	result  string                       // the match history's: the worst of its subjects' so far (verdict)
}

// rankedOutcome: a settled ranked match, for the match history (recordRanked).
type rankedOutcome struct {
	session, host, guest uint64
	registered, settled  time.Time
	result               string // agreed, disagreed, unconfirmed (a report the other game never confirmed), unreported
}

// rankedVerdicts orders a match's results: one subject's disagreement makes the match "disagreed", else an
// unconfirmed report "unconfirmed", else "agreed".
var rankedVerdicts = map[string]int{"agreed": 1, "unconfirmed": 2, "disagreed": 3}

type rankedState struct {
	mu      sync.Mutex
	matches map[uint64]*rankedMatch // by session id
	open    map[uint64]int          // Steam ID -> its counted registrations
}

// SetStats sets where the leaderboards are kept (before Serve); nil: the server keeps no statistics.
func (h *Hub) SetStats(s StatsStore) {
	h.stats = s
	h.statReads = make(chan struct{}, statReadSlots)
}

// canonical is a write in one byte form (views by id, properties by id) - two games' reports compare by it
// (readWrites refuses repeated ids, so the order is total).
func canonical(writes []stats.Write) []byte {
	ws := append([]stats.Write(nil), writes...)
	sort.Slice(ws, func(i, j int) bool { return ws[i].View < ws[j].View })
	var b []byte
	for _, w := range ws {
		props := append([]stats.Property(nil), w.Props...)
		sort.Slice(props, func(i, j int) bool { return props[i].ID < props[j].ID })
		b = binary.LittleEndian.AppendUint32(b, w.View)
		b = binary.LittleEndian.AppendUint32(b, uint32(len(props)))
		for _, p := range props {
			b = binary.LittleEndian.AppendUint32(b, p.ID)
			b = append(b, p.Value.Type)
			b = binary.LittleEndian.AppendUint64(b, p.Value.Bits)
		}
	}
	return b
}

func decanonical(b []byte) []stats.Write {
	var out []stats.Write
	r := wire.Reader{B: b}
	for len(r.B) > 0 {
		w := stats.Write{View: r.U32()}
		n := r.U32()
		for i := uint32(0); i < n && r.Err() == nil; i++ {
			w.Props = append(w.Props, stats.Property{ID: r.U32(), Value: stats.Value{Type: r.U8(), Bits: r.U64()}})
		}
		if r.Err() != nil {
			return nil
		}
		out = append(out, w)
	}
	return out
}

// difference names the first view / property two reports differ in (for the log).
func difference(a, b []byte) string {
	values := func(c []byte) map[[2]uint32]stats.Value {
		m := map[[2]uint32]stats.Value{}
		for _, w := range decanonical(c) {
			m[[2]uint32{w.View, 0}] = stats.Value{}
			for _, p := range w.Props {
				m[[2]uint32{w.View, p.ID}] = p.Value
			}
		}
		return m
	}
	va, vb := values(a), values(b)
	keys := make([][2]uint32, 0, len(va)+len(vb))
	for k := range va {
		keys = append(keys, k)
	}
	for k := range vb {
		if _, ok := va[k]; !ok {
			keys = append(keys, k)
		}
	}
	sort.Slice(keys, func(i, j int) bool {
		return keys[i][0] < keys[j][0] || (keys[i][0] == keys[j][0] && keys[i][1] < keys[j][1])
	})
	for _, k := range keys {
		x, okx := va[k]
		y, oky := vb[k]
		if okx != oky || x != y {
			if k[1] == 0 {
				return fmt.Sprintf("view %d written by one game only", k[0])
			}
			return fmt.Sprintf("view %d property %08X: %d / %d", k[0], k[1], int64(x.Bits), int64(y.Bits))
		}
	}
	return "no difference"
}

type statsAction struct {
	steamID, xuid uint64
	writes        []stats.Write // nil: nothing to keep (`why` says why)
	why           string
	outcome       *rankedOutcome // set: not a player's results - the match is settled (its match history row)
}

// claimXUID fixes the online XUID a connection's game uses: the first one it reports. Another one (the player
// switched profiles) only while the player has no ranked registration open. The XUID is recorded for lookups; one a
// checked player already uses is refused. False: refused.
func (h *Hub) claimXUID(p *Player, own uint64) bool {
	if own == 0 {
		return false
	}
	if own == p.ownXUID {
		return true
	}
	if p.ownXUID != 0 {
		h.ranked.mu.Lock()
		busy := h.ranked.open[p.Peer] > 0
		h.ranked.mu.Unlock()
		if busy {
			return false
		}
	}
	if h.stats != nil {
		if err := h.stats.SetXUID(p.Peer, own); err != nil {
			h.logPlayer(p, "XUID %016X of %q refused: %v", own, p.Name, err)
			return false
		}
	}
	p.ownXUID = own
	return true
}

// logPlayer logs about a player's frames at most a few times a minute (a client cannot flood the log).
func (h *Hub) logPlayer(p *Player, format string, args ...any) {
	if p.logs.take(5.0/60, 5, time.Now()) {
		h.log.Printf(format, args...)
	}
}

// rankedRegister: ARB_REGISTER - a game starting a ranked match in `session` against `opponent`.
func (h *Hub) rankedRegister(p *Player, session, opponent uint64, now time.Time) error {
	h.mu.Lock()
	s := h.sessions[session]
	host := uint64(0)
	if s != nil && s.ranked {
		host = s.host.Peer
	}
	maxMatches := 16 * h.cfg.Load().MaxPlayers
	h.mu.Unlock()
	if host == 0 {
		return errors.New("not a live ranked session")
	}
	h.ranked.mu.Lock()
	defer h.ranked.mu.Unlock()
	if h.ranked.matches == nil {
		h.ranked.matches = map[uint64]*rankedMatch{}
		h.ranked.open = map[uint64]int{}
	}
	m := h.ranked.matches[session]
	if m != nil && (!m.settled.IsZero() || m.host != host) {
		m.release(h) // the next match in this room
		delete(h.ranked.matches, session)
		m = nil
	}
	c := claim{xuid: p.ownXUID, opponent: opponent}
	if m != nil {
		if old, ok := m.claims[p.Peer]; ok {
			if m.paired != 0 && old != c {
				return errors.New("a registration of a paired match changed")
			}
			m.claims[p.Peer] = c
			m.pair(h)
			return nil
		}
		if m.paired != 0 {
			return errors.New("the match is paired already")
		}
	}
	if h.ranked.open[p.Peer] >= maxOpenPerPlayer {
		return errors.New("too many open ranked registrations")
	}
	if m == nil {
		if len(h.ranked.matches) >= maxMatches {
			return errors.New("too many ranked matches")
		}
		m = &rankedMatch{session: session, host: host, created: now, claims: map[uint64]claim{}, reports: map[uint64]map[uint64][]byte{},
			done: map[uint64]bool{}}
		h.ranked.matches[session] = m
	}
	m.claims[p.Peer] = c
	h.ranked.open[p.Peer]++
	m.pair(h)
	return nil
}

// pair makes the match when the host names a registered player who names the host back (another XUID); the other
// registrations are given back. Under the ranked lock.
func (m *rankedMatch) pair(h *Hub) {
	if m.paired != 0 {
		return
	}
	hc, okh := m.claims[m.host]
	if !okh || hc.opponent == 0 || hc.opponent == m.host {
		return
	}
	gc, okg := m.claims[hc.opponent]
	if !okg || gc.opponent != m.host || gc.xuid == hc.xuid {
		return
	}
	m.paired = hc.opponent
	m.xuids = map[uint64]uint64{m.host: hc.xuid, m.paired: gc.xuid}
	for steam := range m.claims {
		if steam != m.host && steam != m.paired {
			m.give(h, steam)
		}
	}
}

// give hands one registration's count back.
func (m *rankedMatch) give(h *Hub, steam uint64) {
	if _, ok := m.claims[steam]; !ok {
		return
	}
	delete(m.claims, steam)
	if h.ranked.open[steam]--; h.ranked.open[steam] <= 0 {
		delete(h.ranked.open, steam)
	}
}

// release hands every registration's count back.
func (m *rankedMatch) release(h *Hub) {
	for steam := range m.claims {
		m.give(h, steam)
	}
}

// rankedReport files one game's ranked results about `subject` and returns what is settled by it.
func (h *Hub) rankedReport(p *Player, session, subject uint64, writes []stats.Write, now time.Time) ([]statsAction, error) {
	h.ranked.mu.Lock()
	defer h.ranked.mu.Unlock()
	m := h.ranked.matches[session]
	switch {
	case m == nil:
		return nil, errors.New("not a registered ranked match")
	case m.paired == 0 || (p.Peer != m.host && p.Peer != m.paired):
		return nil, errors.New("not a player of this ranked match")
	case m.xuids[p.Peer] != p.ownXUID:
		return nil, errors.New("not the XUID it registered with")
	case subject != m.xuids[m.host] && subject != m.xuids[m.paired]:
		return nil, errors.New("about a player not in the match")
	case m.done[subject]:
		return nil, nil // settled already (a resend)
	}
	if m.first.IsZero() {
		m.first = now
	}
	if m.reports[subject] == nil {
		m.reports[subject] = map[uint64][]byte{}
	}
	m.reports[subject][p.Peer] = canonical(writes)
	return m.settle(h, subject, now), nil
}

// ownerOf is the paired player whose game uses that XUID (0: none).
func (m *rankedMatch) ownerOf(xuid uint64) uint64 {
	for steam, x := range m.xuids {
		if x == xuid {
			return steam
		}
	}
	return 0
}

// settle decides a subject once both games reported it: the same -> counted, else discarded. Under the ranked lock.
func (m *rankedMatch) settle(h *Hub, subject uint64, now time.Time) []statsAction {
	reps := m.reports[subject]
	if len(reps) < 2 {
		return nil
	}
	a, b := reps[m.host], reps[m.paired]
	owner := m.ownerOf(subject)
	var out []statsAction
	if bytes.Equal(a, b) {
		m.verdict("agreed")
		out = append(out, statsAction{steamID: owner, xuid: subject, writes: decanonical(a), why: "both games agree"})
	} else {
		m.verdict("disagreed")
		out = append(out, statsAction{steamID: owner, xuid: subject, why: "the two games' ranked results disagree (" + difference(a, b) + "): discarded"})
	}
	if m.close(h, subject, now) {
		out = append(out, m.outcome(now))
	}
	return out
}

// close marks a subject settled and frees its reports; when both are, the match is settled and its registrations
// given back (true: this settled the match).
func (m *rankedMatch) close(h *Hub, subject uint64, now time.Time) bool {
	m.done[subject] = true
	delete(m.reports, subject)
	if m.done[m.xuids[m.host]] && m.done[m.xuids[m.paired]] && m.settled.IsZero() {
		m.settled = now
		m.release(h)
		return true
	}
	return false
}

// verdict folds a subject's settling into the match's result (rankedVerdicts).
func (m *rankedMatch) verdict(result string) {
	if rankedVerdicts[result] > rankedVerdicts[m.result] {
		m.result = result
	}
}

// outcome is the settled match for the match history.
func (m *rankedMatch) outcome(now time.Time) statsAction {
	return statsAction{outcome: &rankedOutcome{session: m.session, host: m.host, guest: m.paired, registered: m.created,
		settled: now, result: m.result}}
}

// sweepRanked discards reports the other game never confirmed and gives back or forgets old matches (every minute,
// from Serve).
func (h *Hub) sweepRanked(now time.Time) []statsAction {
	h.ranked.mu.Lock()
	defer h.ranked.mu.Unlock()
	var out []statsAction
	for id, m := range h.ranked.matches {
		switch {
		case !m.first.IsZero() && m.settled.IsZero() && now.Sub(m.first) >= rankedSettle:
			for subject, reps := range m.reports {
				for reporter := range reps {
					out = append(out, statsAction{steamID: m.ownerOf(subject), xuid: subject,
						why: fmt.Sprintf("only one game (%d) reported its ranked results: discarded", reporter)})
				}
				m.close(h, subject, now)
			}
			m.verdict("unconfirmed") // a report the other game never confirmed, or a subject nobody reported
			m.settled = now          // a subject nobody reported: the match is over all the same
			m.release(h)
			out = append(out, m.outcome(now))
		case m.paired == 0 && now.Sub(m.created) >= rankedUnpaired,
			m.paired != 0 && m.first.IsZero() && now.Sub(m.created) >= rankedSilent,
			!m.settled.IsZero() && now.Sub(m.settled) >= rankedKeep:
			if m.paired != 0 && m.settled.IsZero() {
				m.result = "unreported" // a pair whose games never reported
				out = append(out, m.outcome(now))
			}
			m.release(h)
			delete(h.ranked.matches, id)
		}
	}
	return out
}

// apply carries out settled ranked results (outside every lock).
func (h *Hub) apply(actions []statsAction, now time.Time) {
	for _, a := range actions {
		if a.outcome != nil {
			h.recordRanked(*a.outcome)
			continue
		}
		if a.writes == nil {
			h.log.Printf("ranked results for %d (XUID %016X): %s", a.steamID, a.xuid, a.why)
			continue
		}
		if err := h.stats.WriteStats(a.steamID, now, a.writes); err != nil {
			h.log.Printf("ranked results for %d not kept: %v", a.steamID, err)
			continue
		}
		h.log.Printf("ranked results for %d kept (%s)", a.steamID, a.why)
	}
}

func (h *Hub) rankedSweeper(stop <-chan struct{}) {
	t := time.NewTicker(time.Minute)
	defer t.Stop()
	for {
		select {
		case <-stop:
			return
		case now := <-t.C:
			if h.stats != nil {
				h.apply(h.sweepRanked(now), now)
			}
		}
	}
}

// readWrites reads a STATS_WRITE's views; every value is checked, and a view or property twice is refused.
func readWrites(r *wire.Reader) ([]stats.Write, error) {
	n := r.U8()
	if n == 0 || n > stats.MaxViewsPerWrite {
		return nil, errors.New("bad view count")
	}
	writes := make([]stats.Write, 0, n)
	seenViews := map[uint32]bool{}
	for i := uint8(0); i < n; i++ {
		w := stats.Write{View: r.U32()}
		np := r.U8()
		if np > stats.MaxPropsPerView {
			return nil, errors.New("bad property count")
		}
		seenProps := map[uint32]bool{}
		for k := uint8(0); k < np; k++ {
			p := stats.Property{ID: r.U32(), Value: stats.Value{Type: r.U8(), Bits: r.U64()}}
			if seenProps[p.ID] {
				return nil, errors.New("a property twice")
			}
			seenProps[p.ID] = true
			w.Props = append(w.Props, p)
		}
		if r.Err() != nil {
			return nil, r.Err()
		}
		v, ok := stats.Lookup(w.View)
		if !ok || seenViews[w.View] {
			return nil, errors.New("no such view, or a view twice")
		}
		seenViews[w.View] = true
		if _, err := v.Apply(nil, w.Props); err != nil {
			return nil, err
		}
		writes = append(writes, w)
	}
	return writes, nil
}

// statsWrite: STATS_WRITE. Malformed writes are logged and dropped (a game bug must not cost the connection).
func (h *Hub) statsWrite(p *Player, r *wire.Reader, now time.Time) {
	session, own, subject := r.U64(), r.U64(), r.U64()
	writes, err := readWrites(r)
	if err == nil && (r.Err() != nil || len(r.B) != 0) {
		err = errors.New("malformed")
	}
	if err == nil && !h.claimXUID(p, own) {
		err = errors.New("not the XUID this connection uses")
	}
	if err != nil {
		h.logPlayer(p, "statistics from %q dropped: %v", p.Name, err)
		return
	}
	var mine, ranked []stats.Write
	for _, w := range writes {
		if v, _ := stats.Lookup(w.View); v.Arbitrated {
			ranked = append(ranked, w)
		} else {
			mine = append(mine, w)
		}
	}
	if len(mine) > 0 {
		if subject != p.ownXUID {
			h.logPlayer(p, "statistics from %q about another player dropped", p.Name)
		} else if err := h.stats.WriteStats(p.Peer, now, mine); err != nil {
			h.logPlayer(p, "statistics from %q not kept: %v", p.Name, err)
		}
	}
	if len(ranked) > 0 {
		actions, err := h.rankedReport(p, session, subject, ranked, now)
		if err != nil {
			h.logPlayer(p, "ranked results from %q dropped: %v", p.Name, err)
			return
		}
		h.apply(actions, now)
	}
}

// statsAnswer: STATS_RESULT with the rows' requested columns (nothing written: type 0xFF).
func statsAnswer(request uint32, ok bool, total int64, rows []stats.Entry, columns []uint16) []byte {
	var w wire.Writer
	w.U32(request)
	if ok {
		w.U8(1)
	} else {
		w.U8(0)
	}
	w.U32(uint32(min(max(total, 0), 0xFFFFFFFF)))
	w.U16(uint16(len(rows)))
	for _, e := range rows {
		w.U64(e.XUID)
		w.U32(uint32(min(e.Rank, 0xFFFFFFFF)))
		w.U64(uint64(e.Rating))
		w.Str(e.Name)
		w.U8(uint8(len(columns)))
		for _, c := range columns {
			v, have := e.Columns[c]
			if !have {
				v = stats.Value{Type: stats.TypeNull}
			}
			w.U16(c)
			w.U8(v.Type)
			w.U64(v.Bits)
		}
	}
	return w.B
}

func readColumns(r *wire.Reader) []uint16 {
	n := r.U8()
	if n > stats.MaxColumns {
		r.Bytes(1 << 30) // poison the reader
		return nil
	}
	cols := make([]uint16, n)
	for i := range cols {
		cols[i] = r.U16()
	}
	return cols
}

// readSlot takes one of the server's statistics read slots (false: the database is busy - answer "not ok").
func (h *Hub) readSlot() bool {
	select {
	case h.statReads <- struct{}{}:
		return true
	default:
		return false
	}
}

func (h *Hub) readDone() { <-h.statReads }

// statsBoard: STATS_BOARD (a page of a board, from a place or around a player).
func (h *Hub) statsBoard(p *Player, r *wire.Reader, now time.Time) error {
	request, view, start, count, pivot := r.U32(), r.U32(), r.U32(), r.U32(), r.U64()
	cols := readColumns(r)
	if r.Err() != nil || len(r.B) != 0 {
		return errors.New("bad STATS_BOARD")
	}
	count = min(count, stats.MaxRows)
	var total int64
	var rows []stats.Entry
	ok := false
	if _, known := stats.Lookup(view); known && p.statReadRate.take(5, 20, now) && h.readSlot() {
		var err error
		if pivot != 0 {
			if ids, e := h.stats.SteamIDsOf([]uint64{pivot}); e == nil && ids[pivot] != 0 {
				if _, me, e := h.stats.Entries(view, now, []uint64{ids[pivot]}); e == nil && len(me) == 1 {
					start = uint32(max(1, me[0].Rank-int64(count/2)))
				}
			}
		}
		total, rows, err = h.stats.Board(view, now, int(max(start, 1)), int(count))
		h.readDone()
		if err != nil {
			h.logPlayer(p, "database: %v", err)
		} else {
			ok = true
		}
	}
	p.send(wire.StatsResult, statsAnswer(request, ok, total, rows, cols))
	return nil
}

// statsPlayers: STATS_PLAYERS (those players' rows; at most 16, each once).
func (h *Hub) statsPlayers(p *Player, r *wire.Reader, now time.Time) error {
	request, view := r.U32(), r.U32()
	cols := readColumns(r)
	n := r.U8()
	if n > stats.MaxRows {
		return errors.New("bad STATS_PLAYERS")
	}
	var xuids []uint64
	seen := map[uint64]bool{}
	for i := uint8(0); i < n; i++ {
		x := r.U64()
		if !seen[x] && len(xuids) < maxStatsPlayers {
			seen[x] = true
			xuids = append(xuids, x)
		}
	}
	if r.Err() != nil || len(r.B) != 0 {
		return errors.New("bad STATS_PLAYERS")
	}
	var total int64
	var rows []stats.Entry
	ok := false
	if _, known := stats.Lookup(view); known && p.statReadRate.take(5, 20, now) && h.readSlot() {
		ids, err := h.stats.SteamIDsOf(xuids)
		if err == nil {
			var steam []uint64
			for _, x := range xuids {
				if id := ids[x]; id != 0 {
					steam = append(steam, id)
				}
			}
			total, rows, err = h.stats.Entries(view, now, steam)
		}
		h.readDone()
		if err != nil {
			h.logPlayer(p, "database: %v", err)
		} else {
			ok = true
		}
	}
	p.send(wire.StatsResult, statsAnswer(request, ok, total, rows, cols))
	return nil
}
