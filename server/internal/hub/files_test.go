package hub

import (
	"bytes"
	"path/filepath"
	"testing"
	"time"

	"ttt2-server/internal/store"
	"ttt2-server/internal/wire"
)

func startFiles(t *testing.T) *server {
	t.Helper()
	db, err := store.Open(filepath.Join(t.TempDir(), "server.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	return startSetup(t, trusting(), nil, func(h *Hub) {
		h.SetRecords(db)
		h.SetStats(db)
		h.SetFiles(db)
	})
}

func (cl *client) putFile(own uint64, item string, data []byte) {
	cl.send(wire.FilePut, func(w *wire.Writer) {
		w.U64(own)
		w.Str(item)
		w.Blob(data)
	})
}

func (cl *client) getFile(t *testing.T, request uint32, owner uint64, item string) (uint8, []byte) {
	t.Helper()
	cl.send(wire.FileGet, func(w *wire.Writer) {
		w.U32(request)
		w.U64(owner)
		w.Str(item)
	})
	typ, r := cl.next()
	if typ != wire.FileResult || r.U32() != request {
		t.Fatalf("FILE_RESULT expected, got frame %d", typ)
	}
	found, data := r.U8(), r.Blob()
	if r.Err() != nil || len(r.B) != 0 {
		t.Fatal("malformed FILE_RESULT")
	}
	return found, data
}

func TestPlayerFiles(t *testing.T) {
	s := startFiles(t)
	if flags, _ := welcomeFlags(t, s, peerC); flags&16 == 0 {
		t.Fatal("WELCOME must offer player files")
	}
	a := s.login(t, peerA, "Alice")
	b := s.login(t, peerB, "Bob")
	licA, licB := bytes.Repeat([]byte{0xA1}, 3420), bytes.Repeat([]byte{0xB2}, 3420)
	a.putFile(xuidA, "licence.dat", licA)
	b.putFile(xuidB, "licence.dat", licB)
	b.putFile(xuidB, "penalty.dat", []byte{1, 2, 3}) // not one players read: dropped
	deadline := time.Now().Add(3 * time.Second)
	for {
		found, data := a.getFile(t, 1, xuidB, "licence.dat")
		if found == 1 && bytes.Equal(data, licB) {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("Bob's license: found %d, %d bytes", found, len(data))
		}
		time.Sleep(20 * time.Millisecond)
	}
	if found, data := b.getFile(t, 2, xuidA, "licence.dat"); found != 1 || !bytes.Equal(data, licA) {
		t.Fatalf("Alice's license: found %d, %d bytes", found, len(data))
	}
	if found, _ := a.getFile(t, 3, xuidB, "penalty.dat"); found != 0 {
		t.Fatal("penalty.dat is nobody else's to read")
	}
	if found, _ := a.getFile(t, 4, xuidC, "licence.dat"); found != 0 {
		t.Fatal("an unknown XUID has no license")
	}
	// a newer upload replaces the one before
	licB2 := bytes.Repeat([]byte{0xB3}, 3420)
	time.Sleep(5 * time.Second) // FILE_PUT: one every 5 s after a burst of 6
	b.putFile(xuidB, "licence.dat", licB2)
	deadline = time.Now().Add(3 * time.Second)
	for {
		if found, data := a.getFile(t, 5, xuidB, "licence.dat"); found == 1 && bytes.Equal(data, licB2) {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("the newer license must replace the older")
		}
		time.Sleep(20 * time.Millisecond)
	}
	// too large: a malformed frame
	a.putFile(xuidA, "licence.dat", make([]byte, maxPlayerFile+1))
	if !closedSoon(a.c, 3*time.Second) {
		t.Fatal("a too large player file must close the connection")
	}
}

func TestPlayerFilesNeedTheServerToKeepThem(t *testing.T) {
	plain := start(t, trusting(), nil)
	flags, cl := welcomeFlags(t, plain, peerA)
	if flags&16 != 0 {
		t.Fatal("a server without a database keeps no player files")
	}
	cl.putFile(xuidA, "licence.dat", []byte{1})
	if !closedSoon(cl.c, 3*time.Second) {
		t.Fatal("FILE_PUT to a server without player files must close the connection")
	}
}
