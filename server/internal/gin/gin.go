// Package gin answers Namco's GIN web API the way the game speaks it (PROTOCOL.md "GIN"): HTTP/1.1 requests to
// /2.2/ttt2/xbox/<endpoint>, carried over the player's control connection, answered with JSON the game's parser
// accepts. The game's reply schemas (measured in its image, COMMUNITY_SERVER_PLAN.md phase 0 S4): connect =
// {"message", "session_id"}; settings = {"message", "data": {maintenance-mode, messages-{news,motd,misc}-<lang>
// (10 x char[120]), ghost-download-count, throttle-uploads-per-hour-{max,ave}}}; other endpoints {"message",
// "data"}.
//
// Who is asking comes from the TLS login (Caller), never from the request: the game's cookies and user ids are
// only recorded.
package gin

import (
	"bufio"
	"bytes"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"ttt2-server/internal/store"
)

// Limits on what a game may send.
const (
	MaxRequest = 256 << 10 // a whole request (head and body)
	MaxProfile = 64 << 10  // the profile the game uploads to /player
	MaxUpload  = 200 << 10 // a match report, ghost, replay or leaderboard upload
)

// Uploads the game makes that are kept as sent (kind -> how many per player are kept): match reports after ranked
// matches, offline records (POST leaderboard/, after ranked matches and offline modes). Ghosts and replays are kept
// as items and served back (media.go).
var uploads = map[string]int{"match": 50, "leaderboard": 20}

// The game's languages (messages-<kind>-<lang>).
var languages = []string{"en", "jp", "ko", "fr", "it", "ge", "sp", "ru"}

// Caller is the player a request comes from (their control connection).
type Caller struct {
	SteamID  uint64
	Name     string
	Verified bool
	// ClaimXUID, when set, is the only way an XUID is recorded for the player (the hub fixes a connection's XUID);
	// false: refused.
	ClaimXUID func(xuid uint64) bool
}

// Store keeps what the game sends.
type Store interface {
	// SetXUID records the online XUID the player's game uses (its GIN user id, its Live statistics).
	SetXUID(steamID, xuid uint64) error
	// SaveProfile keeps the profile the game uploads on every ONLINE MODE visit (JSON).
	SaveProfile(steamID uint64, body []byte, at time.Time) error
	// SaveUpload keeps an upload (JSON) of a kind, the newest `keep` of each player.
	SaveUpload(steamID uint64, kind string, body []byte, keep int, at time.Time) error
	// SteamIDsOf maps online XUIDs to players; TeamOf is a player's team (false: in none).
	SteamIDsOf(xuids []uint64) (map[uint64]uint64, error)
	TeamOf(steamID uint64) (store.Team, bool, error)
	// ghosts and replays (store/media.go)
	SaveGhost(steamID uint64, belt int, kind string, body []byte, at time.Time) error
	Ghosts(belt int, kind string, not uint64, n int) ([]store.Ghost, error)
	GhostOf(steamID uint64, belt int, kind string) ([]byte, bool, error)
	SaveReplay(steamID uint64, r store.Replay, at time.Time) (bool, error)
	Replays(q store.ReplayQuery) ([]store.Replay, error)
}

// Settings is what the server tells the game (from server.toml; read for each request).
type Settings struct {
	MOTD string   // message of the day: cut into lines of the game's length
	News []string // news lines
}

// Service answers requests.
type Service struct {
	store    Store
	settings func() Settings
	log      *log.Logger
	once     sync.Map // endpoints already logged as unknown (at most maxLogged)
	logged   atomic.Int32
	internal sync.Map // the server's own messages already logged

	mu      sync.Mutex
	uploads map[uint64]*uploadRate // per player: what the settings tell the game (30 an hour)
}

const maxLogged = 64

// uploadRate: a token bucket per player, refilled at uploadsPerHour.
type uploadRate struct {
	tokens float64
	last   time.Time
}

const uploadsPerHour, uploadBurst = 30, 10

