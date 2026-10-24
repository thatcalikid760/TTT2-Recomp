package hub

import (
	"errors"
	"fmt"
	"time"

	"ttt2-server/internal/store"
	"ttt2-server/internal/wire"
)

// ConductStore keeps the players' ranked conduct (internal/store). Called on the players' connection goroutines and
// the ranked-results sweeper, never under the hub's lock.
type ConductStore interface {
	Conduct(steamID uint64) (store.Conduct, error)
	RankedPlayed(steamID uint64) error
	RankedQuit(steamID uint64, at time.Time, penalty func(before store.Conduct) (int, time.Time)) (store.Conduct, error)
}

// Ranked conduct (PROTOCOL.md "Ranked conduct"). A player who leaves a ranked match before its end - their opponent's
// game says so (RANKED_LEFT) and theirs does not say it of the opponent, or (games without RANKED_LEFT) only the
// opponent's game reported results - is the match's quitter: the opponent's results count for both (stats.go), the
// quit counts on the quitter's record and earns a strike, and each strike locks the quitter out of ranked matchmaking
// for a while (cooldownLadder; strikes past its end take its last step). A strike is forgiven every strikeForgiven
// without a quit.
var (
	cooldownLadder = []time.Duration{5 * time.Minute, 15 * time.Minute, time.Hour}
	strikeForgiven = 24 * time.Hour
)

const conductReads = 5 // CONDUCT_GET a second per player (burst 4x)

// SetConduct sets where ranked conduct is kept (before Serve); nil: quits cost nothing and WELCOME does not offer
// RANKED_LEFT / CONDUCT_GET.
func (h *Hub) SetConduct(c ConductStore) { h.conduct = c }

// penalty is a quit's strikes and cooldown, after the record `before`.
func penalty(before store.Conduct, now time.Time) (int, time.Time) {
	strikes := before.Strikes
	if !before.LastQuit.IsZero() && strikeForgiven > 0 {
		strikes -= int(now.Sub(before.LastQuit) / strikeForgiven)
	}
	strikes = max(strikes, 0) + 1
	step := cooldownLadder[min(strikes, len(cooldownLadder))-1]
	return strikes, now.Add(step)
}

// rankedBlocked: is the player in a ranked cooldown now?
func (p *Player) rankedBlocked(now time.Time) bool { return p.rankedUntil.Load() > now.Unix() }

// loadConduct reads a logging-in player's cooldown (their login goroutine).
func (h *Hub) loadConduct(p *Player) {
	if h.conduct == nil {
		return
	}
	c, err := h.conduct.Conduct(p.Peer)
	if err != nil {
		h.log.Printf("database: ranked conduct of %q: %v", p.Name, err)
		return
	}
	if !c.CooldownUntil.IsZero() {
		p.rankedUntil.Store(c.CooldownUntil.Unix())
	}
}

// rankedQuit counts a quit, starts the quitter's cooldown and tells them (from apply, outside every lock).
func (h *Hub) rankedQuit(steamID uint64, now time.Time) {
	c, err := h.conduct.RankedQuit(steamID, now, func(before store.Conduct) (int, time.Time) { return penalty(before, now) })
	if err != nil {
		h.log.Printf("database: ranked quit of %d not kept: %v", steamID, err)
		return
	}
	h.mu.Lock()
	p := h.players[steamID]
	h.mu.Unlock()
	name := fmt.Sprint(steamID)
	if p != nil {
		name = fmt.Sprintf("%q", p.Name)
		p.rankedUntil.Store(c.CooldownUntil.Unix())
		var w wire.Writer
		w.Str("You left a ranked match before its end. Ranked matchmaking is locked for " +
			minutesText(c.CooldownUntil.Sub(now)) + ".")
		p.send(wire.Notice, w.B)
	}
	h.log.Printf("ranked quit by %s: %d of %d matches, strike %d, ranked cooldown until %s", name, c.Quits, c.Matches,
		c.Strikes, c.CooldownUntil.UTC().Format(time.DateTime))
}

// minutesText: "5 minutes", "1 minute", "2 hours".
func minutesText(d time.Duration) string {
	m := int((d + 30*time.Second) / time.Minute)
	switch {
	case m >= 120 && m%60 == 0:
		return fmt.Sprintf("%d hours", m/60)
	case m == 60:
		return "1 hour"
	case m == 1:
		return "1 minute"
	}
	return fmt.Sprintf("%d minutes", max(m, 1))
}

// Pardon ends an online player's ranked cooldown (the admin's `pardon`, after the database's).
func (h *Hub) Pardon(steamID uint64) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if p := h.players[steamID]; p != nil {
		p.rankedUntil.Store(0)
	}
}

// rankedPlayed counts a settled ranked match for its players (from apply).
func (h *Hub) rankedPlayed(o rankedOutcome) {
	for _, id := range []uint64{o.host, o.guest} {
		if id == 0 {
			continue
		}
		if err := h.conduct.RankedPlayed(id); err != nil {
			h.log.Printf("database: ranked match of %d not counted: %v", id, err)
		}
	}
}

// rankedLeft: RANKED_LEFT - the player's game says its opponent in ranked match `session` left before the end.
func (h *Hub) rankedLeft(p *Player, session uint64, now time.Time) error {
	h.ranked.mu.Lock()
	defer h.ranked.mu.Unlock()
	m := h.ranked.matches[session]
	switch {
	case m == nil:
		return errors.New("not a registered ranked match")
	case m.paired == 0 || (p.Peer != m.host && p.Peer != m.paired):
		return errors.New("not a player of this ranked match")
	case m.judged:
		return errors.New("the match is judged already")
	}
	m.left[p.Peer] = true
	if m.first.IsZero() {
		m.first = now // the other game has rankedSettle to say the same of this one (a broken connection, nobody's quit)
	}
	return nil
}

// conductGet: CONDUCT_GET - a player's ranked record (u64 Steam ID, 0 = the asker's own): u32 request, u8 found
// (1, 0 none kept, 2 ask again later), u32 matches, u32 quits, u64 the asker's own cooldown end (unix seconds, 0 none;
// always 0 for another player).
func (h *Hub) conductGet(p *Player, r *wire.Reader, now time.Time) error {
	request, who := r.U32(), r.U64()
	if r.Err() != nil || len(r.B) != 0 {
		return errors.New("bad CONDUCT_GET")
	}
	if who == 0 {
		who = p.Peer
	}
	var w wire.Writer
	w.U32(request)
	found := uint8(2)
	var c store.Conduct
	if p.conductReads.take(conductReads, 4*conductReads, now) && h.readSlot() {
		var err error
		c, err = h.conduct.Conduct(who)
		h.readDone()
		found = 1
		if err != nil {
			h.logPlayer(p, "database: %v", err)
			found = 2
		}
	}
	w.U8(found)
	w.U32(uint32(min(max(c.Matches, 0), 0x7FFFFFFF)))
	w.U32(uint32(min(max(c.Quits, 0), 0x7FFFFFFF)))
	until := uint64(0)
	if who == p.Peer && found == 1 && c.CooldownUntil.After(now) {
		until = uint64(c.CooldownUntil.Unix())
	}
	w.U64(until)
	p.send(wire.ConductResult, w.B)
	return nil
}
