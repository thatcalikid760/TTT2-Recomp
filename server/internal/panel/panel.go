// Package panel is the web admin panel (README.md "Admin panel"): the admin commands' players, kicks, bans and
// matches in a browser, over HTTPS on its own TCP port (admin_panel_port). It is off unless server.toml turns it on,
// and does not start before its login exists (the admin command panel-setup).
//
// What keeps it to its owner: admin_panel_allow (other addresses are closed before TLS); TLS 1.2 at least; a
// password (PBKDF2) and an authenticator code (TOTP, each code once) to log in, at most 5 failures per address
// (IPv6: /64) in 15 minutes and 30 on the whole panel in a minute, each answered a second late; sessions in an
// HttpOnly, Secure, SameSite=Strict __Host- cookie, ending 12 h after the login or an hour unused; every action a
// POST from the panel's own page (Origin / Referer) with the session's CSRF token; every player's text escaped by
// html/template (pages) and encoding/json (live updates), and panel.js puts it in the page as text only; one script,
// the panel's own (the Content-Security-Policy allows no other, no inline script, no other site), and every page
// and action works without it; strict security headers; every login and action in the log and in
// <data dir>/admin_audit.log.
//
// Pages update live over Server-Sent Events (live.go): one producer goroutine, while a page is open, compares a
// snapshot of the hub a second and sends what changed.
package panel

import (
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"fmt"
	"html/template"
	"log"
	"net"
	"net/http"
	"net/netip"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"ttt2-server/internal/config"
	"ttt2-server/internal/hub"
	"ttt2-server/internal/store"
	"ttt2-server/internal/tlsid"
)

const (
	auditFile    = "admin_audit.log"
	maxConns     = 256 // connections to the panel at once ...
	maxConnsEach = 16  // ... from one address (IPv6: one /64)
	verifyAtOnce = 2   // password checks at once (each is 600000 PBKDF2 iterations)
)

// Panel is the admin panel: always made (the admin commands manage its login), listening only while Apply says so.
type Panel struct {
	h       *hub.Hub
	db      *store.Store
	id      tlsid.Identity
	dataDir string
	baseDir string // relative certificate paths start here (the configuration file's folder)
	log     *log.Logger
	now     func() time.Time // the tests' clock
	pages   *template.Template
	verify  chan struct{} // password checks in progress
	live    *live

	mu       sync.Mutex // the login, the sessions, the limits, the codes used
	creds    *credentials
	sessions map[[32]byte]*session
	limits   limiter
	used     map[int64]time.Time // TOTP steps accepted, remembered until then

	runMu       sync.Mutex // the running server and what it was started with
	srv         *http.Server
	listen      listenSettings
	addr, url   string
	fingerprint string
	on          bool   // admin_panel, as Apply last saw it
	why         string // why a panel server.toml turns on is not running
	cert        atomic.Pointer[tls.Certificate]
	realCert    atomic.Bool // admin_panel_cert: a real certificate (HSTS is sent)

	auditMu sync.Mutex
}

// listenSettings are what a running panel restarts for when they change.
type listenSettings struct {
	bind      string
	port      int
	cert, key string
}

// New makes the panel of a hub and its database (dataDir holds the login and the audit log; baseDir is the
// configuration file's folder).
func New(h *hub.Hub, db *store.Store, id tlsid.Identity, dataDir, baseDir string, logger *log.Logger) *Panel {
	p := &Panel{
		h: h, db: db, id: id, dataDir: dataDir, baseDir: baseDir, log: logger, now: time.Now,
		pages:    parsePages(),
		verify:   make(chan struct{}, verifyAtOnce),
		sessions: map[[32]byte]*session{},
		limits:   newLimiter(),
		used:     map[int64]time.Time{},
	}
	p.live = newLive(p)
	return p
}

func (p *Panel) loginPath() string { return filepath.Join(p.dataDir, loginFile) }

