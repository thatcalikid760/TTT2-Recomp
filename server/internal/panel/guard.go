package panel

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"net"
	"net/http"
	"net/netip"
	"net/url"
	"strings"
	"time"
)

// What keeps the panel to its owner: the login limits (password guessing), the sessions and the same-origin check
// every state-changing request passes.

const (
	sessionCookie  = "__Host-ttt2admin" // __Host-: the browser keeps it for this host only, over HTTPS, Path=/
	sessionLife    = 12 * time.Hour     // from the login, however busy
	sessionIdle    = time.Hour          // without a request
	maxSessions    = 20                 // a new login ends the one used least recently
	addrFailLimit  = 5                  // failed logins from one address (IPv6: one /64) ...
	addrFailWindow = 15 * time.Minute
	addrBlockTime  = 15 * time.Minute // ... refuse it this long
	allFailLimit   = 30               // failed logins on the whole panel ...
	allFailWindow  = time.Minute
	allPauseTime   = time.Minute      // ... pause every login this long
	usedCodeMemory = 90 * time.Second // an accepted code is refused again this long (a code is valid 90 s at most)
)

// failDelay: every refused login is answered this late after it arrived (a variable for the tests).
var failDelay = time.Second

// session is one logged-in browser.
type session struct {
	created, seen time.Time
	csrf          string // in every form of its pages
	ip            string
	flash         string // the last action's answer, shown on the next page
	flashErr      bool   // ... and it says the action was not done
}

// newSession starts a session for a login from ip and returns its cookie value (32 random bytes in hex; the panel
// keeps only their SHA-256). Under p.mu.
func (p *Panel) newSessionLocked(ip string, now time.Time) (string, error) {
	var token, csrf [32]byte
	if _, err := rand.Read(token[:]); err != nil {
		return "", err
	}
	if _, err := rand.Read(csrf[:]); err != nil {
		return "", err
	}
	if len(p.sessions) >= maxSessions {
		var oldest [32]byte
		first := true
		for k, s := range p.sessions {
			if first || s.seen.Before(p.sessions[oldest].seen) {
				oldest, first = k, false
			}
		}
		delete(p.sessions, oldest)
		p.live.endSession(oldest, "the session ended")
		p.audit("panel: the session used least recently ended (%d sessions at most)", maxSessions)
	}
	p.sessions[sha256.Sum256(token[:])] = &session{created: now, seen: now, csrf: hex.EncodeToString(csrf[:]), ip: ip}
	return hex.EncodeToString(token[:]), nil
}

// sessionKey is the map key of a request's session cookie (false: none, or not one the panel could have made).
func sessionKey(r *http.Request) ([32]byte, bool) {
	c, err := r.Cookie(sessionCookie)
	if err != nil || len(c.Value) != 64 {
		return [32]byte{}, false
	}
	token, err := hex.DecodeString(c.Value)
	if err != nil {
		return [32]byte{}, false
	}
	return sha256.Sum256(token), true
}

// sessionOf is the request's session, nil when it has none or it ended (12 h after the login, an hour unused);
// a request keeps it alive.
func (p *Panel) sessionOf(r *http.Request) *session {
	key, ok := sessionKey(r)
	if !ok {
		return nil
	}
	now := p.now()
	p.mu.Lock()
	defer p.mu.Unlock()
	s := p.sessions[key]
	if s == nil {
		return nil
	}
	if now.Sub(s.created) >= sessionLife || now.Sub(s.seen) >= sessionIdle {
		delete(p.sessions, key)
		return nil
	}
	s.seen = now
	return s
}

// sessionPeek is the request's live session's key, without keeping the session alive (a live stream is not a sign
// that anyone is there: an hour without a page view still ends the session).
func (p *Panel) sessionPeek(r *http.Request) ([32]byte, bool) {
	key, ok := sessionKey(r)
	return key, ok && p.sessionAlive(key)
}

// sessionAlive reports whether a session still is (an ended one is forgotten).
func (p *Panel) sessionAlive(key [32]byte) bool {
	now := p.now()
	p.mu.Lock()
	defer p.mu.Unlock()
	s := p.sessions[key]
	if s == nil {
		return false
	}
	if now.Sub(s.created) >= sessionLife || now.Sub(s.seen) >= sessionIdle {
		delete(p.sessions, key)
		return false
	}
	return true
}

// endSession ends the request's session (logout) and its live streams.
func (p *Panel) endSession(r *http.Request) {
	if key, ok := sessionKey(r); ok {
		p.mu.Lock()
		delete(p.sessions, key)
		p.mu.Unlock()
		p.live.endSession(key, "logged out")
	}
}

// LogoutAll ends every session (panel-logout-all; a new login or password too) and returns how many there were.
func (p *Panel) LogoutAll() int {
	p.mu.Lock()
	n := len(p.sessions)
	clear(p.sessions)
	p.mu.Unlock()
	p.live.endAll("every session was ended", false)
	return n
}

// setCookie gives the browser its session: Secure, HttpOnly, SameSite=Strict, for this host's every path; no expiry
// (the panel ends it) - "" removes it.
func setCookie(w http.ResponseWriter, value string) {
	c := &http.Cookie{Name: sessionCookie, Value: value, Path: "/", Secure: true, HttpOnly: true, SameSite: http.SameSiteStrictMode}
	if value == "" {
		c.MaxAge = -1
	}
	http.SetCookie(w, c)
}

