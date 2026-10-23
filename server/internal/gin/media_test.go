package gin

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"ttt2-server/internal/store"
)

// mediaService is a service on a real store (a database in a test directory) with two players.
func mediaService(t *testing.T) (*Service, *store.Store, Caller, Caller) {
	t.Helper()
	st, err := store.Open(filepath.Join(t.TempDir(), "media.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	a, b := Caller{SteamID: 76561198000000011, Name: "A"}, Caller{SteamID: 76561198000000012, Name: "B"}
	for _, c := range []Caller{a, b} {
		if err := st.PlayerSeen(c.SteamID, c.Name, true, time.Now()); err != nil {
			t.Fatal(err)
		}
	}
	return New(st, func() Settings { return Settings{} }, log.New(io.Discard, "", 0)), st, a, b
}

// event wraps a payload the way the game uploads it (MEASURED: every value a string).
func event(payload string) string {
	return `{"sessionEventCount": 1, "userId": "2766880500960170", "titleId": "ttt2", "platform": "xbox", "sessionId": "x", "payload":` + payload + `}`
}

func replayPayload(match, fighter string, data string) string {
	return fmt.Sprintf(`{"match-type":"ranked",
"match-beg":"1792647528",
"match-end":"1792647637",
"stage":"Eternal Paradise",
"match":"%s",
"party-1-player-1":"2766880500960170",
"party-1-player-2":"",
"party-1-fighter-1":"%s",
"party-1-fighter-1-rank":"12",
"party-1-fighter-2":"17",
"party-1-fighter-2-rank":"1",
"party-2-player-1":"2623705622042519",
"party-2-player-2":"",
"party-2-fighter-1":"42",
"party-2-fighter-1-rank":"10",
"party-2-fighter-2":"36",
"party-2-fighter-2-rank":"0",
"replay-data":"%s"}`, match, fighter, data)
}

func ghostObject(fighter string) string {
	return `{"player":"2766880500960170","fighter":"` + fighter + `","fighter-rank":"12","match-beg":"1792647528","match-end":"1792647637","ghost-data":"` +
		strings.Repeat("QUJD", 21846) + `%2B"}`
}

func TestReplayUploadAndChannel(t *testing.T) {
	s, _, a, b := mediaService(t)
	data := strings.Repeat("ZMAm", 8000) + "%2BAA=="
	for _, m := range []string{"xbox-1-a", "xbox-2-a", "xbox-1-a"} { // the last: the same match again, kept once
		code, v := parse(t, s.Serve(a, request("POST", "custom/replay", event(replayPayload(m, "7", data)))))
		if code != 200 || v["message"] != "Created" || len(v) != 1 {
			t.Fatalf("upload %s: %d %v", m, code, v)
		}
	}
	code, v := parse(t, s.Serve(b, request("GET", "custom/replay?page=0&count=4&replay-data=true", "")))
	if code != 200 || v["message"] != "OK" {
		t.Fatalf("%d %v", code, v)
	}
	list := v["data"].(map[string]any)["replays"].([]any)
	if len(list) != 2 {
		t.Fatalf("2 replays expected, got %d", len(list))
	}
	first := list[0].(map[string]any)
	if first["match"] != "xbox-2-a" || first["replay-data"] != data || first["stage"] != "Eternal Paradise" {
		t.Fatalf("newest first, with its data: %v", first["match"])
	}
	if n, ok := first["match-beg"].(float64); !ok || n != 1792647528 {
		t.Fatalf("match-beg is an integer in the game's schema: %#v", first["match-beg"])
	}
	// the listing without data, and the game's filters
	for q, want := range map[string]int{
		"page=0&count=100&replay-data=false":                               2,
		"page=0&count=100&replay-data=false&fighter=7":                     2,
		"page=0&count=100&replay-data=false&fighter=8":                     0,
		"page=0&count=100&replay-data=false&tag-fighter=17,7":              2,
		"page=0&count=100&replay-data=false&tag-fighter=7,42":              0,
		"page=0&count=100&replay-data=false&solo-fighter=7":                0,
		"page=0&count=100&replay-data=false&match-type=ranked":             2,
		"page=0&count=100&replay-data=false&match-type=championship":       0,
		"page=1&count=1&replay-data=false&fighter=7&sort-by-dan-rank=true": 1,
	} {
		code, v := parse(t, s.Serve(b, request("GET", "custom/replay?"+q, "")))
		got := v["data"].(map[string]any)["replays"].([]any)
		if code != 200 || len(got) != want {
			t.Fatalf("%s: %d replays, want %d", q, len(got), want)
		}
		if strings.Contains(q, "replay-data=false") && len(got) > 0 {
			if _, has := got[0].(map[string]any)["replay-data"]; has {
				t.Fatalf("%s: a listing without data has replay-data", q)
			}
		}
	}
	// what the game's schema does not take is refused
	for name, p := range map[string]string{
		"not base64": replayPayload("xbox-3-a", "7", "not base64!"),
		"long stage": strings.Replace(replayPayload("xbox-4-a", "7", data), "Eternal Paradise", strings.Repeat("s", 0x78), 1),
		"no match":   strings.Replace(replayPayload("xbox-5-a", "7", data), `"match":"xbox-5-a",`, "", 1),
		"markup":     strings.Replace(replayPayload("xbox-6-a", "7", data), "Eternal Paradise", "~Eternal%s", 1),
	} {
		if code, _ := parse(t, s.Serve(a, request("POST", "custom/replay", event(p)))); code != 400 {
			t.Fatalf("%s: %d", name, code)
		}
	}
}

func TestGhostUploadAndBattle(t *testing.T) {
	s, _, a, b := mediaService(t)
	up := event(`{"dan-rank-belt":"3","tag-ghost-1":` + ghostObject("7") + `,"tag-ghost-2":` + ghostObject("17") + `}`)
	if len(up) > MaxUpload {
		t.Fatalf("a tag pair (%d bytes) must fit an upload", len(up))
	}
	if code, v := parse(t, s.Serve(a, request("POST", "custom/ghost", up))); code != 200 || v["message"] != "Created" {
		t.Fatalf("%d %v", code, v)
	}
	// B fights A's ghosts on belt 3; A gets none of its own; nobody's on belt 4
	_, v := parse(t, s.Serve(b, request("GET", "custom/ghost?dan-belts=3", "")))
	set, ok := v["ghosts"].(map[string]any)["dan-belt-ghosts-3"].(map[string]any)
	if !ok || set["dan-rank-belt"] != float64(3) {
		t.Fatalf("belt 3 for B: %v", v)
	}
	g1, g2 := set["tag-ghost-1"].(map[string]any), set["tag-ghost-2"].(map[string]any)
	if g1["fighter"] != "7" || g2["fighter"] != "17" || g1["match-end"] != float64(1792647637) || g1["ghost-data"] != strings.Repeat("QUJD", 21846)+"%2B" {
		t.Fatalf("the tag pair as uploaded: %v %v", g1["fighter"], g2["fighter"])
	}
	if _, solo := set["solo-ghost"]; solo {
		t.Fatal("no solo ghost was uploaded")
	}
	for _, ask := range []struct {
		who Caller
		q   string
	}{{a, "dan-belts=3"}, {b, "dan-belts=4"}, {me, "dan-belts=99"}} {
		who, q := ask.who, ask.q
		_, v := parse(t, s.Serve(who, request("GET", "custom/ghost?"+q, "")))
		if g := v["ghosts"].(map[string]any); len(g) != 0 || v["message"] != "OK" {
			t.Fatalf("%s %s: no ghosts expected, got %v", who.Name, q, g)
		}
	}
	// what the game's schema does not take is refused
	for name, p := range map[string]string{
		"belt 11":     `{"dan-rank-belt":"11","solo-ghost":` + ghostObject("7") + `}`,
		"no ghost":    `{"dan-rank-belt":"3"}`,
		"long player": `{"dan-rank-belt":"3","solo-ghost":` + strings.Replace(ghostObject("7"), "2766880500960170", strings.Repeat("9", 32), 1) + `}`,
		"bad data":    `{"dan-rank-belt":"3","solo-ghost":` + strings.Replace(ghostObject("7"), "QUJD", "Q<JD", 1) + `}`,
		"bad escape":  `{"dan-rank-belt":"3","solo-ghost":` + strings.Replace(ghostObject("7"), "%2B", "%2G", 1) + `}`,
	} {
		if code, _ := parse(t, s.Serve(a, request("POST", "custom/ghost", event(p)))); code != 400 {
			t.Fatalf("%s: %d", name, code)
		}
	}
	var check map[string]any
	if json.Unmarshal([]byte(event(`{}`)), &check) != nil {
		t.Fatal("event")
	}
}
