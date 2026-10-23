package panel

import (
	"bytes"
	"context"
	"crypto/subtle"
	"embed"
	"fmt"
	"html/template"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"ttt2-server/internal/config"
	"ttt2-server/internal/hub"
)

// The panel's pages: server-rendered html/template (every text a player or a game sent is escaped), one stylesheet,
// one script (panel.js: live updates, dialogs, countdowns, filters - every page and action works without it). GET
// only shows; every change is a POST form with the session's CSRF token.

//go:embed pages
var pagesFS embed.FS

const (
	maxBody    = 16 << 10 // a request body (the forms are small)
	maxReason  = 200      // characters of a kick's or ban's reason
	maxSearch  = 64       // characters of a search
	maxBanDays = 36500
	// csp: the panel's own stylesheet, script and live stream, and forms to itself; nothing inline, nothing from
	// another site, no frames; scripts may not write HTML from strings (Trusted Types, where the browser has them)
	csp = "default-src 'none'; script-src 'self'; connect-src 'self'; style-src 'self'; img-src 'self'; " +
		"form-action 'self'; frame-ancestors 'none'; base-uri 'none'; require-trusted-types-for 'script'; trusted-types 'none'"
)

// Handler is the panel's HTTP handler (the server wraps it in TLS).
func (p *Panel) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /panel.css", serveFile("pages/panel.css", "text/css; charset=utf-8"))
	mux.HandleFunc("GET /panel.js", serveFile("pages/panel.js", "text/javascript; charset=utf-8"))
	mux.HandleFunc("GET /favicon.svg", serveFile("pages/favicon.svg", "image/svg+xml"))
	mux.HandleFunc("GET /login", p.loginPage)
	mux.HandleFunc("POST /login", p.loginPost)
	mux.HandleFunc("POST /logout", p.action(p.logout))
	mux.HandleFunc("GET /{$}", p.page(p.dashboardPage))
	mux.HandleFunc("GET /players", p.page(p.playersPage))
	mux.HandleFunc("GET /player", p.page(p.playerPage))
	mux.HandleFunc("GET /bans", p.page(p.bansPage))
	mux.HandleFunc("GET /matches", p.page(p.matchesPage))
	mux.HandleFunc("GET /audit", p.page(p.auditPage))
	mux.HandleFunc("GET /events", p.events)
	mux.HandleFunc("POST /kick", p.action(p.kick))
	mux.HandleFunc("POST /ban", p.action(p.ban))
	mux.HandleFunc("POST /unban", p.action(p.unban))
	return p.guard(mux)
}

// guard comes first on every request: an address outside admin_panel_allow gets a bare 403 (the listener closes
// such connections already); every answer carries the security headers; bodies are small.
func (p *Panel) guard(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !p.allowed(clientIP(r)) {
			w.Header().Set("Connection", "close")
			w.WriteHeader(http.StatusForbidden)
			return
		}
		h := w.Header()
		h.Set("Content-Security-Policy", csp)
		h.Set("X-Frame-Options", "DENY")
		h.Set("X-Content-Type-Options", "nosniff")
		// same-origin, not no-referrer: with no-referrer a browser sends "Origin: null" on the panel's own forms,
		// and the same-origin check needs the real one; nothing is ever sent to another site either way
		h.Set("Referrer-Policy", "same-origin")
		h.Set("Cache-Control", "no-store")
		h.Set("Cross-Origin-Opener-Policy", "same-origin")
		h.Set("Cross-Origin-Resource-Policy", "same-origin")
		h.Set("Permissions-Policy", "camera=(), microphone=(), geolocation=(), payment=(), usb=()")
		if p.realCert.Load() {
			// only with a real certificate: HSTS would stop the browser from letting its owner past the
			// self-signed certificate's warning
			h.Set("Strict-Transport-Security", "max-age=31536000")
		}
		r.Body = http.MaxBytesReader(w, r.Body, maxBody)
		next.ServeHTTP(w, r)
	})
}

