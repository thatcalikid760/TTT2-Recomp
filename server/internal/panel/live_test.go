package panel

import (
	"bufio"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	"ttt2-server/internal/hub"
	"ttt2-server/internal/store"
	"ttt2-server/internal/wire"
)

func TestMain(m *testing.M) {
	// the live view, fast: a snapshot every 20 ms, a heartbeat every 200 ms, the producer gone 100 ms after its last stream
	liveTick, liveHeartbeat, liveLinger = 20*time.Millisecond, 200*time.Millisecond, 100*time.Millisecond
	os.Exit(m.Run())
}

// fakeSource is a hub with one player online (named `name`) in a match with an offline player, and a ban (its reason
// `reason`): what the hub itself cannot hold (it cleans online names to letters and digits).
func fakeSource(name, reason string) func() (hub.PanelSnapshot, []hub.BanInfo) {
	start := time.Now().Add(-time.Hour)
	return func() (hub.PanelSnapshot, []hub.BanInfo) {
		return hub.PanelSnapshot{
			Started: start,
			Stats:   hub.Stats{Online: 1, InMatches: 1, Sessions: 1, InRooms: 2, Uptime: time.Hour},
			Players: []hub.PanelPlayer{{Peer: 76561198000000050, Name: name, IP: "203.0.113.50", Since: start, Rooms: 1, RoomFilled: 2,
				RoomMax: 2, Match: 7}},
			Matches: []hub.PanelMatch{{Session: 7, Started: start, Players: []uint64{76561198000000050, 76561198000000051}}},
		}, []hub.BanInfo{{SteamID: 76561198000000052, Ban: hub.Ban{Until: start.Add(48 * time.Hour), Reason: reason}},
			{SteamID: 76561198000000053, Ban: hub.Ban{Reason: "kicked: " + reason, Until: start.Add(2 * time.Hour)}}}
	}
}

// ---- reading a live stream as a browser does

type sseEvent struct{ ID, Event, Data string }

type sseConn struct {
	t      testing.TB
	resp   *http.Response
	events chan sseEvent
	mu     sync.Mutex
	raw    strings.Builder // everything read
}

// server serves the fixture's handler over plain HTTP on loopback (the stream's logic; TestStreamOverTLS has TLS).
func (f *fixture) server() string {
	srv := httptest.NewServer(f.handler)
	f.t.Cleanup(func() {
		f.p.live.endAll("the test is over", true)
		srv.CloseClientConnections()
		srv.Close()
	})
	return srv.URL
}

// stream opens GET /events with a session cookie ("" none).
func (f *fixture) stream(base, cookie string, mods ...func(*http.Request)) (*http.Response, *sseConn) {
	f.t.Helper()
	req, _ := http.NewRequest(http.MethodGet, base+"/events", nil)
	if cookie != "" {
		req.AddCookie(&http.Cookie{Name: sessionCookie, Value: cookie})
	}
	for _, m := range mods {
		m(req)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		f.t.Fatal(err)
	}
	f.t.Cleanup(func() { resp.Body.Close() })
	if resp.StatusCode != http.StatusOK {
		io.Copy(io.Discard, resp.Body)
		return resp, nil
	}
	s := &sseConn{t: f.t, resp: resp, events: make(chan sseEvent, 1024)}
	go s.read()
	return resp, s
}

func (s *sseConn) read() {
	defer close(s.events)
	sc := bufio.NewScanner(s.resp.Body)
	sc.Buffer(make([]byte, 64<<10), 4<<20)
	var ev sseEvent
	for sc.Scan() {
		line := sc.Text()
		s.mu.Lock()
		s.raw.WriteString(line + "\n")
		s.mu.Unlock()
		switch {
		case line == "":
			if ev.Event != "" || ev.Data != "" {
				s.events <- ev
			}
			ev = sseEvent{}
		case strings.HasPrefix(line, ":"):
			s.events <- sseEvent{Event: "comment", Data: line}
		case strings.HasPrefix(line, "id: "):
			ev.ID = line[4:]
		case strings.HasPrefix(line, "event: "):
			ev.Event = line[7:]
		case strings.HasPrefix(line, "data: "):
			ev.Data = line[6:]
		}
	}
}

func (s *sseConn) text() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.raw.String()
}

