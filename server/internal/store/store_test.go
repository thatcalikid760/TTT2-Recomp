package store

import (
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

// A folder like the project's own ("... (USA, Europe)"): spaces and brackets in the path.
func tempDB(t *testing.T) string {
	dir := filepath.Join(t.TempDir(), "data (test, one)")
	if err := os.MkdirAll(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	return filepath.Join(dir, "server.db")
}

func TestOpenMigratesAndKeeps(t *testing.T) {
	path := tempDB(t)
	s, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	if s.Version() != len(migrations) {
		t.Fatalf("schema %d, want %d", s.Version(), len(migrations))
	}
	first := time.Unix(1_800_000_000, 0)
	if err := s.PlayerSeen(76561198000000001, "KAZUYA", false, first); err != nil {
		t.Fatal(err)
	}
	if err := s.PlayerSeen(76561198000000001, "Kazuya2", true, first.Add(time.Hour)); err != nil {
		t.Fatal(err)
	}
	s.Close()
	if runtime.GOOS != "windows" {
		if st, err := os.Stat(path); err != nil || st.Mode().Perm() != 0o600 {
			t.Fatalf("database file mode %v (%v): the owner's only", st.Mode().Perm(), err)
		}
	}

	s, err = Open(path) // again: nothing to migrate, the row is still there
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	got, err := s.FindPlayers("76561198000000001")
	if err != nil || len(got) != 1 {
		t.Fatalf("by id: %v %v", got, err)
	}
	p := got[0]
	if p.Name != "Kazuya2" || !p.Verified || p.Logins != 2 || !p.FirstSeen.Equal(first) || !p.LastSeen.Equal(first.Add(time.Hour)) {
		t.Fatalf("player %+v", p)
	}
	if byName, _ := s.FindPlayers("KAZUYA2"); len(byName) != 1 || byName[0].SteamID != p.SteamID {
		t.Fatalf("by name (any case): %v", byName)
	}
	if none, _ := s.FindPlayers("nobody"); len(none) != 0 {
		t.Fatalf("unknown name: %v", none)
	}
	if n, _ := s.PlayerCount(); n != 1 {
		t.Fatalf("count %d", n)
	}
}

func TestNewerDatabaseRefused(t *testing.T) {
	path := tempDB(t)
	s, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.db.Exec(`PRAGMA user_version = 999`); err != nil {
		t.Fatal(err)
	}
	s.Close()
	if s, err := Open(path); err == nil {
		s.Close()
		t.Fatal("a database from a newer server must not be opened")
	}
}

func TestNotADatabase(t *testing.T) {
	path := tempDB(t)
	if err := os.WriteFile(path, []byte("this is not an SQLite file, it is a test of a damaged one................"), 0o600); err != nil {
		t.Fatal(err)
	}
	if s, err := Open(path); err == nil {
		s.Close()
		t.Fatal("a damaged file must be refused, not overwritten")
	}
}

// Logins arrive on their own goroutines: concurrent writers wait for each other, none fails.
func TestConcurrentLogins(t *testing.T) {
	s, err := Open(tempDB(t))
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	var wg sync.WaitGroup
	errs := make(chan error, 1000)
	for g := 0; g < 40; g++ {
		wg.Add(1)
		go func(g int) {
			defer wg.Done()
			for i := 0; i < 25; i++ {
				id := uint64(76561198000000000 + g*100 + i%5)
				if err := s.PlayerSeen(id, "P", g%2 == 0, time.Now()); err != nil {
					errs <- err
				}
				if _, err := s.FindPlayers("P"); err != nil {
					errs <- err
				}
			}
		}(g)
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Fatal(err)
	}
	if n, _ := s.PlayerCount(); n != 40*5 {
		t.Fatalf("%d players, want 200", n)
	}
	var logins int64
	s.db.QueryRow(`SELECT SUM(logins) FROM players`).Scan(&logins)
	if logins != 40*25 {
		t.Fatalf("%d logins, want 1000", logins)
	}
}

func TestBackupWhileOpen(t *testing.T) {
	s, err := Open(tempDB(t))
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	for i := 0; i < 100; i++ {
		s.PlayerSeen(uint64(1000+i), "B", true, time.Now())
	}
	dest := filepath.Join(t.TempDir(), "backup (1).db")
	if err := s.Backup(dest); err != nil {
		t.Fatal(err)
	}
	if err := s.Backup(dest); err == nil {
		t.Fatal("a backup must not overwrite a file")
	}
	b, err := Open(dest)
	if err != nil {
		t.Fatal(err)
	}
	defer b.Close()
	if n, _ := b.PlayerCount(); n != 100 {
		t.Fatalf("backup holds %d players", n)
	}
}

func TestURIPath(t *testing.T) {
	for in, want := range map[string]string{
		`C:\a b\x (1).db`:          "/C:/a%20b/x%20%281%29.db",
		"/srv/ttt2/data/server.db": "/srv/ttt2/data/server.db",
	} {
		if runtime.GOOS != "windows" && in[0] != '/' {
			continue // a Windows path is not absolute elsewhere
		}
		if got := fileURIPath(in); got != want {
			t.Errorf("%q -> %q, want %q", in, got, want)
		}
	}
}

func TestXUIDAndProfile(t *testing.T) {
	s, err := Open(tempDB(t))
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	const id = 76561198000000009
	if err := s.SaveProfile(id, []byte(`{"a":1}`), time.Now()); err == nil {
		t.Fatal("a profile needs a player who logged in (foreign key)")
	}
	s.PlayerSeen(id, "P", true, time.Now())
	if err := s.SetXUID(id, 0x0009F11C6844C6AB); err != nil {
		t.Fatal(err)
	}
	at := time.Unix(1_800_000_000, 0)
	if err := s.SaveProfile(id, []byte(`{"a":1}`), at); err != nil {
		t.Fatal(err)
	}
	if err := s.SaveProfile(id, []byte(`{"a":2}`), at.Add(time.Minute)); err != nil {
		t.Fatal(err)
	}
	body, when, found, err := s.Profile(id)
	if err != nil || !found || string(body) != `{"a":2}` || !when.Equal(at.Add(time.Minute)) {
		t.Fatalf("profile %q %v %v %v", body, when, found, err)
	}
	if _, _, found, _ := s.Profile(1); found {
		t.Fatal("nobody's profile")
	}
	p, _ := s.FindPlayers("P")
	if len(p) != 1 || p[0].XUID != 0x0009F11C6844C6AB {
		t.Fatalf("xuid %+v", p)
	}
}

// TestStoreScale: thousands of players logging in and uploading their profile (4.5 KB, as the game's) at once -
// measured, and every write kept. STORE_SCALE_PLAYERS sets the count (default 2000).
func TestStoreScale(t *testing.T) {
	if testing.Short() {
		t.Skip("scale test")
	}
	n := 2000
	if v, err := strconv.Atoi(os.Getenv("STORE_SCALE_PLAYERS")); err == nil && v > 0 {
		n = v
	}
	s, err := Open(tempDB(t))
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	profile := []byte(`{"payload":"` + strings.Repeat("x", 4500) + `"}`)
	var wg sync.WaitGroup
	errs := make(chan error, n)
	start := time.Now()
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func(id uint64) {
			defer wg.Done()
			if err := s.PlayerSeen(id, "P", true, time.Now()); err != nil {
				errs <- err
				return
			}
			if err := s.SetXUID(id, id^0x0009000000000000); err != nil {
				errs <- err
				return
			}
			if err := s.SaveProfile(id, profile, time.Now()); err != nil {
				errs <- err
			}
		}(76561198000000000 + uint64(i))
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Fatal(err)
	}
	took := time.Since(start)
	if c, _ := s.PlayerCount(); c != int64(n) {
		t.Fatalf("%d players", c)
	}
	t.Logf("%d players x (login, XUID, profile) = %d writes in %s (%.0f writes per second)", n, 3*n, took.Round(time.Millisecond),
		float64(3*n)/took.Seconds())
}

// players all: newest last-seen first, a name part (any case, % and _ as themselves) or a Steam ID, 200 at most.
func TestAllPlayers(t *testing.T) {
	s, err := Open(tempDB(t))
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	at := time.Unix(1_800_000_000, 0)
	for i, name := range []string{"Jin Kazama", "KAZUYA", "Nina 100%", "nina_w", "Lili"} {
		if err := s.PlayerSeen(76561198000000001+uint64(i), name, i%2 == 0, at.Add(time.Duration(i)*time.Minute)); err != nil {
			t.Fatal(err)
		}
	}
	names := func(search string, count int) string {
		t.Helper()
		got, err := s.AllPlayers(search, count)
		if err != nil {
			t.Fatal(err)
		}
		var out []string
		for _, p := range got {
			out = append(out, p.Name)
		}
		return strings.Join(out, ",")
	}
	if got := names("", 50); got != "Lili,nina_w,Nina 100%,KAZUYA,Jin Kazama" {
		t.Fatalf("every player, last seen first: %s", got)
	}
	if got := names("", 2); got != "Lili,nina_w" {
		t.Fatalf("count: %s", got)
	}
	if got := names("kaz", 50); got != "KAZUYA,Jin Kazama" {
		t.Fatalf("a name part, any case: %s", got)
	}
	if got := names("%", 50); got != "Nina 100%" {
		t.Fatalf("%% is no wildcard: %s", got)
	}
	if got := names("_", 50); got != "nina_w" {
		t.Fatalf("_ is no wildcard: %s", got)
	}
	if got := names("76561198000000002", 50); got != "KAZUYA" {
		t.Fatalf("a Steam ID: %s", got)
	}
	if got := names("100", 50); got != "Nina 100%" {
		t.Fatalf("digits that are no Steam ID match names: %s", got)
	}
	if got := names("nobody", 50); got != "" {
		t.Fatalf("%s", got)
	}
	for i := 0; i < 250; i++ {
		s.PlayerSeen(76561198100000000+uint64(i), "P"+strconv.Itoa(i), true, at)
	}
	if got, _ := s.AllPlayers("", 1000); len(got) != 200 {
		t.Fatalf("%d rows: 200 at most", len(got))
	}
}