// serveFile serves one embedded file.
func serveFile(name, contentType string) http.HandlerFunc {
	b, err := pagesFS.ReadFile(name)
	if err != nil {
		panic(err)
	}
	return func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", contentType)
		w.Write(b)
	}
}

// view is what a page template gets.
type view struct {
	page        string // the template
	Title       string
	Nav         string // the menu entry it is
	Server      string
	CSRF        string // "": the login page (no menu, no status)
	Flash       string
	FlashErr    bool
	Back        string // this page (path and query): where its forms come back to
	Now         time.Time
	Started     time.Time // the server's start (uptime)
	Online      int
	Playing     int
	Bans        int
	KickMinutes int
	Zone        string // the server's time zone, as the pages' times are in it
	LiveSince   string // the activity feed's newest event id (panel.js asks for what comes after)
	Data        any
}

// page serves a page to a logged-in admin (anyone else goes to the login page); fill gives it its data.
func (p *Panel) page(fill func(r *http.Request, v *view) error) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		s := p.sessionOf(r)
		if s == nil {
			http.Redirect(w, r, "/login", http.StatusSeeOther)
			return
		}
		st := p.h.Stats()
		now := time.Now()
		zone, _ := now.Zone()
		cfg := p.h.Config()
		v := &view{Server: cfg.Name, CSRF: s.csrf, Back: r.URL.Path, Now: now, Started: now.Add(-st.Uptime), Online: st.Online,
			Playing: st.InMatches, Bans: len(p.h.Bans()), KickMinutes: cfg.KickMinutes, Zone: zone, LiveSince: p.live.lastID()}
		if r.URL.RawQuery != "" {
			v.Back += "?" + r.URL.RawQuery
		}
		p.mu.Lock()
		v.Flash, v.FlashErr, s.flash, s.flashErr = s.flash, s.flashErr, "", false
		p.mu.Unlock()
		status := http.StatusOK
		if err := fill(r, v); err != nil {
			v.Flash, v.FlashErr = "Error: "+err.Error(), true
			if e, ok := err.(pageError); ok {
				status = e.status
			}
		}
		p.render(w, status, v)
	}
}

// pageError is a page's error with its HTTP status (a bad Steam ID: 400).
type pageError struct {
	status int
	text   string
}

func (e pageError) Error() string { return e.text }