// wait reads events until one of that kind satisfies ok (nil: any), failing on a timeout or the stream's end.
func (s *sseConn) wait(kind string, ok func(sseEvent) bool) sseEvent {
	s.t.Helper()
	timeout := time.After(5 * time.Second)
	for {
		select {
		case ev, open := <-s.events:
			if !open {
				s.t.Fatalf("the stream ended while waiting for %s:\n%s", kind, s.text())
			}
			if ev.Event == kind && (ok == nil || ok(ev)) {
				return ev
			}
		case <-timeout:
			s.t.Fatalf("no %s in 5 s:\n%s", kind, s.text())
		}
	}
}

// state waits for a state the test wants.
func (s *sseConn) state(ok func(liveState) bool) liveState {
	s.t.Helper()
	var st liveState
	s.wait("state", func(ev sseEvent) bool {
		st = liveState{}
		if err := json.Unmarshal([]byte(ev.Data), &st); err != nil {
			s.t.Fatalf("%v: %s", err, ev.Data)
		}
		return ok(st)
	})
	return st
}

// activity waits for a feed entry of a kind whose text contains `text`.
func (s *sseConn) activity(kind, text string) feedEntry {
	s.t.Helper()
	var hit feedEntry
	s.wait("activity", func(ev sseEvent) bool {
		var list []feedEntry
		if err := json.Unmarshal([]byte(ev.Data), &list); err != nil {
			s.t.Fatalf("%v: %s", err, ev.Data)
		}
		for _, e := range list {
			whole := e.Text
			for _, w := range e.Who {
				whole += " " + w.Name
			}
			if e.Kind == kind && strings.Contains(whole, text) {
				hit = e
				return true
			}
		}
		return false
	})
	return hit
}

// closed reads the stream to its end and returns the kinds of event it had left.
func (s *sseConn) closed() []string {
	s.t.Helper()
	var kinds []string
	timeout := time.After(5 * time.Second)
	for {
		select {
		case ev, open := <-s.events:
			if !open {
				return kinds
			}
			kinds = append(kinds, ev.Event)
		case <-timeout:
			s.t.Fatalf("the stream did not end:\n%s", s.text())
		}
	}
}

// ---- the tests

// GET /events needs a session (401 without one, never a redirect), comes from the panel's own pages, is a stream
// that cannot be cached, and never keeps its session alive: an hour without a page view ends it.
func TestEventsNeedSession(t *testing.T) {
	f := newFixture(t)
	base := f.server()
	for name, cookie := range map[string]string{"no cookie": "", "a made-up cookie": strings.Repeat("ab", 32)} {
		resp, s := f.stream(base, cookie)
		if resp.StatusCode != http.StatusUnauthorized || s != nil || resp.Header.Get("Location") != "" ||
			resp.Header.Get("Content-Security-Policy") != csp {
			t.Fatalf("%s: %d %v", name, resp.StatusCode, resp.Header)
		}
	}
	f.login()
	if resp, _ := f.stream(base, f.cookie, header("Sec-Fetch-Site", "cross-site")); resp.StatusCode != http.StatusForbidden {
		t.Fatalf("from another site: %d", resp.StatusCode)
	}
	resp, s := f.stream(base, f.cookie)
	if resp.StatusCode != http.StatusOK || resp.Header.Get("Content-Type") != "text/event-stream; charset=utf-8" ||
		resp.Header.Get("Cache-Control") != "no-store" || resp.Header.Get("Content-Security-Policy") != csp ||
		resp.Header.Get("X-Content-Type-Options") != "nosniff" {
		t.Fatalf("%d %v", resp.StatusCode, resp.Header)
	}
	s.state(func(st liveState) bool { return st.Stats.Online == 0 && st.Players != nil })
	s.wait("clock", nil)
	s.wait("comment", func(ev sseEvent) bool { return ev.Data == ": ping" })
	// 59 minutes without a page view: alive; the stream did not count as a view, so a minute later it ends
	f.advance(59 * time.Minute)
	s.wait("comment", nil)
	f.advance(2 * time.Minute)
	if ev := s.wait("end", nil); !strings.Contains(ev.Data, "the session ended") {
		t.Fatal(ev.Data)
	}
	s.closed()
	if f.request(http.MethodGet, "/", nil).Code != http.StatusSeeOther {
		t.Fatal("the session lives on")
	}
}

