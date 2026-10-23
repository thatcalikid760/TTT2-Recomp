package record

import (
	"encoding/binary"
	"testing"

	"ttt2-server/internal/wire"
)

const host = 0x0110000100000042

// Build makes a record like the game's (matchmaking.cpp Serialize).
func build(id, hostPeer, infoID, infoHost uint64, ctx, props int, propSize int, netcode bool) []byte {
	var w wire.Writer
	w.U32(Magic)
	w.U64(id)
	w.U64(hostPeer)
	w.U64(0x0009000000000001)
	w.U32(0x32F)
	w.U32(2)
	w.U32(0)
	w.U32(1)
	w.U32(0)
	info := make([]byte, infoSize)
	binary.BigEndian.PutUint64(info[0:], infoID)
	binary.BigEndian.PutUint64(info[infoHostOff:], infoHost)
	w.Raw(info)
	w.U32(uint32(ctx))
	for i := 0; i < ctx; i++ {
		w.U32(0x800A)
		w.U32(1)
	}
	w.U32(uint32(props))
	for i := 0; i < props; i++ {
		w.U32(0x10000002)
		w.U32(uint32(propSize))
		w.Raw(make([]byte, propSize))
	}
	if netcode {
		w.U32(0x80000001)
	}
	return w.B
}

func TestGood(t *testing.T) {
	for _, nc := range []bool{false, true} {
		s, err := Check(build(0x8000AAAA, host, 0x8000AAAA, host, 4, 7, 4, nc), host)
		if err != nil || s.ID != 0x8000AAAA || s.Host != host || s.Filled != 1 || s.Max != 2 {
			t.Fatal(s, err)
		}
	}
}

func TestBad(t *testing.T) {
	cases := map[string][]byte{
		"other sender":      build(0x8000AAAA, host+1, 0x8000AAAA, host+1, 1, 1, 4, false),
		"info id":           build(0x8000AAAA, host, 0x8000BBBB, host, 1, 1, 4, false),
		"info host":         build(0x8000AAAA, host, 0x8000AAAA, host+1, 1, 1, 4, false),
		"too many contexts": build(0x8000AAAA, host, 0x8000AAAA, host, 65, 1, 4, false),
		"value too large":   build(0x8000AAAA, host, 0x8000AAAA, host, 1, 1, 4097, false),
		"zero id":           build(0, host, 0, host, 1, 1, 4, false),
	}
	trailing := append(build(0x8000AAAA, host, 0x8000AAAA, host, 1, 1, 4, true), 1, 2)
	cases["trailing bytes"] = trailing
	cases["truncated"] = build(0x8000AAAA, host, 0x8000AAAA, host, 2, 2, 8, false)[:120]
	for name, rec := range cases {
		if _, err := Check(rec, host); err == nil {
			t.Errorf("%s accepted", name)
		}
	}
}

func FuzzCheck(f *testing.F) {
	f.Add(build(0x8000AAAA, host, 0x8000AAAA, host, 4, 7, 4, true))
	f.Add([]byte{})
	f.Fuzz(func(t *testing.T, b []byte) {
		s, err := Check(b, host)
		if err == nil && (s.Host != host || s.ID == 0 || len(b) > MaxSize) {
			t.Fatalf("accepted an invalid record: %+v", s)
		}
	})
}
