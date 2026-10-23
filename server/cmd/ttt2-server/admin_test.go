package main

import (
	"bytes"
	"crypto/tls"
	"log"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"

	"ttt2-server/internal/config"
	"ttt2-server/internal/hub"
	"ttt2-server/internal/panel"
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

type adminServer struct {
	cmds    *commands
	h       *hub.Hub
	log     *syncBuffer
	game    string // the game port
	addr    string // the admin port
	token   string
	stopped chan struct{}
	ln      net.Listener
	dir     string // the data folder
}

// startAdminServer: a hub (trusting Steam IDs, serving on loopback) with its database and an admin port.
func startAdminServer(t *testing.T) *adminServer {
	t.Helper()
	oldDelay := adminRefusalDelay
	adminRefusalDelay = 10 * time.Millisecond
	t.Cleanup(func() { adminRefusalDelay = oldDelay })
	dir := t.TempDir()
	db, err := store.Open(filepath.Join(dir, "server.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	id, err := tlsid.LoadOrCreate(dir)
	if err != nil {
		t.Fatal(err)
	}
	logs := &syncBuffer{}
	logger := log.New(logs, "", 0)
	cfg := config.Default()
	cfg.TrustUnverified = true
	h := hub.New(cfg, id, nil, hub.NewBans(""), logger)
	h.SetRecords(db)
	h.SetStats(db)
	h.SetMatches(db)
	tcp, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	udp, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	go h.Serve(tcp, udp)
	t.Cleanup(func() { tcp.Close(); udp.Close() })

	s := &adminServer{h: h, log: logs, game: tcp.Addr().String(), stopped: make(chan struct{}), dir: dir}
	s.cmds = &commands{h: h, db: db, panel: panel.New(h, db, id, dir, dir, logger), configPath: filepath.Join(dir, "server.toml"),
		logger: logger, console: os.Stdout}
	if s.ln, err = net.Listen("tcp", "127.0.0.1:0"); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.ln.Close() })
	if s.token, err = newAdminToken(); err != nil || len(s.token) != 64 {
		t.Fatalf("token %q %v", s.token, err)
	}
	s.addr = s.ln.Addr().String()
	var once sync.Once
	go serveAdmin(s.ln, s.token, s.cmds, func() { once.Do(func() { close(s.stopped) }) }, logger)
	return s
}

func (s *adminServer) ask(t *testing.T, token, command string) string {
	t.Helper()
	var out bytes.Buffer
	if err := adminRequest(s.addr, token, command, &out); err != nil {
		t.Fatal(err)
	}
	return out.String()
}

func TestAdminPortCommands(t *testing.T) {
	s := startAdminServer(t)
	if got := s.ask(t, s.token, "bans"); got != "nobody is banned\n" {
		t.Fatalf("%q", got)
	}
	if got := s.ask(t, s.token, "ban 76561198000000001 7 cheating"); got != "1 banned\n" {
		t.Fatalf("%q", got)
	}
	if got := s.ask(t, s.token, "bans"); !strings.HasPrefix(got, "76561198000000001") || !strings.Contains(got, " until ") ||
		!strings.HasSuffix(got, " cheating\n") {
		t.Fatalf("%q", got)
	}
	// a wrong token runs nothing, and is logged
	wrong := strings.Repeat("0", 64)
	if got := s.ask(t, wrong, "unban 76561198000000001"); got != "wrong admin token\n" {
		t.Fatalf("%q", got)
	}
	if got := s.ask(t, s.token[:63], "unban 76561198000000001"); got != "wrong admin token\n" {
		t.Fatalf("a shorter token: %q", got)
	}
	if got := s.ask(t, s.token, "bans"); !strings.Contains(got, "76561198000000001") {
		t.Fatal("a wrong token's command ran")
	}
	if !strings.Contains(s.log.String(), "admin port: a connection with a wrong token refused") {
		t.Fatalf("log:\n%s", s.log)
	}
	// an admin connection's answer carries what the console sees in the log
	if got := s.ask(t, s.token, "unban 76561198000000001"); got != "admin: unbanned 76561198000000001\n" {
		t.Fatalf("%q", got)
	}
	if !strings.Contains(s.log.String(), "admin: unbanned 76561198000000001\n") || !strings.Contains(s.log.String(), `admin: "bans" (admin port)`) {
		t.Fatalf("remote commands are logged:\n%s", s.log)
	}
	if got := s.ask(t, s.token, "status"); !strings.HasPrefix(got, "0 players online, 0 in rooms, 0 in matches,") {
		t.Fatalf("%q", got)
	}
	if got := s.ask(t, s.token, "matches"); got != "no matches recorded\n" {
		t.Fatalf("%q", got)
	}
	if got := s.ask(t, s.token, "kick nobody 5000"); !strings.HasPrefix(got, "kick: minutes 0..1440") {
		t.Fatalf("%q", got)
	}
	if got := s.ask(t, s.token, "kick nobody 30 griefing"); got != "no online player \"nobody\"\n" {
		t.Fatalf("%q", got)
	}
	if got := s.ask(t, s.token, "frobnicate"); got != "unknown command (help lists them)\n" {
		t.Fatalf("%q", got)
	}
	select {
	case <-s.stopped:
		t.Fatal("stopped early")
	default:
	}
	if got := s.ask(t, s.token, "quit"); got != "stopping\n" {
		t.Fatalf("%q", got)
	}
	select {
	case <-s.stopped:
	case <-time.After(time.Second):
		t.Fatal("quit over the admin port must stop the server")
	}
}

func TestAdminPortLimits(t *testing.T) {
	oldTimeout := adminTimeout
	adminTimeout = 300 * time.Millisecond
	t.Cleanup(func() { adminTimeout = oldTimeout })
	s := startAdminServer(t)
	// connections that send nothing hold the slots until their deadline; one more is turned away
	var idle []net.Conn
	for i := 0; i < maxAdminConns; i++ {
		c, err := net.Dial("tcp", s.addr)
		if err != nil {
			t.Fatal(err)
		}
		idle = append(idle, c)
	}
	time.Sleep(50 * time.Millisecond)
	if got := s.ask(t, s.token, "bans"); got != "the admin port is busy: try again\n" {
		t.Fatalf("%q", got)
	}
	// the idle ones are closed after the deadline, the port answers again
	for _, c := range idle {
		c.SetReadDeadline(time.Now().Add(2 * time.Second))
		if _, err := c.Read(make([]byte, 1)); err == nil {
			t.Fatal("an idle admin connection got an answer")
		}
		c.Close()
	}
	time.Sleep(50 * time.Millisecond)
	if got := s.ask(t, s.token, "bans"); got != "nobody is banned\n" {
		t.Fatalf("%q", got)
	}
	// a command line over the limit is not run: the connection is dropped (a reset, or nothing)
	var out bytes.Buffer
	if err := adminRequest(s.addr, s.token, "say "+strings.Repeat("x", maxAdminLine), &out); err == nil && out.Len() != 0 {
		t.Fatalf("%q", out.String())
	}
	if strings.Contains(s.log.String(), "admin: \"say ") {
		t.Fatal("an over-long command ran")
	}
}

// The `admin` command line: the configuration's data folder, its token file, the answer on stdout.
func TestAdminClient(t *testing.T) {
	s := startAdminServer(t)
	dir := t.TempDir()
	cfgPath := filepath.Join(dir, "server.toml")
	if err := os.WriteFile(cfgPath, []byte(config.DefaultFile), 0o600); err != nil {
		t.Fatal(err)
	}
	run := func(path string, words ...string) (int, string, string) {
		var out, errs bytes.Buffer
		code := adminClient(path, words, &out, &errs)
		return code, out.String(), errs.String()
	}
	tokenPath := filepath.Join(dir, "data", adminTokenFile)
	if code, _, errs := run(cfgPath, "status"); code != 1 || errs != "the server is not running (no "+tokenPath+")\n" {
		t.Fatalf("%d %q", code, errs)
	}
	missing := filepath.Join(dir, "elsewhere.toml")
	if code, _, errs := run(missing, "status"); code != 1 || !strings.Contains(errs, "no configuration") {
		t.Fatalf("%d %q", code, errs)
	}
	if _, err := os.Stat(missing); err == nil {
		t.Fatal("the admin command must not write a configuration")
	}

	os.MkdirAll(filepath.Join(dir, "data"), 0o700)
	if err := writeAdminToken(tokenPath, s.addr, s.token); err != nil {
		t.Fatal(err)
	}
	if runtime.GOOS != "windows" {
		if st, _ := os.Stat(tokenPath); st.Mode().Perm() != 0o600 {
			t.Fatalf("token file mode %v", st.Mode().Perm())
		}
	}
	if addr, token, err := readAdminToken(tokenPath); err != nil || addr != s.addr || token != s.token {
		t.Fatalf("%q %q %v", addr, token, err)
	}
	if code, out, _ := run(cfgPath); code != 0 || out != helpText+"\n" {
		t.Fatalf("no words: help (%d %q)", code, out)
	}
	if code, out, _ := run(cfgPath, "ban", "76561198000000002", "1", "spam"); code != 0 || out != "1 banned\n" {
		t.Fatalf("%d %q", code, out)
	}
	if code, out, _ := run(cfgPath, "bans"); code != 0 || !strings.HasSuffix(out, " spam\n") {
		t.Fatalf("%d %q", code, out)
	}
	if code, _, _ := run(cfgPath, "say", "two\nlines"); code != 2 {
		t.Fatal("a command with a line break")
	}
	// the server stopped without removing its file (a crash)
	s.ln.Close()
	time.Sleep(50 * time.Millisecond)
	if code, _, errs := run(cfgPath, "status"); code != 1 || !strings.Contains(errs, "the server is not running") {
		t.Fatalf("%d %q", code, errs)
	}
}

// A kick through the admin port: a name with a space, minutes and a reason; the player's game is told.
func TestAdminKick(t *testing.T) {
	s := startAdminServer(t)
	c := login(t, s, 0x0110000100000A0A, "Jin Kazama")
	if got := s.ask(t, s.token, "kick jin kazama 30 griefing again"); got != "1 kicked\n" {
		t.Fatalf("%q", got)
	}
	c.SetReadDeadline(time.Now().Add(3 * time.Second))
	for {
		typ, payload, err := wire.ReadFrame(c)
		if err != nil {
			t.Fatal(err)
		}
		if typ != wire.Error {
			continue
		}
		r := wire.Reader{B: payload}
		if code, text := r.U32(), r.Str(); code != wire.ErrBanned ||
			text != "You were removed from this server. You can come back in 30 minutes. Reason: griefing again" {
			t.Fatalf("%d %q", code, text)
		}
		break
	}
	if got := s.ask(t, s.token, "bans"); !strings.Contains(got, "Jin Kazama") || !strings.HasSuffix(got, " kicked: griefing again\n") {
		t.Fatalf("%q", got)
	}
}

// players all: every player the database has seen, newest first, with ONLINE and BANNED marks; a search narrows it.
func TestAdminPlayersAll(t *testing.T) {
	s := startAdminServer(t)
	db := s.cmds.db
	at := time.Now().Add(-time.Hour)
	db.PlayerSeen(76561198000000001, "Kazuya", true, at)
	db.PlayerSeen(76561198000000002, "Nina", false, at.Add(time.Minute))
	db.PlayerSeen(76561198000000003, "Lili", true, at.Add(2*time.Minute))
	login(t, s, 0x0110000100000A0A, "Jin Kazama")
	login(t, s, 0x0110000100000B0B, "Hwoarang")
	for _, cmd := range []string{"ban 76561198000000002 7 cheating", "ban 76561198000000003", "kick hwoarang 30"} {
		if got := s.ask(t, s.token, cmd); !strings.HasPrefix(got, "1 ") {
			t.Fatalf("%s: %q", cmd, got)
		}
	}
	lines := strings.Split(strings.TrimSuffix(s.ask(t, s.token, "players all"), "\n"), "\n")
	if len(lines) != 5 {
		t.Fatalf("%q", lines)
	}
	byID := map[string]string{}
	for _, l := range lines {
		byID[strings.Fields(l)[0]] = l
	}
	for id, want := range map[string][]string{
		"76561197960268298": {"Jin Kazama", "first ", "  ONLINE"},
		"76561197960268555": {"Hwoarang", "  BANNED kicked until "},
		"76561198000000003": {"Lili", "  BANNED for good"},
		"76561198000000002": {"Nina", " (unverified)", "  BANNED until "},
		"76561198000000001": {"Kazuya", ", 1 logins"},
	} {
		for _, w := range want {
			if !strings.Contains(byID[id], w) {
				t.Errorf("%s: %q lacks %q", id, byID[id], w)
			}
		}
	}
	if strings.Contains(byID["76561197960268298"], "BANNED") || strings.Contains(byID["76561198000000003"], "ONLINE") ||
		!strings.HasSuffix(byID["76561198000000001"], " 1 logins") {
		t.Fatalf("%q", lines)
	}
	if !strings.HasPrefix(lines[2], "76561198000000003") || !strings.HasPrefix(lines[4], "76561198000000001") {
		t.Fatalf("newest last-seen first: %q", lines)
	}
	if got := s.ask(t, s.token, "players all KAZ"); strings.Count(got, "\n") != 2 || !strings.Contains(got, "Kazuya") || !strings.Contains(got, "Jin Kazama") {
		t.Fatalf("%q", got)
	}
	if got := s.ask(t, s.token, "players all 76561198000000002"); strings.Count(got, "\n") != 1 || !strings.Contains(got, "Nina") {
		t.Fatalf("%q", got)
	}
	if got := s.ask(t, s.token, "players ALL nobody"); got != "no player found\n" {
		t.Fatalf("%q", got)
	}
	// players alone: the online players, as before
	if got := s.ask(t, s.token, "players"); !strings.Contains(got, "Jin Kazama") || strings.Contains(got, "first ") || strings.Contains(got, "Kazuya") {
		t.Fatalf("%q", got)
	}
}

// panel-setup, panel-password, panel-status, panel-logout-all: the secrets only in the command's answer, once.
func TestAdminPanelCommands(t *testing.T) {
	s := startAdminServer(t)
	if s.cmds.panel.Running() {
		t.Fatal("the panel runs while server.toml has it off")
	}
	// admin_panel = true but no login yet: panel-setup starts it
	cfg := s.h.Config()
	cfg.AdminPanel, cfg.AdminPanelBind, cfg.AdminPanelPort = true, "127.0.0.1", 0
	s.h.SetConfig(cfg)
	t.Cleanup(s.cmds.panel.Close)
	field := func(out, name string) string {
		for _, l := range strings.Split(out, "\n") {
			if _, v, ok := strings.Cut(l, name); ok {
				return strings.TrimSpace(v)
			}
		}
		return ""
	}
	out := s.ask(t, s.token, "panel-setup")
	password, secret := field(out, "password: "), field(out, "authenticator secret: ")
	if len(password) != 20 || len(secret) != 32 || !strings.Contains(out, "otpauth://totp/TTT2%20Server:admin?secret="+secret+"&issuer=TTT2%20Server") {
		t.Fatalf("%q", out)
	}
	if !s.cmds.panel.Running() || !strings.Contains(out, "admin panel: running on https://127.0.0.1:") || !strings.Contains(out, "login: user admin") {
		t.Fatalf("panel-setup starts a panel that waited for its login:\n%s", out)
	}
	loginPath := filepath.Join(s.dir, "admin_panel.json")
	first, err := os.ReadFile(loginPath)
	if err != nil {
		t.Fatal(err)
	}
	if runtime.GOOS != "windows" {
		if st, _ := os.Stat(loginPath); st.Mode().Perm() != 0o600 {
			t.Fatalf("mode %v", st.Mode().Perm())
		}
	}
	if got := s.ask(t, s.token, "panel-setup"); !strings.Contains(got, "a login exists already") {
		t.Fatalf("%q", got)
	}
	if again, _ := os.ReadFile(loginPath); string(again) != string(first) {
		t.Fatal("panel-setup without force replaced the login")
	}
	if got := s.ask(t, s.token, "panel-setup now"); got != "panel-setup [force] | panel-password\n" {
		t.Fatalf("%q", got)
	}
	out = s.ask(t, s.token, "panel-setup force")
	password2, secret2 := field(out, "password: "), field(out, "authenticator secret: ")
	if len(password2) != 20 || password2 == password || len(secret2) != 32 || secret2 == secret {
		t.Fatalf("%q", out)
	}
	out = s.ask(t, s.token, "panel-password")
	password3 := field(out, "password: ")
	if len(password3) != 20 || password3 == password2 || strings.Contains(out, "secret") {
		t.Fatalf("%q", out)
	}
	if b, _ := os.ReadFile(loginPath); !strings.Contains(string(b), `"totp_secret": "`+secret2+`"`) {
		t.Fatal("panel-password must keep the authenticator secret")
	}
	if got := s.ask(t, s.token, "panel-status"); !strings.Contains(got, "admin panel: running") || !strings.Contains(got, "certificate SHA-256 fingerprint ") ||
		!strings.Contains(got, "sessions: 0 (at most 20)") {
		t.Fatalf("%q", got)
	}
	if got := s.ask(t, s.token, "panel-logout-all"); got != "panel: 0 sessions ended (admin command)\n" {
		t.Fatalf("%q", got)
	}
	// typed in the console (its output is the log): refused, nothing changes
	var console bytes.Buffer
	s.cmds.console = &console
	s.cmds.run("panel-setup force", &console)
	s.cmds.run("panel-password", &console)
	if !strings.Contains(console.String(), "run it from another terminal, as ttt2admin panel-setup") || strings.Contains(console.String(), "  password:") {
		t.Fatalf("%q", console.String())
	}
	if b, _ := os.ReadFile(loginPath); !strings.Contains(string(b), secret2) {
		t.Fatal("the console's panel-setup ran")
	}
	// never in the log or the audit file
	audit, _ := os.ReadFile(filepath.Join(s.dir, "admin_audit.log"))
	for _, text := range []string{password, password2, password3, secret, secret2, "otpauth"} {
		if strings.Contains(s.log.String(), text) || strings.Contains(string(audit), text) {
			t.Fatalf("%q in the log:\n%s\naudit:\n%s", text, s.log, audit)
		}
	}
	for _, line := range []string{"panel: new login made (admin command); every session ended", "panel: new password made (admin command); every session ended",
		"panel: 0 sessions ended (admin command)"} {
		if !strings.Contains(s.log.String(), line) || !strings.Contains(string(audit), line) {
			t.Fatalf("%q missing:\n%s\naudit:\n%s", line, s.log, audit)
		}
	}
}

// login connects to the hub's game port as the game does and returns the connection after WELCOME.
func login(t *testing.T, s *adminServer, peer uint64, name string) *tls.Conn {
	t.Helper()
	c, err := tls.Dial("tcp", s.game, &tls.Config{InsecureSkipVerify: true, MinVersion: tls.VersionTLS12})
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
