package gin

import (
	"encoding/json"
	"math/rand/v2"
	"net/url"
	"strconv"
	"strings"
	"time"

	"ttt2-server/internal/store"
)

// Ghosts and replays (PROTOCOL.md "GIN: ghosts and replays"). The game uploads them after a ranked match a dan-ranked
// player won outright: the replay first (a 1st-7th dan player one a session, higher ranks more), else the winner's
// ghosts - at most once a day (MEASURED 2026-10-22, the checks in the game: port gin_uploads.cpp / gin_trace.cpp).
// An upload is the game's event envelope {"sessionEventCount", "userId", ..., "payload": {...}}; the payload holds the
// item the download reply has (the reply schemas in the game's image: replay item 0x82E17100, ghost 0x82E16B88). Every
// field is checked against those schemas - the game copies a value into a fixed-size buffer - and kept normalized:
// strings where the schema has text, numbers where it has an integer.

// GhostBelts: the dan belts of the ghost download (dan-belt-ghosts-0 .. -10).
const GhostBelts = 11

// a schema's text field: at most size-1 characters (the game's buffer keeps a NUL)
type field struct {
	key  string
	size int // 0: an integer
}

var ghostFields = []field{{"player", 0x20}, {"fighter", 8}, {"fighter-rank", 8}, {"match-beg", 0}, {"match-end", 0},
	{"ghost-data", dataSize}}

var replayFields = []field{{"match", 0x28}, {"match-type", 0x78}, {"match-beg", 0}, {"match-end", 0}, {"stage", 0x78},
	{"party-1-player-1", 0x20}, {"party-1-player-2", 0x20}, {"party-2-player-1", 0x20}, {"party-2-player-2", 0x20},
	{"party-1-fighter-1", 8}, {"party-1-fighter-2", 8}, {"party-2-fighter-1", 8}, {"party-2-fighter-2", 8},
	{"party-1-fighter-1-rank", 8}, {"party-1-fighter-2-rank", 8}, {"party-2-fighter-1-rank", 8}, {"party-2-fighter-2-rank", 8}}

// payload is an upload's "payload" object (false: not the game's envelope).
func payload(body []byte) (map[string]json.RawMessage, bool) {
	var envelope struct {
		Payload map[string]json.RawMessage `json:"payload"`
	}
	if json.Unmarshal(body, &envelope) != nil || envelope.Payload == nil {
		return nil, false
	}
	return envelope.Payload, true
}

// dataSize is ghost-data's and replay-data's buffer: those are checked as base64Text, not as text the game shows.
const dataSize = 0x16760

// value is a field the game sent (as a string, as it sends every value, or a number) - false: missing, not text,
// too long or (text the game shows) with characters its parser should not get (as clean).
func value(raw json.RawMessage, size int) (string, bool) {
	var s string
	if json.Unmarshal(raw, &s) != nil {
		var n json.Number
		if json.Unmarshal(raw, &n) != nil {
			return "", false
		}
		s = n.String()
	}
	if size > 0 && (len(s) >= size || size != dataSize && clean(s, size) != strings.TrimSpace(s)) {
		return "", false
	}
	return s, true
}

// item is an object of a schema's fields, normalized (false: a field is missing or wrong). An empty integer is 0.
func item(fields map[string]json.RawMessage, schema []field) (map[string]any, bool) {
	out := make(map[string]any, len(schema))
	for _, f := range schema {
		raw, ok := fields[f.key]
		if !ok {
			return nil, false
		}
		v, ok := value(raw, f.size)
		if !ok {
			return nil, false
		}
		if f.size == 0 {
			n := int64(0)
			if v != "" {
				var err error
				if n, err = strconv.ParseInt(v, 10, 64); err != nil {
					return nil, false
				}
			}
			out[f.key] = n
		} else {
			out[f.key] = v
		}
	}
	return out, true
}

// base64Text: the game's ghost-data / replay-data, not empty - base64 with its '+' written %2B (MEASURED: the game
// URL-encodes it; kept as sent, the game gets back what it wrote): base64 characters and %XX escapes.
func base64Text(s string) bool {
	if s == "" {
		return false
	}
	hex := func(c byte) bool { return c >= '0' && c <= '9' || c >= 'A' && c <= 'F' || c >= 'a' && c <= 'f' }
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '+' || c == '/' || c == '=':
		case c == '%' && i+2 < len(s) && hex(s[i+1]) && hex(s[i+2]):
			i += 2
		default:
			return false
		}
	}
	return true
}

