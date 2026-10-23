package panel

import (
	"bytes"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"ttt2-server/internal/hub"
)

// The live view: the panel's pages update themselves over Server-Sent Events (GET /events, panel.js's EventSource) -
// plain HTTPS on the panel's own connection, its session cookie, server to browser only.
//
// One producer goroutine runs while a stream is open (and liveLinger after the last one closed, so moving from page
// to page loses nothing): every liveTick it takes one snapshot of the hub (hub.PanelSnapshot: one short lock) and
// the bans, and when something changed it sends every stream the new state (one JSON message, made once and shared)
// and the activity that the change was (joined, left, match started / ended, kicked, banned, unbanned), kept in a
// ring of the last feedSize events for pages opened later. A stream gets a heartbeat every liveHeartbeat, when its
// session is checked too; it ends when its session ends (logout, expiry, panel-logout-all, a new login) or the panel
// stops. Writing to a browser never holds a lock, and a browser that does not read never holds anyone up: each
// stream has a small queue, and a stream whose queue is full is closed (its browser reconnects and gets the state
// afresh).

// Timings (variables for the tests).
var (
	liveTick        = time.Second
	liveHeartbeat   = 15 * time.Second
	liveLinger      = time.Minute      // the producer runs on this long after the last stream closed
	liveWriteWait   = 10 * time.Second // each write to a stream
	liveCountEvery  = 15 * time.Second // the matches of the last 24 hours are counted again this often
	liveGapToReport = 5 * time.Second  // a pause of the producer this long is shown in the feed
)

const (
	maxStreams        = 32 // live streams on the whole panel ...
	maxStreamsSession = 4  // ... and per session (a browser has 6 connections per host: pages must still load)
	feedSize          = 100
	streamQueue       = 16    // messages waiting for one stream
	liveRetryMillis   = 5000  // the browser's reconnect delay (SSE retry)
	maxLiveBans       = 1000  // bans in the live state (the Bans page lists them all)
	maxNames          = 20000 // names the producer remembers
)

var errTooManyStreams = errors.New("too many live pages open")

// live is the panel's live view: the open streams, the producer, the activity ring.
type live struct {
	source func() (hub.PanelSnapshot, []hub.BanInfo)     // the hub's state (the tests give their own)
	names  func(ids []uint64) (map[uint64]string, error) // the database's names for Steam IDs
	count  func(from time.Time) (int, error)             // matches started since

	mu        sync.Mutex
	streams   map[*stream]struct{}
	sessions  map[[32]byte]int // streams per session
	running   bool
	stopNow   bool      // the panel stopped: the producer stops without lingering
	idleSince time.Time // when the last stream closed
	stopped   time.Time // when the producer last stopped (activity until it starts again is not seen)
	state     []byte    // the newest state message (SSE), for streams that open meanwhile
	feed      []feedEntry
	seq       uint64
	epoch     string // this process's feed: an event id from another one starts afresh
}

// stream is one open GET /events.
type stream struct {
	key   [32]byte // its session
	ch    chan []byte
	done  chan struct{}
	once  sync.Once
	why   string // why it ended (set before done closes)
	final bool   // its session ended: the page is told to stop (else the browser reconnects by itself)
}

// end ends a stream: final when its session ended, not when it was too slow or the panel stopped (the browser
// reconnects then, and gets the state afresh).
func (s *stream) end(why string, final bool) {
	s.once.Do(func() {
		s.why, s.final = why, final
		close(s.done)
	})
}

// feedEntry is one event of the activity feed.
type feedEntry struct {
	Seq  uint64    `json:"seq"`
	At   int64     `json:"at"` // unix seconds
	Kind string    `json:"kind"`
	Who  []liveWho `json:"who,omitempty"`
	Text string    `json:"text"`
}

// Feed kinds (panel.js knows them; a CSS class each).
const (
	evJoin       = "join"
	evLeave      = "leave"
	evMatch      = "match"
	evMatchEnd   = "matchend"
	evKick       = "kick"
	evBan        = "ban"
	evUnban      = "unban"
	evBanEnd     = "banend"
	evInfo       = "info"
	kindRanked   = "ranked"
	stateMatch   = "match"
	stateRoom    = "room"
	stateOnline  = "online"
	kickedPrefix = "kicked"
)

