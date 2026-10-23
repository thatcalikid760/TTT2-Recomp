package hub

import (
	"sort"
	"time"
)

// The admin panel's view of the hub (internal/panel): read-only, one snapshot under one lock, so the panel's live
// page (a snapshot a second while a page is open) costs the game server one short lock and a copy.

// PanelSnapshot is the hub's state as the admin panel shows it, all taken at one moment.
type PanelSnapshot struct {
	Started time.Time // when the hub started (uptime)
	Stats   Stats
	Players []PanelPlayer // online players, by name (then Steam ID)
	Matches []PanelMatch  // the open recorded matches, oldest first
}

// PanelPlayer is an online player and what they are doing.
type PanelPlayer struct {
	Peer     uint64
	Name     string
	Verified bool
	IP       string
	Since    time.Time // connected at
	// the rooms (sessions) they host: how many, and the newest one's slots and kind
	Rooms               int
	RoomFilled, RoomMax uint32
	RoomRanked          bool
	// Match: the session of the open recorded match that is their newest (0: none)
	Match uint64
}

// PanelMatch is an open recorded match (MATCH_EVENT).
type PanelMatch struct {
	Session uint64
	Ranked  bool
	Started time.Time
	Players []uint64 // Steam IDs, in no particular order
}

// PanelSnapshot takes the snapshot (copied under h.mu, then the match lock: the hub's order; sorted after).
func (h *Hub) PanelSnapshot() PanelSnapshot {
	s := h.panelCopy()
	sort.Slice(s.Players, func(i, j int) bool {
		a, b := s.Players[i], s.Players[j]
		if a.Name != b.Name {
			return a.Name < b.Name
		}
		return a.Peer < b.Peer
	})
	sort.Slice(s.Matches, func(i, j int) bool {
		a, b := s.Matches[i], s.Matches[j]
		if !a.Started.Equal(b.Started) {
			return a.Started.Before(b.Started)
		}
		return a.Session < b.Session
	})
	return s
}

func (h *Hub) panelCopy() PanelSnapshot {
	h.mu.Lock()
	defer h.mu.Unlock()
	s := PanelSnapshot{Started: h.started, Players: make([]PanelPlayer, 0, len(h.players))}
	s.Stats = Stats{Online: len(h.players), Sessions: len(h.sessions), Uptime: time.Since(h.started)}
	index := make(map[uint64]int, len(h.players))
	for _, p := range h.players {
		index[p.Peer] = len(s.Players)
		s.Players = append(s.Players, PanelPlayer{Peer: p.Peer, Name: p.Name, Verified: p.Verified, IP: p.ip, Since: p.since})
	}
	newest := map[uint64]time.Time{}
	for _, ses := range h.sessions {
		s.Stats.InRooms += int(min(ses.filled, ses.max))
		i, ok := index[ses.host.Peer]
		if !ok || h.players[ses.host.Peer] != ses.host {
			continue
		}
		pp := &s.Players[i]
		pp.Rooms++
		if t, seen := newest[pp.Peer]; !seen || ses.created.After(t) {
			newest[pp.Peer] = ses.created
			pp.RoomFilled, pp.RoomMax, pp.RoomRanked = ses.filled, ses.max, ses.ranked
		}
	}
	ms := &h.matches
	ms.mu.Lock()
	for id, m := range ms.current {
		if i, ok := index[id]; ok {
			s.Players[i].Match = m.session
			s.Stats.InMatches++
		}
	}
	for _, m := range ms.bySession {
		pm := PanelMatch{Session: m.session, Ranked: m.ranked, Started: m.started, Players: make([]uint64, 0, len(m.players))}
		for id := range m.players {
			pm.Players = append(pm.Players, id)
		}
		s.Matches = append(s.Matches, pm)
	}
	ms.mu.Unlock()
	return s
}
