package gin

import (
	"bufio"
	"bytes"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"strconv"
	"strings"
	"testing"
	"time"
	"ttt2-server/internal/store"
)

type fakeStore struct {
	xuids    map[uint64]uint64
	profiles map[uint64][]byte
	uploads  map[string]int // kind -> count
	keep     map[string]int
	owners   map[uint64]uint64 // XUID -> Steam ID
	teams    map[uint64]store.Team
}

func (f *fakeStore) SteamIDsOf(xuids []uint64) (map[uint64]uint64, error) {
	out := map[uint64]uint64{}
	for _, x := range xuids {
		if id, ok := f.owners[x]; ok {
			out[x] = id
		}
	}
	return out, nil
}

func (f *fakeStore) TeamOf(steamID uint64) (store.Team, bool, error) {
	t, ok := f.teams[steamID]
	return t, ok, nil
}

func (f *fakeStore) SaveUpload(_ uint64, kind string, _ []byte, keep int, _ time.Time) error {
	f.uploads[kind]++
	f.keep[kind] = keep
	return nil
}

// ghosts and replays: media_test.go tests them on a real store
func (f *fakeStore) SaveGhost(uint64, int, string, []byte, time.Time) error   { return nil }
func (f *fakeStore) Ghosts(int, string, uint64, int) ([]store.Ghost, error)   { return nil, nil }
func (f *fakeStore) GhostOf(uint64, int, string) ([]byte, bool, error)        { return nil, false, nil }
func (f *fakeStore) SaveReplay(uint64, store.Replay, time.Time) (bool, error) { return true, nil }
func (f *fakeStore) Replays(store.ReplayQuery) ([]store.Replay, error)        { return nil, nil }

func (f *fakeStore) SetXUID(steamID, xuid uint64) error { f.xuids[steamID] = xuid; return nil }
func (f *fakeStore) SaveProfile(steamID uint64, body []byte, _ time.Time) error {
	f.profiles[steamID] = append([]byte(nil), body...)
	return nil
}

func service(settings Settings) (*Service, *fakeStore) {
	st := &fakeStore{xuids: map[uint64]uint64{}, profiles: map[uint64][]byte{}, uploads: map[string]int{}, keep: map[string]int{},
		owners: map[uint64]uint64{}, teams: map[uint64]store.Team{}}
	return New(st, func() Settings { return settings }, log.New(io.Discard, "", 0)), st
}

var me = Caller{SteamID: 76561198000000001, Name: "ME", Verified: true}

// request as the game sends it (GINware/2.2 (xbox), sometimes JSON under a form content type)
func request(method, path, body string) []byte {
	var b strings.Builder
	b.WriteString(method + " /2.2/ttt2/xbox/" + path + " HTTP/1.1\r\nUser-Agent: GINware/2.2 (xbox)\r\nHost: ttt2-tu2.api.ginware.com:80\r\n")
	if body != "" {
		b.WriteString("Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " + strconv.Itoa(len(body)) + " \r\n")
	}
	b.WriteString("Cookie: sessionid=abc;\r\n\r\n" + body)
	return []byte(b.String())
}

// parse reads a response as the game does: status, the length given, then the body (JSON).
func parse(t *testing.T, raw []byte) (int, map[string]any) {
	t.Helper()
	resp, err := http.ReadResponse(bufio.NewReader(bytes.NewReader(raw)), nil)
	if err != nil {
		t.Fatal(err)
	}
	body, _ := io.ReadAll(resp.Body)
	if !bytes.Contains(raw, []byte("Connection: close")) {
		t.Fatal("the game reads until the connection closes: say so")
	}
	var v map[string]any
	if len(body) > 0 {
		if err := json.Unmarshal(body, &v); err != nil {
			t.Fatalf("body %q: %v", body, err)
		}
	}
	return resp.StatusCode, v
}

