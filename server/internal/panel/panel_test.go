package panel

import (
	"bytes"
	"crypto/tls"
	"io"
	"log"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"ttt2-server/internal/config"
	"ttt2-server/internal/hub"
	"ttt2-server/internal/store"
	"ttt2-server/internal/tlsid"
	"ttt2-server/internal/wire"
)

// syncBuffer is a log both the test and the server's goroutines write.
type syncBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (s *syncBuffer) Write(p []byte) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.Write(p)
}

func (s *syncBuffer) String() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.String()
}

const (
	panelHost = "panel.test:28201"
	adminIP   = "198.51.100.7"
)

// fixture: a hub serving on loopback (trusting Steam IDs) with its database, and a panel with a login, on a clock
// the test moves.
type fixture struct {
	t                testing.TB
	p                *Panel
	h                *hub.Hub
	db               *store.Store
	id               tlsid.Identity
	log              *syncBuffer
	dir              string
	game             string
	password, secret string
	clock            atomic.Int64
	handler          http.Handler
	cookie           string
}

func newFixture(t testing.TB) *fixture {
	t.Helper()
	oldIterations, oldDelay := pbkdf2Iterations, failDelay
	pbkdf2Iterations, failDelay = 1000, 0
	t.Cleanup(func() { pbkdf2Iterations, failDelay = oldIterations, oldDelay })
	f := &fixture{t: t, dir: t.TempDir(), log: &syncBuffer{}}
	var err error
	if f.db, err = store.Open(filepath.Join(f.dir, "server.db")); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { f.db.Close() })
	if f.id, err = tlsid.LoadOrCreate(f.dir); err != nil {
		t.Fatal(err)
	}
	logger := log.New(f.log, "", 0)
	cfg := config.Default()
	cfg.TrustUnverified = true
	cfg.Name = "Test Server"
	f.h = hub.New(cfg, f.id, nil, hub.NewBans(filepath.Join(f.dir, "bans.txt")), logger)
	f.h.SetRecords(f.db)
	f.h.SetMatches(f.db)
	tcp, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	udp, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	go f.h.Serve(tcp, udp)
	t.Cleanup(func() { tcp.Close(); udp.Close() })
	f.game = tcp.Addr().String()
	f.clock.Store(time.Unix(1_800_000_000, 0).UnixNano())
	f.p = New(f.h, f.db, f.id, f.dir, f.dir, logger)
	f.p.now = func() time.Time { return time.Unix(0, f.clock.Load()) }
	t.Cleanup(f.p.Close)
	if f.password, f.secret, err = f.p.Setup(false); err != nil {
		t.Fatal(err)
	}
	f.handler = f.p.Handler()
	return f
}

func (f *fixture) advance(d time.Duration) { f.clock.Add(int64(d)) }

// code is the authenticator's code now.
func (f *fixture) code() string {
	b, _ := secretEncoding.DecodeString(f.secret)
	return totp(b, f.p.now().Unix()/30, 6)
}

// request is a request as the admin's browser sends it: from adminIP, to panelHost, the Origin its own, the session
// cookie when there is one; mods change it.
func (f *fixture) request(method, path string, form url.Values, mods ...func(*http.Request)) *httptest.ResponseRecorder {
	f.t.Helper()
	var body io.Reader
	if form != nil {
		body = strings.NewReader(form.Encode())
	}
	r := httptest.NewRequest(method, "https://"+panelHost+path, body)
	r.RemoteAddr = adminIP + ":40000"
	if form != nil {
		r.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	}
	if method == http.MethodPost {
		r.Header.Set("Origin", "https://"+panelHost)
	}
	if f.cookie != "" {
		r.AddCookie(&http.Cookie{Name: sessionCookie, Value: f.cookie})
	}
	for _, m := range mods {
		m(r)
	}
	w := httptest.NewRecorder()
	f.handler.ServeHTTP(w, r)
	return w
}

func from(ip string) func(*http.Request) {
	return func(r *http.Request) { r.RemoteAddr = net.JoinHostPort(ip, "40000") }
}

func header(name, value string) func(*http.Request) {
	return func(r *http.Request) {
		if value == "" {
			r.Header.Del(name)
		} else {
			r.Header.Set(name, value)
		}
	}
}

// tryLogin posts the login form and returns the answer (the session cookie is kept on success).
func (f *fixture) tryLogin(password, code string, mods ...func(*http.Request)) *httptest.ResponseRecorder {
	f.t.Helper()
	w := f.request(http.MethodPost, "/login", url.Values{"password": {password}, "code": {code}}, mods...)
	for _, c := range w.Result().Cookies() {
		if c.Name == sessionCookie && c.Value != "" {
			f.cookie = c.Value
		}
	}
	return w
}

// login logs in (the clock moves on a step, so the next login has a new code).
func (f *fixture) login() {
	f.t.Helper()
	f.cookie = ""
	if w := f.tryLogin(f.password, f.code()); w.Code != http.StatusSeeOther || w.Header().Get("Location") != "/" || f.cookie == "" {
		f.t.Fatalf("login: %d %q\n%s", w.Code, w.Header().Get("Location"), w.Body)
	}
	f.advance(30 * time.Second)
}

