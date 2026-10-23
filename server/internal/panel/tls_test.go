package panel

import (
	"bufio"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"fmt"
	"io"
	"math/big"
	"net"
	"net/http"
	"net/http/cookiejar"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"ttt2-server/internal/config"
)

// browser is an HTTPS client that keeps cookies and sends its Origin on POSTs, as a browser does.
type browser struct {
	t      *testing.T
	base   string
	client *http.Client
}

func newBrowser(t *testing.T, addr string, tlsCfg *tls.Config) *browser {
	jar, _ := cookiejar.New(nil)
	return &browser{t: t, base: "https://" + addr, client: &http.Client{Jar: jar, Timeout: 10 * time.Second,
		Transport: &http.Transport{TLSClientConfig: tlsCfg}}}
}

func (b *browser) do(method, path string, form url.Values) (*http.Response, string) {
	b.t.Helper()
	req, err := http.NewRequest(method, b.base+path, strings.NewReader(form.Encode()))
	if err != nil {
		b.t.Fatal(err)
	}
	if method == http.MethodPost {
		req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
		req.Header.Set("Origin", b.base)
	}
	resp, err := b.client.Do(req)
	if err != nil {
		b.t.Fatal(err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	return resp, string(body)
}

func panelConfig(port int) config.Config {
	cfg := config.Default()
	cfg.AdminPanel, cfg.AdminPanelBind, cfg.AdminPanelPort = true, "127.0.0.1", port
	return cfg
}

// The panel over real TLS on loopback with the server's own certificate: TLS 1.3 (1.2 still, older refused), the
// fingerprint the log shows is the served certificate's, a browser logs in and bans, plain HTTP gets no page, the
// allowlist closes connections before TLS, and the connection limit holds.
func TestRealTLS(t *testing.T) {
	f := newFixture(t)
	if err := f.p.Apply(panelConfig(0)); err != nil || !f.p.Running() {
		t.Fatalf("%v", err)
	}
	addr := f.p.Addr()
	fp := certFingerprint(f.id.Cert.Certificate[0])
	if !strings.Contains(f.log.String(), "admin panel on https://127.0.0.1:"+addr[strings.LastIndex(addr, ":")+1:]+"/ ") ||
		!strings.Contains(f.log.String(), "its certificate's SHA-256 fingerprint is "+fp+"\n") || len(fp) != 95 {
		t.Fatalf("log:\n%s", f.log)
	}
	c, err := tls.Dial("tcp", addr, &tls.Config{InsecureSkipVerify: true})
	if err != nil {
		t.Fatal(err)
	}
	st := c.ConnectionState()
	if st.Version != tls.VersionTLS13 || certFingerprint(st.PeerCertificates[0].Raw) != fp {
		t.Fatalf("version %x, fingerprint %s", st.Version, certFingerprint(st.PeerCertificates[0].Raw))
	}
	c.Close()
	if c, err := tls.Dial("tcp", addr, &tls.Config{InsecureSkipVerify: true, MaxVersion: tls.VersionTLS12}); err != nil ||
		c.ConnectionState().Version != tls.VersionTLS12 {
		t.Fatalf("TLS 1.2: %v", err)
	} else {
		c.Close()
	}
	if c, err := tls.Dial("tcp", addr, &tls.Config{InsecureSkipVerify: true, MaxVersion: tls.VersionTLS11}); err == nil {
		c.Close()
		t.Fatal("TLS 1.1 accepted")
	}
	if resp, err := http.Get("http://" + addr + "/login"); err == nil {
		body, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		if resp.StatusCode != http.StatusBadRequest || strings.Contains(string(body), "<form") {
			t.Fatalf("plain HTTP: %d %q", resp.StatusCode, body)
		}
	}

	b := newBrowser(t, addr, &tls.Config{InsecureSkipVerify: true})
	resp, body := b.do(http.MethodPost, "/login", url.Values{"password": {f.password}, "code": {f.code()}})
	if resp.StatusCode != http.StatusOK || resp.Request.URL.Path != "/" || !strings.Contains(body, "Online players") {
		t.Fatalf("%d %s\n%s", resp.StatusCode, resp.Request.URL, body)
	}
	if resp.Header.Get("Content-Security-Policy") == "" || resp.Header.Get("X-Frame-Options") != "DENY" ||
		resp.Header.Get("Strict-Transport-Security") != "" || resp.Proto != "HTTP/1.1" {
		t.Fatalf("%s %v", resp.Proto, resp.Header)
	}
	setCookie := ""
	u, _ := url.Parse(b.base)
	for _, k := range b.client.Jar.Cookies(u) {
		if k.Name == sessionCookie {
			setCookie = k.Value
		}
	}
	if len(setCookie) != 64 {
		t.Fatal("no session cookie kept")
	}
	const mark = `name="csrf" value="`
	i := strings.Index(body, mark)
	csrf := body[i+len(mark) : i+len(mark)+64]
	if resp, body := b.do(http.MethodPost, "/ban", url.Values{"csrf": {csrf}, "steam_id": {"76561198000000001"}, "days": {"7"},
		"reason": {"cheating"}}); resp.StatusCode != http.StatusOK || !strings.Contains(body, "Banned 76561198000000001 7 days.") {
		t.Fatalf("%d\n%s", resp.StatusCode, body)
	}
	if len(f.h.Bans()) != 1 || !strings.Contains(f.audited(), "panel: admin from 127.0.0.1 banned 76561198000000001 7 days: cheating") {
		t.Fatalf("%+v\n%s", f.h.Bans(), f.audited())
	}

	// the connection limit per address: the 17th connection is closed at once
	var held []net.Conn
	for i := 0; i < maxConnsEach; i++ {
		c, err := net.Dial("tcp", addr)
		if err != nil {
			t.Fatal(err)
		}
		held = append(held, c)
	}
	time.Sleep(100 * time.Millisecond)
	if c, err := tls.Dial("tcp", addr, &tls.Config{InsecureSkipVerify: true}); err == nil {
		c.Close()
		t.Fatal("a 17th connection")
	}
	for _, c := range held {
		c.Close()
	}
	time.Sleep(100 * time.Millisecond)
	if c, err := tls.Dial("tcp", addr, &tls.Config{InsecureSkipVerify: true}); err != nil {
		t.Fatalf("after the others closed: %v", err)
	} else {
		c.Close()
	}

	// outside admin_panel_allow: closed before TLS (a reload applies the list at once)
	cfg := f.h.Config()
	cfg.AdminPanelAllow = []string{"203.0.113.0/24"}
	f.h.SetConfig(cfg)
	if c, err := tls.Dial("tcp", addr, &tls.Config{InsecureSkipVerify: true}); err == nil {
		c.Close()
		t.Fatal("a connection from outside the allowlist")
	}
	cfg.AdminPanelAllow = []string{"127.0.0.0/8"}
	f.h.SetConfig(cfg)
	if resp, _ := b.do(http.MethodGet, "/bans", nil); resp.StatusCode != http.StatusOK {
		t.Fatal(resp.StatusCode)
	}

	// a reload moves it to another port (the sessions stay), and stops it when it is off
	port := freePort(t)
	if err := f.p.Apply(panelConfig(port)); err != nil || f.p.Addr() != "127.0.0.1:"+strconv.Itoa(port) {
		t.Fatalf("%v %s", err, f.p.Addr())
	}
	if _, err := net.DialTimeout("tcp", addr, time.Second); err == nil {
		t.Fatal("the old port still listens")
	}
	b.base = "https://" + f.p.Addr()
	if resp, body := b.do(http.MethodGet, "/bans", nil); resp.StatusCode != http.StatusOK || !strings.Contains(body, "cheating") {
		t.Fatalf("%d", resp.StatusCode)
	}
	if err := f.p.Apply(config.Default()); err != nil || f.p.Running() {
		t.Fatal("still running")
	}
	if _, err := net.DialTimeout("tcp", "127.0.0.1:"+strconv.Itoa(port), time.Second); err == nil {
		t.Fatal("a stopped panel listens")
	}
}

// admin_panel_cert / admin_panel_key: the panel serves that certificate (its name in the URL), sends HSTS, and a
// reload reads renewed files.
func TestRealCertificate(t *testing.T) {
	f := newFixture(t)
	writeCert(t, f.dir, "panel.example")
	cfg := panelConfig(0)
	cfg.AdminPanelCert, cfg.AdminPanelKey = "fullchain.pem", filepath.Join(f.dir, "privkey.pem")
	if err := f.p.Apply(cfg); err != nil {
		t.Fatal(err)
	}
	addr := f.p.Addr()
	_, port, _ := net.SplitHostPort(addr)
	if !strings.Contains(f.log.String(), "admin panel on https://panel.example:"+port+"/ ") {
		t.Fatalf("log:\n%s", f.log)
	}
	served := func() (*x509.Certificate, *http.Response) {
		pemBytes, _ := os.ReadFile(filepath.Join(f.dir, "fullchain.pem"))
		pool := x509.NewCertPool()
		pool.AppendCertsFromPEM(pemBytes)
		b := newBrowser(t, addr, &tls.Config{RootCAs: pool, ServerName: "panel.example"})
		resp, _ := b.do(http.MethodGet, "/login", nil)
		return resp.TLS.PeerCertificates[0], resp
	}
	first, resp := served()
	if resp.Header.Get("Strict-Transport-Security") != "max-age=31536000" || first.DNSNames[0] != "panel.example" {
		t.Fatalf("%v", resp.Header)
	}
	writeCert(t, f.dir, "panel.example")
	if err := f.p.Apply(cfg); err != nil {
		t.Fatal(err)
	}
	second, _ := served()
	if second.Equal(first) {
		t.Fatal("the renewed certificate is not served")
	}
	if fp := certFingerprint(second.Raw); !strings.Contains(f.p.Status(), "certificate SHA-256 fingerprint "+fp) ||
		!strings.Contains(f.log.String(), "admin panel: a new certificate, SHA-256 fingerprint "+fp) {
		t.Fatalf("the new fingerprint:\n%s\n%s", f.p.Status(), f.log)
	}
	// a broken renewal keeps the certificate in use
	os.WriteFile(filepath.Join(f.dir, "fullchain.pem"), []byte("broken"), 0o600)
	if err := f.p.Apply(cfg); err == nil || !f.p.Running() {
		t.Fatalf("%v", err)
	}
	// a missing certificate: the panel does not start
	f.p.Close()
	if err := f.p.Apply(cfg); err == nil || f.p.Running() {
		t.Fatal("started without its certificate")
	}
}

// writeCert writes a self-signed certificate for name (as a CA would issue one: the name in the SAN) and its key.
func writeCert(t *testing.T, dir, name string) {
	t.Helper()
	key, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	serial, _ := rand.Int(rand.Reader, big.NewInt(1<<62))
	tmpl := &x509.Certificate{
		SerialNumber: serial, Subject: pkix.Name{CommonName: name}, DNSNames: []string{name},
		NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().AddDate(0, 3, 0),
		KeyUsage: x509.KeyUsageDigitalSignature | x509.KeyUsageCertSign, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true, IsCA: true,
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	keyDER, _ := x509.MarshalPKCS8PrivateKey(key)
	os.WriteFile(filepath.Join(dir, "fullchain.pem"), pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), 0o600)
	os.WriteFile(filepath.Join(dir, "privkey.pem"), pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER}), 0o600)
}