// AtTime is the entry's time (templates).
func (e feedEntry) AtTime() time.Time { return time.Unix(e.At, 0) }

// Joiner is how the entry's players are joined in a sentence.
func (e feedEntry) Joiner() string {
	if e.Kind == evMatch || e.Kind == evMatchEnd {
		return " vs "
	}
	return ", "
}

type liveWho struct {
	ID   string `json:"id"` // a Steam ID, as text (JavaScript's numbers cannot hold one)
	Name string `json:"name"`
}

// liveState is what a page shows live; every field compares, so an unchanged hub sends nothing.
type liveState struct {
	Started int64        `json:"started"`
	Stats   liveStats    `json:"stats"`
	Players []livePlayer `json:"players"`
	Bans    []liveBan    `json:"bans"`
	MoreBan bool         `json:"moreBans,omitempty"`
}

type liveStats struct {
	Online     int `json:"online"`
	Playing    int `json:"playing"`
	Rooms      int `json:"rooms"`
	InRooms    int `json:"inRooms"`
	Bans       int `json:"bans"`
	Matches24h int `json:"matches24h"`
}

type livePlayer struct {
	ID         string    `json:"id"`
	Name       string    `json:"name"`
	Verified   bool      `json:"verified"`
	IP         string    `json:"ip"`
	Since      int64     `json:"since"`
	State      string    `json:"state"` // match, room, online
	Ranked     bool      `json:"ranked,omitempty"`
	Filled     uint32    `json:"filled,omitempty"`
	Max        uint32    `json:"max,omitempty"`
	MatchSince int64     `json:"matchSince,omitempty"`
	Vs         []liveWho `json:"vs,omitempty"`
}

type liveBan struct {
	ID     string `json:"id"`
	Name   string `json:"name"`
	Until  int64  `json:"until"` // 0: for good
	Reason string `json:"reason"`
	Kick   bool   `json:"kick"`
}

func (a livePlayer) equal(b livePlayer) bool {
	if a.ID != b.ID || a.Name != b.Name || a.Verified != b.Verified || a.IP != b.IP || a.Since != b.Since || a.State != b.State ||
		a.Ranked != b.Ranked || a.Filled != b.Filled || a.Max != b.Max || a.MatchSince != b.MatchSince || len(a.Vs) != len(b.Vs) {
		return false
	}
	for i := range a.Vs {
		if a.Vs[i] != b.Vs[i] {
			return false
		}
	}
	return true
}

func (a *liveState) equal(b *liveState) bool {
	if a.Started != b.Started || a.Stats != b.Stats || a.MoreBan != b.MoreBan || len(a.Players) != len(b.Players) || len(a.Bans) != len(b.Bans) {
		return false
	}
	for i := range a.Players {
		if !a.Players[i].equal(b.Players[i]) {
			return false
		}
	}
	for i := range a.Bans {
		if a.Bans[i] != b.Bans[i] {
			return false
		}
	}
	return true
}

func newLive(p *Panel) *live {
	l := &live{streams: map[*stream]struct{}{}, sessions: map[[32]byte]int{}}
	var b [4]byte
	rand.Read(b[:])
	l.epoch = hex.EncodeToString(b[:])
	l.source = func() (hub.PanelSnapshot, []hub.BanInfo) { return p.h.PanelSnapshot(), p.h.Bans() }
	l.names = p.db.PanelNamesOf
	l.count = p.db.MatchesSince
	return l
}

// open registers a stream of a session and returns it with what it gets first: the state now (when the producer
// has one) and the activity after `since` ("<epoch>-<seq>", from the page or the browser's Last-Event-ID).
func (l *live) open(key [32]byte, since string) (*stream, []byte, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if len(l.streams) >= maxStreams || l.sessions[key] >= maxStreamsSession {
		return nil, nil, errTooManyStreams
	}
	s := &stream{key: key, ch: make(chan []byte, streamQueue), done: make(chan struct{})}
	l.streams[s] = struct{}{}
	l.sessions[key]++
	l.stopNow = false
	var first bytes.Buffer
	fmt.Fprintf(&first, "retry: %d\n\n", liveRetryMillis)
	if l.running && l.state != nil {
		first.Write(l.state)
	}
	if after, ok := l.cursor(since); ok {
		var backlog []feedEntry
		for _, e := range l.feed {
			if e.Seq > after {
				backlog = append(backlog, e)
			}
		}
		first.Write(l.activityMessage(backlog))
	}
	if !l.running {
		l.running = true
		go l.run()
	}
	return s, first.Bytes(), nil
}