// csrf is the session's CSRF token, as its pages carry it.
func (f *fixture) csrf() string {
	f.t.Helper()
	body := f.request(http.MethodGet, "/", nil).Body.String()
	const mark = `name="csrf" value="`
	i := strings.Index(body, mark)
	if i < 0 {
		f.t.Fatalf("no CSRF token on the page:\n%s", body)
	}
	return body[i+len(mark) : i+len(mark)+64]
}

func (f *fixture) audited() string {
	b, _ := os.ReadFile(filepath.Join(f.dir, auditFile))
	return string(b)
}

func TestLoginSuccess(t *testing.T) {
	f := newFixture(t)
	if w := f.request(http.MethodGet, "/", nil); w.Code != http.StatusSeeOther || w.Header().Get("Location") != "/login" {
		t.Fatalf("a page without a session: %d %q", w.Code, w.Header().Get("Location"))
	}
	if w := f.request(http.MethodGet, "/login", nil); w.Code != http.StatusOK || !strings.Contains(w.Body.String(), `name="code"`) {
		t.Fatalf("%d\n%s", w.Code, w.Body)
	}
	w := f.tryLogin(f.password, f.code()[:3]+" "+f.code()[3:]) // as the app shows it
	if w.Code != http.StatusSeeOther {
		t.Fatalf("%d\n%s", w.Code, w.Body)
	}
	var c *http.Cookie
	for _, k := range w.Result().Cookies() {
		if k.Name == sessionCookie {
			c = k
		}
	}
	if c == nil || len(c.Value) != 64 || !c.Secure || !c.HttpOnly || c.SameSite != http.SameSiteStrictMode || c.Path != "/" || c.Domain != "" {
		t.Fatalf("cookie %+v", c)
	}
	page := f.request(http.MethodGet, "/", nil)
	if page.Code != http.StatusOK || !strings.Contains(page.Body.String(), "Online players") || !strings.Contains(page.Body.String(), "Test Server") ||
		!strings.Contains(page.Body.String(), "Nobody is online.") {
		t.Fatalf("%d\n%s", page.Code, page.Body)
	}
	if w := f.request(http.MethodGet, "/login", nil); w.Code != http.StatusSeeOther {
		t.Fatal("the login page of a logged-in admin goes to the panel")
	}
	for _, path := range []string{"/players", "/bans", "/matches", "/panel.css"} {
		if w := f.request(http.MethodGet, path, nil); w.Code != http.StatusOK {
			t.Fatalf("%s: %d", path, w.Code)
		}
	}
	if !strings.Contains(f.log.String(), "panel: admin from 198.51.100.7 logged in") || !strings.Contains(f.audited(), " panel: admin from 198.51.100.7 logged in\n") {
		t.Fatalf("log:\n%s\naudit:\n%s", f.log, f.audited())
	}
	// logging out ends the session
	if w := f.request(http.MethodPost, "/logout", url.Values{"csrf": {f.csrf()}}); w.Code != http.StatusSeeOther || w.Header().Get("Location") != "/login" {
		t.Fatalf("logout: %d", w.Code)
	}
	if w := f.request(http.MethodGet, "/", nil); w.Code != http.StatusSeeOther {
		t.Fatal("the session survived its logout")
	}
}

func TestLoginRefused(t *testing.T) {
	f := newFixture(t)
	wrongPassword := "Wrong-" + f.password[6:]
	for i, try := range []struct{ password, code, why string }{
		{wrongPassword, f.code(), "wrong password"},
		{f.password, "000000", "wrong code"},
		{f.password, "12345", "wrong code"},
		{f.password, "", "wrong code"},
		{"", "", "wrong password and code"},
	} {
		if try.code == "000000" && f.code() == "000000" {
			try.code = "000001"
		}
		ip := net.IPv4(198, 51, 100, byte(10+i)).String() // five failures from one address would lock it out
		w := f.tryLogin(try.password, try.code, from(ip))
		if w.Code != http.StatusUnauthorized || f.cookie != "" || !strings.Contains(w.Body.String(), "Wrong password or code.") {
			t.Fatalf("%+v: %d %q", try, w.Code, f.cookie)
		}
		if !strings.Contains(f.log.String(), "panel: failed login from "+ip+" ("+try.why+")") {
			t.Fatalf("%+v: log\n%s", try, f.log)
		}
	}
	// a code is good once: the same code again (from anywhere, within its 90 s) is refused
	code := f.code()
	if w := f.tryLogin(f.password, code); w.Code != http.StatusSeeOther {
		t.Fatalf("%d", w.Code)
	}
	f.cookie = ""
	f.advance(20 * time.Second)
	if w := f.tryLogin(f.password, code, from("198.51.100.99")); w.Code != http.StatusUnauthorized || f.cookie != "" {
		t.Fatalf("a replayed code: %d", w.Code)
	}
	if !strings.Contains(f.log.String(), "(code already used)") {
		t.Fatalf("log\n%s", f.log)
	}
	// the attempts never reach the log or the audit file
	for _, text := range []string{wrongPassword, f.password, f.secret} {
		if strings.Contains(f.log.String(), text) || strings.Contains(f.audited(), text) {
			t.Fatalf("%q logged", text)
		}
	}
	if !strings.Contains(f.audited(), "panel: failed login from 198.51.100.10 (wrong password)") {
		t.Fatalf("audit:\n%s", f.audited())
	}
}

