package store

import (
	"context"
	"database/sql"
	"errors"
	"strings"
	"time"
)

// The match history: one row per match the games report (MATCH_EVENT: started, ended - internal/hub/matches.go
// decides what is one match) or the server's ranked results settle, with its players: Steam ID, the profile name the
// server knew then, how many players sat at that PC (2 for pair play). The server sees no match traffic (the games
// play over Steam P2P), so a row is what the games said.

// Match kinds.
const (
	KindPlayer = "player" // a Player Match (or any match in a session that is not ranked)
	KindRanked = "ranked" // the session is arbitrated (XSESSION_CREATE flag 0x10)
)

// Match is one match with its players, the first to report it first.
type Match struct {
	ID      int64
	Session uint64
	Kind    string
	Started time.Time
	Ended   time.Time // zero: still open
	// Result: "ended" (a Player Match whose game said so), "unfinished" (nobody said it ended: closed after an hour, when
	// its players left, or when a new match began in the room); a ranked match's comes from its ranked results -
	// "agreed", "disagreed", "unconfirmed" (one game reported), "unreported" (none did) - and is "" until they settle
	Result  string
	Players []MatchPlayer
}

// MatchPlayer is one PC's player in a match.
type MatchPlayer struct {
	SteamID      uint64
	Name         string
	LocalPlayers int // players at that PC
	Won          int // 1 won, 0 lost, -1 not known
}

// RankedResult is a settled ranked match (internal/hub/stats.go): the room, its two players (host first) and their
// names as the server knows them now ("" = the database's), when the match registered and when it settled.
type RankedResult struct {
	Session             uint64
	Players             [2]uint64
	Names               [2]string
	Registered, Settled time.Time
	Result              string // agreed, disagreed, unconfirmed, unreported
}

// matchLongest: no match is open longer (the hub closes every match an hour after it opened) - so a ranked match's
// row started at most this long before the match registered.
const matchLongest = time.Hour

// OpenMatch adds an open match and returns its row id.
func (s *Store) OpenMatch(session uint64, kind string, at time.Time) (int64, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	r, err := s.db.ExecContext(ctx, `INSERT INTO matches (session_id, kind, started) VALUES (?, ?, ?)`, int64(session), kind, at.Unix())
	if err != nil {
		return 0, err
	}
	return r.LastInsertId()
}

// AddMatchPlayer adds a player to a match. `own`: the player's own game reported it, and its local players count
// (one another game named keeps the first count it got).
func (s *Store) AddMatchPlayer(match int64, steamID uint64, name string, localPlayers int, own bool) error {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	conflict := `DO NOTHING`
	if own {
		conflict = `DO UPDATE SET local_players = excluded.local_players`
	}
	_, err := s.db.ExecContext(ctx, `INSERT INTO match_players (match_id, steam_id, name, local_players) VALUES (?, ?, ?, ?)
		ON CONFLICT (match_id, steam_id) `+conflict, match, int64(steamID), name, localPlayers)
	return err
}

// EndMatch closes a match; `result` is kept only where it has none yet (a ranked match settled before its end keeps
// its own).
func (s *Store) EndMatch(match int64, at time.Time, result string) error {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	_, err := s.db.ExecContext(ctx, `UPDATE matches SET ended = COALESCE(ended, ?), result = CASE WHEN result = '' THEN ? ELSE result END
		WHERE id = ?`, at.Unix(), result, match)
	return err
}

// SettleRanked gives a ranked match its result: the newest row of that session with both players and no ranked
// result yet, else a new row from the pair (a game that sends no MATCH_EVENT).
//
// `won` stays NULL: the reports cannot tell the winner. Every property the game writes is a running total its own
// profile keeps (WINS_COUNT 0x10000010, LOSE 0x10000011, the rank: aggregation Last in its configuration), not this
// match's outcome, and MEASURED (2026-10-22, a ranked match played to the end through the server) the games wrote
// only RANK views (14, 15 + fighter), no RANKED MATCH WINS (77); a total's change against the row kept here would
// also count matches the player played elsewhere.
func (s *Store) SettleRanked(r RankedResult) error {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var id int64
	err = tx.QueryRowContext(ctx, `SELECT m.id FROM match_players a
		JOIN match_players b ON b.match_id = a.match_id AND b.steam_id = ?
		JOIN matches m ON m.id = a.match_id
		WHERE a.steam_id = ? AND m.session_id = ? AND m.started >= ? AND m.result NOT IN ('agreed', 'disagreed', 'unconfirmed', 'unreported')
		ORDER BY a.match_id DESC LIMIT 1`,
		int64(r.Players[1]), int64(r.Players[0]), int64(r.Session), r.Registered.Add(-matchLongest).Unix()).Scan(&id)
	switch {
	case err == nil:
		_, err = tx.ExecContext(ctx, `UPDATE matches SET kind = ?, result = ? WHERE id = ?`, KindRanked, r.Result, id)
	case errors.Is(err, sql.ErrNoRows):
		err = insertRanked(ctx, tx, r)
	}
	if err != nil {
		return err
	}
	return tx.Commit()
}

