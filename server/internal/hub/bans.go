package hub

import (
	"bufio"
	"fmt"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

// Bans is the ban list by Steam ID, kept in a text file (one ban per line: steam id, until (unix seconds, 0 =
// for good), reason) so hosts can read and edit it. A kick's short block is a timed ban whose reason starts with
// "kicked". Expired bans are dropped from the file the next time it is saved.
type Bans struct {
	path string
	mu   sync.Mutex
	list map[uint64]Ban
}

// Ban is one entry: until when (zero: for good) and why.
type Ban struct {
	Until  time.Time
	Reason string
}

// BanInfo is a ban in force, with whom it bans (the admin's `bans`).
type BanInfo struct {
	SteamID uint64
	Ban
}

// kickedReason starts the reason of a kick's block ("kicked" or "kicked: <the admin's reason>").
const kickedReason = "kicked"

// Kick reports whether the ban is a kick's block (a timed ban the kick command made).
func (e Ban) Kick() bool { return !e.Until.IsZero() && strings.HasPrefix(e.Reason, kickedReason) }

// KickReason is a kick's reason as the admin gave it ("" for none).
func (e Ban) KickReason() string {
	return strings.TrimSpace(strings.TrimPrefix(strings.TrimPrefix(e.Reason, kickedReason), ":"))
}

func (e Ban) active(now time.Time) bool { return e.Until.IsZero() || now.Before(e.Until) }

// NewBans makes an empty list kept in path ("" = memory only).
func NewBans(path string) *Bans { return &Bans{path: path, list: map[uint64]Ban{}} }

// LoadBans reads the list (a missing file is an empty list).
func LoadBans(path string) (*Bans, error) {
	b := NewBans(path)
	f, err := os.Open(path)
	if os.IsNotExist(err) {
		return b, nil
	}
	if err != nil {
		return nil, err
	}
	defer f.Close()
	s := bufio.NewScanner(f)
	for line := 1; s.Scan(); line++ {
		text := strings.TrimSpace(s.Text())
		if text == "" || strings.HasPrefix(text, "#") {
			continue
		}
		fields := strings.SplitN(text, " ", 3)
		if len(fields) < 2 {
			return nil, fmt.Errorf("%s:%d: expected <steam id> <until> [reason]", path, line)
		}
		id, err1 := strconv.ParseUint(fields[0], 10, 64)
		until, err2 := strconv.ParseInt(fields[1], 10, 64)
		if err1 != nil || err2 != nil || id == 0 {
			return nil, fmt.Errorf("%s:%d: bad ban line", path, line)
		}
		e := Ban{}
		if until > 0 {
			e.Until = time.Unix(until, 0)
		}
		if len(fields) == 3 {
			e.Reason = fields[2]
		}
		b.list[id] = e
	}
	return b, s.Err()
}

// Reload reads the file again (a host edited it); on an error the list stays as it was.
func (b *Bans) Reload() error {
	if b.path == "" {
		return nil
	}
	fresh, err := LoadBans(b.path)
	if err != nil {
		return err
	}
	b.mu.Lock()
	b.list = fresh.list
	b.mu.Unlock()
	return nil
}

// Banned reports whether a Steam ID is banned now (and why).
func (b *Bans) Banned(id uint64) (string, bool) {
	e, ok := b.Lookup(id)
	return e.Reason, ok
}

// Lookup is the ban in force on a Steam ID now (kick blocks included).
func (b *Bans) Lookup(id uint64) (Ban, bool) {
	b.mu.Lock()
	defer b.mu.Unlock()
	e, ok := b.list[id]
	if !ok || !e.active(time.Now()) {
		return Ban{}, false
	}
	return e, true
}

// Active is the bans in force at `now`, the soonest to end first (bans for good last).
func (b *Bans) Active(now time.Time) []BanInfo {
	b.mu.Lock()
	var out []BanInfo
	for id, e := range b.list {
		if e.active(now) {
			out = append(out, BanInfo{id, e})
		}
	}
	b.mu.Unlock()
	sort.Slice(out, func(i, j int) bool {
		a, c := out[i].Until, out[j].Until
		if a.IsZero() != c.IsZero() {
			return c.IsZero()
		}
		if !a.Equal(c) {
			return a.Before(c)
		}
		return out[i].SteamID < out[j].SteamID
	})
	return out
}

// Add bans id until `until` (zero = for good) and saves the list.
func (b *Bans) Add(id uint64, until time.Time, reason string) error {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.list[id] = Ban{until, cleanReason(reason)}
	return b.saveLocked()
}

// Block bans id until `until` unless a ban that lasts longer is in force (a kick never shortens a ban), and saves
// the list.
func (b *Bans) Block(id uint64, until time.Time, reason string) error {
	b.mu.Lock()
	defer b.mu.Unlock()
	if e, ok := b.list[id]; ok && e.active(time.Now()) && (e.Until.IsZero() || !e.Until.Before(until)) {
		return nil
	}
	b.list[id] = Ban{until, cleanReason(reason)}
	return b.saveLocked()
}

// Remove lifts a ban and saves the list.
func (b *Bans) Remove(id uint64) error {
	b.mu.Lock()
	defer b.mu.Unlock()
	if _, ok := b.list[id]; !ok {
		return fmt.Errorf("%d is not banned", id)
	}
	delete(b.list, id)
	return b.saveLocked()
}

// cleanReason keeps a reason on its line of the file.
func cleanReason(reason string) string {
	return strings.Map(func(r rune) rune {
		if r < 0x20 || r == 0x7F {
			return -1
		}
		return r
	}, reason)
}

// saveLocked writes the list; bans that have ended are dropped (kicks would pile up otherwise).
func (b *Bans) saveLocked() error {
	now := time.Now()
	for id, e := range b.list {
		if !e.active(now) {
			delete(b.list, id)
		}
	}
	if b.path == "" {
		return nil
	}
	var sb strings.Builder
	sb.WriteString("# ttt2-server bans: <steam id> <until, unix seconds, 0 = for good> <reason>\n")
	for id, e := range b.list {
		until := int64(0)
		if !e.Until.IsZero() {
			until = e.Until.Unix()
		}
		fmt.Fprintf(&sb, "%d %d %s\n", id, until, e.Reason)
	}
	tmp := b.path + ".tmp"
	if err := os.WriteFile(tmp, []byte(sb.String()), 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, b.path)
}

// refusalText is what a banned or kicked player's game shows (ERROR 6, PROTOCOL.md): until when, and why.
func refusalText(e Ban, now time.Time) string {
	switch {
	case e.Kick():
		return "You were removed from this server. You can come back " + comeBack(e.Until.Sub(now)) + "." + reasonText(e.KickReason())
	case e.Until.IsZero():
		return "You are banned from this server for good." + reasonText(e.Reason)
	}
	return "You are banned from this server until " + e.Until.UTC().Format(time.DateOnly) + "." + reasonText(e.Reason)
}

// comeBack: when a kicked player can come back, as the game's message says it ("in 10 minutes").
func comeBack(d time.Duration) string {
	m := int(math.Ceil(d.Minutes()))
	switch {
	case m <= 1:
		return "in a minute"
	case m < 120:
		return fmt.Sprintf("in %d minutes", m)
	}
	return fmt.Sprintf("in %d hours", (m+59)/60)
}

func reasonText(reason string) string {
	if reason == "" {
		return ""
	}
	if len(reason) > 100 {
		cut := 100
		for cut > 0 && !utf8.RuneStart(reason[cut]) {
			cut--
		}
		reason = reason[:cut]
	}
	return " Reason: " + reason
}