func TestConnect(t *testing.T) {
	s, st := service(Settings{})
	code, v := parse(t, s.Serve(me, request("POST", "connect?expire=2", "userid=2798379101111979\r\n")))
	if code != 200 || v["message"] != "OK" {
		t.Fatalf("%d %v", code, v)
	}
	id, _ := v["session_id"].(string)
	if len(id) == 0 || len(id) > 63 {
		t.Fatalf("session id %q: the game keeps at most 63 characters", id)
	}
	if st.xuids[me.SteamID] != 2798379101111979 {
		t.Fatalf("xuid %d", st.xuids[me.SteamID])
	}
}

func TestSettingsCarryNewsAndMOTD(t *testing.T) {
	long := strings.Repeat("word ", 40) // 200 characters: two lines
	s, _ := service(Settings{MOTD: "Welcome!\n" + long + "\nRanked night ~red~Friday 100% <b>", News: []string{"News one", `quote " and \ gone`}})
	code, v := parse(t, s.Serve(me, request("GET", "settings", "")))
	if code != 200 || v["message"] != "OK" {
		t.Fatalf("%d %v", code, v)
	}
	data := v["data"].(map[string]any)
	motd := data["messages-motd-en"].([]any)
	if len(motd) != 4 || motd[0] != "Welcome!" || motd[3] != "Ranked night redFriday 100 b" {
		t.Fatalf("motd %q", motd)
	}
	for _, l := range motd {
		if len(l.(string)) > 119 {
			t.Fatalf("a line longer than the game's 119 characters: %q", l)
		}
	}
	news := data["messages-news-jp"].([]any)
	if len(news) != 2 || news[1] != "quote  and  gone" {
		t.Fatalf("news %q", news)
	}
	for _, k := range []string{"maintenance-mode", "ghost-download-count", "throttle-uploads-per-hour-max", "messages-misc-ru"} {
		if _, ok := data[k]; !ok {
			t.Fatalf("settings without %s", k)
		}
	}
}

func TestPlayerProfileKept(t *testing.T) {
	s, st := service(Settings{})
	body := `{"sessionEventCount": 1, "userId": "1", "payload":{"player":"1",` + "\n" + `"battle-points":"0"}}`
	code, v := parse(t, s.Serve(me, request("POST", "player", body)))
	if code != 200 || v["message"] != "Created" || len(v) != 1 || string(st.profiles[me.SteamID]) != body {
		t.Fatalf("%d %v %q", code, v, st.profiles[me.SteamID])
	}
	if code, _ := parse(t, s.Serve(me, request("POST", "player", `{"broken`))); code != 400 {
		t.Fatalf("broken JSON: %d", code)
	}
	if code, _ := parse(t, s.Serve(me, request("POST", "player", `{"x":"`+strings.Repeat("a", MaxProfile)+`"}`))); code != 400 {
		t.Fatalf("oversized profile: %d", code)
	}
}

func TestOtherRequests(t *testing.T) {
	s, _ := service(Settings{})
	for _, path := range []string{"team?count=1&for-players=1", "custom/ghost?dan-belts=3", "custom/replay?page=0&count=4", "leaderboard?lbid=x"} {
		if code, v := parse(t, s.Serve(me, request("GET", path, ""))); code != 200 || v["message"] != "OK" {
			t.Fatalf("%s: %d %v", path, code, v)
		}
	}
	if code, _ := parse(t, s.Serve(me, []byte("GET /other HTTP/1.1\r\n\r\n"))); code != 404 {
		t.Fatal("outside the GIN path: 404")
	}
	if code, _ := parse(t, s.Serve(me, []byte("not http at all"))); code != 400 {
		t.Fatal("garbage: 400")
	}
	if code, _ := parse(t, s.Serve(me, make([]byte, MaxRequest+1))); code != 413 {
		t.Fatal("oversized: 413")
	}
	if code, _ := parse(t, Busy()); code != 503 {
		t.Fatal("busy: 503")
	}
}

