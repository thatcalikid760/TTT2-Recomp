package store

import (
	"context"
	"database/sql"
	"strings"
	"time"
)

// Ghosts and replays: what the game uploads after a ranked match a dan-ranked player won outright (MEASURED
// 2026-10-22), shown to other players - ghosts in GHOST BATTLE (by dan belt), replays in TEKKEN CHANNEL. Each is
// kept as the item the game's reply schema holds (a JSON object, the GIN service checks it) and served back as it
// is. Their bytes count with the other uploads (max_upload_storage_mb, the player's share).

// GhostKinds are the ghost slots of a dan belt, as the game names them: a tag pair and a solo ghost.
var GhostKinds = []string{"tag-ghost-1", "tag-ghost-2", "solo-ghost"}

const (
	maxGhostPlayersPerBelt = 300 // per belt and kind: the newest players' ghosts are kept
	maxReplaysPerPlayer    = 20
	maxReplays             = 20000
)

// Ghost is a kept ghost.
type Ghost struct {
	SteamID uint64
	Body    []byte // the ghost object (player, fighter, fighter-rank, match-beg, match-end, ghost-data)
}

// SaveGhost keeps a player's ghost of a kind for a dan belt, in place of the one they had there.
func (s *Store) SaveGhost(steamID uint64, belt int, kind string, body []byte, at time.Time) error {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var old int64
	if err := tx.QueryRowContext(ctx, `SELECT COALESCE(SUM(LENGTH(CAST(body AS BLOB))), 0) FROM ghosts
		WHERE steam_id = ? AND belt = ? AND kind = ?`, int64(steamID), belt, kind).Scan(&old); err != nil {
		return err
	}
	if err := s.roomFor(ctx, tx, steamID, int64(len(body))-old); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO ghosts (steam_id, belt, kind, body, at) VALUES (?, ?, ?, ?, ?)
		ON CONFLICT (steam_id, belt, kind) DO UPDATE SET body = excluded.body, at = excluded.at`,
		int64(steamID), belt, kind, string(body), at.Unix()); err != nil {
		return err
	}
	// the belt keeps its newest players' ghosts
	var dropped int64
	trim := `FROM ghosts WHERE belt = ? AND kind = ? AND steam_id NOT IN
		(SELECT steam_id FROM ghosts WHERE belt = ? AND kind = ? ORDER BY at DESC, steam_id LIMIT ?)`
	args := []any{belt, kind, belt, kind, maxGhostPlayersPerBelt}
	if err := tx.QueryRowContext(ctx, `SELECT COALESCE(SUM(LENGTH(CAST(body AS BLOB))), 0) `+trim, args...).Scan(&dropped); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, `DELETE `+trim, args...); err != nil {
		return err
	}
	if err := tx.Commit(); err != nil {
		return err
	}
	s.uploadBytes.Add(int64(len(body)) - old - dropped)
	return nil
}

// Ghosts are up to n of the newest ghosts of a kind for a belt, of players other than `not`, newest first.
func (s *Store) Ghosts(belt int, kind string, not uint64, n int) ([]Ghost, error) {
	rows, err := s.db.Query(`SELECT steam_id, body FROM ghosts WHERE belt = ? AND kind = ? AND steam_id != ?
		ORDER BY at DESC LIMIT ?`, belt, kind, int64(not), n)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Ghost
	for rows.Next() {
		var id int64
		var body string
		if err := rows.Scan(&id, &body); err != nil {
			return nil, err
		}
		out = append(out, Ghost{SteamID: uint64(id), Body: []byte(body)})
	}
	return out, rows.Err()
}

// GhostOf is a player's ghost of a kind for a belt (false: none).
func (s *Store) GhostOf(steamID uint64, belt int, kind string) ([]byte, bool, error) {
	var body string
	err := s.db.QueryRow(`SELECT body FROM ghosts WHERE steam_id = ? AND belt = ? AND kind = ?`, int64(steamID), belt, kind).Scan(&body)
	if err == sql.ErrNoRows {
		return nil, false, nil
	}
	return []byte(body), err == nil, err
}

// Replay is a kept replay.
type Replay struct {
	Match    string // the game's match id: one replay per match
	Item     []byte // the replay as the game lists it (a JSON object without replay-data)
	Data     string // replay-data (base64 text)
	Fighters [4]int // party 1 fighters 1, 2, party 2 fighters 1, 2 (-1: none)
	Solo     bool   // one fighter a side
	Rank     int    // the highest fighter rank in the match
	Type     string // match-type ("ranked", "championship")
}

// SaveReplay keeps a replay (false: that match's replay is kept already). A player keeps their newest
// maxReplaysPerPlayer, the server its newest maxReplays.
func (s *Store) SaveReplay(steamID uint64, r Replay, at time.Time) (bool, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return false, err
	}
	defer tx.Rollback()
	size := int64(len(r.Item) + len(r.Data))
	if err := s.roomFor(ctx, tx, steamID, size); err != nil {
		return false, err
	}
	solo := 0
	if r.Solo {
		solo = 1
	}
	res, err := tx.ExecContext(ctx, `INSERT INTO replays (steam_id, match_id, item, data, f1, f2, f3, f4, solo, best_rank, type, at)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT (match_id) DO NOTHING`, int64(steamID), r.Match, string(r.Item),
		r.Data, r.Fighters[0], r.Fighters[1], r.Fighters[2], r.Fighters[3], solo, r.Rank, r.Type, at.Unix())
	if err != nil {
		return false, err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return false, nil
	}
	var dropped int64
	for _, trim := range []struct {
		where string
		args  []any
	}{
		{`id NOT IN (SELECT id FROM replays WHERE steam_id = ? ORDER BY at DESC, id DESC LIMIT ?) AND steam_id = ?`,
			[]any{int64(steamID), maxReplaysPerPlayer, int64(steamID)}},
		{`id NOT IN (SELECT id FROM replays ORDER BY at DESC, id DESC LIMIT ?)`, []any{maxReplays}},
	} {
		var n int64
		if err := tx.QueryRowContext(ctx, `SELECT COALESCE(SUM(LENGTH(CAST(item AS BLOB)) + LENGTH(CAST(data AS BLOB))), 0)
			FROM replays WHERE `+trim.where, trim.args...).Scan(&n); err != nil {
			return false, err
		}
		if _, err := tx.ExecContext(ctx, `DELETE FROM replays WHERE `+trim.where, trim.args...); err != nil {
			return false, err
		}
		dropped += n
	}
	if err := tx.Commit(); err != nil {
		return false, err
	}
	s.uploadBytes.Add(size - dropped)
	return true, nil
}

// ReplayQuery is what TEKKEN CHANNEL asks for (the game's filters; 0 / "" = any).
type ReplayQuery struct {
	Fighter     int    // either side has this fighter (fighter=)
	Tag         [2]int // a side has these two fighters (tag-fighter=)
	SoloFighter int    // a one-fighter-a-side match with this fighter (solo-fighter=)
	Type        string // match-type=
	ByRank      bool   // sort-by-dan-rank=true: the highest ranks first (else the newest)
	Offset      int
	Count       int
	Data        bool // with replay-data
}

// Replays are the replays a query finds.
func (s *Store) Replays(q ReplayQuery) ([]Replay, error) {
	var where []string
	var args []any
	if q.Fighter > 0 {
		where = append(where, `(f1 = ? OR f2 = ? OR f3 = ? OR f4 = ?)`)
		args = append(args, q.Fighter, q.Fighter, q.Fighter, q.Fighter)
	}
	if q.Tag[0] > 0 && q.Tag[1] > 0 {
		a, b := q.Tag[0], q.Tag[1]
		where = append(where, `((f1 = ? AND f2 = ?) OR (f1 = ? AND f2 = ?) OR (f3 = ? AND f4 = ?) OR (f3 = ? AND f4 = ?))`)
		args = append(args, a, b, b, a, a, b, b, a)
	}
	if q.SoloFighter > 0 {
		where = append(where, `solo = 1 AND (f1 = ? OR f3 = ?)`)
		args = append(args, q.SoloFighter, q.SoloFighter)
	}
	if q.Type != "" {
		where = append(where, `type = ?`)
		args = append(args, q.Type)
	}
	query := `SELECT match_id, item, ` + map[bool]string{true: `data`, false: `''`}[q.Data] + `, f1, f2, f3, f4, solo, best_rank, type FROM replays`
	if len(where) > 0 {
		query += ` WHERE ` + strings.Join(where, ` AND `)
	}
	if q.ByRank {
		query += ` ORDER BY best_rank DESC, at DESC, id DESC`
	} else {
		query += ` ORDER BY at DESC, id DESC`
	}
	query += ` LIMIT ? OFFSET ?`
	args = append(args, q.Count, q.Offset)
	rows, err := s.db.Query(query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Replay
	for rows.Next() {
		var r Replay
		var item string
		var solo int
		if err := rows.Scan(&r.Match, &item, &r.Data, &r.Fighters[0], &r.Fighters[1], &r.Fighters[2], &r.Fighters[3], &solo,
			&r.Rank, &r.Type); err != nil {
			return nil, err
		}
		r.Item, r.Solo = []byte(item), solo != 0
		out = append(out, r)
	}
	return out, rows.Err()
}

// roomFor: ErrStorageFull when `more` bytes would go over the server's upload storage or the player's share.
func (s *Store) roomFor(ctx context.Context, tx *sql.Tx, steamID uint64, more int64) error {
	if more <= 0 {
		return nil
	}
	if s.uploadBytes.Load()+more > s.uploadQuota.Load() {
		return ErrStorageFull
	}
	if mine, err := s.bytesOf(ctx, tx, steamID); err != nil {
		return err
	} else if mine+more > maxUploadBytesEach {
		return ErrStorageFull
	}
	return nil
}

// bytesOf is how much of uploads, ghosts and replays a player has kept.
func (s *Store) bytesOf(ctx context.Context, tx *sql.Tx, steamID uint64) (int64, error) {
	var n int64
	err := tx.QueryRowContext(ctx, `SELECT
		(SELECT COALESCE(SUM(LENGTH(CAST(body AS BLOB))), 0) FROM uploads WHERE steam_id = ?) +
		(SELECT COALESCE(SUM(LENGTH(CAST(body AS BLOB))), 0) FROM ghosts WHERE steam_id = ?) +
		(SELECT COALESCE(SUM(LENGTH(CAST(item AS BLOB)) + LENGTH(CAST(data AS BLOB))), 0) FROM replays WHERE steam_id = ?)`,
		int64(steamID), int64(steamID), int64(steamID)).Scan(&n)
	return n, err
}