// Every refused login is answered a second after it arrived.
func TestLoginFailureDelay(t *testing.T) {
	f := newFixture(t)
	failDelay = 300 * time.Millisecond
	start := time.Now()
	if w := f.tryLogin("nope", "000000"); w.Code != http.StatusUnauthorized {
		t.Fatal(w.Code)
	}
	if took := time.Since(start); took < 300*time.Millisecond {
		t.Fatalf("answered after %s", took)
	}
	start = time.Now()
	f.tryLogin(f.password, f.code())
	if took := time.Since(start); took > 250*time.Millisecond {
		t.Fatalf("a good login waited %s", took)
	}
}

// Five failures from one address (IPv6: one /64) lock it out for 15 minutes - even with the right login - and no
// other address.
func TestLoginLockout(t *testing.T) {
	f := newFixture(t)
	for i := 0; i < 5; i++ {
		if w := f.tryLogin("nope", "000000"); w.Code != http.StatusUnauthorized {
			t.Fatalf("failure %d: %d", i, w.Code)
		}
	}
	w := f.tryLogin(f.password, f.code())
	if w.Code != http.StatusTooManyRequests || f.cookie != "" || !strings.Contains(w.Body.String(), "from your address") {
		t.Fatalf("locked out: %d\n%s", w.Code, w.Body)
	}
	if !strings.Contains(f.log.String(), "panel: 198.51.100.7 locked out for 15m0s (5 failed logins)") ||
		!strings.Contains(f.audited(), "locked out") {
		t.Fatalf("log\n%s", f.log)
	}
	if w := f.tryLogin(f.password, f.code(), from("198.51.100.8")); w.Code != http.StatusSeeOther {
		t.Fatalf("another address: %d", w.Code)
	}
	f.advance(14 * time.Minute)
	if w := f.tryLogin(f.password, f.code()); w.Code != http.StatusTooManyRequests {
		t.Fatalf("after 14 minutes: %d", w.Code)
	}
	f.advance(time.Minute + time.Second)
	if w := f.tryLogin(f.password, f.code()); w.Code != http.StatusSeeOther {
		t.Fatalf("after 15 minutes: %d", w.Code)
	}

	// IPv6: the addresses of one /64 count together
	f.cookie = ""
	f.advance(30 * time.Second) // a new code
	for i, ip := range []string{"2001:db8:1:2::1", "2001:db8:1:2::2", "2001:db8:1:2:ffff::9", "2001:db8:1:2::1", "2001:db8:1:2::7"} {
		if w := f.tryLogin("nope", "000000", from(ip)); w.Code != http.StatusUnauthorized {
			t.Fatalf("v6 failure %d: %d", i, w.Code)
		}
	}
	if w := f.tryLogin(f.password, f.code(), from("2001:db8:1:2::abcd")); w.Code != http.StatusTooManyRequests {
		t.Fatalf("the /64: %d", w.Code)
	}
	if w := f.tryLogin(f.password, f.code(), from("2001:db8:1:3::1")); w.Code != http.StatusSeeOther {
		t.Fatalf("the next /64: %d", w.Code)
	}
	// failures older than 15 minutes are forgotten
	f.cookie = ""
	f.advance(time.Minute)
	for i := 0; i < 4; i++ {
		f.tryLogin("nope", "000000", from("192.0.2.1"))
	}
	f.advance(16 * time.Minute)
	f.tryLogin("nope", "000000", from("192.0.2.1"))
	if w := f.tryLogin(f.password, f.code(), from("192.0.2.1")); w.Code != http.StatusSeeOther {
		t.Fatalf("old failures counted: %d", w.Code)
	}
}

// 30 failures in a minute, from any addresses, pause every login for a minute.
func TestLoginGlobalPause(t *testing.T) {
	f := newFixture(t)
	for i := 0; i < 30; i++ {
		ip := net.IPv4(10, 0, byte(i/4), byte(i)).String()
		if w := f.tryLogin("nope", "000000", from(ip)); w.Code != http.StatusUnauthorized {
			t.Fatalf("failure %d: %d", i, w.Code)
		}
	}
	w := f.tryLogin(f.password, f.code(), from("203.0.113.50"))
	if w.Code != http.StatusTooManyRequests || f.cookie != "" || !strings.Contains(w.Body.String(), "paused") {
		t.Fatalf("paused: %d\n%s", w.Code, w.Body)
	}
	if !strings.Contains(f.log.String(), "panel: every login paused for 1m0s (30 failed logins in 1m0s)") {
		t.Fatalf("log\n%s", f.log)
	}
	f.advance(time.Minute + time.Second)
	if w := f.tryLogin(f.password, f.code(), from("203.0.113.50")); w.Code != http.StatusSeeOther {
		t.Fatalf("after the pause: %d", w.Code)
	}
}