func freePort(t *testing.T) int {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	return ln.Addr().(*net.TCPAddr).Port
}

// A refused login is answered after its request was read, and whole, also to a client that sends "Connection:
// close": a connection closed with unread input is reset, and the reset can take the end of the page with it (a
// Python client lost it so). The body is sent late: no answer may come before it.
func TestRefusedLoginArrivesWhole(t *testing.T) {
	f := newFixture(t)
	if err := f.p.Apply(panelConfig(0)); err != nil {
		t.Fatal(err)
	}
	addr := f.p.Addr()
	for i := 0; i < addrFailLimit+2; i++ {
		c, err := tls.Dial("tcp", addr, &tls.Config{InsecureSkipVerify: true})
		if err != nil {
			t.Fatal(err)
		}
		body := "password=x&code=000000"
		head := strings.Join([]string{"POST /login HTTP/1.1", "Host: " + addr, "Origin: https://" + addr,
			"Content-Type: application/x-www-form-urlencoded", fmt.Sprintf("Content-Length: %d", len(body)), "Connection: close", "", ""}, "\r\n")
		io.WriteString(c, head)
		c.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
		if n, err := c.Read(make([]byte, 1)); n > 0 || err == nil {
			t.Fatalf("login %d: answered before its request was read", i)
		}
		c.SetReadDeadline(time.Now().Add(5 * time.Second))
		io.WriteString(c, body)
		resp, err := http.ReadResponse(bufio.NewReader(c), nil)
		if err != nil {
			t.Fatal(err)
		}
		page, err := io.ReadAll(resp.Body)
		c.Close()
		want := http.StatusUnauthorized
		if i >= addrFailLimit {
			want = http.StatusTooManyRequests
		}
		if resp.StatusCode != want || err != nil || int64(len(page)) != resp.ContentLength || !strings.Contains(string(page), "</html>") {
			t.Fatalf("login %d: %d, %d of %d bytes, %v", i, resp.StatusCode, len(page), resp.ContentLength, err)
		}
	}
}