// At most 4 streams per session and 32 on the whole panel; a closed one makes room.
func TestEventsLimits(t *testing.T) {
	f := newFixture(t)
	base := f.server()
	var sessions []string
	var first []*http.Response
	for k := 0; k < maxStreams/maxStreamsSession; k++ {
		f.login()
		sessions = append(sessions, f.cookie)
		for i := 0; i < maxStreamsSession; i++ {
			resp, s := f.stream(base, f.cookie)
			if resp.StatusCode != http.StatusOK {
				t.Fatalf("session %d stream %d: %d", k, i, resp.StatusCode)
			}
			s.wait("state", nil)
			if k == 0 {
				first = append(first, resp)
			}
		}
		if k == 0 {
			if resp, _ := f.stream(base, f.cookie); resp.StatusCode != http.StatusTooManyRequests {
				t.Fatalf("a 5th stream of a session: %d", resp.StatusCode)
			}
		}
	}
	if n := f.p.live.openCount(); n != maxStreams {
		t.Fatalf("%d streams", n)
	}
	f.login()
	if resp, _ := f.stream(base, f.cookie); resp.StatusCode != http.StatusTooManyRequests {
		t.Fatalf("a 33rd stream: %d", resp.StatusCode)
	}
	if !strings.Contains(f.p.Status(), "live pages open: 32") {
		t.Fatal(f.p.Status())
	}
	first[0].Body.Close()
	for deadline := time.Now().Add(3 * time.Second); f.p.live.openCount() != maxStreams-1; {
		if time.Now().After(deadline) {
			t.Fatalf("%d streams after one closed", f.p.live.openCount())
		}
		time.Sleep(10 * time.Millisecond)
	}
	if resp, _ := f.stream(base, f.cookie); resp.StatusCode != http.StatusOK {
		t.Fatalf("after one closed: %d", resp.StatusCode)
	}
}

// A stream ends with its session: logout (that session's streams only), panel-logout-all, a new login; the panel
// stopping ends them without "end" (the browser tries again: its session is still there when the panel is back).
func TestEventsEnd(t *testing.T) {
	f := newFixture(t)
	base := f.server()
	f.login()
	a := f.cookie
	csrfA := f.csrf()
	_, s1 := f.stream(base, a)
	_, s2 := f.stream(base, a)
	f.login()
	_, s3 := f.stream(base, f.cookie)
	for _, s := range []*sseConn{s1, s2, s3} {
		s.wait("state", nil)
	}
	f.cookie = a
	if w := f.request(http.MethodPost, "/logout", url.Values{"csrf": {csrfA}}); w.Code != http.StatusSeeOther {
		t.Fatal(w.Code)
	}
	for _, s := range []*sseConn{s1, s2} {
		if ev := s.wait("end", nil); !strings.Contains(ev.Data, "logged out") {
			t.Fatal(ev.Data)
		}
		s.closed()
	}
	s3.wait("comment", nil) // the other session's stream goes on
	f.p.LogoutAll()
	if ev := s3.wait("end", nil); !strings.Contains(ev.Data, "every session was ended") {
		t.Fatal(ev.Data)
	}
	s3.closed()

	f.login()
	_, s4 := f.stream(base, f.cookie)
	s4.wait("state", nil)
	password, secret, err := f.p.Setup(true)
	if err != nil {
		t.Fatal(err)
	}
	s4.wait("end", nil)
	s4.closed()

	f.password, f.secret = password, secret
	f.login()
	_, s5 := f.stream(base, f.cookie)
	s5.wait("state", nil)
	f.p.Close()
	for _, k := range s5.closed() {
		if k == "end" {
			t.Fatal("the panel stopping told the page to stop")
		}
	}
}

