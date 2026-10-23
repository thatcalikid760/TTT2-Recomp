package hub

import (
	"bytes"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"ttt2-server/internal/config"
	"ttt2-server/internal/wire"
)

// refused logs in and expects an ERROR 6; it returns its text.
func (s *server) refused(t *testing.T, peer uint64) string {
	t.Helper()
	c, typ, r := s.handshake(t, hello, auth(peer, "BACK", nil, ""))
	code, text := r.U32(), r.Str()
	if typ != wire.Error || code != wire.ErrBanned {
		t.Fatalf("login of %d: got frame %d code %d, want ERROR 6", peer, typ, code)
	}
	c.Close()
	return text
}

// removedText waits for the ERROR a removed player gets and returns its text.
func (cl *client) removedText() string {
	for {
		typ, r := cl.next()
		if typ == wire.Error {
			if code := r.U32(); code != wire.ErrBanned {
				cl.t.Fatalf("ERROR %d, want 6", code)
			}
			return r.Str()
		}
	}
}

func TestKickKeepsOutForMinutes(t *testing.T) {
	path := filepath.Join(t.TempDir(), "bans.txt")
	bans, err := LoadBans(path)
	if err != nil {
		t.Fatal(err)
	}
	s := startSetup(t, trusting(), nil, func(h *Hub) { h.bans = bans })
	const griefer, other = 0x0110000100000E01, 0x0110000100000E02
	a := s.login(t, griefer, "Bad Guy")
	if n, err := s.h.Kick("bad guy", 10, "griefing"); n != 1 || err != nil {
		t.Fatalf("%d %v", n, err)
	}
	want := "You were removed from this server. You can come back in 10 minutes. Reason: griefing"
	if text := a.removedText(); text != want {
		t.Fatalf("kicked player told %q", text)
	}
	if text := s.refused(t, griefer); text != want {
		t.Fatalf("a kicked player coming back is told %q", text)
	}
	file, _ := os.ReadFile(path)
	if !strings.Contains(string(file), strconv.FormatUint(griefer, 10)+" ") || !strings.Contains(string(file), " kicked: griefing\n") {
		t.Fatalf("bans.txt:\n%s", file)
	}
	if list := s.h.Bans(); len(list) != 1 || !list[0].Kick() || list[0].KickReason() != "griefing" ||
		list[0].Until.Sub(time.Now()) > 10*time.Minute {
		t.Fatalf("%+v", list)
	}

	// a kick without a block: back at once, nothing in the list
	b := s.login(t, other, "OTHER")
	if n, _ := s.h.Kick(strconv.FormatUint(other, 10), 0, ""); n != 1 {
		t.Fatal("kick by Steam ID")
	}
	if text := b.removedText(); text != "You were removed from this server." {
		t.Fatalf("%q", text)
	}
	b.c.Close() // as the game does after an ERROR; then it may come back at once
	for deadline := time.Now().Add(3 * time.Second); len(s.h.find(strconv.FormatUint(other, 10))) != 0; {
		if time.Now().After(deadline) {
			t.Fatal("the kicked connection stayed")
		}
		time.Sleep(20 * time.Millisecond)
	}
	s.login(t, other, "OTHER")
	if len(s.h.Bans()) != 1 {
		t.Fatal("a kick for 0 minutes must not be a ban")
	}
	if _, err := s.h.Kick("nobody", 10, ""); err == nil {
		t.Fatal("kicking nobody")
	}
}

func TestKickBlockExpiresAndIsPruned(t *testing.T) {
	path := filepath.Join(t.TempDir(), "bans.txt")
	bans, _ := LoadBans(path)
	s := startSetup(t, trusting(), nil, func(h *Hub) { h.bans = bans })
	const kicked, banned = 0x0110000100000E11, 0x0110000100000E12
	bans.Block(kicked, time.Now().Add(150*time.Millisecond), "kicked")
	if !strings.Contains(s.refused(t, kicked), "You can come back in a minute.") {
		t.Fatal("a block about to end")
	}
	time.Sleep(200 * time.Millisecond)
	s.login(t, kicked, "BACK AGAIN")
	if len(s.h.Bans()) != 0 {
		t.Fatal("an ended block is not in force")
	}
	// the next save drops it from the file
	if _, err := s.h.Ban(strconv.FormatUint(banned, 10), 0, "cheating"); err != nil {
		t.Fatal(err)
	}
	if file, _ := os.ReadFile(path); strings.Contains(string(file), strconv.FormatUint(kicked, 10)) {
		t.Fatalf("an ended block must be pruned:\n%s", file)
	}
	// a kick never shortens a ban
	bans.Block(banned, time.Now().Add(time.Minute), "kicked")
	if e, ok := bans.Lookup(banned); !ok || !e.Until.IsZero() || e.Reason != "cheating" {
		t.Fatalf("%+v", e)
	}
}

