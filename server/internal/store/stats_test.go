package store

import (
	"os"
	"strconv"
	"testing"
	"time"

	"ttt2-server/internal/stats"
)

func init() { rankIndexDirtyAge = 0 } // tests read a board right after writing it

func bpWrite(rating int64, wins int32) []stats.Write {
	return []stats.Write{{View: 9, Props: []stats.Property{
		{ID: 0x20000001, Value: stats.Value{Type: stats.TypeInt64, Bits: uint64(rating)}},
		{ID: 0x10000010, Value: stats.Value{Type: stats.TypeInt32, Bits: uint64(int64(wins))}},
	}}}
}

func openWithPlayers(t *testing.T, n int, at time.Time) *Store {
	t.Helper()
	s, err := Open(tempDB(t))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close() })
	for i := 1; i <= n; i++ {
		if err := s.PlayerSeen(uint64(i), "P"+strconv.Itoa(i), true, at); err != nil {
			t.Fatal(err)
		}
		if err := s.SetXUID(uint64(i), 0x0009000000000000+uint64(i)); err != nil {
			t.Fatal(err)
		}
	}
	return s
}

func TestBoardOrderAndPlaces(t *testing.T) {
	at := time.Unix(1_800_000_000, 0)
	s := openWithPlayers(t, 5, at)
	// ratings 300, 500, 500 (player 2 first), 100; player 5 never played
	for i, r := range []int64{300, 500, 500, 100} {
		if err := s.WriteStats(uint64(i+1), at.Add(time.Duration(i)*time.Second), bpWrite(r, int32(i))); err != nil {
			t.Fatal(err)
		}
	}
	total, rows, err := s.Board(9, at, 1, 10)
	if err != nil || total != 4 || len(rows) != 4 {
		t.Fatalf("%d %v %v", total, rows, err)
	}
	order := []uint64{2, 3, 1, 4}
	for i, e := range rows {
		if e.SteamID != order[i] || e.Rank != int64(i+1) || e.XUID != 0x0009000000000000+order[i] || e.Name != "P"+strconv.Itoa(int(order[i])) {
			t.Fatalf("row %d: %+v", i, e)
		}
	}
	// a page further down
	_, page, _ := s.Board(9, at, 3, 2)
	if len(page) != 2 || page[0].SteamID != 1 || page[0].Rank != 3 || page[1].Rank != 4 {
		t.Fatalf("%+v", page)
	}
	// single players with their places; one without a row is left out
	_, mine, err := s.Entries(9, at, []uint64{4, 3, 5})
	if err != nil || len(mine) != 2 || mine[0].Rank != 4 || mine[1].Rank != 2 || mine[1].Columns[5].Bits != 2 {
		t.Fatalf("%+v %v", mine, err)
	}
	// Max keeps the best rating: a lower write does not move player 2 down (nor its time of reaching it)
	if err := s.WriteStats(2, at.Add(time.Minute), bpWrite(50, 9)); err != nil {
		t.Fatal(err)
	}
	_, rows, _ = s.Board(9, at, 1, 2)
	if rows[0].SteamID != 2 || rows[0].Rating != 500 || rows[0].Columns[5].Bits != 9 {
		t.Fatalf("%+v", rows[0])
	}
	// an unknown view is refused, nothing half-written
	err = s.WriteStats(1, at, append(bpWrite(999, 0), stats.Write{View: 12345}))
	if err == nil {
		t.Fatal("unknown view accepted")
	}
	_, mine, _ = s.Entries(9, at, []uint64{1})
	if mine[0].Rating != 300 {
		t.Fatal("a refused write changed a row")
	}
}

func TestMonthlyBoardsStartAgain(t *testing.T) {
	oct := time.Date(2026, 10, 20, 12, 0, 0, 0, time.UTC)
	s := openWithPlayers(t, 1, oct)
	w := []stats.Write{{View: 152, Props: []stats.Property{{ID: 0x20000001, Value: stats.Value{Type: stats.TypeInt64, Bits: 77}}}}}
	if err := s.WriteStats(1, oct, w); err != nil {
		t.Fatal(err)
	}
	if total, _, _ := s.Board(152, oct, 1, 10); total != 1 {
		t.Fatal("october board")
	}
	if total, _, _ := s.Board(152, oct.AddDate(0, 1, 0), 1, 10); total != 0 {
		t.Fatal("november board must start empty")
	}
}

func TestSteamIDsOf(t *testing.T) {
	at := time.Unix(1_800_000_000, 0)
	s := openWithPlayers(t, 3, at)
	// two players reported the same XUID: the most recently seen one has it
	s.SetXUID(1, 0x00090000000000AA)
	s.SetXUID(2, 0x00090000000000AA)
	s.PlayerSeen(1, "P1", true, at.Add(time.Hour))
	got, err := s.SteamIDsOf([]uint64{0x00090000000000AA, 0x0009000000000003, 0x1234})
	if err != nil || len(got) != 2 || got[0x00090000000000AA] != 1 || got[0x0009000000000003] != 3 {
		t.Fatalf("%v %v", got, err)
	}
	if got, err := s.SteamIDsOf(nil); err != nil || len(got) != 0 {
		t.Fatal("empty")
	}
}