// cursor reads an event id ("<epoch>-<seq>"): the sequence number to send after; an id of another process (a
// restarted server) means everything kept. No id at all: nothing (the page showed the feed it wanted).
func (l *live) cursor(id string) (uint64, bool) {
	epoch, seq, ok := strings.Cut(id, "-")
	if !ok {
		return 0, false
	}
	n, err := strconv.ParseUint(seq, 10, 64)
	if epoch != l.epoch || err != nil || n > l.seq {
		return 0, true
	}
	return n, true
}

// lastID is the feed's newest event id (the pages hand it to panel.js).
func (l *live) lastID() string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.epoch + "-" + strconv.FormatUint(l.seq, 10)
}

// close unregisters a stream (its handler returned).
func (l *live) close(s *stream) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if _, ok := l.streams[s]; !ok {
		return
	}
	delete(l.streams, s)
	if l.sessions[s.key]--; l.sessions[s.key] <= 0 {
		delete(l.sessions, s.key)
	}
	if len(l.streams) == 0 {
		l.idleSince = time.Now()
	}
}

// endSession ends the streams of a session (logout); endAll every stream (every session ended, the panel stopped).
func (l *live) endSession(key [32]byte, why string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	for s := range l.streams {
		if s.key == key {
			s.end(why, true)
		}
	}
}

func (l *live) endAll(why string, stop bool) {
	l.mu.Lock()
	defer l.mu.Unlock()
	for s := range l.streams {
		s.end(why, !stop)
	}
	if stop {
		l.stopNow = true
	}
}

// openCount is how many streams are open (panel-status, tests).
func (l *live) openCount() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return len(l.streams)
}

// recent is the feed's newest entries, newest first (at most n).
func (l *live) recent(n int) []feedEntry {
	l.mu.Lock()
	defer l.mu.Unlock()
	out := make([]feedEntry, 0, min(n, len(l.feed)))
	for i := len(l.feed) - 1; i >= 0 && len(out) < n; i-- {
		out = append(out, l.feed[i])
	}
	return out
}

// note adds activity the hub's state does not show (a kick that keeps nobody out) and sends it.
func (l *live) note(kind string, who []liveWho, text string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.addLocked(time.Now(), []feedEntry{{Kind: kind, Who: who, Text: text}})
}

// addLocked numbers entries, keeps them in the ring and sends them to every stream (l.mu held).
func (l *live) addLocked(now time.Time, entries []feedEntry) {
	if len(entries) == 0 {
		return
	}
	for i := range entries {
		l.seq++
		entries[i].Seq, entries[i].At = l.seq, now.Unix()
	}
	l.feed = append(l.feed, entries...)
	if over := len(l.feed) - feedSize; over > 0 {
		l.feed = append(l.feed[:0:0], l.feed[over:]...)
	}
	l.sendLocked(l.activityMessage(entries))
}

// sendLocked queues a message on every stream; a stream whose queue is full is ended, never waited for.
func (l *live) sendLocked(msg []byte) {
	for s := range l.streams {
		select {
		case s.ch <- msg:
		default:
			s.end("too slow", false)
		}
	}
}

// activityMessage frames feed entries as one SSE event whose id is the newest's ("" for none).
func (l *live) activityMessage(entries []feedEntry) []byte {
	if len(entries) == 0 {
		return nil
	}
	data, _ := json.Marshal(entries) // escapes < > & (and U+2028/2029): safe in any page; never a raw line break
	return []byte(fmt.Sprintf("id: %s-%d\nevent: activity\ndata: %s\n\n", l.epoch, entries[len(entries)-1].Seq, data))
}

func sseMessage(event string, v any) []byte {
	data, _ := json.Marshal(v)
	return []byte("event: " + event + "\ndata: " + string(data) + "\n\n")
}

