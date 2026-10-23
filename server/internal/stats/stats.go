// Package stats is the game's Xbox LIVE statistics on a community server: the leaderboards ("views") the game's
// own configuration defines (views.go, generated from it), each column keeping its value the way that
// configuration says - the newest write, or the best one ever.
//
// A view row is one player's entry: the rating the board is sorted by (highest first) and up to eight columns
// (the fighters played, their ranks, wins and losses). The game writes properties (XSessionWriteStats); each
// column takes the property it is bound to.
package stats

import (
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"time"
)

// Keep is how a column keeps its value across writes (the configuration's aggregation).
type Keep uint8

const (
	Last Keep = iota // the newest write
	Max              // the highest ever written
	Min              // the lowest ever written
	Sum              // every write added up
)

// Reset is when a view starts again from empty.
type Reset uint8

const (
	ResetNever   Reset = iota
	ResetMonthly       // a new board every calendar month (UTC): World Arena
)

// Column binds a column id (what the game asks for; 0 for the rating) to the property written into it.
type Column struct {
	ID       uint16
	Property uint32
	Keep     Keep
}

// View is one leaderboard.
type View struct {
	ID         uint32
	Arbitrated bool // ranked results: kept only from a ranked match the server saw both players register for
	Reset      Reset
	Rating     Column
	Columns    []Column
}

// Data types (XUSER_DATA); the high nibble of a property id is its type.
const (
	TypeInt32    = 1
	TypeInt64    = 2
	TypeDouble   = 3
	TypeFloat    = 5
	TypeDateTime = 7
	TypeNull     = 0xFF // a column with nothing written
)

// Value is one XUSER_DATA. Bits: integers sign-extended to 64 bits, a double's IEEE bits, a float's in the low half.
type Value struct {
	Type uint8
	Bits uint64
}

// Property is one written property.
type Property struct {
	ID    uint32
	Value Value
}

// Write is what one XSessionWriteStats view entry carries.
type Write struct {
	View  uint32
	Props []Property
}

// Row is one player's entry in one view.
type Row struct {
	Rating  int64
	Columns map[uint16]Value
}

// Entry is one player's row on a board, with its place.
type Entry struct {
	Rank    int64 // 1 = first
	SteamID uint64
	XUID    uint64 // the online XUID the player's game uses (0: not known)
	Name    string
	Row
}

// Limits on what a game may send.
const (
	MaxViewsPerWrite = 16
	MaxPropsPerView  = 32
	MaxColumns       = 64
	MaxRows          = 100 // one page of a board
)

// Lookup is the view with that id.
func Lookup(id uint32) (View, bool) {
	v, ok := views[id]
	return v, ok
}

// Count is the number of views the game defines.
func Count() int { return len(views) }

// Period is the board a write at `at` lands on: 0 for views that never reset, yyyymm for monthly ones.
func (v View) Period(at time.Time) int64 {
	if v.Reset == ResetMonthly {
		at = at.UTC()
		return int64(at.Year())*100 + int64(at.Month())
	}
	return 0
}

func numeric(t uint8) bool {
	return t == TypeInt32 || t == TypeInt64 || t == TypeDouble || t == TypeFloat || t == TypeDateTime
}

// Check refuses values the game would never write: a type that does not match the property's id, a non-numeric
// type, or an int32 / float that does not fit its width.
func (p Property) Check() error {
	t := uint8(p.ID >> 28)
	if p.Value.Type != t {
		return fmt.Errorf("property %08X written as type %d", p.ID, p.Value.Type)
	}
	if !numeric(t) {
		return fmt.Errorf("property %08X: type %d is not a statistic", p.ID, t)
	}
	switch t {
	case TypeInt32:
		if int64(p.Value.Bits) != int64(int32(p.Value.Bits)) {
			return fmt.Errorf("property %08X: not an int32", p.ID)
		}
	case TypeFloat:
		if p.Value.Bits>>32 != 0 {
			return fmt.Errorf("property %08X: not a float", p.ID)
		}
	case TypeDouble:
		if f := math.Float64frombits(p.Value.Bits); math.IsNaN(f) || math.IsInf(f, 0) {
			return fmt.Errorf("property %08X: not a number", p.ID)
		}
	}
	return nil
}