// Apply makes the panel what the configuration says, at start and at every reload: started when admin_panel is on
// (and its login exists), stopped when it is off, restarted when its port, address or certificate files changed; a
// running panel with a real certificate reads its files again (a renewed certificate). The error says why a panel
// that should run does not; the game server runs on either way.
func (p *Panel) Apply(cfg config.Config) error {
	p.runMu.Lock()
	defer p.runMu.Unlock()
	want := listenSettings{cfg.AdminPanelBind, cfg.AdminPanelPort, cfg.AdminPanelCert, cfg.AdminPanelKey}
	if p.srv != nil && (!cfg.AdminPanel || want != p.listen) {
		p.srv.Close()
		p.srv = nil
		p.log.Printf("admin panel stopped")
	}
	p.on, p.why = cfg.AdminPanel, ""
	switch {
	case !cfg.AdminPanel:
		return nil
	case p.srv != nil:
		if cfg.AdminPanelCert != "" {
			cert, err := p.loadCertificate(cfg)
			if err != nil {
				return fmt.Errorf("admin panel certificate not reloaded (the old one stays): %w", err)
			}
			p.cert.Store(cert)
			if fp := certFingerprint(cert.Certificate[0]); fp != p.fingerprint {
				p.fingerprint = fp
				p.log.Printf("admin panel: a new certificate, SHA-256 fingerprint %s", fp)
			}
		}
		return nil
	}
	err := p.startLocked(cfg, want)
	if err != nil {
		p.why = err.Error()
	}
	return err
}

// startLocked loads the login and the certificate and starts listening.
func (p *Panel) startLocked(cfg config.Config, want listenSettings) error {
	creds, err := loadLogin(p.loginPath())
	if isNotExist(err) {
		err = ErrNoLogin
	}
	if err != nil {
		return err
	}
	if runtime.GOOS != "windows" {
		if st, err := os.Stat(p.loginPath()); err == nil && st.Mode().Perm()&0o077 != 0 {
			p.log.Printf("WARNING: %s can be read by other users (chmod 600 %s)", p.loginPath(), p.loginPath())
		}
	}
	cert, err := p.loadCertificate(cfg)
	if err != nil {
		return err
	}
	ln, err := net.Listen("tcp", net.JoinHostPort(cfg.AdminPanelBind, strconv.Itoa(cfg.AdminPanelPort)))
	if err != nil {
		return err
	}
	p.mu.Lock()
	p.creds = creds
	p.mu.Unlock()
	p.cert.Store(cert)
	p.realCert.Store(cfg.AdminPanelCert != "")
	srv := p.newServer()
	go func() {
		if err := srv.ServeTLS(&gate{Listener: ln, p: p, per: map[string]int{}}, "", ""); !errors.Is(err, http.ErrServerClosed) {
			p.log.Printf("admin panel: %v", err)
		}
	}()
	p.srv, p.listen, p.addr = srv, want, ln.Addr().String()
	p.url = panelURL(cfg.AdminPanelBind, ln.Addr().(*net.TCPAddr).Port, cert, cfg.AdminPanelCert != "")
	p.fingerprint = certFingerprint(cert.Certificate[0])
	p.log.Printf("admin panel on %s (listening on %s; %s); its certificate's SHA-256 fingerprint is %s",
		p.url, p.addr, p.allowText(), p.fingerprint)
	return nil
}

// Close stops a running panel (the sessions stay: Apply can start it again) and its live streams.
func (p *Panel) Close() {
	p.runMu.Lock()
	defer p.runMu.Unlock()
	if p.srv != nil {
		p.srv.Close()
		p.srv = nil
	}
	p.live.endAll("the panel stopped", true)
}

// Running reports whether the panel listens.
func (p *Panel) Running() bool {
	p.runMu.Lock()
	defer p.runMu.Unlock()
	return p.srv != nil
}

// Addr is the address the panel listens on ("" when it does not).
func (p *Panel) Addr() string {
	p.runMu.Lock()
	defer p.runMu.Unlock()
	if p.srv == nil {
		return ""
	}
	return p.addr
}

// The panel server's timeouts (variables for the tests). A live stream (GET /events) moves its own deadlines on with
// every write; every other request has these.
var (
	serverReadTimeout  = 10 * time.Second
	serverWriteTimeout = 30 * time.Second
)

// newServer is the panel's HTTPS server: HTTP/1.1 only, every timeout set, small headers.
func (p *Panel) newServer() *http.Server {
	srv := &http.Server{
		Handler: p.Handler(),
		TLSConfig: &tls.Config{
			MinVersion:     tls.VersionTLS12, // TLS 1.3 when the browser has it (all current ones)
			GetCertificate: func(*tls.ClientHelloInfo) (*tls.Certificate, error) { return p.cert.Load(), nil },
			// TLS 1.2 only with forward-secret AEAD suites (TLS 1.3 suites are not configurable and all fine)
			CipherSuites: []uint16{
				tls.TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, tls.TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
				tls.TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, tls.TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
				tls.TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, tls.TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
			},
		},
		ReadHeaderTimeout: 5 * time.Second, // the TLS handshake too
		ReadTimeout:       serverReadTimeout,
		WriteTimeout:      serverWriteTimeout,
		IdleTimeout:       2 * time.Minute,
		MaxHeaderBytes:    8 << 10,
		ErrorLog:          log.New(httpLog{p.log}, "", 0),
	}
	srv.Protocols = new(http.Protocols)
	srv.Protocols.SetHTTP1(true)
	return srv
}