// The hub's changes reach the stream: players joining and leaving, matches, kicks, bans, unbans - as state and as
// activity.
func TestEventsLive(t *testing.T) {
	f := newFixture(t)
	base := f.server()
	f.login()
	_, s := f.stream(base, f.cookie)
	s.state(func(st liveState) bool { return st.Stats.Online == 0 })

	jin := gameLogin(t, f.game, 0x0110000100000A0A, "Jin Kazama") // 76561197960268298
	if e := s.activity(evJoin, "Jin Kazama"); e.Who[0].ID != "76561197960268298" {
		t.Fatalf("%+v", e)
	}
	s.state(func(st liveState) bool {
		return st.Stats.Online == 1 && len(st.Players) == 1 && st.Players[0].State == stateOnline && st.Players[0].IP == "127.0.0.1"
	})
	gameLogin(t, f.game, 0x0110000100000B0B, "Kazuya") // 76561197960268555
	s.activity(evJoin, "Kazuya")
	var w wire.Writer
	w.U64(0x1234)
	w.U8(1) // started
	w.U8(1) // one player at this PC
	w.U8(1)
	w.U64(76561197960268555)
	wire.WriteFrame(jin, wire.MatchEvent, w.B)
	s.activity(evMatch, "Kazuya")
	st := s.state(func(st liveState) bool { return st.Stats.Playing == 2 })
	for _, p := range st.Players {
		if p.State != stateMatch || len(p.Vs) != 1 || p.Vs[0].Name == p.Name {
			t.Fatalf("%+v", st.Players)
		}
	}
	// the dashboard, drawn now, shows the same
	if page := f.request(http.MethodGet, "/", nil).Body.String(); !strings.Contains(page, "In a match") || !strings.Contains(page, "Jin Kazama") {
		t.Fatalf("dashboard:\n%s", page)
	}

	csrf := f.csrf()
	f.request(http.MethodPost, "/kick", url.Values{"csrf": {csrf}, "steam_id": {"76561197960268298"}, "minutes": {"custom"},
		"minutes_custom": {"30"}, "reason": {"griefing"}})
	if e := s.activity(evKick, "Jin Kazama"); !strings.Contains(e.Text, "for 30 minutes - griefing") {
		t.Fatalf("%+v", e)
	}
	s.activity(evLeave, "Jin Kazama")
	s.state(func(st liveState) bool {
		return st.Stats.Online == 1 && len(st.Bans) == 1 && st.Bans[0].Kick && st.Bans[0].Reason == "griefing"
	})
	f.request(http.MethodPost, "/unban", url.Values{"csrf": {csrf}, "steam_id": {"76561197960268298"}})
	s.activity(evUnban, "kick lifted")
	f.request(http.MethodPost, "/ban", url.Values{"csrf": {csrf}, "steam_id": {"76561198000000077"}, "days": {"7"}, "reason": {"cheating"}})
	if e := s.activity(evBan, "banned until"); !strings.HasSuffix(e.Text, " - cheating") {
		t.Fatalf("%+v", e)
	}
	f.request(http.MethodPost, "/ban", url.Values{"csrf": {csrf}, "steam_id": {"76561197960268555"}, "days": {"0"}})
	s.activity(evBan, "banned for good")
	s.activity(evLeave, "Kazuya")
	s.state(func(st liveState) bool { return st.Stats.Online == 0 && st.Stats.Bans == 2 })
	// a kick that keeps nobody out leaves no ban: the panel tells the feed itself
	gameLogin(t, f.game, 0x0110000100000C0C, "Lili")
	s.activity(evJoin, "Lili")
	f.request(http.MethodPost, "/kick", url.Values{"csrf": {csrf}, "steam_id": {"76561197960268812"}, "minutes": {"0"}})
	s.activity(evKick, "can come back at once")
	s.activity(evLeave, "Lili")

	// a page opened now shows what happened, newest first
	page := f.request(http.MethodGet, "/", nil).Body.String()
	if i, j := strings.Index(page, "can come back at once"), strings.Index(page, "griefing"); i < 0 || j < 0 || i > j {
		t.Fatalf("the feed on the dashboard:\n%s", page)
	}
	// and a stream opened with the page's cursor gets only what came after it
	cursor := f.p.live.lastID()
	f.p.live.note(evInfo, nil, "after the page")
	_, s2 := f.stream(base, f.cookie, func(r *http.Request) { r.URL.RawQuery = "since=" + cursor })
	s2.activity(evInfo, "after the page")
	if strings.Contains(s2.text(), "griefing") {
		t.Fatal("the stream sent what the page had")
	}
}