// ---- the producer

// producer is one run of the producer goroutine: the state it compares against, and what it remembers.
type producer struct {
	have      bool
	prev      liveState
	prevSnap  hub.PanelSnapshot
	prevList  []hub.BanInfo
	prevBans  map[uint64]hub.BanInfo
	names     map[uint64]string // Steam ID -> name: online players seen, matches' players, bans
	matches24 int
	countedAt time.Time
	recount   bool
}

// run is the producer: a snapshot every liveTick until no stream has been open for liveLinger.
func (l *live) run() {
	pr := &producer{names: map[uint64]string{}}
	t := time.NewTicker(liveTick)
	defer t.Stop()
	for {
		l.tick(pr, time.Now())
		<-t.C
		l.mu.Lock()
		if len(l.streams) == 0 && (l.stopNow || time.Since(l.idleSince) >= liveLinger) {
			l.running, l.state, l.stopped = false, nil, time.Now()
			l.mu.Unlock()
			return
		}
		l.mu.Unlock()
	}
}

// tick takes a snapshot; when it differs from the last one, sends the new state and the activity in between.
func (l *live) tick(pr *producer, now time.Time) {
	snap, bans := l.source()
	pr.observe(l, snap, bans, now)
	if pr.same(snap, bans) {
		return // the usual second: nothing changed, nothing to build or send
	}
	cur := pr.state(snap, bans)
	var events []feedEntry
	banMap := make(map[uint64]hub.BanInfo, len(bans))
	for _, b := range bans {
		banMap[b.SteamID] = b
	}
	if pr.have {
		events = pr.diff(snap, banMap, now)
	}
	changed := !pr.have || !cur.equal(&pr.prev)
	first := !pr.have
	pr.have, pr.prev, pr.prevSnap, pr.prevList, pr.prevBans = true, cur, snap, bans, banMap

	l.mu.Lock()
	defer l.mu.Unlock()
	if first && len(l.feed) > 0 && !l.stopped.IsZero() && now.Sub(l.stopped) >= liveGapToReport {
		events = append([]feedEntry{{Kind: evInfo, Text: fmt.Sprintf("No panel page was open from %s to %s: what happened then is not listed.",
			l.stopped.Format(time.TimeOnly), now.Format(time.TimeOnly))}}, events...)
	}
	l.addLocked(now, events)
	if changed {
		l.state = sseMessage("state", &cur)
		l.sendLocked(l.state)
	}
}

// same reports whether a snapshot is the last one again (the uptime aside) and the count of matches has not
// changed: a comparison, no allocation.
func (pr *producer) same(snap hub.PanelSnapshot, bans []hub.BanInfo) bool {
	a, b := &pr.prevSnap.Stats, &snap.Stats
	if !pr.have || pr.prev.Stats.Matches24h != pr.matches24 || !pr.prevSnap.Started.Equal(snap.Started) || a.Online != b.Online ||
		a.InRooms != b.InRooms || a.Sessions != b.Sessions || a.InMatches != b.InMatches ||
		len(pr.prevSnap.Players) != len(snap.Players) || len(pr.prevSnap.Matches) != len(snap.Matches) || len(pr.prevList) != len(bans) {
		return false
	}
	for i := range snap.Players {
		x, y := &pr.prevSnap.Players[i], &snap.Players[i]
		if x.Peer != y.Peer || x.Name != y.Name || x.Verified != y.Verified || x.IP != y.IP || !x.Since.Equal(y.Since) || x.Rooms != y.Rooms ||
			x.RoomFilled != y.RoomFilled || x.RoomMax != y.RoomMax || x.RoomRanked != y.RoomRanked || x.Match != y.Match {
			return false
		}
	}
	for i := range snap.Matches {
		x, y := &pr.prevSnap.Matches[i], &snap.Matches[i]
		if x.Session != y.Session || x.Ranked != y.Ranked || !x.Started.Equal(y.Started) || len(x.Players) != len(y.Players) {
			return false
		}
		for j := range x.Players {
			if x.Players[j] != y.Players[j] {
				return false
			}
		}
	}
	for i := range bans {
		x, y := &pr.prevList[i], &bans[i]
		if x.SteamID != y.SteamID || x.Reason != y.Reason || !x.Until.Equal(y.Until) {
			return false
		}
	}
	return true
}

