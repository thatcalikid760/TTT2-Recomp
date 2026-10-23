package panel

import (
	"net/http"
	"strconv"
	"strings"
	"time"

	"ttt2-server/internal/hub"
	"ttt2-server/internal/store"
)

// The pages' data. Every page reads the hub (one snapshot) and the database when it is asked for, never more; the
// live view (live.go) keeps it up to date afterwards.

const (
	playersPerPage = 50
	matchesPerPage = 50
	maxPage        = 10000
	playerMatches  = 20 // a player's page: their newest matches ...
	playerNames    = 20 // ... the names they played under ...
	playerAudit    = 30 // ... the panel's actions on them
	dashboardFeed  = 40
)

// search is a page's search text (q), cut to maxSearch characters.
func search(r *http.Request) string {
	return cut(strings.TrimSpace(r.URL.Query().Get("q")), maxSearch)
}

// pageNumber is a list page's page (1..maxPage).
func pageNumber(r *http.Request) int {
	n, err := strconv.Atoi(r.URL.Query().Get("page"))
	if err != nil || n < 1 {
		return 1
	}
	return min(n, maxPage)
}

// nameOf is the name the database knows for a Steam ID ("-" for none).
func (p *Panel) nameOf(id uint64) string {
	if found, err := p.db.FindPlayers(strconv.FormatUint(id, 10)); err == nil && len(found) > 0 {
		return found[0].Name
	}
	return "-"
}

// who is a Steam ID with the name the database knows for it: "Jin Kazama (7656...)", or the ID alone.
func (p *Panel) who(id uint64) string {
	if name := p.nameOf(id); name != "-" {
		return name + " (" + strconv.FormatUint(id, 10) + ")"
	}
	return strconv.FormatUint(id, 10)
}

// liveNow is the hub's state now as the live view shows it (the pages start from it).
func (p *Panel) liveNow() (liveState, hub.PanelSnapshot, []hub.BanInfo) {
	snap, bans := p.live.source()
	pr := &producer{names: map[uint64]string{}}
	pr.observe(p.live, snap, bans, time.Now())
	return pr.state(snap, bans), snap, bans
}

// ---- the dashboard

type dashData struct {
	State      liveState
	MaxPlayers int
	Known      int64 // players seen
	Feed       []feedEntry
}

func (p *Panel) dashboardPage(r *http.Request, v *view) error {
	v.page, v.Title, v.Nav = "dashboard", "Dashboard", "dashboard"
	st, _, _ := p.liveNow()
	d := &dashData{State: st, MaxPlayers: p.h.Config().MaxPlayers, Feed: p.live.recent(dashboardFeed)}
	v.Data = d
	v.Online, v.Playing, v.Bans = st.Stats.Online, st.Stats.Playing, st.Stats.Bans
	var err error
	d.Known, err = p.db.PlayerCount()
	return err
}

// ---- players

type playersData struct {
	Query, Show        string
	Rows               []PlayerRow
	Total, Page, Pages int
	First, Last        int // the rows' numbers in the whole list
}

func (p *Panel) playersPage(r *http.Request, v *view) error {
	v.page, v.Title, v.Nav = "players", "Players", "players"
	d := &playersData{Query: search(r), Show: r.URL.Query().Get("show"), Page: pageNumber(r)}
	v.Data = d
	online := map[uint64]bool{}
	ids := make([]uint64, 0)
	for _, pl := range p.h.Players() {
		online[pl.Peer] = true
		ids = append(ids, pl.Peer)
	}
	bans := map[uint64]hub.Ban{}
	banned := make([]uint64, 0)
	for _, b := range p.h.Bans() {
		bans[b.SteamID] = b.Ban
		banned = append(banned, b.SteamID)
	}
	f := store.PlayerFilter{Search: d.Query, Offset: (d.Page - 1) * playersPerPage, Limit: playersPerPage}
	switch d.Show {
	case "online":
		f.Only = ids
	case "offline":
		f.Not = ids
	case "banned":
		f.Only = banned
	default:
		d.Show = ""
	}
	found, total, err := p.db.PanelPlayers(f)
	d.Total, d.Pages = total, max(1, (total+playersPerPage-1)/playersPerPage)
	for _, pl := range found {
		b, isBanned := bans[pl.SteamID]
		d.Rows = append(d.Rows, PlayerRow{pl, online[pl.SteamID], isBanned, b})
	}
	if len(d.Rows) > 0 {
		d.First = f.Offset + 1
		d.Last = f.Offset + len(d.Rows)
	}
	return err
}

// ---- one player

type playerData struct {
	ID      uint64
	Known   bool // the database has seen them
	Player  store.Player
	Live    *livePlayer // online now
	Ban     *hub.BanInfo
	BanText string
	Names   []store.NameUse
	Matches []matchRow
	More    bool // more matches than shown
	Audit   []auditLine
}