// httpLog passes the HTTP server's own errors to the log, except failed TLS handshakes (scanners, and browsers
// meeting the self-signed certificate: noise).
type httpLog struct{ log *log.Logger }

func (l httpLog) Write(b []byte) (int, error) {
	if line := strings.TrimSpace(string(b)); !strings.Contains(line, "TLS handshake error") {
		l.log.Printf("admin panel: %s", line)
	}
	return len(b), nil
}

// loadCertificate is the certificate the panel serves: admin_panel_cert + admin_panel_key, or the server's own.
func (p *Panel) loadCertificate(cfg config.Config) (*tls.Certificate, error) {
	if cfg.AdminPanelCert == "" {
		c := p.id.Cert
		return &c, nil
	}
	certPath, keyPath := p.path(cfg.AdminPanelCert), p.path(cfg.AdminPanelKey)
	c, err := tls.LoadX509KeyPair(certPath, keyPath)
	if err != nil {
		return nil, fmt.Errorf("admin_panel_cert %s, admin_panel_key %s: %w", certPath, keyPath, err)
	}
	if c.Leaf, err = x509.ParseCertificate(c.Certificate[0]); err != nil {
		return nil, err
	}
	if p.now().After(c.Leaf.NotAfter) {
		p.log.Printf("WARNING: admin panel certificate %s expired on %s", certPath, c.Leaf.NotAfter.Format(time.DateTime))
	}
	return &c, nil
}

// path is a configured file's path (a relative one is relative to the configuration file).
func (p *Panel) path(name string) string {
	if filepath.IsAbs(name) {
		return name
	}
	return filepath.Join(p.baseDir, name)
}

// panelURL is the panel's address for the log: the certificate's domain, the address it listens on, or a
// placeholder when it listens on every address.
func panelURL(bind string, port int, cert *tls.Certificate, real bool) string {
	host := bind
	if real && cert.Leaf != nil && len(cert.Leaf.DNSNames) > 0 && !strings.HasPrefix(cert.Leaf.DNSNames[0], "*") {
		host = cert.Leaf.DNSNames[0]
	} else if ip := net.ParseIP(bind); bind == "" || ip.IsUnspecified() {
		host = "<this server's address>"
	}
	return "https://" + net.JoinHostPort(host, strconv.Itoa(port)) + "/"
}

// certFingerprint is a certificate's SHA-256 fingerprint as browsers show it (AB:CD:...).
func certFingerprint(der []byte) string {
	sum := sha256.Sum256(der)
	parts := make([]string, len(sum))
	for i, b := range sum {
		parts[i] = fmt.Sprintf("%02X", b)
	}
	return strings.Join(parts, ":")
}

// allowed reports whether an address may reach the panel (admin_panel_allow; empty: every address). It reads the
// current configuration: a reload applies a new list at once.
func (p *Panel) allowed(ip netip.Addr) bool {
	nets, err := config.ParseAllow(p.h.Config().AdminPanelAllow)
	if err != nil {
		return false // never: the configuration was checked when it was read
	}
	if len(nets) == 0 {
		return true
	}
	for _, n := range nets {
		if n.Contains(ip) {
			return true
		}
	}
	return false
}

func (p *Panel) allowText() string {
	list := p.h.Config().AdminPanelAllow
	if len(list) == 0 {
		return "open to every address: admin_panel_allow is empty"
	}
	return "only from " + strings.Join(list, ", ")
}

// gate is the panel's listener: a connection from outside admin_panel_allow is closed before anything else (no TLS,
// no page), and connections are limited in all and per address.
type gate struct {
	net.Listener
	p   *Panel
	mu  sync.Mutex
	all int
	per map[string]int
}

func (g *gate) Accept() (net.Conn, error) {
	for {
		c, err := g.Listener.Accept()
		if err != nil {
			return nil, err
		}
		ip := connIP(c.RemoteAddr())
		key := limitKey(ip)
		if !g.p.allowed(ip) || !g.take(key) {
			c.Close()
			continue
		}
		return &gatedConn{Conn: c, done: func() { g.give(key) }}, nil
	}
}

func (g *gate) take(key string) bool {
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.all >= maxConns || g.per[key] >= maxConnsEach {
		return false
	}
	g.all++
	g.per[key]++
	return true
}

func (g *gate) give(key string) {
	g.mu.Lock()
	defer g.mu.Unlock()
	g.all--
	if g.per[key]--; g.per[key] <= 0 {
		delete(g.per, key)
	}
}

