package wire

import (
	"bytes"
	"testing"
)

func TestValuesRoundTrip(t *testing.T) {
	var w Writer
	w.U8(7)
	w.U16(0xBEEF)
	w.U32(0xDEADBEEF)
	w.U64(0x0110000100000001)
	w.Str("TEKKEN ARABIA")
	w.Blob([]byte{1, 2, 3})
	r := Reader{B: w.B}
	if r.U8() != 7 || r.U16() != 0xBEEF || r.U32() != 0xDEADBEEF || r.U64() != 0x0110000100000001 {
		t.Fatal("integers")
	}
	if r.Str() != "TEKKEN ARABIA" || !bytes.Equal(r.Blob(), []byte{1, 2, 3}) || r.Err() != nil || len(r.B) != 0 {
		t.Fatal("string / blob")
	}
}

func TestShortReads(t *testing.T) {
	r := Reader{B: []byte{5, 0, 'a'}}
	if r.Str() != "" || r.Err() == nil {
		t.Fatal("a string longer than the buffer must fail")
	}
	r = Reader{B: []byte{0xFF, 0xFF, 0xFF, 0x7F}}
	if r.Blob() != nil || r.Err() == nil {
		t.Fatal("a huge blob length must fail without allocating")
	}
}

func TestFrames(t *testing.T) {
	var buf bytes.Buffer
	if err := WriteFrame(&buf, Ping, []byte{1, 2}); err != nil {
		t.Fatal(err)
	}
	if err := WriteFrame(&buf, Pong, nil); err != nil {
		t.Fatal(err)
	}
	typ, p, err := ReadFrame(&buf)
	if err != nil || typ != Ping || !bytes.Equal(p, []byte{1, 2}) {
		t.Fatal("first frame", typ, p, err)
	}
	typ, p, err = ReadFrame(&buf)
	if err != nil || typ != Pong || len(p) != 0 {
		t.Fatal("empty frame", typ, p, err)
	}
	bad := bytes.NewReader([]byte{0, 0, 0, 0, 1})
	if _, _, err := ReadFrame(bad); err != ErrFrameSize {
		t.Fatal("a zero size must be refused")
	}
	huge := bytes.NewReader([]byte{0xFF, 0xFF, 0xFF, 0xFF, 1})
	if _, _, err := ReadFrame(huge); err != ErrFrameSize {
		t.Fatal("an oversized frame must be refused")
	}
}

func FuzzReadFrame(f *testing.F) {
	var buf bytes.Buffer
	WriteFrame(&buf, Hello, []byte{1, 0, 3, 0, 'a', 'b', 'c'})
	f.Add(buf.Bytes())
	f.Add([]byte{0xFF, 0xFF, 0xFF, 0xFF, 1})
	f.Fuzz(func(t *testing.T, b []byte) {
		typ, p, err := ReadFrameMax(bytes.NewReader(b), 16<<10)
		if err == nil && len(p)+1 > 16<<10 {
			t.Fatalf("frame of %d bytes accepted (type %d)", len(p), typ)
		}
		r := Reader{B: p}
		r.U16()
		r.Str()
		r.Blob()
		r.Bytes(int(r.U32()))
		r.Rest()
	})
}