// Players' names and reasons reach the stream as JSON with < > & escaped, never as raw HTML or as an SSE line of
// their own.
func TestEventsEscaping(t *testing.T) {
	f := newFixture(t)
	img := `<img src=x onerror=alert(1)>`
	f.p.live.source = fakeSource(img, img+"\n\nevent: end\ndata: {}")
	base := f.server()
	f.login()
	_, s := f.stream(base, f.cookie)
	st := s.state(func(st liveState) bool { return len(st.Players) == 1 })
	if st.Players[0].Name != img || st.Bans[0].Reason != img+"\n\nevent: end\ndata: {}" {
		t.Fatalf("%+v", st)
	}
	f.p.live.note(evKick, []liveWho{{"76561198000000050", img}}, "kicked - "+img)
	s.activity(evKick, img)
	raw := s.text()
	if strings.Contains(raw, "<img") || strings.Contains(raw, "\nevent: end") || !strings.Contains(raw, `\u003cimg src=x onerror=alert(1)\u003e`) {
		t.Fatalf("%s", raw)
	}
}

// The server's WriteTimeout and ReadTimeout stay for requests, and do not end a stream: over real TLS, with both at
// a second, a stream lives on.
func TestStreamOverTLS(t *testing.T) {
	oldRead, oldWrite := serverReadTimeout, serverWriteTimeout
	serverReadTimeout, serverWriteTimeout = time.Second, time.Second
	t.Cleanup(func() { serverReadTimeout, serverWriteTimeout = oldRead, oldWrite })
	f := newFixture(t)
	if err := f.p.Apply(panelConfig(0)); err != nil {
		t.Fatal(err)
	}
	b := newBrowser(t, f.p.Addr(), &tls.Config{InsecureSkipVerify: true})
	if resp, _ := b.do(http.MethodPost, "/login", url.Values{"password": {f.password}, "code": {f.code()}}); resp.StatusCode != http.StatusOK {
		t.Fatal(resp.StatusCode)
	}
	req, _ := http.NewRequest(http.MethodGet, b.base+"/events", nil)
	b.client.Timeout = 0
	resp, err := b.client.Do(req)
	if err != nil || resp.StatusCode != http.StatusOK {
		t.Fatalf("%v", err)
	}
	t.Cleanup(func() { resp.Body.Close() })
	s := &sseConn{t: t, resp: resp, events: make(chan sseEvent, 1024)}
	go s.read()
	s.wait("state", nil)
	start := time.Now()
	for time.Since(start) < 3*time.Second {
		s.wait("comment", nil)
	}
	// a page still answers within the timeouts
	if resp, _ := b.do(http.MethodGet, "/bans", nil); resp.StatusCode != http.StatusOK {
		t.Fatal(resp.StatusCode)
	}
	f.p.Close()
	s.closed()
}

// A browser that stops reading never holds the producer up: its queue fills, its stream ends (not for good: the
// browser reconnects), the others go on.
func TestSlowStream(t *testing.T) {
	f := newFixture(t)
	var mu sync.Mutex
	n := 0
	f.p.live.source = func() (hub.PanelSnapshot, []hub.BanInfo) {
		mu.Lock()
		defer mu.Unlock()
		n++ // a new state every snapshot
		return hub.PanelSnapshot{Stats: hub.Stats{Online: n}}, nil
	}
	slow, _, err := f.p.live.open([32]byte{1}, "")
	if err != nil {
		t.Fatal(err)
	}
	fast, _, _ := f.p.live.open([32]byte{2}, "")
	got := 0
	timeout := time.After(5 * time.Second)
	for {
		select {
		case <-fast.ch:
			got++
			continue
		case <-slow.done:
		case <-timeout:
			t.Fatal("the slow stream was not ended")
		}
		break
	}
	if slow.final || slow.why != "too slow" {
		t.Fatalf("%q %v", slow.why, slow.final)
	}
	before := got
	for got < before+5 {
		select {
		case <-fast.ch:
			got++
		case <-time.After(2 * time.Second):
			t.Fatal("the other stream stopped getting updates")
		}
	}
	f.p.live.close(slow)
	f.p.live.close(fast)
}