// gatedConn gives its place back when it closes.
type gatedConn struct {
	net.Conn
	once sync.Once
	done func()
}

func (c *gatedConn) Close() error {
	c.once.Do(c.done)
	return c.Conn.Close()
}

// ---- the login (admin commands)

// Setup makes the login: a new random password and a new authenticator secret (base32), returned to be shown once.
// An existing login stays unless force; a new one ends every session.
func (p *Panel) Setup(force bool) (password, secret string, err error) {
	if _, err := os.Stat(p.loginPath()); err == nil && !force {
		return "", "", ErrLoginExists
	}
	creds, password, err := newLogin(p.now())
	if err != nil {
		return "", "", err
	}
	if err := p.store(creds); err != nil {
		return "", "", err
	}
	return password, creds.TOTPSecret, nil
}

// NewPassword gives the login a new random password (the authenticator secret stays) and ends every session.
func (p *Panel) NewPassword() (string, error) {
	creds, err := loadLogin(p.loginPath())
	if isNotExist(err) {
		err = ErrNoLogin
	}
	if err != nil {
		return "", err
	}
	password, err := randomPassword()
	if err != nil {
		return "", err
	}
	if creds.Password, err = hashPassword(password); err != nil {
		return "", err
	}
	creds.PasswordSet = p.now().UTC()
	return password, p.store(creds)
}

// store writes a new login and makes it the panel's: every session ends, no code counts as used.
func (p *Panel) store(creds *credentials) error {
	if err := os.MkdirAll(p.dataDir, 0o700); err != nil {
		return err
	}
	if err := saveLogin(p.loginPath(), creds); err != nil {
		return err
	}
	p.mu.Lock()
	p.creds = creds
	clear(p.sessions)
	clear(p.used)
	p.mu.Unlock()
	p.live.endAll("every session was ended (a new login)", false)
	return nil
}

// Status is panel-status: running or why not, its address and certificate, the login, sessions and lockouts.
func (p *Panel) Status() string {
	var b strings.Builder
	p.runMu.Lock()
	switch {
	case p.srv != nil:
		fmt.Fprintf(&b, "admin panel: running on %s (listening on %s; %s)\ncertificate SHA-256 fingerprint %s\n",
			p.url, p.addr, p.allowText(), p.fingerprint)
	case !p.on:
		b.WriteString("admin panel: off (admin_panel = false in server.toml)\n")
	case p.why != "":
		fmt.Fprintf(&b, "admin panel: not running: %s\n", p.why)
	default:
		b.WriteString("admin panel: not running\n")
	}
	p.runMu.Unlock()
	creds, err := loadLogin(p.loginPath())
	switch {
	case isNotExist(err):
		b.WriteString("login: none yet (panel-setup makes it)\n")
	case err != nil:
		fmt.Fprintf(&b, "login: %v (panel-setup force makes a new one)\n", err)
	default:
		fmt.Fprintf(&b, "login: user %s, made %s, password set %s\n", creds.User,
			creds.Created.Local().Format(time.DateTime), creds.PasswordSet.Local().Format(time.DateTime))
	}
	now := p.now()
	p.mu.Lock()
	defer p.mu.Unlock()
	p.limits.prune(now)
	fmt.Fprintf(&b, "sessions: %d (at most %d), live pages open: %d\nfailed logins in the last 15 minutes: %d\n",
		len(p.sessions), maxSessions, p.live.openCount(), p.limits.failures(now))
	if out := p.limits.lockedOut(now); len(out) > 0 {
		fmt.Fprintf(&b, "locked out: %s\n", strings.Join(out, ", "))
	}
	if now.Before(p.limits.paused) {
		fmt.Fprintf(&b, "every login paused until %s\n", p.limits.paused.Format(time.DateTime))
	}
	return b.String()
}

// Audit logs a line and appends it to <data dir>/admin_audit.log (readable by the server's account only).
func (p *Panel) Audit(format string, args ...any) { p.audit(format, args...) }

func (p *Panel) audit(format string, args ...any) {
	line := fmt.Sprintf(format, args...)
	p.log.Print(line)
	p.auditMu.Lock()
	defer p.auditMu.Unlock()
	f, err := os.OpenFile(filepath.Join(p.dataDir, auditFile), os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o600)
	if err == nil {
		_, err = fmt.Fprintf(f, "%s %s\n", p.now().Format(time.DateTime), line)
		if cerr := f.Close(); err == nil {
			err = cerr
		}
	}
	if err != nil {
		p.log.Printf("WARNING: %s not written: %v", auditFile, err)
	}
}
