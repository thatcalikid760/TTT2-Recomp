package panel

import (
	"bytes"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"
)

// Reading <data dir>/admin_audit.log back for the Audit log page and a player's page: only its end (auditTailBytes,
// whatever the file's size), only whole lines, at most auditMaxLines, each cut to auditMaxLine bytes; html/template
// escapes them (a reason is a player's or an admin's text).

const (
	auditTailBytes = 256 << 10
	auditMaxLines  = 500
	auditMaxLine   = 1000
)

// auditLine is one line of the audit log.
type auditLine struct {
	At   time.Time // zero: the line had no time
	Text string
	Kind string // login, failed, lockout, logout, kick, ban, unban, session, other (a CSS class)
}

// auditTail is the audit log's newest lines (newest first) that contain `search` (any case; "" all), at most `max`,
// and whether the file is longer than what was read.
func (p *Panel) auditTail(search string, max int) ([]auditLine, bool, error) {
	f, err := os.Open(filepath.Join(p.dataDir, auditFile))
	if isNotExist(err) {
		return nil, false, nil
	}
	if err != nil {
		return nil, false, err
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		return nil, false, err
	}
	start := max64(st.Size()-auditTailBytes, 0)
	buf := make([]byte, st.Size()-start)
	if _, err := f.ReadAt(buf, start); err != nil && err != io.EOF {
		return nil, false, err
	}
	if start > 0 {
		if i := bytes.IndexByte(buf, '\n'); i >= 0 {
			buf = buf[i+1:] // a cut line
		}
	}
	search = strings.ToLower(search)
	lines := bytes.Split(bytes.TrimRight(buf, "\n"), []byte("\n"))
	var out []auditLine
	for i := len(lines) - 1; i >= 0 && len(out) < max; i-- {
		raw := strings.TrimRight(string(lines[i]), "\r")
		if raw == "" || (search != "" && !strings.Contains(strings.ToLower(raw), search)) {
			continue
		}
		out = append(out, parseAuditLine(raw))
	}
	return out, start > 0, nil
}

func max64(a, b int64) int64 {
	if a > b {
		return a
	}
	return b
}

// parseAuditLine splits "2006-01-02 15:04:05 panel: ..." into its time and text, and says what kind of line it is.
func parseAuditLine(raw string) auditLine {
	if len(raw) > auditMaxLine {
		cut := auditMaxLine
		for cut > 0 && !utf8.RuneStart(raw[cut]) {
			cut--
		}
		raw = raw[:cut] + "..."
	}
	l := auditLine{Text: raw}
	if len(raw) > len(time.DateTime) {
		if t, err := time.ParseInLocation(time.DateTime, raw[:len(time.DateTime)], time.Local); err == nil {
			l.At, l.Text = t, strings.TrimSpace(raw[len(time.DateTime):])
		}
	}
	text := strings.TrimPrefix(l.Text, "panel: ")
	switch {
	case strings.Contains(text, "failed login"):
		l.Kind = "failed"
	case strings.Contains(text, "locked out"), strings.Contains(text, "paused"):
		l.Kind = "lockout"
	case strings.HasSuffix(text, "logged in"):
		l.Kind = "login"
	case strings.HasSuffix(text, "logged out"):
		l.Kind = "logout"
	case strings.Contains(text, " unbanned "):
		l.Kind = "unban"
	case strings.Contains(text, " banned "):
		l.Kind = "ban"
	case strings.Contains(text, " kicked "):
		l.Kind = "kick"
	case strings.Contains(text, "session"), strings.Contains(text, "login"), strings.Contains(text, "password"):
		l.Kind = "session"
	default:
		l.Kind = "other"
	}
	return l
}

// auditActionRE finds the panel's actions on a Steam ID in the audit log ("kicked <id> ...", "banned <id> ...",
// "unbanned <id>").
var auditActionRE = regexp.MustCompile(`\b(kicked|banned|unbanned) ([0-9]{1,20})\b`)

// auditOf is the audit log's panel actions on one player (newest first, at most max).
func (p *Panel) auditOf(id uint64, max int) ([]auditLine, error) {
	want := strconv.FormatUint(id, 10)
	lines, _, err := p.auditTail(want, auditMaxLines)
	var out []auditLine
	for _, l := range lines {
		for _, m := range auditActionRE.FindAllStringSubmatch(l.Text, -1) {
			if m[2] == want {
				out = append(out, l)
				break
			}
		}
		if len(out) == max {
			break
		}
	}
	return out, err
}