// render executes a page into a buffer first: a failing template never sends half a page.
func (p *Panel) render(w http.ResponseWriter, status int, v *view) {
	var b bytes.Buffer
	if err := p.pages.ExecuteTemplate(&b, v.page, v); err != nil {
		p.log.Printf("admin panel: page %s: %v", v.page, err)
		http.Error(w, "internal error", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.WriteHeader(status)
	w.Write(b.Bytes())
}

// action serves a state-changing POST: of a logged-in admin, from the panel's own page (same origin), with that
// session's CSRF token - anything else is refused (403) before the action runs.
func (p *Panel) action(do func(w http.ResponseWriter, r *http.Request, s *session, ip string)) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		readRequest(r)
		s := p.sessionOf(r)
		if s == nil {
			http.Error(w, "forbidden: not logged in (log in again)", http.StatusForbidden)
			return
		}
		ip := clientIP(r).String()
		if !sameOrigin(r) {
			p.log.Printf("panel: a request from %s refused: not from the panel's own page", ip)
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		if subtle.ConstantTimeCompare([]byte(r.PostFormValue("csrf")), []byte(s.csrf)) != 1 {
			p.log.Printf("panel: a request from %s refused: wrong CSRF token", ip)
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		do(w, r, s, ip)
	}
}

// readRequest reads a POST's (small) form before anything is answered, refusals too: a connection closed with
// unread input is reset, and the reset can take the answer with it (a client that sent "Connection: close" lost the
// end of a refused login's page so).
func readRequest(r *http.Request) { r.ParseForm() }

// done answers an action that was done: its message on the next page, which is the one the form came from.
func (p *Panel) done(w http.ResponseWriter, r *http.Request, s *session, format string, args ...any) {
	p.answer(w, r, s, false, format, args...)
}

// failed answers an action that was not done.
func (p *Panel) failed(w http.ResponseWriter, r *http.Request, s *session, format string, args ...any) {
	p.answer(w, r, s, true, format, args...)
}

func (p *Panel) answer(w http.ResponseWriter, r *http.Request, s *session, failed bool, format string, args ...any) {
	p.mu.Lock()
	s.flash, s.flashErr = fmt.Sprintf(format, args...), failed
	p.mu.Unlock()
	http.Redirect(w, r, backTo(r), http.StatusSeeOther)
}

// The pages a form may come back to, and the parts of their address kept.
var (
	backPaths = map[string]bool{"/": true, "/players": true, "/player": true, "/bans": true, "/matches": true, "/audit": true}
	backKeys  = []string{"q", "show", "kind", "page", "steam"}
)

// backTo is the page an action's form came from ("back": its path and query), rebuilt from the pages and keys the
// panel has - never another site or path. The forms of earlier versions said "online", "players", ... with "q".
func backTo(r *http.Request) string {
	back := r.PostFormValue("back")
	switch back {
	case "", "online":
		return "/"
	case "players", "bans", "matches":
		back = "/" + back
		if q := r.PostFormValue("q"); q != "" {
			back += "?" + url.Values{"q": {cut(q, maxSearch)}}.Encode()
		}
		return back
	}
	u, err := url.Parse(back)
	if err != nil || u.Scheme != "" || u.Host != "" || u.User != nil || u.Opaque != "" || !backPaths[u.Path] {
		return "/"
	}
	in, out := u.Query(), url.Values{}
	for _, k := range backKeys {
		if v := in.Get(k); v != "" {
			out.Set(k, cut(v, maxSearch))
		}
	}
	if len(out) == 0 {
		return u.Path
	}
	return u.Path + "?" + out.Encode()
}

// cut is s at most n characters long.
func cut(s string, n int) string {
	if utf8.RuneCountInString(s) > n {
		s = string([]rune(s)[:n])
	}
	return s
}

// ---- logging in and out

func (p *Panel) loginPage(w http.ResponseWriter, r *http.Request) {
	if p.sessionOf(r) != nil {
		http.Redirect(w, r, "/", http.StatusSeeOther)
		return
	}
	p.render(w, http.StatusOK, &view{page: "login", Title: "Log in", Server: p.h.Config().Name})
}

// loginPost checks a login: password and authenticator code, within the limits (guard.go); a refusal is answered a
// second after the request arrived and says nothing about which part was wrong (the log does).
func (p *Panel) loginPost(w http.ResponseWriter, r *http.Request) {
	arrived := time.Now()
	readRequest(r)
	if !sameOrigin(r) {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}
	ip := clientIP(r)
	key := limitKey(ip)
	p.mu.Lock()
	refusal, ok := p.limits.begin(key, p.now())
	p.mu.Unlock()
	if !ok {
		p.refuseLogin(w, arrived, http.StatusTooManyRequests, refusal)
		return
	}
	why := p.checkLogin(r.Context(), r.PostFormValue("password"), strings.ReplaceAll(r.PostFormValue("code"), " ", ""))
	now := p.now()
	p.mu.Lock()
	lockedOut, paused := p.limits.end(key, now, why != "")
	var token string
	var err error
	if why == "" {
		token, err = p.newSessionLocked(ip.String(), now)
	}
	p.mu.Unlock()
	if why != "" {
		p.audit("panel: failed login from %s (%s)", ip, why)
		if lockedOut {
			p.audit("panel: %s locked out for %s (%d failed logins)", key, addrBlockTime, addrFailLimit)
		}
		if paused {
			p.audit("panel: every login paused for %s (%d failed logins in %s)", allPauseTime, allFailLimit, allFailWindow)
		}
		p.refuseLogin(w, arrived, http.StatusUnauthorized, "Wrong password or code.")
		return
	}
	if err != nil {
		http.Error(w, "internal error", http.StatusInternalServerError)
		return
	}
	p.audit("panel: admin from %s logged in", ip)
	setCookie(w, token)
	http.Redirect(w, r, "/", http.StatusSeeOther)
}

// checkLogin is "" for the right password with a right code not used before (the code is used then), else what
// was wrong (for the log). Both parts are always checked; a few at a time (PBKDF2 is slow on purpose).
func (p *Panel) checkLogin(ctx context.Context, password, code string) string {
	select {
	case p.verify <- struct{}{}:
		defer func() { <-p.verify }()
	case <-ctx.Done():
		return "the request ended"
	}
	p.mu.Lock()
	creds := p.creds
	p.mu.Unlock()
	if creds == nil {
		return "no login"
	}
	passwordOK := creds.Password.verify(password)
	now := p.now()
	step, codeOK := matchCode(creds.secret(), code, now)
	switch {
	case !passwordOK && !codeOK:
		return "wrong password and code"
	case !passwordOK:
		return "wrong password"
	case !codeOK:
		return "wrong code"
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.creds != creds {
		return "the login changed meanwhile"
	}
	for s, until := range p.used {
		if !now.Before(until) {
			delete(p.used, s)
		}
	}
	if _, used := p.used[step]; used {
		return "code already used"
	}
	p.used[step] = now.Add(usedCodeMemory)
	return ""
}

// refuseLogin answers a refused login failDelay after it arrived.
func (p *Panel) refuseLogin(w http.ResponseWriter, arrived time.Time, status int, message string) {
	time.Sleep(time.Until(arrived.Add(failDelay)))
	p.render(w, status, &view{page: "login", Title: "Log in", Server: p.h.Config().Name, Flash: message, FlashErr: true})
}

func (p *Panel) logout(w http.ResponseWriter, r *http.Request, s *session, ip string) {
	p.endSession(r)
	setCookie(w, "")
	p.audit("panel: admin from %s logged out", ip)
	http.Redirect(w, r, "/login", http.StatusSeeOther)
}

// ---- actions (the same hub methods the admin commands use)

func (p *Panel) kick(w http.ResponseWriter, r *http.Request, s *session, ip string) {
	id, idErr := steamID(r)
	minutes, err := formNumber(r, "minutes")
	reason, reasonErr := reasonOf(r)
	switch {
	case idErr != nil:
		p.failed(w, r, s, "%v", idErr)
	case err != nil || minutes < 0 || minutes > config.MaxKickMinutes:
		p.failed(w, r, s, "Kick: minutes 0..%d (longer: a ban).", config.MaxKickMinutes)
	case reasonErr != nil:
		p.failed(w, r, s, "%v", reasonErr)
	default:
		name := p.who(id)
		if _, err := p.h.Kick(strconv.FormatUint(id, 10), minutes, reason); err != nil {
			p.failed(w, r, s, "Not kicked: %v.", err)
			return
		}
		p.audit("panel: admin from %s kicked %d for %d minutes%s", ip, id, minutes, reasonSuffix(reason))
		if minutes == 0 {
			// no block in the ban list for the live view to see: the feed is told
			p.live.note(evKick, []liveWho{{steamText(id), p.nameOf(id)}}, "kicked (can come back at once)"+reasonTail(reason))
			p.done(w, r, s, "Kicked %s (they can come back at once).", name)
			return
		}
		p.done(w, r, s, "Kicked %s for %d minutes.", name, minutes)
	}
}

func (p *Panel) ban(w http.ResponseWriter, r *http.Request, s *session, ip string) {
	id, idErr := steamID(r)
	days, err := formNumber(r, "days")
	reason, reasonErr := reasonOf(r)
	switch {
	case idErr != nil:
		p.failed(w, r, s, "%v", idErr)
	case err != nil || days < 0 || days > maxBanDays:
		p.failed(w, r, s, "Ban: days 0..%d (0 = for good).", maxBanDays)
	case reasonErr != nil:
		p.failed(w, r, s, "%v", reasonErr)
	default:
		if _, err := p.h.Ban(strconv.FormatUint(id, 10), days, reason); err != nil {
			p.failed(w, r, s, "Not banned: %v.", err)
			return
		}
		long := fmt.Sprintf("%d days", days)
		if days == 0 {
			long = "for good"
		}
		p.audit("panel: admin from %s banned %d %s%s", ip, id, long, reasonSuffix(reason))
		p.done(w, r, s, "Banned %s %s.", p.who(id), long)
	}
}

func (p *Panel) unban(w http.ResponseWriter, r *http.Request, s *session, ip string) {
	id, err := steamID(r)
	if err == nil {
		err = p.h.Unban(id)
	}
	if err != nil {
		p.failed(w, r, s, "Not unbanned: %v.", err)
		return
	}
	p.audit("panel: admin from %s unbanned %d", ip, id)
	p.done(w, r, s, "Unbanned %s.", p.who(id))
}

func steamID(r *http.Request) (uint64, error) {
	id, err := strconv.ParseUint(strings.TrimSpace(r.PostFormValue("steam_id")), 10, 64)
	if err != nil || id == 0 {
		return 0, fmt.Errorf("no Steam ID")
	}
	return id, nil
}

// formNumber is a form's number: its field, or - when a preset list's "custom" is chosen - <field>_custom.
func formNumber(r *http.Request, name string) (int, error) {
	v := strings.TrimSpace(r.PostFormValue(name))
	if v == "custom" {
		v = strings.TrimSpace(r.PostFormValue(name + "_custom"))
	}
	return strconv.Atoi(v)
}

// reasonOf is a form's reason: at most maxReason characters, control characters dropped (it goes into the log and
// the ban file, a line each).
func reasonOf(r *http.Request) (string, error) {
	reason := strings.TrimSpace(strings.Map(func(c rune) rune {
		if c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0) || c == utf8.RuneError {
			return -1
		}
		return c
	}, r.PostFormValue("reason")))
	if utf8.RuneCountInString(reason) > maxReason {
		return "", fmt.Errorf("the reason is longer than %d characters", maxReason)
	}
	return reason, nil
}

func reasonSuffix(reason string) string {
	if reason == "" {
		return ""
	}
	return ": " + reason
}

// ---- the templates

func parsePages() *template.Template {
	return template.Must(template.New("").Funcs(template.FuncMap{
		"datetime": func(t time.Time) string {
			if t.IsZero() {
				return "-"
			}
			return t.Format("2006-01-02 15:04")
		},
		"iso": func(t time.Time) string { return t.Format(time.RFC3339) },
		"unix": func(t time.Time) int64 {
			if t.IsZero() {
				return 0
			}
			return t.Unix()
		},
		"fromUnix": func(s int64) time.Time { return time.Unix(s, 0) },
		"ago": func(now, t time.Time) string {
			if now.Sub(t) < 10*time.Second {
				return "just now"
			}
			return roundDuration(now.Sub(t)) + " ago"
		},
		"at":     func(now, t time.Time) timeArg { return timeArg{now, t} },
		"atUnix": func(now time.Time, t int64) timeArg { return timeArg{now, time.Unix(t, 0)} },
		"target": func(v *view, id any, name string, kick bool) formArg {
			return formArg{CSRF: v.CSRF, ID: fmt.Sprint(id), Name: name, Back: v.Back, KickMinutes: v.KickMinutes, Kick: kick}
		},
		"blank":    func(v *view) formArg { return formArg{CSRF: v.CSRF, Back: v.Back, KickMinutes: v.KickMinutes} },
		"dur":      roundDuration,
		"between":  func(a, b time.Time) string { return roundDuration(b.Sub(a)) },
		"steamURL": func(id uint64) string { return "https://steamcommunity.com/profiles/" + strconv.FormatUint(id, 10) },
		"add":      func(a, b int) int { return a + b },
		"pageURL":  pageURL,
		"until":    untilText,
		"matchTable": func(v *view, rows []matchRow, self any) matchTableArg {
			return matchTableArg{Now: v.Now, Rows: rows, Self: fmt.Sprint(self)}
		},
		"auditList": func(v *view, lines []auditLine) auditListArg { return auditListArg{Now: v.Now, Lines: lines} },
		"pager": func(path string, page, pages int, kv ...string) pagerArg {
			a := pagerArg{Text: fmt.Sprintf("Page %d of %d", page, pages), PrevLabel: "Previous", NextLabel: "Next"}
			if page > 1 {
				a.Prev = pageURL(path, min(page-1, pages), kv...)
			}
			if page < pages {
				a.Next = pageURL(path, page+1, kv...)
			}
			return a
		},
		"pagerMore": func(path string, page int, more bool, kv ...string) pagerArg {
			a := pagerArg{Text: fmt.Sprintf("Page %d", page), PrevLabel: "Newer", NextLabel: "Older"}
			if page > 1 {
				a.Prev = pageURL(path, page-1, kv...)
			}
			if more {
				a.Next = pageURL(path, page+1, kv...)
			}
			return a
		},
		"kickPresets": func(def int) []preset {
			return presetsWith([]preset{{"0", "Just remove", false}, {"10", "10 minutes", false}, {"60", "1 hour", false},
				{"1440", "1 day", false}}, strconv.Itoa(def), roundDuration(time.Duration(def)*time.Minute))
		},
		"banPresets": func() []preset {
			return []preset{{"1", "1 day", false}, {"7", "7 days", true}, {"30", "30 days", false}, {"0", "For good", false}}
		},
	}).ParseFS(pagesFS, "pages/*.html"))
}

// timeArg is a time and the page's now (the "ago" template).
type timeArg struct{ Now, T time.Time }

// formArg is what a kick, ban or unban form needs: the session's CSRF token, whom, the page to come back to.
type formArg struct {
	CSRF, ID, Name, Back string
	KickMinutes          int
	Kick                 bool // an unban form lifts a kick's block
}

// untilText is how long a ban lasts: "for good", "until 2026-11-01 18:00".
func untilText(b hub.Ban) string {
	if b.Until.IsZero() {
		return "for good"
	}
	return "until " + b.Until.Format("2006-01-02 15:04")
}

// matchTableArg: a table of matches (Self: the player whose page it is, "" for none).
type matchTableArg struct {
	Now  time.Time
	Rows []matchRow
	Self string
}

type auditListArg struct {
	Now   time.Time
	Lines []auditLine
}

// pagerArg is a list's links to the pages before and after.
type pagerArg struct {
	Prev, Next, Text, PrevLabel, NextLabel string
}

// preset is one choice of a kick's or ban's length.
type preset struct {
	Value, Label string
	Checked      bool
}

// presetsWith checks the preset of value `def`, adding it when the list has none.
func presetsWith(list []preset, def, label string) []preset {
	for i := range list {
		if list[i].Value == def {
			list[i].Checked = true
			return list
		}
	}
	return append([]preset{{def, label, true}}, list...)
}

// pageURL is a list page's address with its filters and page number.
func pageURL(path string, page int, kv ...string) string {
	q := url.Values{}
	for i := 0; i+1 < len(kv); i += 2 {
		if kv[i+1] != "" {
			q.Set(kv[i], kv[i+1])
		}
	}
	if page > 1 {
		q.Set("page", strconv.Itoa(page))
	}
	if len(q) == 0 {
		return path
	}
	return path + "?" + q.Encode()
}
