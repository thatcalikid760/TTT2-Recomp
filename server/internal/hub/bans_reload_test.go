package hub

import (
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"
)

func TestReloadBansRemovesOnlinePlayers(t *testing.T) {
	path := filepath.Join(t.TempDir(), "bans.txt")
	bans, err := LoadBans(path)
	if err != nil {
		t.Fatal(err)
	}
	s := startSetup(t, trusting(), nil, func(h *Hub) { h.bans = bans })
	a := s.login(t, 0x0110000100000D01, "STAYS")
	b := s.login(t, 0x0110000100000D02, "GOES")
	// the host edits the file while the server runs, then reloads
	line := strconv.FormatUint(0x0110000100000D02, 10) + " 0 cheating\n"
	if err := os.WriteFile(path, []byte("# edited by the host\n"+line), 0o600); err != nil {
		t.Fatal(err)
	}
	n, err := s.h.ReloadBans()
	if err != nil || n != 1 {
		t.Fatalf("%d %v", n, err)
	}
	if !closedSoon(b.c, 3*time.Second) {
		t.Fatal("the banned player must be removed")
	}
	a.sync()
	// a broken file keeps the old list
	if err := os.WriteFile(path, []byte("not a ban line\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := s.h.ReloadBans(); err == nil {
		t.Fatal("a broken ban file must be refused")
	}
	if _, banned := s.h.bans.Banned(0x0110000100000D02); !banned {
		t.Fatal("the old list must stay")
	}
}
