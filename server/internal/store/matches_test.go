package store

import (
	"database/sql"
	"path/filepath"
	"testing"
	"time"
)

// A database a server of the previous version wrote (schema 6, a player in it) upgrades in place: the match history's
// tables are added, nothing else changes.
func TestMigrationFromSchema6(t *testing.T) {
	path := tempDB(t)
	abs, _ := filepath.Abs(path)
	old, err := sql.Open("sqlite", "file:"+fileURIPath(abs)+"?_pragma=foreign_keys(1)")
	if err != nil {
		t.Fatal(err)
	}
	for v, m := range migrations[:6] {
		if _, err := old.Exec(m); err != nil {
			t.Fatalf("schema %d: %v", v+1, err)
		}
	}
	if _, err := old.Exec(`PRAGMA user_version = 6`); err != nil {
		t.Fatal(err)
	}
	if _, err := old.Exec(`INSERT INTO players (steam_id, name, verified, first_seen, last_seen, logins) VALUES (76561198000000001, 'OLD', 1, 1, 2, 3)`); err != nil {
		t.Fatal(err)
	}
	old.Close()

	s, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	if s.Version() != len(migrations) || len(migrations) < 7 {
		t.Fatalf("schema %d after the upgrade, %d migrations", s.Version(), len(migrations))
	}
	if p, _ := s.FindPlayers("76561198000000001"); len(p) != 1 || p[0].Name != "OLD" || p[0].Logins != 3 {
		t.Fatalf("the old rows must stay: %+v", p)
	}
	id, err := s.OpenMatch(0x8000000000000001, KindPlayer, time.Unix(1_800_000_000, 0))
	if err != nil || id == 0 {
		t.Fatalf("the new tables: %d %v", id, err)
	}
	if err := s.AddMatchPlayer(id, 76561198000000001, "OLD", 1, true); err != nil {
		t.Fatal(err)
	}
}

func historyStore(t *testing.T) *Store {
	t.Helper()
	s, err := Open(tempDB(t))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close() })
	return s
}

func TestMatchHistory(t *testing.T) {
	s := historyStore(t)
	const a, b = 76561198000000001, 76561198000000002
	t0 := time.Unix(1_800_000_000, 0)
	s.PlayerSeen(a, "Jin", true, t0)
	s.PlayerSeen(b, "Kazuya", true, t0)
	id, err := s.OpenMatch(0x8000000000000AAA, KindPlayer, t0)
	if err != nil {
		t.Fatal(err)
	}
	// A's game reports A and names B; B's own report gives its local players (pair play: 2); a later naming of A by
	// B's game does not change A's own count
	for _, p := range []struct {
		id    uint64
		name  string
		local int
		own   bool
	}{{a, "Jin", 1, true}, {b, "Kazuya", 1, false}, {b, "Kazuya", 2, true}, {a, "Jin", 3, false}} {
		if err := s.AddMatchPlayer(id, p.id, p.name, p.local, p.own); err != nil {
			t.Fatal(err)
		}
	}
	if err := s.EndMatch(id, t0.Add(3*time.Minute), "ended"); err != nil {
		t.Fatal(err)
	}
	if err := s.EndMatch(id, t0.Add(time.Hour), "unfinished"); err != nil { // a second close changes nothing
		t.Fatal(err)
	}
	got, err := s.RecentMatches("", 20)
	if err != nil || len(got) != 1 {
		t.Fatalf("%+v %v", got, err)
	}
	m := got[0]
	if m.Kind != KindPlayer || m.Result != "ended" || !m.Started.Equal(t0) || !m.Ended.Equal(t0.Add(3*time.Minute)) || len(m.Players) != 2 ||
		m.Players[0] != (MatchPlayer{a, "Jin", 1, -1}) || m.Players[1] != (MatchPlayer{b, "Kazuya", 2, -1}) {
		t.Fatalf("%+v", m)
	}
	// a player's matches by Steam ID or by the name they have now (any case)
	s.PlayerSeen(b, "Heihachi", true, t0.Add(time.Hour))
	if got, _ := s.RecentMatches("76561198000000002", 20); len(got) != 1 || got[0].Players[1].Name != "Kazuya" {
		t.Fatalf("by Steam ID: %+v", got)
	}
	if got, _ := s.RecentMatches("heihachi", 20); len(got) != 1 {
		t.Fatalf("by name: %+v", got)
	}
	// ... and by the name they played under then (a report names them as the game showed them)
	if got, _ := s.RecentMatches("KAZUYA", 20); len(got) != 1 || got[0].Players[1].SteamID != b {
		t.Fatalf("by the name in the match: %+v", got)
	}
	if got, _ := s.RecentMatches("nobody", 20); len(got) != 0 {
		t.Fatalf("unknown name: %+v", got)
	}
	// newest first, at most `count`
	for i := 1; i <= 3; i++ {
		s.OpenMatch(0x8000000000000B00+uint64(i), KindPlayer, t0.Add(time.Duration(i)*time.Hour))
	}
	if got, _ := s.RecentMatches("", 2); len(got) != 2 || got[0].Session != 0x8000000000000B03 || !got[0].Ended.IsZero() {
		t.Fatalf("newest: %+v", got)
	}
}