// The activity ring keeps the newest feedSize events; a cursor of this process gets what came after it, one of
// another process (a restarted server) everything kept.
func TestFeedRing(t *testing.T) {
	f := newFixture(t)
	l := f.p.live
	for i := 0; i < feedSize+50; i++ {
		l.note(evInfo, nil, fmt.Sprint(i))
	}
	got := l.recent(1000)
	if len(got) != feedSize || got[0].Text != fmt.Sprint(feedSize+49) || got[feedSize-1].Text != "50" {
		t.Fatalf("%d %+v", len(got), got[0])
	}
	if n, ok := l.cursor(l.epoch + "-140"); !ok || n != 140 {
		t.Fatal(n, ok)
	}
	if n, ok := l.cursor("deadbeef-140"); !ok || n != 0 {
		t.Fatal(n, ok)
	}
	if _, ok := l.cursor(""); ok {
		t.Fatal("no cursor: no backlog")
	}
	_, first, _ := l.open([32]byte{9}, l.epoch+"-145") // seq 146.. are the texts 145..
	if !strings.Contains(string(first), `"text":"149"`) || !strings.Contains(string(first), `"text":"145"`) ||
		strings.Contains(string(first), `"text":"144"`) {
		t.Fatalf("%s", first)
	}
}

// Where an action's form comes back to: the panel's own pages with their filters, never another site or path.
func TestBackTo(t *testing.T) {
	f := newFixture(t)
	f.login()
	csrf := f.csrf()
	for back, want := range map[string]string{
		"":                                    "/",
		"online":                              "/",
		"bans":                                "/bans",
		"/":                                   "/",
		"/players?q=jin&show=online&page=3":   "/players?page=3&q=jin&show=online",
		"/player?steam=76561198000000001#ban": "/player?steam=76561198000000001",
		"/matches?kind=ranked&evil=1":         "/matches?kind=ranked",
		"/audit":                              "/audit",
		"https://evil.example/":               "/",
		"//evil.example/players":              "/",
		`/\evil.example`:                      "/",
		"javascript:alert(1)":                 "/",
		"/login":                              "/",
		"/players/../logout":                  "/",
		"http://panel.test:28201/players":     "/",
	} {
		w := f.request(http.MethodPost, "/unban", url.Values{"csrf": {csrf}, "steam_id": {"76561198000000001"}, "back": {back}})
		if w.Code != http.StatusSeeOther || w.Header().Get("Location") != want {
			t.Errorf("back %q: %d %q, want %q", back, w.Code, w.Header().Get("Location"), want)
		}
	}
	// the answer is an error's: shown as one
	if page := f.request(http.MethodGet, "/", nil).Body.String(); !strings.Contains(page, `class="flash err"`) ||
		!strings.Contains(page, "Not unbanned: 76561198000000001 is not banned.") {
		t.Fatalf("%s", page)
	}
}