// mayUpload takes one upload of a player's hourly allowance.
func (s *Service) mayUpload(steamID uint64, now time.Time) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.uploads == nil {
		s.uploads = map[uint64]*uploadRate{}
	}
	if len(s.uploads) > 100000 { // forget players whose buckets are full again
		for id, b := range s.uploads {
			if now.Sub(b.last) > time.Hour {
				delete(s.uploads, id)
			}
		}
	}
	b := s.uploads[steamID]
	if b == nil {
		b = &uploadRate{tokens: uploadBurst, last: now}
		s.uploads[steamID] = b
	}
	b.tokens = min(uploadBurst, b.tokens+uploadsPerHour*now.Sub(b.last).Hours())
	b.last = now
	if b.tokens < 1 {
		return false
	}
	b.tokens--
	return true
}

// New makes a service.
func New(store Store, settings func() Settings, logger *log.Logger) *Service {
	return &Service{store: store, settings: settings, log: logger}
}

// Serve answers one request (raw HTTP bytes) with a complete HTTP response.
func (s *Service) Serve(c Caller, raw []byte) []byte {
	if len(raw) > MaxRequest {
		return response(413, nil)
	}
	req, err := http.ReadRequest(bufio.NewReader(bytes.NewReader(raw)))
	if err != nil {
		return response(400, nil)
	}
	body, err := io.ReadAll(io.LimitReader(req.Body, MaxRequest))
	if err != nil {
		return response(400, nil)
	}
	endpoint, ok := strings.CutPrefix(req.URL.Path, "/2.2/ttt2/xbox/")
	if !ok {
		return response(404, nil)
	}
	switch {
	case endpoint == "connect" && req.Method == http.MethodPost:
		return s.connect(c, body)
	case endpoint == "settings" && req.Method == http.MethodGet:
		return s.settingsReply()
	case endpoint == "player" && req.Method == http.MethodPost:
		return s.player(c, body)
	case req.Method == http.MethodPost && uploads[strings.TrimSuffix(endpoint, "/")] > 0:
		return s.upload(c, strings.TrimSuffix(endpoint, "/"), body)
	case endpoint == "team" && req.Method == http.MethodGet:
		return s.team(req.URL.Query())
	case endpoint == "custom/ghost" && req.Method == http.MethodPost:
		return s.ghostUpload(c, body)
	case endpoint == "custom/replay" && req.Method == http.MethodPost:
		return s.replayUpload(c, body)
	case endpoint == "custom/ghost" && req.Method == http.MethodGet:
		return s.ghosts(c, req.URL.Query())
	case endpoint == "custom/replay" && req.Method == http.MethodGet:
		return s.replays(req.URL.Query())
	default:
		s.logOnce(req.Method+" "+endpoint, "GIN: %.80q answered empty (not kept yet)", req.Method+" "+endpoint)
		return success(map[string]any{})
	}
}

// connect: the game's GIN login (form body userid=<online XUID>); the reply's session id goes back in its cookie.
func (s *Service) connect(c Caller, body []byte) []byte {
	if form, err := url.ParseQuery(strings.TrimSpace(string(body))); err == nil {
		if xuid, err := strconv.ParseUint(form.Get("userid"), 10, 64); err == nil && xuid != 0 {
			if c.ClaimXUID != nil {
				c.ClaimXUID(xuid) // refusals are logged there
			} else if err := s.store.SetXUID(c.SteamID, xuid); err != nil {
				s.logInternal("xuid", "GIN: XUID of %q not recorded: %v", c.Name, err)
			}
		}
	}
	var id [16]byte
	rand.Read(id[:])
	return reply(map[string]any{"message": "OK", "session_id": hex.EncodeToString(id[:])})
}

func (s *Service) settingsReply() []byte {
	st := s.settings()
	// every value is a string or an array of strings in the game's schema (0x82E17B18: type 5 = char[120], 6 = up to
	// 10 of them) - a number where it expects a string is not taken
	data := map[string]any{
		"maintenance-mode":              "",
		"ghost-download-count":          "10",
		"throttle-uploads-per-hour-max": "30",
		"throttle-uploads-per-hour-ave": "10",
	}
	motd, news := Lines(st.MOTD), make([]string, 0, len(st.News))
	for _, n := range st.News {
		if t := gameText(n); t != "" && len(news) < 10 {
			news = append(news, t)
		}
	}
	for _, lang := range languages {
		data["messages-motd-"+lang] = motd
		data["messages-news-"+lang] = news
		data["messages-misc-"+lang] = []string{}
	}
	return reply(map[string]any{"message": "OK", "data": data})
}