func TestBanTexts(t *testing.T) {
	s := start(t, trusting(), nil)
	const timed, forever = 0x0110000100000E21, 0x0110000100000E22
	a := s.login(t, timed, "TIMED")
	if n, err := s.h.Ban("TIMED", 7, "cheating"); n != 1 || err != nil {
		t.Fatal(n, err)
	}
	until := time.Now().Add(7 * 24 * time.Hour).UTC().Format(time.DateOnly)
	want := "You are banned from this server until " + until + ". Reason: cheating"
	if text := a.removedText(); text != want {
		t.Fatalf("%q, want %q", text, want)
	}
	if text := s.refused(t, timed); text != want {
		t.Fatalf("%q", text)
	}
	s.h.Ban(strconv.FormatUint(forever, 10), 0, "")
	if text := s.refused(t, forever); text != "You are banned from this server for good." {
		t.Fatalf("%q", text)
	}
	for d, want := range map[time.Duration]string{30 * time.Second: "in a minute", 10 * time.Minute: "in 10 minutes",
		119 * time.Minute: "in 119 minutes", 125 * time.Minute: "in 3 hours", 24 * time.Hour: "in 24 hours"} {
		if got := comeBack(d); got != want {
			t.Errorf("comeBack(%s) = %q, want %q", d, got, want)
		}
	}
}

// INFO is anonymous: it never says anything about bans, whatever a request carries in its padding (games of test
// build 7d's first days put their Steam ID there), and a banned ID in it costs the server nothing more.
func TestInfoTellsNoBans(t *testing.T) {
	s := start(t, trusting(), nil)
	const banned = 0x0110000100000E32
	s.h.bans.Add(banned, time.Time{}, "cheating")
	u := s.udpConn(t)
	var w wire.Writer
	w.U8(wire.InfoReq)
	w.U32(wire.InfoMagic)
	w.U64(9)
	w.U32(0x44495454) // "TTID"
	w.U64(banned)
	w.Raw(make([]byte, wire.InfoReqMinSize-len(w.B)))
	u.Write(w.B)
	buf := make([]byte, 2048)
	u.SetReadDeadline(time.Now().Add(time.Second))
	n, err := u.Read(buf)
	if err != nil {
		t.Fatal("no INFO answer")
	}
	r := wire.Reader{B: buf[:n]}
	if r.U8() != wire.Info || r.U32() != wire.InfoMagic || r.U64() != 9 || r.U16() != wire.ProtocolVersion {
		t.Fatal("INFO header")
	}
	if flags := r.U8(); flags&^3 != 0 {
		t.Fatalf("INFO flags %#x: only password and Steam check", flags)
	}
	r.Str()
	r.Str()
	r.U16()
	r.U16()
	r.U16()
	r.U16()
	r.Str()
	if r.Str() != proto || r.Err() != nil || len(r.B) != 0 {
		t.Fatalf("INFO body (%d bytes after the protocols)", len(r.B))
	}
}

// On a server that checks Steam IDs a ban is told only to the player it bans: someone claiming a banned Steam ID
// without that account's ticket hears "Steam could not confirm", not the ban. The banned player's ERROR 6 carries
// until when (0 = for good), after the text.
func TestBanToldOnlyAfterSteamCheck(t *testing.T) {
	const real, claimed = 0x0110000100000042, 0x0110000100000E77
	s := start(t, config.Default(), fakeVerifier{id: real})
	until := time.Now().Add(48 * time.Hour).Truncate(time.Second)
	s.h.bans.Add(real, until, "cheating")
	s.h.bans.Add(claimed, time.Time{}, "for good")
	ticket := bytes.Repeat([]byte("ab"), 40)
	// the ticket proves `real`: claiming `claimed` (banned too) fails the Steam check before any ban is looked at
	c, typ, r := s.handshake(t, hello, auth(claimed, "FAKE", ticket, ""))
	if code := r.U32(); typ != wire.Error || code != wire.ErrAuth {
		t.Fatalf("a claimer of a banned ID got frame %d code %d, want ERROR %d", typ, code, wire.ErrAuth)
	}
	if text := r.Str(); strings.Contains(text, "banned") {
		t.Fatalf("a claimer learned about the ban: %q", text)
	}
	c.Close()
	c, typ, r = s.handshake(t, hello, auth(real, "ME", ticket, ""))
	if code := r.U32(); typ != wire.Error || code != wire.ErrBanned {
		t.Fatalf("the banned player got frame %d code %d", typ, code)
	}
	if text := r.Str(); !strings.Contains(text, "until") || !strings.Contains(text, "cheating") {
		t.Fatalf("the banned player was told %q", text)
	}
	if got := r.U64(); r.Err() != nil || got != uint64(until.Unix()) {
		t.Fatalf("until %d (%v), want %d", got, r.Err(), until.Unix())
	}
	c.Close()
}

// A kick's ERROR 6 carries the end of its block; a kick without one carries now (the game keeps nothing out).
func TestKickSaysUntil(t *testing.T) {
	s := start(t, trusting(), nil)
	const blocked, free = 0x0110000100000E41, 0x0110000100000E42
	a := s.login(t, blocked, "Blocked")
	b := s.login(t, free, "Free")
	before := time.Now()
	if _, err := s.h.Kick("blocked", 5, ""); err != nil {
		t.Fatal(err)
	}
	if _, err := s.h.Kick("free", 0, ""); err != nil {
		t.Fatal(err)
	}
	untilOf := func(cl *client) uint64 {
		for {
			typ, r := cl.next()
			if typ == wire.Error {
				r.U32()
				r.Str()
				return r.U64()
			}
		}
	}
	if u := untilOf(a); u < uint64(before.Add(5*time.Minute).Unix()) || u > uint64(time.Now().Add(5*time.Minute).Unix()) {
		t.Fatalf("a 5-minute kick ends at %d", u)
	}
	if u := untilOf(b); u < uint64(before.Unix()) || u > uint64(time.Now().Unix()) {
		t.Fatalf("a kick without a block ends at %d, want now", u)
	}
}