// Logins sent at once cannot pass the limit together: each counts as a failure until it is checked.
func TestLoginLimitsConcurrent(t *testing.T) {
	f := newFixture(t)
	failDelay = 50 * time.Millisecond
	var wg sync.WaitGroup
	var unauthorized, refused atomic.Int32
	for i := 0; i < 40; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			r := httptest.NewRequest(http.MethodPost, "https://"+panelHost+"/login", strings.NewReader("password=nope&code=000000"))
			r.RemoteAddr = adminIP + ":1"
			r.Header.Set("Content-Type", "application/x-www-form-urlencoded")
			r.Header.Set("Origin", "https://"+panelHost)
			w := httptest.NewRecorder()
			f.handler.ServeHTTP(w, r)
			switch w.Code {
			case http.StatusUnauthorized:
				unauthorized.Add(1)
			case http.StatusTooManyRequests:
				refused.Add(1)
			}
		}()
	}
	wg.Wait()
	if unauthorized.Load() > 5 || unauthorized.Load()+refused.Load() != 40 {
		t.Fatalf("%d checked, %d refused: 5 at most are checked", unauthorized.Load(), refused.Load())
	}
}

func TestSessionExpiry(t *testing.T) {
	f := newFixture(t)
	f.login()
	// used every 50 minutes, it lives 12 hours from the login, not longer
	for i := 0; i < 16; i++ {
		f.advance(50 * time.Minute)
		ok := f.request(http.MethodGet, "/", nil).Code == http.StatusOK
		if want := time.Duration(i+1)*50*time.Minute+30*time.Second < 12*time.Hour; ok != want {
			t.Fatalf("after %d x 50 minutes: alive %v", i+1, ok)
		}
	}
	// an hour unused ends it
	f.login()
	f.advance(59 * time.Minute)
	if f.request(http.MethodGet, "/players", nil).Code != http.StatusOK {
		t.Fatal("59 minutes idle")
	}
	f.advance(59 * time.Minute)
	if f.request(http.MethodGet, "/players", nil).Code != http.StatusOK {
		t.Fatal("a request keeps it alive")
	}
	f.advance(time.Hour)
	if w := f.request(http.MethodGet, "/players", nil); w.Code != http.StatusSeeOther {
		t.Fatalf("an hour idle: %d", w.Code)
	}
	// panel-logout-all
	f.login()
	if n := f.p.LogoutAll(); n != 1 {
		t.Fatalf("%d sessions", n)
	}
	if w := f.request(http.MethodGet, "/", nil); w.Code != http.StatusSeeOther {
		t.Fatal("LogoutAll left the session")
	}
	// a cookie the panel did not make
	f.cookie = strings.Repeat("ab", 32)
	if w := f.request(http.MethodGet, "/", nil); w.Code != http.StatusSeeOther {
		t.Fatal("a made-up cookie")
	}
}

// At most 20 sessions: a new login ends the one used least recently.
func TestSessionLimit(t *testing.T) {
	f := newFixture(t)
	f.login()
	first := f.cookie
	var last string
	for i := 0; i < maxSessions; i++ {
		f.login()
		last = f.cookie
	}
	f.cookie = first
	if w := f.request(http.MethodGet, "/", nil); w.Code != http.StatusSeeOther {
		t.Fatal("21 sessions")
	}
	f.cookie = last
	if w := f.request(http.MethodGet, "/", nil); w.Code != http.StatusOK {
		t.Fatal("the newest session ended")
	}
	if len(f.p.sessions) != maxSessions {
		t.Fatalf("%d sessions", len(f.p.sessions))
	}
}

func TestCSRF(t *testing.T) {
	f := newFixture(t)
	f.login()
	ban := url.Values{"steam_id": {"76561198000000001"}, "days": {"7"}, "reason": {"cheating"}}
	for _, token := range []string{"", "x", strings.Repeat("0", 64), f.csrf()[:63]} {
		form := url.Values{"csrf": {token}}
		for k, v := range ban {
			form[k] = v
		}
		if token == "" {
			delete(form, "csrf")
		}
		if w := f.request(http.MethodPost, "/ban", form); w.Code != http.StatusForbidden {
			t.Fatalf("token %q: %d", token, w.Code)
		}
	}
	// in the URL instead of the body
	if w := f.request(http.MethodPost, "/ban?csrf="+f.csrf(), ban); w.Code != http.StatusForbidden {
		t.Fatalf("token in the URL: %d", w.Code)
	}
	// another session's token
	other := f.csrf()
	f.login()
	form := url.Values{"csrf": {other}}
	for k, v := range ban {
		form[k] = v
	}
	if w := f.request(http.MethodPost, "/ban", form); w.Code != http.StatusForbidden {
		t.Fatalf("another session's token: %d", w.Code)
	}
	if len(f.h.Bans()) != 0 {
		t.Fatal("a refused request banned")
	}
	// without a session
	f.cookie = ""
	form.Set("csrf", other)
	if w := f.request(http.MethodPost, "/ban", form); w.Code != http.StatusForbidden || len(f.h.Bans()) != 0 {
		t.Fatalf("no session: %d", w.Code)
	}
}