// upload: kept as sent (valid JSON, bounded); the game only needs to hear it was taken.
func (s *Service) upload(c Caller, kind string, body []byte) []byte {
	if len(body) > MaxUpload || !json.Valid(body) {
		return response(400, nil)
	}
	if !s.mayUpload(c.SteamID, time.Now()) {
		return response(429, nil)
	}
	if err := s.store.SaveUpload(c.SteamID, kind, body, uploads[kind], time.Now()); err != nil {
		s.logInternal("upload error "+kind, "GIN: %s upload of %q not kept: %v", kind, c.Name, err)
		return response(503, nil)
	}
	s.logInternal("upload "+kind, "GIN: the first %s upload (%d bytes, from %q) kept", kind, len(body), c.Name)
	return posted()
}

// logInternal logs a message of the server's own (a fixed set of keys) the first time.
func (s *Service) logInternal(key, format string, args ...any) {
	if _, seen := s.internal.LoadOrStore(key, true); !seen {
		s.log.Printf(format, args...)
	}
}

// logOnce logs a message about a client-chosen key the first time (at most maxLogged keys: a client cannot grow
// the set).
func (s *Service) logOnce(key, format string, args ...any) {
	if _, seen := s.once.Load(key); seen || s.logged.Load() >= maxLogged {
		return
	}
	if _, seen := s.once.LoadOrStore(key, true); !seen {
		s.logged.Add(1)
		s.log.Printf(format, args...)
	}
}

// team: the teams of the players asked for (for-players=<XUID>[,<XUID>]: the game asks for its own on every ONLINE
// MODE visit), in the game's reply schema (0x82E179EC): data.teams[] = {player, team: {...}} and
// data.player-team-ids[] = {player, team}, at most 2 of each. XUIDs are decimal strings, as the game sends them.
func (s *Service) team(q url.Values) []byte {
	var xuids []uint64
	for _, part := range strings.Split(q.Get("for-players"), ",") {
		if x, err := strconv.ParseUint(strings.TrimSpace(part), 10, 64); err == nil && x != 0 && len(xuids) < 2 {
			xuids = append(xuids, x)
		}
	}
	teams, ids := []any{}, []any{}
	owners, err := s.store.SteamIDsOf(xuids)
	if err != nil {
		return response(503, nil)
	}
	for _, x := range xuids {
		steam := owners[x]
		if steam == 0 {
			continue
		}
		t, ok, err := s.store.TeamOf(steam)
		if err != nil {
			return response(503, nil)
		}
		if !ok {
			continue
		}
		player := strconv.FormatUint(x, 10)
		teams = append(teams, map[string]any{"player": player, "team": teamJSON(t)})
		ids = append(ids, map[string]any{"player": player, "team": strconv.FormatInt(t.ID, 10)})
	}
	return success(map[string]any{"teams": teams, "player-team-ids": ids})
}