// observe brings what the producer remembers up to date with a snapshot: the matches of the last 24 hours (counted
// again now and then), the names of the players it names (the database's for those not online, asked once).
func (pr *producer) observe(l *live, snap hub.PanelSnapshot, bans []hub.BanInfo, now time.Time) {
	if len(pr.names) > maxNames {
		clear(pr.names) // a dashboard open for days on a busy server: asked again
	}
	if !pr.have || pr.recount || now.Sub(pr.countedAt) >= liveCountEvery {
		if n, err := l.count(now.Add(-24 * time.Hour)); err == nil {
			pr.matches24 = n
		}
		pr.countedAt, pr.recount = now, false
	}
	for _, p := range snap.Players {
		pr.names[p.Peer] = p.Name
	}
	var unknown []uint64
	for _, b := range bans {
		if _, ok := pr.names[b.SteamID]; !ok {
			unknown = append(unknown, b.SteamID)
		}
	}
	for _, m := range snap.Matches {
		for _, id := range m.Players {
			if _, ok := pr.names[id]; !ok {
				unknown = append(unknown, id)
			}
		}
	}
	if len(unknown) > 0 {
		found, _ := l.names(unknown)
		for _, id := range unknown {
			pr.names[id] = found[id] // "" when the database has none: asked once
		}
	}
}

// state is the snapshot as the pages show it.
func (pr *producer) state(snap hub.PanelSnapshot, bans []hub.BanInfo) liveState {
	st := liveState{Started: snap.Started.Unix(), Players: make([]livePlayer, 0, len(snap.Players)), Bans: make([]liveBan, 0, min(len(bans), maxLiveBans))}
	st.Stats = liveStats{Online: snap.Stats.Online, Playing: snap.Stats.InMatches, Rooms: snap.Stats.Sessions, InRooms: snap.Stats.InRooms,
		Bans: len(bans), Matches24h: pr.matches24}
	matches := make(map[uint64]*hub.PanelMatch, len(snap.Matches))
	for i := range snap.Matches {
		matches[snap.Matches[i].Session] = &snap.Matches[i]
	}
	for _, p := range snap.Players {
		lp := livePlayer{ID: steamText(p.Peer), Name: p.Name, Verified: p.Verified, IP: p.IP, Since: p.Since.Unix(), State: stateOnline}
		if m := matches[p.Match]; p.Match != 0 && m != nil {
			lp.State, lp.Ranked, lp.MatchSince = stateMatch, m.Ranked, m.Started.Unix()
			lp.Vs = pr.who(m.Players, p.Peer)
		} else if p.Rooms > 0 {
			lp.State, lp.Ranked, lp.Filled, lp.Max = stateRoom, p.RoomRanked, p.RoomFilled, p.RoomMax
		}
		st.Players = append(st.Players, lp)
	}
	for i, b := range bans {
		if i == maxLiveBans {
			st.MoreBan = true
			break
		}
		lb := liveBan{ID: steamText(b.SteamID), Name: pr.names[b.SteamID], Reason: b.Reason, Kick: b.Kick()}
		if lb.Kick {
			lb.Reason = b.KickReason()
		}
		if !b.Until.IsZero() {
			lb.Until = b.Until.Unix()
		}
		st.Bans = append(st.Bans, lb)
	}
	return st
}

// who names Steam IDs (but `except`), sorted by name.
func (pr *producer) who(ids []uint64, except uint64) []liveWho {
	var out []liveWho
	for _, id := range ids {
		if id != except {
			out = append(out, liveWho{steamText(id), pr.nameOf(id)})
		}
	}
	sortWho(out)
	return out
}

func (pr *producer) nameOf(id uint64) string {
	if n := pr.names[id]; n != "" {
		return n
	}
	return steamText(id)
}

func sortWho(w []liveWho) {
	for i := 1; i < len(w); i++ {
		for j := i; j > 0 && (w[j].Name < w[j-1].Name || (w[j].Name == w[j-1].Name && w[j].ID < w[j-1].ID)); j-- {
			w[j], w[j-1] = w[j-1], w[j]
		}
	}
}

