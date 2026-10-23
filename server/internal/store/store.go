// Package store is the server's database: what has to outlive a restart, in data/server.db (SQLite through
// modernc.org/sqlite - pure Go, so the server still cross-compiles without a C toolchain).
//
// The database runs in WAL mode (readers never wait for the writer; a crash or power cut loses at most the last
// transactions, never the file), with foreign keys on and a 5 s busy timeout. Its schema is a numbered list of
// migrations applied in order when it opens (PRAGMA user_version counts them); a database written by a newer
// server is refused rather than guessed at. Backups are taken live with VACUUM INTO (Backup) - copying the file
// of a running server can miss what is still in its WAL.
package store

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync/atomic"
	"time"

	_ "modernc.org/sqlite" // the "sqlite" database/sql driver
)

// migrations[i] takes the schema from version i to i+1. Only ever append.
var migrations = []string{
	// 1: the players the server has seen (Steam ID; the name and whether Steam confirmed the ID at the last login)
	`CREATE TABLE players (
		steam_id   INTEGER PRIMARY KEY,
		name       TEXT    NOT NULL,
		verified   INTEGER NOT NULL,
		first_seen INTEGER NOT NULL,
		last_seen  INTEGER NOT NULL,
		logins     INTEGER NOT NULL
	) STRICT;
	CREATE INDEX players_name ON players (name COLLATE NOCASE);`,
	// 2: the online XUID each player's game uses (GIN, Live statistics), and the profile it uploads (GIN /player)
	`ALTER TABLE players ADD COLUMN xuid INTEGER;
	CREATE TABLE profiles (
		steam_id INTEGER PRIMARY KEY REFERENCES players (steam_id),
		body     TEXT    NOT NULL,
		updated  INTEGER NOT NULL
	) STRICT;`,
	// 3: the leaderboards (internal/stats): one row per view, board period (0, or yyyymm for monthly views) and
	// player; `rated` = when the rating last changed (an equal rating reached earlier ranks higher)
	`CREATE INDEX players_xuid ON players (xuid);
	CREATE TABLE stats (
		view     INTEGER NOT NULL,
		period   INTEGER NOT NULL,
		steam_id INTEGER NOT NULL REFERENCES players (steam_id),
		rating   INTEGER NOT NULL,
		columns  BLOB    NOT NULL,
		rated    INTEGER NOT NULL,
		updated  INTEGER NOT NULL,
		PRIMARY KEY (view, period, steam_id)
	) STRICT;
	CREATE INDEX stats_rank ON stats (view, period, rating DESC, rated, steam_id);`,
	// 4: what the game uploads over GIN and is kept as sent (match reports, offline records, ghosts, replays)
	`CREATE TABLE uploads (
		id       INTEGER PRIMARY KEY,
		steam_id INTEGER NOT NULL REFERENCES players (steam_id),
		kind     TEXT    NOT NULL,
		body     TEXT    NOT NULL,
		at       INTEGER NOT NULL
	) STRICT;
	CREATE INDEX uploads_player ON uploads (steam_id, kind, id);`,
	// 5: teams (the server's admin makes them; the game shows them): a player is in one team at most
	`CREATE TABLE teams (
		id      INTEGER PRIMARY KEY,
		name    TEXT    NOT NULL UNIQUE COLLATE NOCASE,
		leader  INTEGER NOT NULL REFERENCES players (steam_id),
		created INTEGER NOT NULL
	) STRICT;
	CREATE TABLE team_members (
		steam_id INTEGER PRIMARY KEY REFERENCES players (steam_id),
		team_id  INTEGER NOT NULL REFERENCES teams (id) ON DELETE CASCADE,
		joined   INTEGER NOT NULL
	) STRICT;
	CREATE INDEX team_members_team ON team_members (team_id, joined);`,
	// 6: ghosts (one per player, dan belt and kind) and replays (one per match) the game uploads after ranked matches,
	// shown to other players (media.go)
	`CREATE TABLE ghosts (
		steam_id INTEGER NOT NULL REFERENCES players (steam_id),
		belt     INTEGER NOT NULL,
		kind     TEXT    NOT NULL,
		body     TEXT    NOT NULL,
		at       INTEGER NOT NULL,
		PRIMARY KEY (steam_id, belt, kind)
	) STRICT;
	CREATE INDEX ghosts_pick ON ghosts (belt, kind, at);
	CREATE TABLE replays (
		id        INTEGER PRIMARY KEY,
		steam_id  INTEGER NOT NULL REFERENCES players (steam_id),
		match_id  TEXT    NOT NULL UNIQUE,
		item      TEXT    NOT NULL,
		data      TEXT    NOT NULL,
		f1        INTEGER NOT NULL,
		f2        INTEGER NOT NULL,
		f3        INTEGER NOT NULL,
		f4        INTEGER NOT NULL,
		solo      INTEGER NOT NULL,
		best_rank INTEGER NOT NULL,
		type      TEXT    NOT NULL,
		at        INTEGER NOT NULL
	) STRICT;
	CREATE INDEX replays_new ON replays (at);
	CREATE INDEX replays_player ON replays (steam_id, at);`,
	// 7: the match history (matches.go): every match the games report (MATCH_EVENT) or the ranked results settle,
	// with its players as the server knew them then; `ended` NULL while open, `won` NULL when not known
	`CREATE TABLE matches (
		id         INTEGER PRIMARY KEY,
		session_id INTEGER NOT NULL,
		kind       TEXT    NOT NULL,
		started    INTEGER NOT NULL,
		ended      INTEGER,
		result     TEXT    NOT NULL DEFAULT ''
	) STRICT;
	CREATE INDEX matches_started ON matches (started);
	CREATE INDEX matches_open ON matches (started) WHERE ended IS NULL;
	CREATE TABLE match_players (
		match_id      INTEGER NOT NULL REFERENCES matches (id) ON DELETE CASCADE,
		steam_id      INTEGER NOT NULL,
		name          TEXT    NOT NULL,
		local_players INTEGER NOT NULL DEFAULT 1,
		won           INTEGER,
		PRIMARY KEY (match_id, steam_id)
	) STRICT;
	CREATE INDEX match_players_player ON match_players (steam_id, match_id);`,
	// 8: the player files (player_files.go): each player's last upload of the items other players read (licence.dat)
	`CREATE TABLE player_files (
		steam_id INTEGER NOT NULL,
		item     TEXT    NOT NULL,
		data     BLOB    NOT NULL,
		updated  INTEGER NOT NULL,
		PRIMARY KEY (steam_id, item)
	) STRICT;`,
}