// ghostUpload: POST custom/ghost - payload {"dan-rank-belt", "tag-ghost-1"/"tag-ghost-2"/"solo-ghost": ghost}.
func (s *Service) ghostUpload(c Caller, body []byte) []byte {
	p, ok := payload(body)
	if !ok || len(body) > MaxUpload {
		return response(400, nil)
	}
	beltText, ok := value(p["dan-rank-belt"], 0)
	belt, err := strconv.Atoi(beltText)
	if !ok || err != nil || belt < 0 || belt >= GhostBelts {
		s.logOnce("ghost belt "+beltText, "GIN: a ghost upload of %q with dan-rank-belt %.20q refused", c.Name, beltText)
		return response(400, nil)
	}
	ghosts := map[string][]byte{}
	for _, kind := range store.GhostKinds {
		raw, there := p[kind]
		if !there || string(raw) == "null" {
			continue
		}
		var fields map[string]json.RawMessage
		g, ok := map[string]any(nil), json.Unmarshal(raw, &fields) == nil
		if ok {
			g, ok = item(fields, ghostFields)
		}
		if !ok || !base64Text(g["ghost-data"].(string)) {
			s.logOnce("ghost bad "+kind, "GIN: a ghost upload of %q with a %s the game's schema does not take, refused", c.Name, kind)
			return response(400, nil)
		}
		ghosts[kind], _ = json.Marshal(g)
	}
	if len(ghosts) == 0 {
		s.logOnce("ghost empty", "GIN: a ghost upload of %q without a ghost, refused (payload keys %v)", c.Name, keys(p))
		return response(400, nil)
	}
	if !s.mayUpload(c.SteamID, time.Now()) {
		return response(429, nil)
	}
	for kind, g := range ghosts {
		if err := s.store.SaveGhost(c.SteamID, belt, kind, g, time.Now()); err != nil {
			s.logInternal("ghost error", "GIN: a ghost of %q not kept: %v", c.Name, err)
			return response(503, nil)
		}
	}
	s.logInternal("ghost upload", "GIN: the first ghost upload kept (belt %d, %d ghosts, from %q)", belt, len(ghosts), c.Name)
	return posted()
}

// replayUpload: POST custom/replay - payload = the replay item with replay-data.
func (s *Service) replayUpload(c Caller, body []byte) []byte {
	p, ok := payload(body)
	if !ok || len(body) > MaxUpload {
		return response(400, nil)
	}
	it, ok := item(p, replayFields)
	data, okData := value(p["replay-data"], dataSize)
	if !ok || !okData || !base64Text(data) || it["match"] == "" {
		s.logOnce("replay bad", "GIN: a replay upload of %q the game's schema does not take, refused (payload keys %v)", c.Name, keys(p))
		return response(400, nil)
	}
	r := store.Replay{Match: it["match"].(string), Data: data, Type: it["match-type"].(string), Rank: -1}
	for i, k := range []string{"party-1-fighter-1", "party-1-fighter-2", "party-2-fighter-1", "party-2-fighter-2"} {
		r.Fighters[i] = -1
		if n, err := strconv.Atoi(it[k].(string)); err == nil {
			r.Fighters[i] = n
		}
		if n, err := strconv.Atoi(it[k+"-rank"].(string)); err == nil && n > r.Rank {
			r.Rank = n
		}
	}
	r.Solo = it["party-1-fighter-2"] == "" && it["party-2-fighter-2"] == ""
	r.Item, _ = json.Marshal(it)
	if !s.mayUpload(c.SteamID, time.Now()) {
		return response(429, nil)
	}
	if _, err := s.store.SaveReplay(c.SteamID, r, time.Now()); err != nil {
		s.logInternal("replay error", "GIN: a replay of %q not kept: %v", c.Name, err)
		return response(503, nil)
	}
	s.logInternal("replay upload", "GIN: the first replay upload kept (%d bytes of replay, from %q)", len(data), c.Name)
	return posted()
}