// Thousands of players on one board: a page from the middle and a player's place stay fast.
func TestBoardScale(t *testing.T) {
	if testing.Short() {
		t.Skip("scale test")
	}
	at := time.Unix(1_800_000_000, 0)
	s := openWithPlayers(t, 0, at)
	n := 5000 // STORE_SCALE_PLAYERS: another count
	if v, err := strconv.Atoi(os.Getenv("STORE_SCALE_PLAYERS")); err == nil && v > 0 {
		n = v
	}
	start := time.Now()
	for i := 1; i <= n; i++ {
		s.PlayerSeen(uint64(i), "P", true, at)
		if err := s.WriteStats(uint64(i), at, bpWrite(int64(i*7%n), 1)); err != nil {
			t.Fatal(err)
		}
	}
	t.Logf("%d players written in %s", n, time.Since(start).Round(time.Millisecond))
	start = time.Now()
	if _, _, err := s.Board(9, at, 1, 1); err != nil { // the board's order is read once
		t.Fatal(err)
	}
	first := time.Since(start)
	start = time.Now()
	total, rows, err := s.Board(9, at, n-100, 100)
	if err != nil || total != int64(n) || len(rows) != 100 {
		t.Fatalf("%d %d %v", total, len(rows), err)
	}
	page := time.Since(start)
	t.Logf("the board's order read in %s", first.Round(time.Millisecond))
	start = time.Now()
	_, me, err := s.Entries(9, at, []uint64{uint64(n / 3)})
	if err != nil || len(me) != 1 {
		t.Fatal(err)
	}
	t.Logf("the last page (place %d): %s; one player's place (%d): %s", n-100, page.Round(time.Microsecond), me[0].Rank, time.Since(start).Round(time.Microsecond))
	if page > time.Second {
		t.Fatal("too slow")
	}
}

func TestUploadsKeepTheNewest(t *testing.T) {
	at := time.Unix(1_800_000_000, 0)
	s := openWithPlayers(t, 2, at)
	for i := 0; i < 7; i++ {
		if err := s.SaveUpload(1, "custom/ghost", []byte(`{"n":`+strconv.Itoa(i)+`}`), 5, at); err != nil {
			t.Fatal(err)
		}
	}
	s.SaveUpload(2, "custom/ghost", []byte(`{"other":1}`), 5, at)
	s.SaveUpload(1, "match", []byte(`{"m":1}`), 50, at)
	got, err := s.Uploads(1, "custom/ghost")
	if err != nil || len(got) != 5 || string(got[0]) != `{"n":6}` || string(got[4]) != `{"n":2}` {
		t.Fatalf("%q %v", got, err)
	}
	if other, _ := s.Uploads(2, "custom/ghost"); len(other) != 1 {
		t.Fatal("another player's uploads must stay")
	}
	if m, _ := s.Uploads(1, "match"); len(m) != 1 {
		t.Fatal("another kind must stay")
	}
}

func TestTeams(t *testing.T) {
	at := time.Unix(1_800_000_000, 0)
	s := openWithPlayers(t, 4, at)
	s.SaveProfile(2, []byte(`{"payload":{"fighter-3-rank":"20","fighter-3-wins":"7","fighter-9-rank":"4","fighter-9-wins":"5"}}`), at)
	id, err := s.CreateTeam("Arabia", 1, at)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.CreateTeam("ARABIA", 3, at); err == nil {
		t.Fatal("a name twice")
	}
	if _, err := s.CreateTeam("Ghosts", 999, at); err == nil {
		t.Fatal("a leader the server never saw")
	}
	if err := s.JoinTeam(id, 2, at.Add(time.Second)); err != nil {
		t.Fatal(err)
	}
	team, ok, err := s.TeamOf(2)
	if err != nil || !ok || team.Name != "Arabia" || team.Leader != 1 || len(team.Members) != 2 || team.Members[1].BestFighter != 3 ||
		team.Members[1].BestRank != 20 || team.Members[1].Wins != 12 || team.Members[0].BestRank != -1 {
		t.Fatalf("%+v %v %v", team, ok, err)
	}
	if n, err := s.TeamID("arabia"); err != nil || n != id {
		t.Fatal("by name")
	}
	// the leader leaves: the next member leads; the last one leaving ends the team
	if err := s.LeaveTeam(1); err != nil {
		t.Fatal(err)
	}
	if team, _, _ = s.TeamOf(2); team.Leader != 2 {
		t.Fatalf("%+v", team)
	}
	s.LeaveTeam(2)
	if teams, _ := s.Teams(); len(teams) != 0 {
		t.Fatalf("%+v", teams)
	}
	if _, ok, _ := s.TeamOf(2); ok {
		t.Fatal("no team left")
	}
}