// Store is the open database.
type Store struct {
	db    *sql.DB
	path  string
	ranks rankIndexes
	// uploads: the bytes kept (counted at Open, then kept up to date) and the most the server keeps
	uploadBytes atomic.Int64
	uploadQuota atomic.Int64
}

// ErrXUIDTaken: another player, whose Steam ID was checked, uses that XUID.
var ErrXUIDTaken = errors.New("that XUID belongs to another player")

// maxUploadBytesEach: the most of uploads, ghosts and replays kept for one player.
const maxUploadBytesEach = 8 << 20

// ErrStorageFull: the server keeps no more uploads (max_upload_storage_mb, or the player's share).
var ErrStorageFull = errors.New("upload storage full")

// Open opens (creating it on first run, readable by the owner only) and migrates the database at path.
func Open(path string) (*Store, error) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return nil, err
	}
	if f, err := os.OpenFile(abs, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600); err == nil {
		f.Close()
	} else if !errors.Is(err, os.ErrExist) {
		return nil, fmt.Errorf("database %s: %w", abs, err)
	}
	q := url.Values{}
	for _, p := range []string{"journal_mode(WAL)", "synchronous(NORMAL)", "foreign_keys(1)", "busy_timeout(5000)"} {
		q.Add("_pragma", p)
	}
	q.Set("_txlock", "immediate") // a write transaction takes the lock up front (no deadlock between upgrades)
	db, err := sql.Open("sqlite", "file:"+fileURIPath(abs)+"?"+q.Encode())
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(8)
	s := &Store{db: db, path: abs}
	s.uploadQuota.Store(1 << 30)
	if err := s.migrate(); err != nil {
		db.Close()
		return nil, fmt.Errorf("database %s: %w", abs, err)
	}
	s.recountUploads()
	return s, nil
}