// ghosts: GET custom/ghost?dan-belts=N - for GHOST BATTLE, other players' ghosts of that belt: a tag pair (one
// player's two) and a solo ghost, picked among the newest. Reply schema (0x82E16D0C, one per belt): {"message",
// "ghosts": {"dan-belt-ghosts-N": {"dan-rank-belt", "tag-ghost-1", "tag-ghost-2", "solo-ghost"}}}; none: an empty
// "ghosts" (the game fights its own).
func (s *Service) ghosts(c Caller, q url.Values) []byte {
	belt, err := strconv.Atoi(q.Get("dan-belts"))
	if err != nil || belt < 0 || belt >= GhostBelts {
		return reply(map[string]any{"message": "OK", "ghosts": map[string]any{}})
	}
	const pool = 32 // a ghost among the newest of a belt
	set := map[string]any{}
	if firsts, err := s.store.Ghosts(belt, "tag-ghost-1", c.SteamID, pool); err == nil {
		rand.Shuffle(len(firsts), func(i, j int) { firsts[i], firsts[j] = firsts[j], firsts[i] })
		for _, g := range firsts {
			if second, ok, err := s.store.GhostOf(g.SteamID, belt, "tag-ghost-2"); err == nil && ok {
				set["tag-ghost-1"], set["tag-ghost-2"] = json.RawMessage(g.Body), json.RawMessage(second)
				break
			}
		}
	}
	if solos, err := s.store.Ghosts(belt, "solo-ghost", c.SteamID, pool); err == nil && len(solos) > 0 {
		set["solo-ghost"] = json.RawMessage(solos[rand.IntN(len(solos))].Body)
	}
	if len(set) == 0 {
		return reply(map[string]any{"message": "OK", "ghosts": map[string]any{}})
	}
	set["dan-rank-belt"] = belt
	return reply(map[string]any{"message": "OK", "ghosts": map[string]any{"dan-belt-ghosts-" + strconv.Itoa(belt): set}})
}

// replays: GET custom/replay?page=P&count=N&replay-data=true|false[&fighter=F | &tag-fighter=A,B | &solo-fighter=F]
// [&sort-by-dan-rank=true][&match-type=ranked|championship] - TEKKEN CHANNEL. Reply schemas: {"message", "data":
// {"replays": [...]}} - with replay-data at most 4 items (0x82E17340), without at most 100 (0x82E175D0).
func (s *Service) replays(q url.Values) []byte {
	withData := q.Get("replay-data") == "true"
	most := 100
	if withData {
		most = 4
	}
	count, _ := strconv.Atoi(q.Get("count"))
	page, _ := strconv.Atoi(q.Get("page"))
	if count <= 0 || count > most {
		count = most
	}
	if page < 0 || page > 10000 {
		page = 0
	}
	rq := store.ReplayQuery{Count: count, Offset: page * count, Data: withData, ByRank: q.Get("sort-by-dan-rank") == "true"}
	rq.Fighter, _ = strconv.Atoi(q.Get("fighter"))
	rq.SoloFighter, _ = strconv.Atoi(q.Get("solo-fighter"))
	if a, b, ok := strings.Cut(q.Get("tag-fighter"), ","); ok {
		rq.Tag[0], _ = strconv.Atoi(a)
		rq.Tag[1], _ = strconv.Atoi(b)
	}
	if t := q.Get("match-type"); t == "ranked" || t == "championship" {
		rq.Type = t
	}
	found, err := s.store.Replays(rq)
	if err != nil {
		return response(503, nil)
	}
	items := make([]json.RawMessage, 0, len(found))
	for _, r := range found {
		it := r.Item
		if withData && len(it) > 1 && it[len(it)-1] == '}' {
			// the kept item is an object; replay-data (base64: nothing to escape) goes in as its last field
			it = append(append([]byte{}, it[:len(it)-1]...), `,"replay-data":"`+r.Data+`"}`...)
		}
		items = append(items, json.RawMessage(it))
	}
	return success(map[string]any{"replays": items})
}

func keys(m map[string]json.RawMessage) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		if len(out) == 12 {
			break
		}
		out = append(out, clean(k, 32))
	}
	return out
}