// diff is the activity between the last snapshot and this one.
func (pr *producer) diff(snap hub.PanelSnapshot, bans map[uint64]hub.BanInfo, now time.Time) []feedEntry {
	var out []feedEntry
	before := make(map[uint64]hub.PanelPlayer, len(pr.prevSnap.Players))
	for _, p := range pr.prevSnap.Players {
		before[p.Peer] = p
	}
	after := make(map[uint64]bool, len(snap.Players))
	for _, p := range snap.Players {
		after[p.Peer] = true
	}
	// bans and kicks first: a kicked player's leaving (and their match's end) follows
	for _, b := range sortedBans(bans) {
		old, had := pr.prevBans[b.SteamID]
		if had && old.Until.Equal(b.Until) && old.Reason == b.Reason {
			continue
		}
		who := []liveWho{{steamText(b.SteamID), pr.nameOf(b.SteamID)}}
		switch {
		case b.Kick():
			out = append(out, feedEntry{Kind: evKick, Who: who, Text: "kicked: kept out " + comeBackIn(b.Until.Sub(now)) + reasonTail(b.KickReason())})
		case b.Until.IsZero():
			out = append(out, feedEntry{Kind: evBan, Who: who, Text: "banned for good" + reasonTail(b.Reason)})
		default:
			out = append(out, feedEntry{Kind: evBan, Who: who, Text: "banned until " + b.Until.Format("2006-01-02 15:04") + reasonTail(b.Reason)})
		}
	}
	for _, p := range pr.prevSnap.Players {
		if !after[p.Peer] {
			out = append(out, feedEntry{Kind: evLeave, Who: []liveWho{{steamText(p.Peer), p.Name}},
				Text: "left after " + roundDuration(now.Sub(p.Since))})
		}
	}
	nowMatches := make(map[uint64]hub.PanelMatch, len(snap.Matches))
	for _, m := range snap.Matches {
		nowMatches[m.Session] = m
	}
	wasMatches := make(map[uint64]hub.PanelMatch, len(pr.prevSnap.Matches))
	for _, m := range pr.prevSnap.Matches {
		wasMatches[m.Session] = m
		if cur, ok := nowMatches[m.Session]; !ok || !cur.Started.Equal(m.Started) {
			out = append(out, feedEntry{Kind: evMatchEnd, Who: pr.who(m.Players, 0), Text: "match over after " + roundDuration(now.Sub(m.Started))})
		}
	}
	for _, p := range snap.Players {
		if _, was := before[p.Peer]; !was {
			text := "joined"
			if !p.Verified {
				text = "joined (unverified)"
			}
			out = append(out, feedEntry{Kind: evJoin, Who: []liveWho{{steamText(p.Peer), p.Name}}, Text: text})
		}
	}
	for _, m := range snap.Matches {
		if old, ok := wasMatches[m.Session]; !ok || !old.Started.Equal(m.Started) {
			text := "started a match"
			if m.Ranked {
				text = "started a ranked match"
			}
			out = append(out, feedEntry{Kind: evMatch, Who: pr.who(m.Players, 0), Text: text})
			pr.recount = true
		}
	}
	for _, old := range sortedBans(pr.prevBans) {
		if _, still := bans[old.SteamID]; still {
			continue
		}
		who := []liveWho{{steamText(old.SteamID), pr.nameOf(old.SteamID)}}
		switch {
		case !old.Until.IsZero() && !now.Before(old.Until.Add(-liveTick)):
			if !old.Kick() {
				out = append(out, feedEntry{Kind: evBanEnd, Who: who, Text: "ban ended"})
			}
		case old.Kick():
			out = append(out, feedEntry{Kind: evUnban, Who: who, Text: "let back in (kick lifted)"})
		default:
			out = append(out, feedEntry{Kind: evUnban, Who: who, Text: "unbanned"})
		}
	}
	return out
}