// fileURIPath is a path for a file: URI ("C:\a b\x.db" -> "/C:/a%20b/x.db").
func fileURIPath(abs string) string {
	p := filepath.ToSlash(abs)
	if !strings.HasPrefix(p, "/") {
		p = "/" + p
	}
	return (&url.URL{Path: p}).EscapedPath()
}

func (s *Store) migrate() error {
	var have int
	if err := s.db.QueryRow(`PRAGMA user_version`).Scan(&have); err != nil {
		return err
	}
	if have > len(migrations) {
		return fmt.Errorf("written by a newer server (schema %d, this one knows %d) - use that server or a backup", have, len(migrations))
	}
	for v := have; v < len(migrations); v++ {
		tx, err := s.db.Begin()
		if err != nil {
			return err
		}
		if _, err := tx.Exec(migrations[v]); err != nil {
			tx.Rollback()
			return fmt.Errorf("schema %d: %w", v+1, err)
		}
		if _, err := tx.Exec(fmt.Sprintf(`PRAGMA user_version = %d`, v+1)); err != nil {
			tx.Rollback()
			return err
		}
		if err := tx.Commit(); err != nil {
			return err
		}
	}
	return nil
}

// Version is the schema version (the number of migrations applied).
func (s *Store) Version() int {
	var v int
	s.db.QueryRow(`PRAGMA user_version`).Scan(&v)
	return v
}

// Close closes the database (the WAL is folded back into the file).
func (s *Store) Close() error { return s.db.Close() }

// Path is the database file.
func (s *Store) Path() string { return s.path }

// Player is one row of players.
type Player struct {
	SteamID             uint64
	Name                string
	Verified            bool // Steam confirmed the ID at the last login
	FirstSeen, LastSeen time.Time
	Logins              int64
	XUID                uint64 // the online XUID its game uses (0 = not known yet)
}

// PlayerSeen records a login: a new player, or the name, check and time of a known one.
func (s *Store) PlayerSeen(id uint64, name string, verified bool, at time.Time) error {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	_, err := s.db.ExecContext(ctx, `
		INSERT INTO players (steam_id, name, verified, first_seen, last_seen, logins) VALUES (?, ?, ?, ?, ?, 1)
		ON CONFLICT (steam_id) DO UPDATE SET
			name = excluded.name, verified = excluded.verified, last_seen = excluded.last_seen, logins = logins + 1`,
		int64(id), name, verified, at.Unix(), at.Unix())
	return err
}

// FindPlayers is the players with that Steam ID or name (any case), most recently seen first (at most 20).
func (s *Store) FindPlayers(who string) ([]Player, error) {
	var rows *sql.Rows
	var err error
	if id, perr := parseID(who); perr == nil {
		rows, err = s.db.Query(`SELECT steam_id, name, verified, first_seen, last_seen, logins, xuid FROM players WHERE steam_id = ?`, int64(id))
	} else {
		rows, err = s.db.Query(`SELECT steam_id, name, verified, first_seen, last_seen, logins, xuid FROM players
			WHERE name = ? COLLATE NOCASE ORDER BY last_seen DESC LIMIT 20`, who)
	}
	if err != nil {
		return nil, err
	}
	return scanPlayers(rows)
}

