package stats

import (
	"math"
	"testing"
	"time"
)

func i32(v int32) Value { return Value{TypeInt32, uint64(int64(v))} }
func i64(v int64) Value { return Value{TypeInt64, uint64(v)} }

const (
	rating   = 0x20000001
	mainChar = 0x1000000E
	wins     = 0x10000010
)

func TestViewsFromTheGame(t *testing.T) {
	// the boards the game's LEADERBOARDS menu reads (MEASURED), and what its configuration says about them
	for _, id := range []uint32{9, 14, 15, 73, 77, 152, 228, 230, 234} {
		v, ok := Lookup(id)
		if !ok || v.Rating.Property != rating || len(v.Columns) != 8 {
			t.Fatalf("view %d: %+v", id, v)
		}
	}
	if Count() != 85 {
		t.Fatalf("%d views", Count())
	}
	bp, _ := Lookup(9)
	rank, _ := Lookup(14)
	survival, _ := Lookup(230)
	arena, _ := Lookup(152)
	if bp.Rating.Keep != Max || rank.Rating.Keep != Last || !bp.Arbitrated || !rank.Arbitrated || survival.Arbitrated ||
		arena.Reset != ResetMonthly || bp.Reset != ResetNever {
		t.Fatalf("bp %+v rank %+v survival %+v arena %+v", bp, rank, survival, arena)
	}
	if _, ok := Lookup(1); ok {
		t.Fatal("a master view is not a board")
	}
}

func TestApplyKeepsTheWayTheViewSays(t *testing.T) {
	bp, _ := Lookup(9)
	row, err := bp.Apply(nil, []Property{{rating, i64(500)}, {mainChar, i32(7)}, {wins, i32(3)}, {0x10000099, i32(1)}})
	if err != nil || row.Rating != 500 || row.Columns[1] != i32(7) || row.Columns[5] != i32(3) || len(row.Columns) != 2 {
		t.Fatalf("%+v %v", row, err)
	}
	// Max rating: a lower one leaves it; columns are the newest write
	row, err = bp.Apply(&row, []Property{{rating, i64(300)}, {mainChar, i32(9)}})
	if err != nil || row.Rating != 500 || row.Columns[1] != i32(9) || row.Columns[5] != i32(3) {
		t.Fatalf("%+v %v", row, err)
	}
	row, _ = bp.Apply(&row, []Property{{rating, i64(800)}})
	if row.Rating != 800 {
		t.Fatalf("%+v", row)
	}
	// Last rating (rank views): it follows the newest write, down too
	rank, _ := Lookup(14)
	r, _ := rank.Apply(nil, []Property{{rating, i64(12)}})
	r, _ = rank.Apply(&r, []Property{{rating, i64(9)}})
	if r.Rating != 9 {
		t.Fatalf("%+v", r)
	}
	// a negative rating
	r, err = rank.Apply(nil, []Property{{rating, i64(-4)}})
	if err != nil || r.Rating != -4 {
		t.Fatalf("%+v %v", r, err)
	}
}

func TestBadValuesRefused(t *testing.T) {
	bp, _ := Lookup(9)
	for _, p := range []Property{
		{mainChar, Value{TypeInt64, 1}},       // type does not match the property id
		{mainChar, Value{TypeInt32, 1 << 40}}, // not an int32
		{0x40000001, Value{4, 0}},             // text
		{0x30000001, Value{TypeDouble, math.Float64bits(math.NaN())}},
		{0x50000001, Value{TypeFloat, 1 << 33}},
	} {
		if _, err := bp.Apply(nil, []Property{p}); err == nil {
			t.Errorf("accepted %+v", p)
		}
	}
	many := make([]Property, MaxPropsPerView+1)
	for i := range many {
		many[i] = Property{mainChar, i32(1)}
	}
	if _, err := bp.Apply(nil, many); err == nil {
		t.Error("accepted too many properties")
	}
}

func TestKeep(t *testing.T) {
	old := i32(5)
	if keep(Max, &old, i32(3)) != i32(5) || keep(Max, &old, i32(8)) != i32(8) || keep(Min, &old, i32(3)) != i32(3) ||
		keep(Min, &old, i32(8)) != i32(5) || keep(Sum, &old, i32(8)) != i32(13) || keep(Last, &old, i32(1)) != i32(1) {
		t.Fatal("keep")
	}
	big := i32(math.MaxInt32)
	if keep(Sum, &big, i32(10)) != i32(math.MaxInt32) {
		t.Fatal("int32 sum must saturate")
	}
	d := Value{TypeDouble, math.Float64bits(1.5)}
	if got := keep(Sum, &d, Value{TypeDouble, math.Float64bits(2)}); math.Float64frombits(got.Bits) != 3.5 {
		t.Fatal("double sum")
	}
}

func TestPeriod(t *testing.T) {
	arena, _ := Lookup(152)
	bp, _ := Lookup(9)
	at := time.Date(2026, 10, 31, 23, 30, 0, 0, time.FixedZone("x", -3*3600)) // already November in UTC
	if arena.Period(at) != 202611 || bp.Period(at) != 0 {
		t.Fatal(arena.Period(at), bp.Period(at))
	}
}

func TestColumnsRoundTrip(t *testing.T) {
	cols := map[uint16]Value{1: i32(-7), 5: i32(3), 300: Value{TypeDouble, math.Float64bits(2.25)}}
	got, err := DecodeColumns(EncodeColumns(cols))
	if err != nil || len(got) != 3 || got[1] != cols[1] || got[300] != cols[300] {
		t.Fatalf("%v %v", got, err)
	}
	if _, err := DecodeColumns([]byte{1, 2, 3}); err == nil {
		t.Fatal("damaged columns accepted")
	}
}