// insertRanked: a settled ranked match no game reported with MATCH_EVENT, from its registration to its settling.
func insertRanked(ctx context.Context, tx *sql.Tx, r RankedResult) error {
	res, err := tx.ExecContext(ctx, `INSERT INTO matches (session_id, kind, started, ended, result) VALUES (?, ?, ?, ?, ?)`,
		int64(r.Session), KindRanked, r.Registered.Unix(), r.Settled.Unix(), r.Result)
	if err != nil {
		return err
	}
	id, err := res.LastInsertId()
	if err != nil {
		return err
	}
	for i, p := range r.Players {
		if _, err := tx.ExecContext(ctx, `INSERT INTO match_players (match_id, steam_id, name) VALUES (?, ?,
			COALESCE(NULLIF(?, ''), (SELECT name FROM players WHERE steam_id = ?), 'PLAYER')) ON CONFLICT DO NOTHING`,
			id, int64(p), r.Names[i], int64(p)); err != nil {
			return err
		}
	}
	return nil
}

// CloseOpenMatches closes the matches still open ("unfinished", ended at `at` or an hour after they started,
// whichever is first): at start-up those a stopped server left open, at a clean stop the ones in play.
func (s *Store) CloseOpenMatches(at time.Time) (int64, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	r, err := s.db.ExecContext(ctx, `UPDATE matches SET ended = MIN(?, started + ?),
		result = CASE WHEN result = '' THEN 'unfinished' ELSE result END WHERE ended IS NULL`, at.Unix(), int64(matchLongest/time.Second))
	if err != nil {
		return 0, err
	}
	return r.RowsAffected()
}

// PruneMatches forgets the matches that started before `before` (with their players); a few thousand at a time, so
// a large first prune never holds the database for long.
func (s *Store) PruneMatches(before time.Time) (int64, error) {
	var n int64
	for {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		r, err := s.db.ExecContext(ctx, `DELETE FROM matches WHERE id IN (SELECT id FROM matches WHERE started < ? LIMIT 5000)`, before.Unix())
		cancel()
		if err != nil {
			return n, err
		}
		k, _ := r.RowsAffected()
		if n += k; k < 5000 {
			return n, nil
		}
	}
}

// RecentMatches is the newest `count` matches (at most 200): every one (who ""), or those of a player by Steam ID, by
// the name they have now or by the name they played that match under (any case) - a report names the cheater as the
// game showed them then, and players rename their profiles.
func (s *Store) RecentMatches(who string, count int) ([]Match, error) {
	ctx, cancel := context.WithTimeout(context.Background(), readTimeout)
	defer cancel()
	count = min(max(count, 1), 200)
	const cols = `SELECT id, session_id, kind, started, ended, result FROM matches `
	const order = ` ORDER BY started DESC, id DESC LIMIT ?`
	var q *sql.Rows
	var err error
	if id, perr := parseID(who); who == "" {
		q, err = s.db.QueryContext(ctx, cols+order, count)
	} else if perr == nil {
		q, err = s.db.QueryContext(ctx, cols+`WHERE id IN (SELECT match_id FROM match_players WHERE steam_id = ?)`+order, int64(id), count)
	} else {
		q, err = s.db.QueryContext(ctx, cols+`WHERE id IN (SELECT match_id FROM match_players WHERE name = ? COLLATE NOCASE
			OR steam_id IN (SELECT steam_id FROM players WHERE name = ? COLLATE NOCASE))`+order, who, who, count)
	}
	if err != nil {
		return nil, err
	}
	defer q.Close()
	var out []Match
	at := map[int64]int{}
	for q.Next() {
		var m Match
		var session, started int64
		var ended sql.NullInt64
		if err := q.Scan(&m.ID, &session, &m.Kind, &started, &ended, &m.Result); err != nil {
			return nil, err
		}
		m.Session, m.Started = uint64(session), time.Unix(started, 0)
		if ended.Valid {
			m.Ended = time.Unix(ended.Int64, 0)
		}
		at[m.ID] = len(out)
		out = append(out, m)
	}
	if err := q.Err(); err != nil || len(out) == 0 {
		return out, err
	}
	return out, s.matchPlayers(ctx, out, at)
}

// matchPlayers fills in the matches' players (at: match id -> index in ms), in the order they joined.
func (s *Store) matchPlayers(ctx context.Context, ms []Match, at map[int64]int) error {
	args := make([]any, 0, len(ms))
	for _, m := range ms {
		args = append(args, m.ID)
	}
	q, err := s.db.QueryContext(ctx, `SELECT match_id, steam_id, name, local_players, won FROM match_players
		WHERE match_id IN (?`+strings.Repeat(",?", len(args)-1)+`) ORDER BY match_id, rowid`, args...)
	if err != nil {
		return err
	}
	defer q.Close()
	for q.Next() {
		var match, id int64
		var p MatchPlayer
		var won sql.NullInt64
		if err := q.Scan(&match, &id, &p.Name, &p.LocalPlayers, &won); err != nil {
			return err
		}
		p.SteamID, p.Won = uint64(id), -1
		if won.Valid {
			p.Won = int(won.Int64)
		}
		i := at[match]
		ms[i].Players = append(ms[i].Players, p)
	}
	return q.Err()
}