func TestSameOrigin(t *testing.T) {
	f := newFixture(t)
	f.login()
	form := func() url.Values {
		return url.Values{"csrf": {f.csrf()}, "steam_id": {"76561198000000001"}, "days": {"0"}}
	}
	for name, mods := range map[string][]func(*http.Request){
		"another site":           {header("Origin", "https://evil.example")},
		"another port":           {header("Origin", "https://panel.test:443")},
		"http":                   {header("Origin", "http://"+panelHost)},
		"null":                   {header("Origin", "null")},
		"neither":                {header("Origin", "")},
		"a referer of elsewhere": {header("Origin", ""), header("Referer", "https://evil.example/"+panelHost)},
		"cross-site fetch":       {header("Sec-Fetch-Site", "cross-site")},
		"same-site fetch":        {header("Sec-Fetch-Site", "same-site")},
	} {
		if w := f.request(http.MethodPost, "/ban", form(), mods...); w.Code != http.StatusForbidden {
			t.Fatalf("%s: %d", name, w.Code)
		}
	}
	if len(f.h.Bans()) != 0 {
		t.Fatal("a refused request banned")
	}
	if !strings.Contains(f.log.String(), "refused: not from the panel's own page") {
		t.Fatalf("log\n%s", f.log)
	}
	// a browser that sends no Origin but its Referer, and one that says same-origin
	if w := f.request(http.MethodPost, "/ban", form(), header("Origin", ""), header("Referer", "https://"+panelHost+"/players?q=x")); w.Code != http.StatusSeeOther {
		t.Fatalf("referer: %d", w.Code)
	}
	if w := f.request(http.MethodPost, "/unban", form(), header("Sec-Fetch-Site", "same-origin")); w.Code != http.StatusSeeOther {
		t.Fatalf("same-origin: %d", w.Code)
	}
	// the login form too (refused before it counts)
	f.cookie = ""
	if w := f.tryLogin(f.password, f.code(), header("Origin", "https://evil.example")); w.Code != http.StatusForbidden || f.cookie != "" {
		t.Fatalf("login from another site: %d", w.Code)
	}
}

// GET only shows: the actions' paths answer GET with 405, and nothing changes.
func TestGetChangesNothing(t *testing.T) {
	f := newFixture(t)
	f.login()
	if _, err := f.h.Ban("76561198000000002", 0, "x"); err != nil {
		t.Fatal(err)
	}
	q := "?csrf=" + f.csrf() + "&steam_id=76561198000000001&days=0&minutes=5"
	for _, path := range []string{"/ban", "/unban", "/kick", "/logout"} {
		if w := f.request(http.MethodGet, path+q, nil); w.Code != http.StatusMethodNotAllowed {
			t.Fatalf("GET %s: %d", path, w.Code)
		}
	}
	if w := f.request(http.MethodGet, "/unban?csrf="+f.csrf()+"&steam_id=76561198000000002", nil); w.Code != http.StatusMethodNotAllowed {
		t.Fatal(w.Code)
	}
	if b := f.h.Bans(); len(b) != 1 || b[0].SteamID != 76561198000000002 {
		t.Fatalf("%+v", b)
	}
	if f.request(http.MethodGet, "/", nil).Code != http.StatusOK {
		t.Fatal("GET /logout logged out")
	}
}

