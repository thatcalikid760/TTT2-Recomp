package hub

import (
	"bytes"
	"errors"
	"time"

	"ttt2-server/internal/wire"
)

// Player files (PROTOCOL.md "Player files"): the game views a player's license (ranks, ranked record) by
// downloading their licence.dat from Xbox LIVE title storage by XUID. Each game uploads its own (FILE_PUT, after a
// login and whenever the game saves it); any player's game reads anyone's by the XUID their game uses (FILE_GET).
// A modified game can only put its own player's license - as on Xbox LIVE, it is the player's game that writes it.

// FileStore keeps the player files.
type FileStore interface {
	PutPlayerFile(steamID uint64, item string, data []byte, at time.Time) error
	PlayerFile(xuid uint64, item string) ([]byte, bool, error)
}

const maxPlayerFile = 16 << 10 // licence.dat is 3420 bytes

// sharedItems are the items players may read of each other (penalty.dat stays the player's own).
var sharedItems = map[string]bool{"licence.dat": true}

// SetFiles sets where the player files are kept (before Serve); nil: they are not (WELCOME says so, and FILE_PUT /
// FILE_GET close the connection). They need the statistics too: the XUIDs are recorded there.
func (h *Hub) SetFiles(db FileStore) { h.files = db }

// keepsFiles: WELCOME flag bit 4.
func (h *Hub) keepsFiles() bool { return h.files != nil && h.stats != nil }

// filePut: u64 own XUID, str item, blob data.
func (h *Hub) filePut(p *Player, r *wire.Reader, now time.Time) error {
	own, item, data := r.U64(), r.Str(), r.Blob()
	if r.Err() != nil || len(r.B) != 0 || len(data) == 0 || len(data) > maxPlayerFile {
		return errors.New("bad FILE_PUT")
	}
	if !sharedItems[item] {
		h.logPlayer(p, "player file %q of %q dropped: not one players read", trim(item, 32), p.Name)
		return nil
	}
	if !p.filePuts.take(0.2, 6, now) {
		h.logPlayer(p, "player files of %q dropped: too many", p.Name)
		return nil
	}
	if !h.claimXUID(p, own) {
		h.logPlayer(p, "player file of %q dropped: not the XUID this connection uses", p.Name)
		return nil
	}
	if err := h.files.PutPlayerFile(p.Peer, item, bytes.Clone(data), now); err != nil {
		h.logPlayer(p, "player file of %q not kept: %v", p.Name, err)
	}
	return nil
}

// fileGet: u32 request id, u64 owner XUID, str item -> FILE_RESULT u32 request id, u8 found (1 yes, 0 no, 2 asked too
// often), blob data.
func (h *Hub) fileGet(p *Player, r *wire.Reader, now time.Time) error {
	request, owner, item := r.U32(), r.U64(), r.Str()
	if r.Err() != nil || len(r.B) != 0 {
		return errors.New("bad FILE_GET")
	}
	found := uint8(2)
	var data []byte
	if p.fileGets.take(5, 20, now) {
		found = 0
		if sharedItems[item] && owner != 0 {
			d, ok, err := h.files.PlayerFile(owner, item)
			switch {
			case err != nil:
				found = 2 // the database did not answer: ask again
				h.logPlayer(p, "player file for %q not read: %v", p.Name, err)
			case ok:
				found, data = 1, d
			}
		}
	}
	var w wire.Writer
	w.U32(request)
	w.U8(found)
	w.Blob(data)
	p.send(wire.FileResult, w.B)
	return nil
}