// sameOrigin: a state-changing request must come from the panel's own pages - Sec-Fetch-Site (when the browser
// sends it) says same-origin, and the Origin header (or, without one, the Referer) is https://<this host>.
func sameOrigin(r *http.Request) bool {
	if site := r.Header.Get("Sec-Fetch-Site"); site != "" && site != "same-origin" {
		return false
	}
	from := r.Header.Get("Origin")
	if from == "" {
		from = r.Header.Get("Referer")
	}
	if from == "" || r.Host == "" {
		return false
	}
	u, err := url.Parse(from)
	return err == nil && u.Scheme == "https" && u.User == nil && strings.EqualFold(u.Host, r.Host)
}

// ---- the login limits

// limiter counts failed logins per address and on the whole panel (under p.mu). A login being checked counts as a
// failure until it is known, so logins sent at once cannot pass the limits together.
type limiter struct {
	addrs      map[string]*addrFails
	recent     []time.Time // the whole panel's failures in the last allFailWindow
	paused     time.Time   // every login refused until then
	pending    map[string]int
	pendingAll int
}

// addrFails is one address's failures in the window, and until when it is refused.
type addrFails struct {
	at    []time.Time
	until time.Time
}

func newLimiter() limiter {
	return limiter{addrs: map[string]*addrFails{}, pending: map[string]int{}}
}

// begin admits a login from addr ("" and true), or says why not.
func (l *limiter) begin(addr string, now time.Time) (string, bool) {
	l.prune(now)
	a := l.addrs[addr]
	if a == nil {
		a = &addrFails{}
	}
	switch {
	case now.Before(l.paused):
		return "Too many failed logins: logins are paused for a minute.", false
	case now.Before(a.until):
		return "Too many failed logins from your address: try again in 15 minutes.", false
	case len(a.at)+l.pending[addr] >= addrFailLimit, len(l.recent)+l.pendingAll >= allFailLimit:
		return "Too many logins at once: try again.", false
	}
	l.pending[addr]++
	l.pendingAll++
	return "", true
}

// end settles a login begin admitted: a failure counts (and can lock the address out, or pause every login - the
// events say which), a success forgets the address's failures.
func (l *limiter) end(addr string, now time.Time, failed bool) (lockedOut, paused bool) {
	if l.pending[addr]--; l.pending[addr] <= 0 {
		delete(l.pending, addr)
	}
	l.pendingAll--
	a := l.addrs[addr]
	if !failed {
		if a != nil && !now.Before(a.until) {
			delete(l.addrs, addr)
		}
		return false, false
	}
	if a == nil {
		a = &addrFails{}
		l.addrs[addr] = a
	}
	a.at = append(a.at, now)
	if len(a.at) >= addrFailLimit {
		a.at, a.until, lockedOut = nil, now.Add(addrBlockTime), true
	}
	l.recent = append(l.recent, now)
	if len(l.recent) >= allFailLimit {
		l.recent, l.paused, paused = nil, now.Add(allPauseTime), true
	}
	return lockedOut, paused
}

// prune forgets failures older than their window and lockouts that ended.
func (l *limiter) prune(now time.Time) {
	l.recent = since(l.recent, now.Add(-allFailWindow))
	for k, a := range l.addrs {
		if a.at = since(a.at, now.Add(-addrFailWindow)); len(a.at) == 0 && !now.Before(a.until) {
			delete(l.addrs, k)
		}
	}
}

// since is the times after `from` (times are in order).
func since(times []time.Time, from time.Time) []time.Time {
	i := 0
	for i < len(times) && !times[i].After(from) {
		i++
	}
	return times[i:]
}

// lockedOut lists the addresses refused now, with until when (panel-status).
func (l *limiter) lockedOut(now time.Time) []string {
	var out []string
	for k, a := range l.addrs {
		if now.Before(a.until) {
			out = append(out, fmt.Sprintf("%s until %s", k, a.until.Format(time.DateTime)))
		}
	}
	return out
}

// failures counts the failed logins in the address window (panel-status).
func (l *limiter) failures(now time.Time) int {
	n := 0
	for _, a := range l.addrs {
		n += len(since(a.at, now.Add(-addrFailWindow)))
	}
	return n
}

// ---- addresses

// clientIP is a request's address (an IPv4 address written as IPv6 is the IPv4 one).
func clientIP(r *http.Request) netip.Addr {
	ap, err := netip.ParseAddrPort(r.RemoteAddr)
	if err != nil {
		return netip.Addr{}
	}
	return ap.Addr().Unmap()
}

// connIP is a connection's remote address.
func connIP(a net.Addr) netip.Addr {
	if t, ok := a.(*net.TCPAddr); ok {
		return t.AddrPort().Addr().Unmap()
	}
	ap, err := netip.ParseAddrPort(a.String())
	if err != nil {
		return netip.Addr{}
	}
	return ap.Addr().Unmap()
}

// limitKey is what the limits count by: an IPv4 address, or an IPv6 /64 (one customer's network).
func limitKey(a netip.Addr) string {
	if a.Is6() {
		p, _ := a.WithZone("").Prefix(64)
		return p.String()
	}
	return a.String()
}