// Kick, ban and unban through the panel: the hub does them, the log and the audit file say who did.
func TestActions(t *testing.T) {
	f := newFixture(t)
	game := gameLogin(t, f.game, 0x0110000100000A0A, "Jin Kazama")
	f.login()
	page := f.request(http.MethodGet, "/", nil).Body.String()
	if !strings.Contains(page, "Jin Kazama") || !strings.Contains(page, "76561197960268298") || !strings.Contains(page, `name="minutes" value="10"`) {
		t.Fatalf("online page:\n%s", page)
	}
	w := f.request(http.MethodPost, "/kick", url.Values{"csrf": {f.csrf()}, "steam_id": {"76561197960268298"}, "minutes": {"30"},
		"reason": {"griefing"}, "back": {"online"}})
	if w.Code != http.StatusSeeOther || w.Header().Get("Location") != "/" {
		t.Fatalf("kick: %d %q", w.Code, w.Header().Get("Location"))
	}
	game.SetReadDeadline(time.Now().Add(3 * time.Second))
	for {
		typ, payload, err := wire.ReadFrame(game)
		if err != nil {
			t.Fatal(err)
		}
		if typ == wire.Error {
			r := wire.Reader{B: payload}
			if code, text := r.U32(), r.Str(); code != wire.ErrBanned || !strings.Contains(text, "30 minutes. Reason: griefing") {
				t.Fatalf("%d %q", code, text)
			}
			break
		}
	}
	if b := f.h.Bans(); len(b) != 1 || !b[0].Kick() || b[0].KickReason() != "griefing" {
		t.Fatalf("%+v", b)
	}
	if page := f.request(http.MethodGet, "/", nil).Body.String(); !strings.Contains(page, "Kicked Jin Kazama (76561197960268298) for 30 minutes.") {
		t.Fatalf("the answer on the next page:\n%s", page)
	}
	w = f.request(http.MethodPost, "/ban", url.Values{"csrf": {f.csrf()}, "steam_id": {"76561198000000003"}, "days": {"7"},
		"reason": {"cheating"}, "back": {"players"}, "q": {"7656 x"}})
	if w.Code != http.StatusSeeOther || w.Header().Get("Location") != "/players?q=7656+x" {
		t.Fatalf("ban: %d %q", w.Code, w.Header().Get("Location"))
	}
	f.request(http.MethodPost, "/ban", url.Values{"csrf": {f.csrf()}, "steam_id": {"76561198000000004"}, "days": {"0"}})
	if len(f.h.Bans()) != 3 {
		t.Fatalf("%+v", f.h.Bans())
	}
	if page := f.request(http.MethodGet, "/bans", nil).Body.String(); !strings.Contains(page, "76561198000000003") || !strings.Contains(page, "cheating") ||
		!strings.Contains(page, "for good") {
		t.Fatalf("bans page:\n%s", page)
	}
	if w := f.request(http.MethodPost, "/unban", url.Values{"csrf": {f.csrf()}, "steam_id": {"76561198000000003"}, "back": {"bans"}}); w.Header().Get("Location") != "/bans" {
		t.Fatal(w.Header().Get("Location"))
	}
	if len(f.h.Bans()) != 2 {
		t.Fatalf("%+v", f.h.Bans())
	}
	// refused values change nothing
	for _, form := range []url.Values{
		{"steam_id": {"0"}, "days": {"1"}},
		{"steam_id": {"jin"}, "days": {"1"}},
		{"steam_id": {"76561198000000005"}, "days": {"-1"}},
		{"steam_id": {"76561198000000005"}, "days": {"36501"}},
		{"steam_id": {"76561198000000005"}, "days": {""}},
		{"steam_id": {"76561198000000005"}, "days": {"1"}, "reason": {strings.Repeat("x", 201)}},
	} {
		form.Set("csrf", f.csrf())
		f.request(http.MethodPost, "/ban", form)
	}
	f.request(http.MethodPost, "/kick", url.Values{"csrf": {f.csrf()}, "steam_id": {"76561198000000005"}, "minutes": {"1441"}})
	if len(f.h.Bans()) != 2 {
		t.Fatalf("%+v", f.h.Bans())
	}
	for _, line := range []string{
		"panel: admin from 198.51.100.7 kicked 76561197960268298 for 30 minutes: griefing\n",
		"panel: admin from 198.51.100.7 banned 76561198000000003 7 days: cheating\n",
		"panel: admin from 198.51.100.7 banned 76561198000000004 for good\n",
		"panel: admin from 198.51.100.7 unbanned 76561198000000003\n",
	} {
		if !strings.Contains(f.log.String(), line) || !strings.Contains(f.audited(), line) {
			t.Fatalf("%q missing\nlog:\n%s\naudit:\n%s", line, f.log, f.audited())
		}
	}
	if runtime.GOOS != "windows" {
		if st, _ := os.Stat(filepath.Join(f.dir, auditFile)); st.Mode().Perm() != 0o600 {
			t.Fatalf("audit file mode %v", st.Mode().Perm())
		}
	}
	// a reason's line breaks never reach the log or the ban file as lines
	f.request(http.MethodPost, "/ban", url.Values{"csrf": {f.csrf()}, "steam_id": {"76561198000000006"}, "days": {"1"}, "reason": {"a\nb\rc"}})
	if !strings.Contains(f.log.String(), "banned 76561198000000006 1 days: abc\n") {
		t.Fatalf("log\n%s", f.log)
	}
}

// ownScript is the one script tag the logged-in pages have (panel.js, the panel's own).
const ownScript = `<script src="/panel.js" defer></script>`

// withoutOwnScript is a page without its own script tag (what is left must have no script at all).
func withoutOwnScript(t *testing.T, body string) string {
	t.Helper()
	if n := strings.Count(body, ownScript); n > 1 {
		t.Fatalf("panel.js %d times", n)
	}
	return strings.Replace(body, ownScript, "", 1)
}

