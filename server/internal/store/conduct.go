package store

import (
	"context"
	"database/sql"
	"errors"
	"time"
)

// Ranked conduct (PROTOCOL.md "Ranked conduct"): the ranked matches each player played and the ones they left before
// the end (a rage quit), for the quit rate other players see and the ranked cooldown a quit costs.

// Conduct is one player's ranked record of quits.
type Conduct struct {
	Matches, Quits int
	Strikes        int       // recent quits (the hub forgives them over time)
	LastQuit       time.Time // zero: never
	CooldownUntil  time.Time // zero: none
}

// unixOrZero is a stored unix time (0: none).
func unixOrZero(v int64) time.Time {
	if v == 0 {
		return time.Time{}
	}
	return time.Unix(v, 0)
}

func zeroOrUnix(t time.Time) int64 {
	if t.IsZero() {
		return 0
	}
	return t.Unix()
}

// Conduct is a player's record (all zero for one never seen in a ranked match).
func (s *Store) Conduct(steamID uint64) (Conduct, error) {
	ctx, cancel := context.WithTimeout(context.Background(), readTimeout)
	defer cancel()
	var c Conduct
	var last, until int64
	err := s.db.QueryRowContext(ctx, `SELECT matches, quits, strikes, last_quit, cooldown_until FROM ranked_conduct WHERE steam_id = ?`,
		int64(steamID)).Scan(&c.Matches, &c.Quits, &c.Strikes, &last, &until)
	if errors.Is(err, sql.ErrNoRows) {
		return Conduct{}, nil
	}
	if err != nil {
		return Conduct{}, err
	}
	c.LastQuit, c.CooldownUntil = unixOrZero(last), unixOrZero(until)
	return c, nil
}

// RankedPlayed counts a ranked match for a player.
func (s *Store) RankedPlayed(steamID uint64) error {
	ctx, cancel := context.WithTimeout(context.Background(), readTimeout)
	defer cancel()
	_, err := s.db.ExecContext(ctx, `INSERT INTO ranked_conduct (steam_id, matches) VALUES (?, 1)
		ON CONFLICT (steam_id) DO UPDATE SET matches = matches + 1`, int64(steamID))
	return err
}

// RankedQuit counts a quit: `penalty` gets the player's record before it and gives the strikes and the cooldown's end
// after it. Returns the record after.
func (s *Store) RankedQuit(steamID uint64, at time.Time, penalty func(before Conduct) (strikes int, until time.Time)) (Conduct, error) {
	ctx, cancel := context.WithTimeout(context.Background(), readTimeout)
	defer cancel()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Conduct{}, err
	}
	defer tx.Rollback()
	var c Conduct
	var last, until int64
	err = tx.QueryRowContext(ctx, `SELECT matches, quits, strikes, last_quit, cooldown_until FROM ranked_conduct WHERE steam_id = ?`,
		int64(steamID)).Scan(&c.Matches, &c.Quits, &c.Strikes, &last, &until)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return Conduct{}, err
	}
	c.LastQuit, c.CooldownUntil = unixOrZero(last), unixOrZero(until)
	strikes, end := penalty(c)
	c.Quits++
	c.Strikes = strikes
	c.LastQuit = at
	if end.After(c.CooldownUntil) {
		c.CooldownUntil = end
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO ranked_conduct (steam_id, matches, quits, strikes, last_quit, cooldown_until)
		VALUES (?, ?, ?, ?, ?, ?)
		ON CONFLICT (steam_id) DO UPDATE SET quits = excluded.quits, strikes = excluded.strikes, last_quit = excluded.last_quit,
			cooldown_until = excluded.cooldown_until`,
		int64(steamID), c.Matches, c.Quits, c.Strikes, c.LastQuit.Unix(), zeroOrUnix(c.CooldownUntil)); err != nil {
		return Conduct{}, err
	}
	return c, tx.Commit()
}

// ClearCooldown ends a player's ranked cooldown and forgives their strikes (the admin's `pardon`).
func (s *Store) ClearCooldown(steamID uint64) error {
	ctx, cancel := context.WithTimeout(context.Background(), readTimeout)
	defer cancel()
	_, err := s.db.ExecContext(ctx, `UPDATE ranked_conduct SET strikes = 0, cooldown_until = 0 WHERE steam_id = ?`, int64(steamID))
	return err
}
