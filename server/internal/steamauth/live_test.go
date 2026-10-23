package steamauth

import (
	"bufio"
	"os"
	"strings"
	"testing"

	"ttt2-server/internal/config"
)

// TestLiveSteam asks Valve about a real ticket - only when both are given:
//
//	TTT2_STEAM_LIVE_CONFIG = a server.toml with steam_web_api_key (the key is never printed)
//	TTT2_STEAM_LIVE_TICKET = a file "identity\nticket hex" (the game's TTT2_SERVER_TICKET_DUMP)
//
// It reports what Steam answers for: the ticket's own identity, another identity, no identity, and the same
// ticket again - the facts the server's ticket binding relies on (COMMUNITY_SERVER_PLAN.md S7).
func TestLiveSteam(t *testing.T) {
	cfgPath, ticketPath := os.Getenv("TTT2_STEAM_LIVE_CONFIG"), os.Getenv("TTT2_STEAM_LIVE_TICKET")
	if cfgPath == "" || ticketPath == "" {
		t.Skip("live Steam check: set TTT2_STEAM_LIVE_CONFIG and TTT2_STEAM_LIVE_TICKET")
	}
	f, err := os.Open(cfgPath)
	if err != nil {
		t.Fatal(err)
	}
	cfg, err := config.Parse(bufio.NewScanner(f), cfgPath)
	f.Close()
	if err != nil || cfg.SteamWebAPIKey == "" {
		t.Fatalf("no steam_web_api_key in %s (%v)", cfgPath, err)
	}
	raw, err := os.ReadFile(ticketPath)
	if err != nil {
		t.Fatal(err)
	}
	lines := strings.Split(strings.TrimSpace(string(raw)), "\n")
	if len(lines) != 2 || lines[1] == "" {
		t.Fatalf("%s: expected identity and ticket lines", ticketPath)
	}
	identity, ticket := strings.TrimSpace(lines[0]), strings.TrimSpace(lines[1])
	w := WebAPI{Key: cfg.SteamWebAPIKey, AppID: cfg.SteamAppID}
	cases := []struct{ name, identity string }{
		{"its own identity", identity},
		{"another identity", "ttt2-server:00000000000000000000000000000000"},
		{"no identity", ""},
		{"its own identity again", identity},
	}
	// TTT2_STEAM_LIVE_IDENTITIES: more identities to try, comma separated
	for _, extra := range strings.Split(os.Getenv("TTT2_STEAM_LIVE_IDENTITIES"), ",") {
		if extra != "" {
			cases = append(cases, struct{ name, identity string }{"try " + extra, extra})
		}
	}
	for _, c := range cases {
		id, err := w.verify(ticket, c.identity)
		if err != nil {
			t.Logf("%-24s -> refused: %v", c.name, err)
		} else {
			t.Logf("%-24s -> accepted, Steam ID %d", c.name, id)
		}
	}
}