func FuzzServe(f *testing.F) {
	f.Add(request("POST", "connect", "userid=1"))
	f.Add(request("GET", "settings", ""))
	f.Add(request("POST", "player", `{"payload":{}}`))
	s, _ := service(Settings{MOTD: "m", News: []string{"n"}})
	f.Fuzz(func(t *testing.T, raw []byte) {
		out := s.Serve(me, raw)
		if _, err := http.ReadResponse(bufio.NewReader(bytes.NewReader(out)), nil); err != nil {
			t.Fatalf("not an HTTP response: %v", err)
		}
	})
}

func TestUploadsKept(t *testing.T) {
	s, st := service(Settings{})
	for _, path := range []string{"match", "leaderboard/"} {
		if r := s.Serve(me, request("POST", path, `{"payload":{"x":"1"}}`)); !bytes.HasPrefix(r, []byte("HTTP/1.1 200")) {
			t.Fatalf("%s: %s", path, r)
		}
	}
	if st.uploads["match"] != 1 || st.uploads["leaderboard"] != 1 || st.keep["match"] != 50 || st.keep["leaderboard"] != 20 {
		t.Fatalf("%v %v", st.uploads, st.keep)
	}
	// not JSON, or too big: refused and not kept; ghosts and replays are items (media_test.go), not kept as sent
	if r := s.Serve(me, request("POST", "match", "not json")); !bytes.HasPrefix(r, []byte("HTTP/1.1 400")) {
		t.Fatalf("%s", r)
	}
	big := `{"a":"` + strings.Repeat("x", MaxUpload) + `"}`
	for _, path := range []string{"match", "custom/replay", "custom/ghost"} {
		if r := s.Serve(me, request("POST", path, big)); !bytes.HasPrefix(r, []byte("HTTP/1.1 4")) {
			t.Fatalf("%s: %.40s", path, r)
		}
	}
	if st.uploads["match"] != 1 || len(st.uploads) != 2 {
		t.Fatalf("%v", st.uploads)
	}
}

func TestTeamReply(t *testing.T) {
	s, st := service(Settings{})
	const me, mate = 2533274790398465, 2533274790398466
	st.owners[me] = 11
	team := store.Team{ID: 7, Name: "TEKKEN ARABIA", Leader: 11, Created: time.Unix(1_800_000_000, 0), Members: []store.TeamMember{
		{SteamID: 11, XUID: me, Name: "A", BestFighter: 42, BestRank: 12, Wins: 30},
		{SteamID: 12, XUID: mate, Name: "B", BestFighter: 3, BestRank: 20, Wins: 80},
		{SteamID: 13, Name: "never online"},
	}}
	st.teams[11] = team
	r := s.Serve(me2(), request("GET", "team?count=1&for-players=2533274790398465", ""))
	body := r[bytes.Index(r, []byte("\r\n\r\n"))+4:]
	var doc struct {
		Data struct {
			Teams []struct {
				Player string         `json:"player"`
				Team   map[string]any `json:"team"`
			} `json:"teams"`
			IDs []map[string]string `json:"player-team-ids"`
		} `json:"data"`
	}
	if err := json.Unmarshal(body, &doc); err != nil {
		t.Fatalf("%v: %s", err, body)
	}
	if len(doc.Data.Teams) != 1 || doc.Data.Teams[0].Player != "2533274790398465" || len(doc.Data.IDs) != 1 || doc.Data.IDs[0]["team"] != "7" {
		t.Fatalf("%s", body)
	}
	tm := doc.Data.Teams[0].Team
	members, _ := tm["members"].([]any)
	if tm["team-name"] != "TEKKEN ARABIA" || tm["leader"] != "2533274790398465" || len(members) != 2 ||
		tm["highest-rank-fighter-1-player"] != "2533274790398466" || tm["highest-rank-1"] != "20" || tm["highest-rank-fighter-1"] != "3" ||
		tm["highest-rank-2"] != "12" || tm["points"] != float64(110) || tm["team-level"] != float64(3) {
		t.Fatalf("%v", tm)
	}
	// a player in no team, or unknown: empty lists
	r = s.Serve(me2(), request("GET", "team?count=1&for-players=99", ""))
	if !bytes.Contains(r, []byte(`"teams":[]`)) {
		t.Fatalf("%s", r)
	}
}

func me2() Caller { return me }
