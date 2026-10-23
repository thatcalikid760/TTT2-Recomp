package panel

import (
	"strings"
	"time"

	"ttt2-server/internal/hub"
	"ttt2-server/internal/store"
)

// MaxPlayerRows: the most players `players all` and the All players page list.
const MaxPlayerRows = 200

// PlayerRow is one player of `players all` and the All players page: the database's row, whether they are online
// now, and the ban in force on them (a kick's block too).
type PlayerRow struct {
	store.Player
	Online bool
	Banned bool
	Ban    hub.Ban
}

// AllPlayers is every player the database has seen, most recently seen first (at most MaxPlayerRows): all of them,
// or those whose name contains `search` (any case) or whose Steam ID it is.
func AllPlayers(h *hub.Hub, db *store.Store, search string) ([]PlayerRow, error) {
	found, err := db.AllPlayers(strings.TrimSpace(search), MaxPlayerRows)
	if err != nil {
		return nil, err
	}
	online := map[uint64]bool{}
	for _, p := range h.Players() {
		online[p.Peer] = true
	}
	bans := map[uint64]hub.Ban{}
	for _, b := range h.Bans() {
		bans[b.SteamID] = b.Ban
	}
	rows := make([]PlayerRow, len(found))
	for i, p := range found {
		b, banned := bans[p.SteamID]
		rows[i] = PlayerRow{p, online[p.SteamID], banned, b}
	}
	return rows, nil
}

// BanText is the ban in force as the console and the pages show it: "until <time>", "for good", "kicked until
// <time>" ("" when there is none).
func (r PlayerRow) BanText() string {
	switch {
	case !r.Banned:
		return ""
	case r.Ban.Kick():
		return "kicked until " + r.Ban.Until.Format(time.DateTime)
	case r.Ban.Until.IsZero():
		return "for good"
	}
	return "until " + r.Ban.Until.Format(time.DateTime)
}
