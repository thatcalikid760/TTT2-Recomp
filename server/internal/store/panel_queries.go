package store

import (
	"context"
	"database/sql"
	"encoding/json"
	"strings"
	"time"
)

// The admin panel's reads (internal/panel): pages of players and matches, a player's names, counts. Read-only, each
// bounded by readTimeout; lists of Steam IDs go in as one JSON parameter (json_each), however long.

// PlayerFilter is a page of the players list.
type PlayerFilter struct {
	Search string   // "" every player; else a part of the name (any case) or a Steam ID
	Only   []uint64 // nil: anyone; else only these Steam IDs (empty: nobody)
	Not    []uint64 // never these Steam IDs
	Offset int
	Limit  int // 1..200
}

// PanelPlayers is a page of the players the server has seen, most recently seen first, and how many match in all.
func (s *Store) PanelPlayers(f PlayerFilter) ([]Player, int, error) {
	if f.Only != nil && len(f.Only) == 0 {
		return nil, 0, nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), readTimeout)
	defer cancel()
	var where []string
	var args []any
	if f.Search != "" {
		like := "%" + likeEscaper.Replace(f.Search) + "%"
		if id, err := parseID(f.Search); err == nil {
			where = append(where, `(steam_id = ? OR name LIKE ? ESCAPE '\')`)
			args = append(args, int64(id), like)
		} else {
			where = append(where, `name LIKE ? ESCAPE '\'`)
			args = append(args, like)
		}
	}
	if f.Only != nil {
		where = append(where, `steam_id IN (SELECT value FROM json_each(?))`)
		args = append(args, idList(f.Only))
	}
	if len(f.Not) > 0 {
		where = append(where, `steam_id NOT IN (SELECT value FROM json_each(?))`)
		args = append(args, idList(f.Not))
	}
	cond := ""
	if len(where) > 0 {
		cond = " WHERE " + strings.Join(where, " AND ")
	}
	var total int
	if err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM players`+cond, args...).Scan(&total); err != nil {
		return nil, 0, err
	}
	limit := min(max(f.Limit, 1), 200)
	rows, err := s.db.QueryContext(ctx, `SELECT steam_id, name, verified, first_seen, last_seen, logins, xuid FROM players`+cond+
		` ORDER BY last_seen DESC, steam_id LIMIT ? OFFSET ?`, append(args, limit, max(f.Offset, 0))...)
	if err != nil {
		return nil, 0, err
	}
	list, err := scanPlayers(rows)
	return list, total, err
}

// idList is Steam IDs as a JSON array (json_each reads them back as integers).
func idList(ids []uint64) string {
	signed := make([]int64, len(ids))
	for i, id := range ids {
		signed[i] = int64(id)
	}
	b, _ := json.Marshal(signed)
	return string(b)
}

// PanelNamesOf is the names the database has for these Steam IDs (those it has).
func (s *Store) PanelNamesOf(ids []uint64) (map[uint64]string, error) {
	out := make(map[uint64]string, len(ids))
	if len(ids) == 0 {
		return out, nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), readTimeout)
	defer cancel()
	rows, err := s.db.QueryContext(ctx, `SELECT steam_id, name FROM players WHERE steam_id IN (SELECT value FROM json_each(?))`, idList(ids))
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	for rows.Next() {
		var id int64
		var name string
		if err := rows.Scan(&id, &name); err != nil {
			return nil, err
		}
		out[uint64(id)] = name
	}
	return out, rows.Err()
}

// NameUse is a name a player played matches under.
type NameUse struct {
	Name    string
	Last    time.Time // the newest match under it
	Matches int
}

// PlayerNames is the names a player's matches recorded, the one used last first (at most `count`).
func (s *Store) PlayerNames(steamID uint64, count int) ([]NameUse, error) {
	ctx, cancel := context.WithTimeout(context.Background(), readTimeout)
	defer cancel()
	rows, err := s.db.QueryContext(ctx, `SELECT mp.name, MAX(m.started), COUNT(*) FROM match_players mp
		JOIN matches m ON m.id = mp.match_id WHERE mp.steam_id = ? GROUP BY mp.name ORDER BY MAX(m.started) DESC LIMIT ?`,
		int64(steamID), min(max(count, 1), 100))
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []NameUse
	for rows.Next() {
		var u NameUse
		var last int64
		if err := rows.Scan(&u.Name, &last, &u.Matches); err != nil {
			return nil, err
		}
		u.Last = time.Unix(last, 0)
		out = append(out, u)
	}
	return out, rows.Err()
}

// MatchesSince counts the matches that started at `from` or later.
func (s *Store) MatchesSince(from time.Time) (int, error) {
	ctx, cancel := context.WithTimeout(context.Background(), readTimeout)
	defer cancel()
	var n int
	err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM matches WHERE started >= ?`, from.Unix()).Scan(&n)
	return n, err
}

// MatchFilter is a page of the match history.
type MatchFilter struct {
	Who    string // "" everyone; else a Steam ID, or a name (now, or the one played under; any case)
	Kind   string // "" any; KindPlayer or KindRanked
	Offset int
	Limit  int // 1..200
}

// PanelMatches is a page of the match history, newest first, and whether there are more after it.
func (s *Store) PanelMatches(f MatchFilter) ([]Match, bool, error) {
	ctx, cancel := context.WithTimeout(context.Background(), readTimeout)
	defer cancel()
	var where []string
	var args []any
	if f.Who != "" {
		if id, err := parseID(f.Who); err == nil {
			where = append(where, `id IN (SELECT match_id FROM match_players WHERE steam_id = ?)`)
			args = append(args, int64(id))
		} else {
			where = append(where, `id IN (SELECT match_id FROM match_players WHERE name = ? COLLATE NOCASE
				OR steam_id IN (SELECT steam_id FROM players WHERE name = ? COLLATE NOCASE))`)
			args = append(args, f.Who, f.Who)
		}
	}
	if f.Kind != "" {
		where = append(where, `kind = ?`)
		args = append(args, f.Kind)
	}
	cond := ""
	if len(where) > 0 {
		cond = " WHERE " + strings.Join(where, " AND ")
	}
	limit := min(max(f.Limit, 1), 200)
	q, err := s.db.QueryContext(ctx, `SELECT id, session_id, kind, started, ended, result FROM matches`+cond+
		` ORDER BY started DESC, id DESC LIMIT ? OFFSET ?`, append(args, limit+1, max(f.Offset, 0))...)
	if err != nil {
		return nil, false, err
	}
	defer q.Close()
	var out []Match
	at := map[int64]int{}
	for q.Next() {
		var m Match
		var session, started int64
		var ended sql.NullInt64
		if err := q.Scan(&m.ID, &session, &m.Kind, &started, &ended, &m.Result); err != nil {
			return nil, false, err
		}
		m.Session, m.Started = uint64(session), time.Unix(started, 0)
		if ended.Valid {
			m.Ended = time.Unix(ended.Int64, 0)
		}
		at[m.ID] = len(out)
		out = append(out, m)
	}
	if err := q.Err(); err != nil {
		return nil, false, err
	}
	q.Close()
	more := len(out) > limit
	if more {
		delete(at, out[limit].ID)
		out = out[:limit]
	}
	if len(out) == 0 {
		return out, false, nil
	}
	return out, more, s.matchPlayers(ctx, out, at)
}