// AllPlayers is every player the server has seen, most recently seen first (at most `count`, at most 200): all of
// them (search ""), or those whose name contains `search` (any case) or whose Steam ID it is.
func (s *Store) AllPlayers(search string, count int) ([]Player, error) {
	ctx, cancel := context.WithTimeout(context.Background(), readTimeout)
	defer cancel()
	count = min(max(count, 1), 200)
	const cols = `SELECT steam_id, name, verified, first_seen, last_seen, logins, xuid FROM players `
	const order = ` ORDER BY last_seen DESC, steam_id LIMIT ?`
	like := "%" + likeEscaper.Replace(search) + "%"
	var rows *sql.Rows
	var err error
	if id, perr := parseID(search); search == "" {
		rows, err = s.db.QueryContext(ctx, cols+order, count)
	} else if perr == nil {
		rows, err = s.db.QueryContext(ctx, cols+`WHERE steam_id = ? OR name LIKE ? ESCAPE '\'`+order, int64(id), like, count)
	} else {
		rows, err = s.db.QueryContext(ctx, cols+`WHERE name LIKE ? ESCAPE '\'`+order, like, count)
	}
	if err != nil {
		return nil, err
	}
	return scanPlayers(rows)
}

// likeEscaper makes a search text match itself in LIKE (with ESCAPE '\'): % and _ are not wildcards there.
var likeEscaper = strings.NewReplacer(`\`, `\\`, `%`, `\%`, `_`, `\_`)

// scanPlayers reads (and closes) rows of steam_id, name, verified, first_seen, last_seen, logins, xuid.
func scanPlayers(rows *sql.Rows) ([]Player, error) {
	defer rows.Close()
	var out []Player
	for rows.Next() {
		var p Player
		var id, first, last int64
		var xuid sql.NullInt64
		if err := rows.Scan(&id, &p.Name, &p.Verified, &first, &last, &p.Logins, &xuid); err != nil {
			return nil, err
		}
		p.SteamID = uint64(id)
		p.XUID = uint64(xuid.Int64)
		p.FirstSeen, p.LastSeen = time.Unix(first, 0), time.Unix(last, 0)
		out = append(out, p)
	}
	return out, rows.Err()
}

// SetXUID records the online XUID a player's game uses (a seen player only).
// An XUID a checked player already uses is not taken from them (ErrXUIDTaken).
func (s *Store) SetXUID(steamID, xuid uint64) error {
	r, err := s.db.Exec(`UPDATE players SET xuid = ? WHERE steam_id = ? AND NOT EXISTS
		(SELECT 1 FROM players WHERE xuid = ? AND steam_id != ? AND verified = 1)`, int64(xuid), int64(steamID), int64(xuid), int64(steamID))
	if err != nil {
		return err
	}
	if n, _ := r.RowsAffected(); n == 0 {
		var taken int
		if s.db.QueryRow(`SELECT COUNT(*) FROM players WHERE xuid = ? AND steam_id != ? AND verified = 1`, int64(xuid), int64(steamID)).Scan(&taken); taken > 0 {
			return ErrXUIDTaken
		}
	}
	return nil
}

// SaveProfile keeps the profile a player's game uploaded (JSON), replacing the last one.
func (s *Store) SaveProfile(steamID uint64, body []byte, at time.Time) error {
	_, err := s.db.Exec(`INSERT INTO profiles (steam_id, body, updated) VALUES (?, ?, ?)
		ON CONFLICT (steam_id) DO UPDATE SET body = excluded.body, updated = excluded.updated`,
		int64(steamID), string(body), at.Unix())
	return err
}

// SaveUpload keeps an upload of a kind and forgets that player's older ones beyond `keep`.
func (s *Store) SaveUpload(steamID uint64, kind string, body []byte, keep int, at time.Time) error {
	if s.uploadBytes.Load()+int64(len(body)) > s.uploadQuota.Load() {
		return ErrStorageFull
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if err := s.roomFor(ctx, tx, steamID, int64(len(body))); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO uploads (steam_id, kind, body, at) VALUES (?, ?, ?, ?)`,
		int64(steamID), kind, string(body), at.Unix()); err != nil {
		return err
	}
	var dropped int64
	if err := tx.QueryRowContext(ctx, `SELECT COALESCE(SUM(LENGTH(CAST(body AS BLOB))), 0) FROM uploads WHERE steam_id = ? AND kind = ? AND id NOT IN
		(SELECT id FROM uploads WHERE steam_id = ? AND kind = ? ORDER BY id DESC LIMIT ?)`,
		int64(steamID), kind, int64(steamID), kind, keep).Scan(&dropped); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM uploads WHERE steam_id = ? AND kind = ? AND id NOT IN
		(SELECT id FROM uploads WHERE steam_id = ? AND kind = ? ORDER BY id DESC LIMIT ?)`,
		int64(steamID), kind, int64(steamID), kind, keep); err != nil {
		return err
	}
	if err := tx.Commit(); err != nil {
		return err
	}
	s.uploadBytes.Add(int64(len(body)) - dropped)
	return nil
}

// SetUploadQuota sets the most the server keeps of uploads, in bytes.
func (s *Store) SetUploadQuota(bytes int64) { s.uploadQuota.Store(bytes) }

// UploadBytes is how much of uploads the server keeps.
func (s *Store) UploadBytes() int64 { return s.uploadBytes.Load() }

func (s *Store) recountUploads() {
	var n int64
	if s.db.QueryRow(`SELECT (SELECT COALESCE(SUM(LENGTH(CAST(body AS BLOB))), 0) FROM uploads) +
		(SELECT COALESCE(SUM(LENGTH(CAST(body AS BLOB))), 0) FROM ghosts) +
		(SELECT COALESCE(SUM(LENGTH(CAST(item AS BLOB)) + LENGTH(CAST(data AS BLOB))), 0) FROM replays)`).Scan(&n) == nil {
		s.uploadBytes.Store(n)
	}
}

// Uploads is a player's kept uploads of a kind, newest first.
func (s *Store) Uploads(steamID uint64, kind string) ([][]byte, error) {
	q, err := s.db.Query(`SELECT body FROM uploads WHERE steam_id = ? AND kind = ? ORDER BY id DESC`, int64(steamID), kind)
	if err != nil {
		return nil, err
	}
	defer q.Close()
	var out [][]byte
	for q.Next() {
		var b string
		if err := q.Scan(&b); err != nil {
			return nil, err
		}
		out = append(out, []byte(b))
	}
	return out, q.Err()
}

// Profile is the last profile a player's game uploaded.
func (s *Store) Profile(steamID uint64) (body []byte, at time.Time, found bool, err error) {
	var text string
	var updated int64
	err = s.db.QueryRow(`SELECT body, updated FROM profiles WHERE steam_id = ?`, int64(steamID)).Scan(&text, &updated)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, time.Time{}, false, nil
	}
	if err != nil {
		return nil, time.Time{}, false, err
	}
	return []byte(text), time.Unix(updated, 0), true, nil
}

// PlayerCount is how many players the server has seen.
func (s *Store) PlayerCount() (int64, error) {
	var n int64
	err := s.db.QueryRow(`SELECT COUNT(*) FROM players`).Scan(&n)
	return n, err
}

// Backup writes a consistent copy of the database to dest (a new file) while the server runs.
func (s *Store) Backup(dest string) error {
	abs, err := filepath.Abs(dest)
	if err != nil {
		return err
	}
	if _, err := os.Stat(abs); err == nil {
		return fmt.Errorf("%s already exists", abs)
	}
	_, err = s.db.Exec(`VACUUM INTO ?`, abs)
	return err
}

func parseID(s string) (uint64, error) { return strconv.ParseUint(s, 10, 64) }