func TestRankedSettles(t *testing.T) {
	s := historyStore(t)
	const host, guest, stranger = 76561198000000011, 76561198000000012, 76561198000000099
	const room = 0x8000000000000C01
	t0 := time.Unix(1_800_000_000, 0)
	s.PlayerSeen(host, "HOST", true, t0)
	s.PlayerSeen(guest, "GUEST", true, t0)
	// the games reported the match (MATCH_EVENT) and it ended before its results settled
	id, _ := s.OpenMatch(room, KindRanked, t0)
	s.AddMatchPlayer(id, host, "HOST", 1, true)
	s.AddMatchPlayer(id, guest, "GUEST", 1, true)
	s.EndMatch(id, t0.Add(2*time.Minute), "")
	settle := RankedResult{Session: room, Players: [2]uint64{host, guest}, Registered: t0, Settled: t0.Add(3 * time.Minute), Result: "agreed"}
	if err := s.SettleRanked(settle); err != nil {
		t.Fatal(err)
	}
	// the next match in the room was not reported by the games (an older game): a row of its own, from the pair
	next := settle
	next.Registered, next.Settled, next.Result = t0.Add(5*time.Minute), t0.Add(9*time.Minute), "disagreed"
	next.Names = [2]string{"", "GUEST NOW"}
	if err := s.SettleRanked(next); err != nil {
		t.Fatal(err)
	}
	// a match the games reported as a Player Match (the room's record came later) is ranked once its results settle
	late, _ := s.OpenMatch(room, KindPlayer, t0.Add(10*time.Minute))
	s.AddMatchPlayer(late, host, "HOST", 1, true)
	s.AddMatchPlayer(late, guest, "GUEST", 1, true)
	s.EndMatch(late, t0.Add(12*time.Minute), "ended")
	third := settle
	third.Registered, third.Settled, third.Result = t0.Add(10*time.Minute), t0.Add(14*time.Minute), "unconfirmed"
	s.SettleRanked(third)
	// someone the database never saw
	odd := RankedResult{Session: 0x8000000000000C02, Players: [2]uint64{host, stranger}, Registered: t0, Settled: t0.Add(time.Minute), Result: "unreported"}
	if err := s.SettleRanked(odd); err != nil {
		t.Fatal(err)
	}

	got, err := s.RecentMatches("", 20)
	if err != nil || len(got) != 4 {
		t.Fatalf("%d matches %v", len(got), err)
	}
	bySession := map[uint64][]Match{}
	for _, m := range got {
		bySession[m.Session] = append(bySession[m.Session], m)
	}
	r := bySession[room] // newest first
	if len(r) != 3 || r[2].ID != id || r[2].Result != "agreed" || !r[2].Ended.Equal(t0.Add(2*time.Minute)) {
		t.Fatalf("the reported match: %+v", r)
	}
	if r[1].Result != "disagreed" || r[1].Kind != KindRanked || !r[1].Started.Equal(next.Registered) || !r[1].Ended.Equal(next.Settled) ||
		len(r[1].Players) != 2 || r[1].Players[0].Name != "HOST" || r[1].Players[1].Name != "GUEST NOW" || r[1].Players[0].Won != -1 {
		t.Fatalf("the unreported match: %+v", r[1])
	}
	if r[0].ID != late || r[0].Kind != KindRanked || r[0].Result != "unconfirmed" {
		t.Fatalf("the late ranked match: %+v", r[0])
	}
	if o := bySession[0x8000000000000C02]; len(o) != 1 || o[0].Players[1].Name != "PLAYER" || o[0].Result != "unreported" {
		t.Fatalf("an unknown player: %+v", o)
	}
}

func TestCloseAndPruneMatches(t *testing.T) {
	s := historyStore(t)
	now := time.Unix(1_800_000_000, 0)
	open, _ := s.OpenMatch(1, KindPlayer, now.Add(-10*time.Minute))
	stale, _ := s.OpenMatch(2, KindPlayer, now.Add(-5*time.Hour)) // a crashed server's: an hour at most
	settled, _ := s.OpenMatch(3, KindRanked, now.Add(-5*time.Minute))
	s.db.Exec(`UPDATE matches SET result = 'agreed' WHERE id = ?`, settled)
	if n, err := s.CloseOpenMatches(now); err != nil || n != 3 {
		t.Fatalf("%d %v", n, err)
	}
	got, _ := s.RecentMatches("", 10)
	ends := map[int64]Match{}
	for _, m := range got {
		ends[m.ID] = m
	}
	if m := ends[open]; !m.Ended.Equal(now) || m.Result != "unfinished" {
		t.Fatalf("%+v", m)
	}
	if m := ends[stale]; !m.Ended.Equal(now.Add(-4*time.Hour)) || m.Result != "unfinished" {
		t.Fatalf("%+v", m)
	}
	if m := ends[settled]; m.Result != "agreed" {
		t.Fatalf("a settled ranked match keeps its result: %+v", m)
	}

	// matches older than a year are forgotten with their players
	s.PlayerSeen(5, "P", true, now)
	old, _ := s.OpenMatch(4, KindPlayer, now.AddDate(0, 0, -400))
	s.AddMatchPlayer(old, 5, "P", 1, true)
	s.AddMatchPlayer(open, 5, "P", 1, true)
	if n, err := s.PruneMatches(now.AddDate(0, 0, -365)); err != nil || n != 1 {
		t.Fatalf("%d %v", n, err)
	}
	var players int
	s.db.QueryRow(`SELECT COUNT(*) FROM match_players`).Scan(&players)
	if got, _ := s.RecentMatches("", 10); len(got) != 3 || players != 1 {
		t.Fatalf("%d matches, %d players left", len(got), players)
	}
}