// Every text a player or a game sent is escaped, on every page.
func TestEscaping(t *testing.T) {
	f := newFixture(t)
	evil := `<script>alert("x")</script>`
	img := `<img src=x onerror=alert(1)>`
	if err := f.db.PlayerSeen(76561198000000009, evil, true, time.Now()); err != nil {
		t.Fatal(err)
	}
	if err := f.db.PlayerSeen(76561198000000010, img, false, time.Now()); err != nil {
		t.Fatal(err)
	}
	m, _ := f.db.OpenMatch(1, store.KindPlayer, time.Now())
	f.db.AddMatchPlayer(m, 76561198000000009, evil, 1, true)
	f.db.AddMatchPlayer(m, 76561198000000010, img, 1, true)
	f.h.Ban("76561198000000009", 3, img)
	f.h.Ban("76561198000000010", 0, evil)
	// an online player with that name (the hub cleans online names: the live view's snapshot is given instead)
	f.p.live.source = fakeSource(img, img)
	f.login()
	// a panel action whose reason is markup: in the audit log, shown on the Audit log page and the player's page
	f.request(http.MethodPost, "/ban", url.Values{"csrf": {f.csrf()}, "steam_id": {"76561198000000011"}, "days": {"1"}, "reason": {img}})
	for _, path := range []string{"/", "/players", "/players?q=" + url.QueryEscape(evil), "/players?show=banned", "/bans", "/matches",
		"/matches?q=" + url.QueryEscape(`"><script>`), "/matches?q=76561198000000010", "/player?steam=76561198000000009",
		"/player?steam=76561198000000010", "/player?steam=76561198000000011", "/player?steam=76561198000000099", "/audit",
		"/audit?q=" + url.QueryEscape(img)} {
		w := f.request(http.MethodGet, path, nil)
		body := withoutOwnScript(t, w.Body.String())
		if w.Code != http.StatusOK || strings.Contains(body, "<script") || strings.Contains(body, "<img") {
			t.Fatalf("%s: %d\n%s", path, w.Code, body)
		}
		if strings.HasPrefix(path, "/player?steam=7656119800000001") && !strings.Contains(body, "&lt;") {
			t.Fatalf("%s: the name or reason is not on the page:\n%s", path, body)
		}
		if path == "/players" && !strings.Contains(body, "&lt;script&gt;alert(&#34;x&#34;)&lt;/script&gt;") {
			t.Fatalf("%s:\n%s", path, body)
		}
		if strings.HasPrefix(path, "/matches?q=%22") && !strings.Contains(body, `value="&#34;&gt;&lt;script&gt;"`) {
			t.Fatalf("the search, back in its box:\n%s", body)
		}
	}
}

// Every answer carries the security headers; an address outside admin_panel_allow gets a bare 403.
func TestHeadersAndAllowlist(t *testing.T) {
	f := newFixture(t)
	check := func(name string, w *httptest.ResponseRecorder) {
		t.Helper()
		h := w.Result().Header
		for k, v := range map[string]string{
			"Content-Security-Policy": "default-src 'none'; script-src 'self'; connect-src 'self'; style-src 'self'; img-src 'self'; " +
				"form-action 'self'; frame-ancestors 'none'; base-uri 'none'; require-trusted-types-for 'script'; trusted-types 'none'",
			"X-Frame-Options":        "DENY",
			"X-Content-Type-Options": "nosniff",
			"Referrer-Policy":        "same-origin",
			"Cache-Control":          "no-store",
		} {
			if h.Get(k) != v {
				t.Errorf("%s: %s = %q", name, k, h.Get(k))
			}
		}
		if h.Get("Strict-Transport-Security") != "" {
			t.Errorf("%s: HSTS with the self-signed certificate", name)
		}
	}
	check("login page", f.request(http.MethodGet, "/login", nil))
	check("css", f.request(http.MethodGet, "/panel.css", nil))
	check("not found", f.request(http.MethodGet, "/nope", nil))
	check("refused", f.request(http.MethodPost, "/ban", url.Values{}))
	check("failed login", f.tryLogin("x", "000000"))
	check("script", f.request(http.MethodGet, "/panel.js", nil))
	check("live stream refused", f.request(http.MethodGet, "/events", nil))
	for path, typ := range map[string]string{"/panel.css": "text/css; charset=utf-8", "/panel.js": "text/javascript; charset=utf-8",
		"/favicon.svg": "image/svg+xml"} {
		if w := f.request(http.MethodGet, path, nil); w.Code != http.StatusOK || w.Header().Get("Content-Type") != typ {
			t.Fatalf("%s: %d %s", path, w.Code, w.Header().Get("Content-Type"))
		}
	}
	// the login page has no script at all; the others only the panel's own: no inline script, style or handler
	if body := f.request(http.MethodGet, "/login", nil).Body.String(); strings.Contains(body, "<script") {
		t.Fatalf("the login page has a script:\n%s", body)
	}
	f.login()
	check("page", f.request(http.MethodGet, "/matches", nil))
	for _, path := range []string{"/", "/players", "/player?steam=76561198000000001", "/matches", "/bans", "/audit"} {
		page := f.request(http.MethodGet, path, nil).Body.String()
		if !strings.Contains(page, ownScript) {
			t.Fatalf("%s: no panel.js", path)
		}
		body := strings.ToLower(withoutOwnScript(t, page))
		for _, bad := range []string{"<script", " style=", "<style", "javascript:", " onclick=", " onerror=", " onload=", " onsubmit=",
			"http://", "//cdn"} {
			if strings.Contains(body, bad) {
				t.Fatalf("%s: %q:\n%s", path, bad, body)
			}
		}
	}
	// the script writes no HTML from strings and runs no strings as code
	js, _ := pagesFS.ReadFile("pages/panel.js")
	for _, bad := range []string{"innerHTML", "outerHTML", "insertAdjacentHTML", "document.write", "eval(", "new Function",
		"setTimeout('", "setInterval('", "http://", "https://"} {
		if strings.Contains(string(js), bad) {
			t.Fatalf("panel.js has %q", bad)
		}
	}

	cfg := f.h.Config()
	cfg.AdminPanelAllow = []string{"203.0.113.0/24", "2001:db8::/32"}
	f.h.SetConfig(cfg)
	for _, ip := range []string{adminIP, "203.0.114.1", "2001:db9::1"} {
		w := f.request(http.MethodGet, "/login", nil, from(ip))
		if w.Code != http.StatusForbidden || w.Body.Len() != 0 {
			t.Fatalf("%s: %d %q", ip, w.Code, w.Body)
		}
		if w := f.request(http.MethodGet, "/", nil, from(ip)); w.Code != http.StatusForbidden {
			t.Fatalf("%s with a session: %d", ip, w.Code)
		}
	}
	for _, ip := range []string{"203.0.113.9", "::ffff:203.0.113.9", "2001:db8:5::1"} {
		if w := f.request(http.MethodGet, "/players", nil, from(ip)); w.Code != http.StatusOK {
			t.Fatalf("%s: %d", ip, w.Code)
		}
	}
}