func sortedBans(m map[uint64]hub.BanInfo) []hub.BanInfo {
	out := make([]hub.BanInfo, 0, len(m))
	for _, b := range m {
		out = append(out, b)
	}
	for i := 1; i < len(out); i++ {
		for j := i; j > 0 && out[j].SteamID < out[j-1].SteamID; j-- {
			out[j], out[j-1] = out[j-1], out[j]
		}
	}
	return out
}

func reasonTail(reason string) string {
	if reason == "" {
		return ""
	}
	return " - " + reason
}

// comeBackIn says how long a kick keeps a player out ("for 10 minutes").
func comeBackIn(d time.Duration) string {
	m := int((d + 30*time.Second) / time.Minute)
	switch {
	case m <= 1:
		return "for a minute"
	case m < 120:
		return fmt.Sprintf("for %d minutes", m)
	}
	return fmt.Sprintf("for %d hours", (m+30)/60)
}

// roundDuration is a duration as people say it: "45s", "12m", "3h 5m", "2d 4h".
func roundDuration(d time.Duration) string {
	if d < 0 {
		d = 0
	}
	switch s := int64(d / time.Second); {
	case s < 60:
		return fmt.Sprintf("%ds", s)
	case s < 3600:
		return fmt.Sprintf("%dm", s/60)
	case s < 86400:
		if m := s % 3600 / 60; m > 0 {
			return fmt.Sprintf("%dh %dm", s/3600, m)
		}
		return fmt.Sprintf("%dh", s/3600)
	default:
		if h := s % 86400 / 3600; h > 0 {
			return fmt.Sprintf("%dd %dh", s/86400, h)
		}
		return fmt.Sprintf("%dd", s/86400)
	}
}

func steamText(id uint64) string { return strconv.FormatUint(id, 10) }

// ---- GET /events

// events is a live stream: for a session (none: 401, not a redirect - the page's script stops), from the panel's own
// pages, within the limits (429). It never keeps its session alive: an hour without a page view still ends it.
func (p *Panel) events(w http.ResponseWriter, r *http.Request) {
	if site := r.Header.Get("Sec-Fetch-Site"); site != "" && site != "same-origin" {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}
	key, ok := p.sessionPeek(r)
	if !ok {
		http.Error(w, "not logged in", http.StatusUnauthorized)
		return
	}
	since := r.Header.Get("Last-Event-ID")
	if since == "" {
		since = r.URL.Query().Get("since")
	}
	st, first, err := p.live.open(key, since)
	if err != nil {
		http.Error(w, err.Error(), http.StatusTooManyRequests)
		return
	}
	defer p.live.close(st)
	h := w.Header()
	h.Set("Content-Type", "text/event-stream; charset=utf-8")
	h.Set("Cache-Control", "no-store")
	h.Set("X-Accel-Buffering", "no")
	w.WriteHeader(http.StatusOK)
	rc := http.NewResponseController(w)
	write := func(b ...[]byte) bool {
		now := time.Now()
		// the server's WriteTimeout and ReadTimeout are for requests, not streams: each write moves them on (a
		// browser never sends anything on a stream; the read deadline only ends a stream whose handler stalled)
		rc.SetWriteDeadline(now.Add(liveWriteWait))
		rc.SetReadDeadline(now.Add(2*liveHeartbeat + liveWriteWait))
		for _, part := range b {
			if _, err := w.Write(part); err != nil {
				return false
			}
		}
		return rc.Flush() == nil
	}
	if !write(first, clockMessage()) {
		return
	}
	heartbeat := time.NewTicker(liveHeartbeat)
	defer heartbeat.Stop()
	for {
		select {
		case <-r.Context().Done():
			return
		case <-st.done:
			if st.final {
				write(sseMessage("end", map[string]string{"reason": st.why}))
			}
			return
		case msg := <-st.ch:
			if !write(msg) {
				return
			}
		case <-heartbeat.C:
			if !p.sessionAlive(key) {
				write(sseMessage("end", map[string]string{"reason": "the session ended"}))
				return
			}
			if !write([]byte(": ping\n\n"), clockMessage()) {
				return
			}
		}
	}
}

// clockMessage is the server's time (the pages' countdowns correct the browser's clock with it).
func clockMessage() []byte {
	return []byte("event: clock\ndata: " + strconv.FormatInt(time.Now().Unix(), 10) + "\n\n")
}