// teamJSON is a team as the game's schema has it (strings where it keeps text, numbers where it keeps ints): its two
// best-ranked members, level and points from the members' ranked wins, leader and members by XUID.
func teamJSON(t store.Team) map[string]any {
	xuid := func(m store.TeamMember) string {
		if m.XUID == 0 {
			return ""
		}
		return strconv.FormatUint(m.XUID, 10)
	}
	members, best := []string{}, []store.TeamMember{}
	leader, wins := "", 0
	for _, m := range t.Members {
		if x := xuid(m); x != "" {
			members = append(members, x)
		}
		if m.SteamID == t.Leader {
			leader = xuid(m)
		}
		wins += m.Wins
		if m.BestRank >= 0 {
			best = append(best, m)
		}
	}
	sort.SliceStable(best, func(i, j int) bool { return best[i].BestRank > best[j].BestRank })
	out := map[string]any{
		"team": strconv.FormatInt(t.ID, 10), "team-name": clean(t.Name, 32), "leader": leader, "members": members,
		"team-level": 1 + min(9, wins/50), "team-mood": 0, "team-appeal-1": 0, "team-appeal-2": 0, "team-goal-1": 0,
		"team-goal-2": 0, "member-invite-status": 0, "points": wins, "experience": wins * 10, "team-icon-1": 0,
		"team-icon-2": 0, "team-foundation": 0, "team-founded-on": t.Created.Unix(),
	}
	for i := 1; i <= 2; i++ {
		player, fighter, rank := "", "", ""
		if i <= len(best) {
			m := best[i-1]
			player, fighter, rank = xuid(m), strconv.Itoa(m.BestFighter), strconv.Itoa(m.BestRank)
		}
		n := strconv.Itoa(i)
		out["highest-rank-fighter-"+n+"-player"] = player
		out["highest-rank-fighter-"+n] = fighter
		out["highest-rank-"+n] = rank
	}
	return out
}

// player: the profile upload (JSON; the game's "payload" holds battle points, region, per-fighter rank / wins /
// losses ...). Kept as sent, for the rankings (phase 4).
func (s *Service) player(c Caller, body []byte) []byte {
	if len(body) > MaxProfile || !json.Valid(body) {
		return response(400, nil)
	}
	if err := s.store.SaveProfile(c.SteamID, body, time.Now()); err != nil {
		s.log.Printf("GIN: profile of %q not kept: %v", c.Name, err)
	}
	return posted()
}

// Lines cuts a message into the game's message lines: at most 10 of 119 characters, at line breaks and spaces.
func Lines(text string) []string {
	var out []string
	for _, para := range strings.Split(text, "\n") {
		para = clean(para, 2000)
		for para != "" && len(out) < 10 {
			if len(para) <= 119 {
				out = append(out, para)
				break
			}
			cut := strings.LastIndexByte(para[:119], ' ')
			if cut <= 0 {
				cut = 119
			}
			out = append(out, strings.TrimSpace(para[:cut]))
			para = strings.TrimSpace(para[cut:])
		}
	}
	if out == nil {
		out = []string{}
	}
	return out
}

// gameText keeps what the game's text system shows plainly, at most 119 characters (one of its message lines).
func gameText(s string) string { return clean(s, 119) }

// clean keeps printable ASCII without the game's markup ('~' colour, '%' format) and without the characters JSON
// escapes (" \ < > &) - the game's parser is not known to undo escapes - at most max characters.
func clean(s string, max int) string {
	var b strings.Builder
	for _, r := range s {
		if r >= 0x20 && r < 0x7F && !strings.ContainsRune("~%\"\\<>&", r) && b.Len() < max {
			b.WriteRune(r)
		}
	}
	return strings.TrimSpace(b.String())
}

// Busy is the answer to a game asking faster than the server allows (it retries later).
func Busy() []byte { return response(503, nil) }

// The game's GIN library (GINware, sub_82A5E990) takes a reply only when its "message" is "OK" - for a request
// that reads data back - or "Created" - for an upload (its events: player, match report, offline record, ghost,
// replay); anything else is status -1, the request failed. An upload's reply is parsed with a schema of "message"
// alone: an object it does not know (as an empty "data") is a parse error (-4). MEASURED 2026-10-22: every upload
// failed before; a failed match report made the game cancel that match's ghost and replay uploads.

// posted is the answer to an upload (POST).
func posted() []byte { return reply(map[string]any{"message": "Created"}) }

func success(data any) []byte { return reply(map[string]any{"message": "OK", "data": data}) }

func reply(v any) []byte {
	var body bytes.Buffer
	enc := json.NewEncoder(&body)
	enc.SetEscapeHTML(false)
	enc.Encode(v)
	return response(200, bytes.TrimRight(body.Bytes(), "\n"))
}

// response is the game's HTTP: the length given, and the connection closed after it (the game reads until then).
func response(code int, body []byte) []byte {
	var b bytes.Buffer
	fmt.Fprintf(&b, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
		code, http.StatusText(code), len(body))
	b.Write(body)
	return b.Bytes()
}