func TestOffByDefault(t *testing.T) {
	f := newFixture(t)
	if err := f.p.Apply(config.Default()); err != nil || f.p.Running() || f.p.Addr() != "" {
		t.Fatalf("%v %v", err, f.p.Running())
	}
	if !strings.Contains(f.p.Status(), "admin panel: off (admin_panel = false in server.toml)") {
		t.Fatal(f.p.Status())
	}
}

// admin_panel = true without a login: the panel stays down and says why (the game server is not touched).
func TestNoLoginNoPanel(t *testing.T) {
	f := newFixture(t)
	os.Remove(f.p.loginPath())
	cfg := config.Default()
	cfg.AdminPanel, cfg.AdminPanelBind, cfg.AdminPanelPort = true, "127.0.0.1", 0
	if err := f.p.Apply(cfg); err != ErrNoLogin || f.p.Running() {
		t.Fatalf("%v %v", err, f.p.Running())
	}
	if !strings.Contains(f.p.Status(), "not running: no login yet: make one with ttt2admin panel-setup") ||
		!strings.Contains(f.p.Status(), "login: none yet") {
		t.Fatal(f.p.Status())
	}
	if _, _, err := f.p.Setup(false); err != nil {
		t.Fatal(err)
	}
	if err := f.p.Apply(cfg); err != nil || !f.p.Running() {
		t.Fatalf("%v", err)
	}
}

// panel-setup keeps a login unless forced; a new login or password ends every session.
func TestSetupAndNewPassword(t *testing.T) {
	f := newFixture(t)
	f.login()
	if _, _, err := f.p.Setup(false); err != ErrLoginExists {
		t.Fatal(err)
	}
	if f.request(http.MethodGet, "/", nil).Code != http.StatusOK {
		t.Fatal("a refused setup ended the session")
	}
	password, err := f.p.NewPassword()
	if err != nil || password == f.password || len(password) != 20 {
		t.Fatalf("%q %v", password, err)
	}
	if f.request(http.MethodGet, "/", nil).Code != http.StatusSeeOther {
		t.Fatal("a new password must end the sessions")
	}
	creds, _ := loadLogin(f.p.loginPath())
	if creds.TOTPSecret != f.secret || !creds.Password.verify(password) || creds.Password.verify(f.password) {
		t.Fatal("panel-password keeps the secret, not the password")
	}
	if w := f.tryLogin(f.password, f.code()); w.Code != http.StatusUnauthorized {
		t.Fatalf("the old password: %d", w.Code)
	}
	f.password = password
	f.login()
	password, secret, err := f.p.Setup(true)
	if err != nil || secret == f.secret || password == f.password {
		t.Fatal(err)
	}
	if f.request(http.MethodGet, "/", nil).Code != http.StatusSeeOther {
		t.Fatal("a new login must end the sessions")
	}
	if w := f.tryLogin(password, f.code()); w.Code != http.StatusUnauthorized {
		t.Fatal("the old secret's code")
	}
	f.password, f.secret = password, secret
	f.login()
}

// gameLogin connects to the hub's game port as the game does and returns the connection after WELCOME.
func gameLogin(t testing.TB, addr string, peer uint64, name string) *tls.Conn {
	t.Helper()
	c, err := tls.Dial("tcp", addr, &tls.Config{InsecureSkipVerify: true, MinVersion: tls.VersionTLS12})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	var w wire.Writer
	w.U16(wire.ProtocolVersion)
	w.Str(config.Default().AcceptedProtocols[0])
	w.Str("test")
	wire.WriteFrame(c, wire.Hello, w.B)
	w = wire.Writer{}
	w.U64(peer)
	w.Str(name)
	w.Blob(nil)
	w.Str("")
	wire.WriteFrame(c, wire.Auth, w.B)
	c.SetReadDeadline(time.Now().Add(3 * time.Second))
	if typ, _, err := wire.ReadFrame(c); err != nil || typ != wire.Welcome {
		t.Fatalf("login: %d %v", typ, err)
	}
	return c
}