func (v Value) float() float64 {
	switch v.Type {
	case TypeDouble:
		return math.Float64frombits(v.Bits)
	case TypeFloat:
		return float64(math.Float32frombits(uint32(v.Bits)))
	}
	return float64(int64(v.Bits))
}

// less compares two values of the same type.
func less(a, b Value) bool {
	if a.Type == TypeDouble || a.Type == TypeFloat {
		return a.float() < b.float()
	}
	return int64(a.Bits) < int64(b.Bits)
}

func keep(k Keep, old *Value, new Value) Value {
	if old == nil || old.Type != new.Type {
		return new
	}
	switch k {
	case Max:
		if less(new, *old) {
			return *old
		}
	case Min:
		if less(*old, new) {
			return *old
		}
	case Sum:
		if new.Type == TypeDouble {
			return Value{TypeDouble, math.Float64bits(old.float() + new.float())}
		}
		if new.Type == TypeFloat {
			return Value{TypeFloat, uint64(math.Float32bits(float32(old.float() + new.float())))}
		}
		s := int64(old.Bits) + int64(new.Bits)
		if new.Type == TypeInt32 {
			s = int64(int32(max(math.MinInt32, min(math.MaxInt32, s))))
		}
		return Value{new.Type, uint64(s)}
	}
	return new
}

// Apply merges written properties into a player's row (old nil: no entry yet). Properties the view has no column
// for are ignored, as Xbox LIVE did; a rating that is not an integer is refused.
func (v View) Apply(old *Row, props []Property) (Row, error) {
	if len(props) > MaxPropsPerView {
		return Row{}, errors.New("too many properties")
	}
	out := Row{Columns: map[uint16]Value{}}
	var oldRating *Value
	if old != nil {
		out.Rating = old.Rating
		for id, val := range old.Columns {
			out.Columns[id] = val
		}
		oldRating = &Value{TypeInt64, uint64(old.Rating)}
	}
	for _, p := range props {
		if err := p.Check(); err != nil {
			return Row{}, err
		}
		if p.ID == v.Rating.Property {
			val := p.Value
			if val.Type != TypeInt64 && val.Type != TypeInt32 {
				return Row{}, fmt.Errorf("view %d: rating of type %d", v.ID, val.Type)
			}
			val.Type = TypeInt64
			if r := int64(val.Bits); r > math.MaxInt32 || r < math.MinInt32 {
				return Row{}, fmt.Errorf("view %d: rating %d out of the game's range", v.ID, r) // the game writes small numbers
			}
			out.Rating = int64(keep(v.Rating.Keep, oldRating, val).Bits)
			continue
		}
		for _, c := range v.Columns {
			if c.Property != p.ID {
				continue
			}
			var prev *Value
			if pv, ok := out.Columns[c.ID]; ok {
				prev = &pv
			}
			out.Columns[c.ID] = keep(c.Keep, prev, p.Value)
		}
	}
	return out, nil
}

// EncodeColumns / DecodeColumns: a row's columns as stored (u16 id, u8 type, u64 bits each, little-endian).
func EncodeColumns(cols map[uint16]Value) []byte {
	b := make([]byte, 0, len(cols)*11)
	for id, v := range cols {
		b = binary.LittleEndian.AppendUint16(b, id)
		b = append(b, v.Type)
		b = binary.LittleEndian.AppendUint64(b, v.Bits)
	}
	return b
}

func DecodeColumns(b []byte) (map[uint16]Value, error) {
	if len(b)%11 != 0 || len(b) > MaxColumns*11 {
		return nil, errors.New("damaged columns")
	}
	out := make(map[uint16]Value, len(b)/11)
	for ; len(b) > 0; b = b[11:] {
		out[binary.LittleEndian.Uint16(b)] = Value{b[2], binary.LittleEndian.Uint64(b[3:])}
	}
	return out, nil
}