func (p *Panel) playerPage(r *http.Request, v *view) error {
	v.page, v.Title, v.Nav = "player", "Player", "players"
	d := &playerData{}
	v.Data = d
	id, err := strconv.ParseUint(strings.TrimSpace(r.URL.Query().Get("steam")), 10, 64)
	if err != nil || id == 0 {
		return pageError{http.StatusBadRequest, "that is not a Steam ID"}
	}
	d.ID = id
	v.Title = steamText(id)
	found, err := p.db.FindPlayers(steamText(id))
	if err != nil {
		return err
	}
	if len(found) > 0 {
		d.Known, d.Player, v.Title = true, found[0], found[0].Name
	}
	st, _, bans := p.liveNow()
	for i := range st.Players {
		if st.Players[i].ID == steamText(id) {
			d.Live = &st.Players[i]
			v.Title = d.Live.Name
		}
	}
	for i := range bans {
		if bans[i].SteamID == id {
			d.Ban = &bans[i]
			d.BanText = untilText(bans[i].Ban)
		}
	}
	if d.Names, err = p.db.PlayerNames(id, playerNames); err != nil {
		return err
	}
	list, more, err := p.db.PanelMatches(store.MatchFilter{Who: steamText(id), Limit: playerMatches})
	if err != nil {
		return err
	}
	d.Matches, d.More = matchRows(list), more
	d.Audit, err = p.auditOf(id, playerAudit)
	return err
}

// ---- matches

type matchesData struct {
	Query, Kind string
	Matches     []matchRow
	Page        int
	More        bool
}

type matchRow struct {
	store.Match
	Took string // how long ("" while it is open)
	Open bool
}

func matchRows(list []store.Match) []matchRow {
	rows := make([]matchRow, 0, len(list))
	for _, m := range list {
		row := matchRow{Match: m, Open: m.Ended.IsZero()}
		if !row.Open {
			row.Took = roundDuration(m.Ended.Sub(m.Started))
		}
		rows = append(rows, row)
	}
	return rows
}

// ResultText is a match's result in words.
func (m matchRow) ResultText() string {
	switch m.Result {
	case "":
		if m.Open {
			return "pending"
		}
		return "waiting for the results"
	case "ended":
		return "finished"
	case "unfinished":
		return "unfinished"
	case "agreed":
		return "both reported the same"
	case "disagreed":
		return "reports disagree"
	case "unconfirmed":
		return "one report only"
	case "unreported":
		return "not reported"
	}
	return m.Result
}

// ResultClass is the tag colour of a match's result.
func (m matchRow) ResultClass() string {
	switch m.Result {
	case "agreed", "ended":
		return "ok"
	case "disagreed":
		return "danger"
	case "unconfirmed", "unreported", "unfinished":
		return "warn"
	}
	return "accent"
}

func (p *Panel) matchesPage(r *http.Request, v *view) error {
	v.page, v.Title, v.Nav = "matches", "Matches", "matches"
	d := &matchesData{Query: search(r), Kind: r.URL.Query().Get("kind"), Page: pageNumber(r)}
	v.Data = d
	if d.Kind != store.KindPlayer && d.Kind != store.KindRanked {
		d.Kind = ""
	}
	list, more, err := p.db.PanelMatches(store.MatchFilter{Who: d.Query, Kind: d.Kind, Offset: (d.Page - 1) * matchesPerPage, Limit: matchesPerPage})
	d.Matches, d.More = matchRows(list), more
	return err
}

// ---- bans

type banRow struct {
	SteamID      uint64
	Name, Reason string
	Until        time.Time
	Kick         bool
}

func (p *Panel) bansPage(r *http.Request, v *view) error {
	v.page, v.Title, v.Nav = "bans", "Bans", "bans"
	list := p.h.Bans()
	ids := make([]uint64, len(list))
	for i, b := range list {
		ids[i] = b.SteamID
	}
	names, err := p.db.PanelNamesOf(ids)
	rows := make([]banRow, 0, len(list))
	for _, b := range list {
		row := banRow{SteamID: b.SteamID, Name: names[b.SteamID], Reason: b.Reason, Until: b.Until, Kick: b.Kick()}
		if row.Kick {
			row.Reason = b.KickReason()
		}
		rows = append(rows, row)
	}
	v.Data = rows
	v.Bans = len(rows)
	return err
}

// ---- the audit log

type auditData struct {
	Query string
	Lines []auditLine
	Cut   bool // the file is longer than what was read
}

func (p *Panel) auditPage(r *http.Request, v *view) error {
	v.page, v.Title, v.Nav = "audit", "Audit log", "audit"
	d := &auditData{Query: search(r)}
	v.Data = d
	var err error
	d.Lines, d.Cut, err = p.auditTail(d.Query, auditMaxLines)
	return err
}
