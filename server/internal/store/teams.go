package store

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"strconv"
	"strings"
	"time"
)

// Teams: the game only shows them (TEAM in ONLINE MODE, GIN /team); Namco made them on its website. Here the
// server's admin makes them (console `team ...`): a team has a name, a leader and up to 100 members, a player is in
// one team at most.
const MaxTeamMembers = 100

// Team is one team as the game shows it.
type Team struct {
	ID      int64
	Name    string
	Leader  uint64 // Steam ID
	Members []TeamMember
	Created time.Time
}

// TeamMember is a member with what the game shows of them.
type TeamMember struct {
	SteamID uint64
	XUID    uint64 // 0: not known yet
	Name    string
	// the member's best fighter (the profile its game uploads): fighter id and rank, -1 = none
	BestFighter, BestRank int
	Wins                  int // ranked wins over all fighters
}

var ErrNoTeam = errors.New("no such team")

// CreateTeam makes a team led by a player the server has seen (who joins it, leaving any other).
func (s *Store) CreateTeam(name string, leader uint64, at time.Time) (int64, error) {
	name = strings.TrimSpace(name)
	if name == "" || len(name) > 32 {
		return 0, errors.New("a team name is 1..32 characters")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return 0, err
	}
	defer tx.Rollback()
	r, err := tx.ExecContext(ctx, `INSERT INTO teams (name, leader, created) VALUES (?, ?, ?)`, name, int64(leader), at.Unix())
	if err != nil {
		if strings.Contains(err.Error(), "UNIQUE") {
			return 0, fmt.Errorf("a team named %q exists", name)
		}
		if strings.Contains(err.Error(), "FOREIGN KEY") {
			return 0, errors.New("the leader has never played on this server")
		}
		return 0, err
	}
	id, _ := r.LastInsertId()
	if _, err := tx.ExecContext(ctx, `INSERT INTO team_members (steam_id, team_id, joined) VALUES (?, ?, ?)
		ON CONFLICT (steam_id) DO UPDATE SET team_id = excluded.team_id, joined = excluded.joined`, int64(leader), id, at.Unix()); err != nil {
		return 0, err
	}
	return id, tx.Commit()
}

// TeamID is the team with that name (any case) or number.
func (s *Store) TeamID(nameOrID string) (int64, error) {
	var id int64
	err := s.db.QueryRow(`SELECT id FROM teams WHERE name = ? COLLATE NOCASE OR CAST(id AS TEXT) = ?`, nameOrID, nameOrID).Scan(&id)
	if errors.Is(err, sql.ErrNoRows) {
		return 0, ErrNoTeam
	}
	return id, err
}

// JoinTeam puts a player the server has seen in a team (leaving any other); a full team refuses.
func (s *Store) JoinTeam(team int64, steamID uint64, at time.Time) error {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var n int
	if err := tx.QueryRowContext(ctx, `SELECT COUNT(*) FROM team_members WHERE team_id = ? AND steam_id != ?`, team, int64(steamID)).Scan(&n); err != nil {
		return err
	}
	if n >= MaxTeamMembers {
		return fmt.Errorf("the team has %d members already", n)
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO team_members (steam_id, team_id, joined) VALUES (?, ?, ?)
		ON CONFLICT (steam_id) DO UPDATE SET team_id = excluded.team_id, joined = excluded.joined`, int64(steamID), team, at.Unix()); err != nil {
		if strings.Contains(err.Error(), "FOREIGN KEY") {
			return errors.New("no such team, or the player has never played on this server")
		}
		return err
	}
	return tx.Commit()
}

// LeaveTeam takes a player out of their team (a leader leaving hands the team to its longest member; the last
// one leaving ends it).
func (s *Store) LeaveTeam(steamID uint64) error {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var team int64
	if err := tx.QueryRowContext(ctx, `SELECT team_id FROM team_members WHERE steam_id = ?`, int64(steamID)).Scan(&team); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return errors.New("not in a team")
		}
		return err
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM team_members WHERE steam_id = ?`, int64(steamID)); err != nil {
		return err
	}
	var next sql.NullInt64
	tx.QueryRowContext(ctx, `SELECT steam_id FROM team_members WHERE team_id = ? ORDER BY joined, steam_id LIMIT 1`, team).Scan(&next)
	if !next.Valid {
		if _, err := tx.ExecContext(ctx, `DELETE FROM teams WHERE id = ?`, team); err != nil {
			return err
		}
	} else if _, err := tx.ExecContext(ctx, `UPDATE teams SET leader = ? WHERE id = ? AND leader = ?`, next.Int64, team, int64(steamID)); err != nil {
		return err
	}
	return tx.Commit()
}