// The pages: players (search, filter, pages), a player, matches (kind, pages), bans (ban a Steam ID with a custom
// length), the audit log (search), and kick / ban presets.
func TestPages(t *testing.T) {
	f := newFixture(t)
	now := time.Now()
	for i := 0; i < 120; i++ {
		f.db.PlayerSeen(uint64(76561198000001000+i), fmt.Sprintf("Fighter %03d", i), true, now.Add(time.Duration(i)*time.Minute))
	}
	for i := 0; i < 60; i++ {
		kind := store.KindPlayer
		if i%3 == 0 {
			kind = store.KindRanked
		}
		m, _ := f.db.OpenMatch(uint64(100+i), kind, now.Add(time.Duration(i)*time.Minute))
		f.db.AddMatchPlayer(m, 76561198000001000, "Old Name", 1, true)
		f.db.AddMatchPlayer(m, uint64(76561198000001001+i), fmt.Sprintf("Fighter %03d", i+1), 2, true)
		f.db.EndMatch(m, now.Add(time.Duration(i)*time.Minute+95*time.Second), "ended")
	}
	gameLogin(t, f.game, 0x0110000100000A0A, "Jin Kazama")
	f.login()
	get := func(path string, want ...string) string {
		t.Helper()
		w := f.request(http.MethodGet, path, nil)
		body := w.Body.String()
		if w.Code != http.StatusOK {
			t.Fatalf("%s: %d\n%s", path, w.Code, body)
		}
		for _, s := range want {
			if !strings.Contains(body, s) {
				t.Fatalf("%s: no %q:\n%s", path, s, body)
			}
		}
		return body
	}
	get("/players", "121 players seen", "Page 1 of 3", "Fighter 119", `href="/players?page=2"`)
	if body := get("/players?page=3", "Page 3 of 3", "Fighter 000"); strings.Contains(body, "Fighter 119") {
		t.Fatal("page 3 has page 1's rows")
	}
	get("/players?q=fighter+11", "10 players match", "Fighter 110")
	if body := get("/players?show=online", "1 player matches", "Jin Kazama"); strings.Contains(body, "Fighter") {
		t.Fatal("offline players among the online ones")
	}
	if body := get("/players?show=offline", "120 players match"); strings.Contains(body, "Jin Kazama") {
		t.Fatal("an online player among the offline ones")
	}
	get("/players?show=banned", "No player matches.")
	get("/players?page=99", "Back to the first page")

	get("/player?steam=76561198000001000", "Fighter 000", "Old Name", "60</td>", "https://steamcommunity.com/profiles/76561198000001000",
		`rel="noopener noreferrer"`, `target="_blank"`, "Recent matches", `href="/matches?q=76561198000001000"`, `id="ban"`)
	get("/player?steam=76561197960268298", "Jin Kazama", "Online for", "127.0.0.1", `id="kick"`, `name="minutes" value="10" checked`)
	get("/player?steam=76561198000009999", "has not seen this Steam ID")
	if w := f.request(http.MethodGet, "/player?steam=jin", nil); w.Code != http.StatusBadRequest || !strings.Contains(w.Body.String(), "not a Steam ID") {
		t.Fatalf("%d", w.Code)
	}

	get("/matches", "Newest matches", "Fighter 060", "1m", "Ranked", "x2", "Page 1", `href="/matches?page=2"`)
	if body := get("/matches?kind=ranked", "Ranked"); strings.Contains(body, `<span class="tag">Player match</span>`) {
		t.Fatal("a player match among the ranked ones")
	}
	get("/matches?q=Old+Name&page=2", "Newer")

	csrf := f.csrf()
	w := f.request(http.MethodPost, "/ban", url.Values{"csrf": {csrf}, "steam_id": {" 76561198000001005 "}, "days": {"custom"},
		"days_custom": {"3"}, "reason": {"smurfing"}, "back": {"/bans"}})
	if w.Header().Get("Location") != "/bans" {
		t.Fatal(w.Header().Get("Location"))
	}
	get("/bans", "Banned Fighter 005 (76561198000001005) 3 days.", "Fighter 005", "smurfing", "data-until=", `data-confirm="Unban Fighter 005?"`)
	f.request(http.MethodPost, "/ban", url.Values{"csrf": {csrf}, "steam_id": {"76561198000001006"}, "days": {"custom"}, "days_custom": {""}})
	get("/", "Ban: days 0..36500", `class="flash err"`)
	get("/player?steam=76561198000001005", "Banned until", "Unban", "banned 76561198000001005 3 days: smurfing")
	get("/audit", "logged in", "banned 76561198000001005 3 days: smurfing", "al-ban")
	if body := get("/audit?q=SMURF", "smurfing"); strings.Contains(body, "logged in") {
		t.Fatal("the search let other lines through")
	}
	get("/players?show=banned", "1 player matches", "Fighter 005", "until ")
	get("/", "Online players", "Jin Kazama", `data-act="kick"`, `data-act="ban"`, "Activity", `data-stat="matches24h">60<`)
}

// The audit log is read from its end only, whole lines, newest first.
func TestAuditTail(t *testing.T) {
	f := newFixture(t)
	var b strings.Builder
	for b.Len() < auditTailBytes+100<<10 {
		fmt.Fprintf(&b, "2026-10-23 12:00:00 panel: line %d\n", b.Len())
	}
	b.WriteString("no time on this line\n2026-10-23 12:00:01 panel: admin from 192.0.2.1 banned 76561198000000001 for good\n")
	os.WriteFile(f.p.dataDir+"/"+auditFile, []byte(b.String()), 0o600)
	lines, cut, err := f.p.auditTail("", auditMaxLines)
	if err != nil || !cut || len(lines) != auditMaxLines || lines[0].Kind != "ban" || lines[0].At.IsZero() || lines[1].Text != "no time on this line" {
		t.Fatalf("%v %v %d %+v", err, cut, len(lines), lines[:2])
	}
	for _, l := range lines {
		if !strings.HasPrefix(l.Text, "panel: ") && l.Text != "no time on this line" {
			t.Fatalf("a cut line: %q", l.Text)
		}
	}
	if got, _ := f.p.auditOf(76561198000000001, 10); len(got) != 1 {
		t.Fatalf("%+v", got)
	}
}