// DeleteTeam ends a team (its members are in none).
func (s *Store) DeleteTeam(team int64) error {
	r, err := s.db.Exec(`DELETE FROM teams WHERE id = ?`, team)
	if err != nil {
		return err
	}
	if n, _ := r.RowsAffected(); n == 0 {
		return ErrNoTeam
	}
	return nil
}

// Teams lists the teams (id, name, members), largest first.
func (s *Store) Teams() ([]Team, error) {
	q, err := s.db.Query(`SELECT t.id, t.name, t.leader, t.created, COUNT(m.steam_id) FROM teams t LEFT JOIN team_members m ON m.team_id = t.id
		GROUP BY t.id ORDER BY COUNT(m.steam_id) DESC, t.name`)
	if err != nil {
		return nil, err
	}
	defer q.Close()
	var out []Team
	for q.Next() {
		var t Team
		var leader, created int64
		var n int
		if err := q.Scan(&t.ID, &t.Name, &leader, &created, &n); err != nil {
			return nil, err
		}
		t.Leader, t.Created = uint64(leader), time.Unix(created, 0)
		t.Members = make([]TeamMember, n)
		out = append(out, t)
	}
	return out, q.Err()
}

// TeamOf is a player's team with its members (false: in none).
func (s *Store) TeamOf(steamID uint64) (Team, bool, error) {
	ctx, cancel := context.WithTimeout(context.Background(), readTimeout)
	defer cancel()
	var t Team
	var leader, created int64
	err := s.db.QueryRowContext(ctx, `SELECT t.id, t.name, t.leader, t.created FROM teams t JOIN team_members m ON m.team_id = t.id
		WHERE m.steam_id = ?`, int64(steamID)).Scan(&t.ID, &t.Name, &leader, &created)
	if errors.Is(err, sql.ErrNoRows) {
		return Team{}, false, nil
	}
	if err != nil {
		return Team{}, false, err
	}
	t.Leader, t.Created = uint64(leader), time.Unix(created, 0)
	q, err := s.db.QueryContext(ctx, `SELECT p.steam_id, p.xuid, p.name, f.body FROM team_members m JOIN players p USING (steam_id)
		LEFT JOIN profiles f USING (steam_id) WHERE m.team_id = ? ORDER BY m.joined, m.steam_id`, t.ID)
	if err != nil {
		return Team{}, false, err
	}
	defer q.Close()
	for q.Next() {
		var m TeamMember
		var sid int64
		var xuid sql.NullInt64
		var body sql.NullString
		if err := q.Scan(&sid, &xuid, &m.Name, &body); err != nil {
			return Team{}, false, err
		}
		m.SteamID, m.XUID = uint64(sid), uint64(xuid.Int64)
		m.BestFighter, m.BestRank, m.Wins = bestOf(body.String)
		t.Members = append(t.Members, m)
	}
	return t, true, q.Err()
}

// bestOf reads a profile the game uploaded (GIN /player: payload fighter-N-rank / -wins): the best ranked fighter
// and the ranked wins over all fighters.
func bestOf(profile string) (fighter, rank, wins int) {
	fighter, rank = -1, -1
	if profile == "" {
		return
	}
	var doc struct {
		Payload map[string]any `json:"payload"`
	}
	if json.Unmarshal([]byte(profile), &doc) != nil {
		return
	}
	num := func(v any) int {
		switch x := v.(type) {
		case string:
			n, _ := strconv.Atoi(x)
			return n
		case float64:
			return int(x)
		}
		return 0
	}
	for f := 0; f < 64; f++ {
		r, ok := doc.Payload["fighter-"+strconv.Itoa(f)+"-rank"]
		if !ok {
			continue
		}
		if n := num(r); n > rank {
			fighter, rank = f, n
		}
		wins += num(doc.Payload["fighter-"+strconv.Itoa(f)+"-wins"])
	}
	return
}