// ---- the cost

// BenchmarkLiveTick is one producer tick (a second's work while a page is open) with 32 streams open: a snapshot
// that did not change (the usual second), and one that did (a player joined or left, every tick).
func BenchmarkLiveTick(b *testing.B) {
	for _, players := range []int{64, 1000} {
		for _, changing := range []bool{false, true} {
			b.Run(fmt.Sprintf("players=%d/changed=%v", players, changing), func(b *testing.B) {
				full, less := benchSnapshot(players), benchSnapshot(players-1)
				var bans []hub.BanInfo
				for i := 0; i < 50; i++ {
					bans = append(bans, hub.BanInfo{SteamID: uint64(76561198100000000 + i), Ban: hub.Ban{Reason: "cheating", Until: time.Now().Add(time.Hour)}})
				}
				tickNo := 0
				l := &live{streams: map[*stream]struct{}{}, sessions: map[[32]byte]int{}, epoch: "bench"}
				l.source = func() (hub.PanelSnapshot, []hub.BanInfo) {
					tickNo++
					if changing && tickNo%2 == 0 {
						return less, bans
					}
					return full, bans
				}
				l.names = func(ids []uint64) (map[uint64]string, error) { return map[uint64]string{}, nil }
				l.count = func(time.Time) (int, error) { return 42, nil }
				var wg sync.WaitGroup
				for i := 0; i < maxStreams; i++ {
					s := &stream{ch: make(chan []byte, streamQueue), done: make(chan struct{})}
					l.streams[s] = struct{}{}
					wg.Add(1)
					go func() {
						defer wg.Done()
						for {
							select {
							case <-s.ch:
							case <-s.done:
								return
							}
						}
					}()
				}
				pr := &producer{names: map[uint64]string{}}
				l.tick(pr, time.Now())
				b.ReportAllocs()
				b.ResetTimer()
				for b.Loop() {
					l.tick(pr, time.Now())
				}
				b.StopTimer()
				l.endAll("done", true)
				wg.Wait()
			})
		}
	}
}

func benchSnapshot(n int) hub.PanelSnapshot {
	s := hub.PanelSnapshot{Started: time.Now().Add(-time.Hour), Stats: hub.Stats{Online: n}}
	since := time.Now().Add(-10 * time.Minute)
	for i := 0; i < n; i++ {
		p := hub.PanelPlayer{Peer: uint64(76561198000000000 + i), Name: fmt.Sprintf("Player %d", i), Verified: true,
			IP: fmt.Sprintf("198.51.%d.%d", i/250, i%250), Since: since}
		if i%4 == 0 {
			p.Match = uint64(1000 + i/8)
		}
		if i%8 == 0 {
			p.Rooms, p.RoomFilled, p.RoomMax = 1, 2, 2
		}
		s.Players = append(s.Players, p)
	}
	for m := 0; m < n/8; m++ {
		s.Matches = append(s.Matches, hub.PanelMatch{Session: uint64(1000 + m), Started: since,
			Players: []uint64{uint64(76561198000000000 + 8*m), uint64(76561198000000000 + 8*m + 4)}})
	}
	return s
}

// BenchmarkHubSnapshot is what the producer asks of the hub each tick, on a hub with 60 players logged in over TLS:
// one lock, a copy.
func BenchmarkHubSnapshot(b *testing.B) {
	f := newFixture(b)
	cfg := f.h.Config()
	cfg.MaxConnsPerIP, cfg.MaxPlayers = 100, 100
	f.h.SetConfig(cfg)
	for i := 0; i < 60; i++ {
		gameLogin(b, f.game, uint64(0x0110000100100000+i), fmt.Sprintf("Bench %d", i))
	}
	if n := f.h.Stats().Online; n != 60 {
		b.Fatalf("%d online", n)
	}
	b.ReportAllocs()
	b.ResetTimer()
	for b.Loop() {
		f.h.PanelSnapshot()
		f.h.Bans()
	}
}
