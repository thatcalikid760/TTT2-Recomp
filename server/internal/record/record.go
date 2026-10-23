// Package record checks the session records players publish (PROTOCOL.md "Session records"): the whole SES2
// layout with the game's own limits, and that the record really names its sender as the host.
package record

import (
	"encoding/binary"
	"errors"
	"fmt"

	"ttt2-server/internal/wire"
)

const (
	Magic       = 0x32534553 // "SES2"
	MaxSize     = 16 << 10
	MaxEntries  = 64
	MaxValue    = 4096
	MaxSlots    = 32
	infoSize    = 0x3C
	infoHostOff = 8 + 0x18 // XNKID, then XNADDR.abOnline = the host's peer id (big-endian)
)

// Session is what the server keeps from a record.
type Session struct {
	ID       uint64
	Host     uint64
	HostXUID uint64 // the online XUID the host's game uses
	Flags    uint32 // XSESSION_CREATE flags (Arbitrated: a ranked match)
	Filled   uint32
	Max      uint32
	// OpenPublic: a public slot is free (what a search looks for); Contexts: the host's contexts (GAME_TYPE 0x800A:
	// 0 ranked / 1 standard, GAME_MODE 0x800B, ...)
	OpenPublic bool
	Contexts   map[uint32]uint32
}

// Arbitrated is the XSESSION_CREATE_USES_ARBITRATION flag: the session is a ranked match.
const Arbitrated = 0x10

var errBad = errors.New("record: malformed")

// Check parses a record and checks it was published by `sender`.
func Check(rec []byte, sender uint64) (Session, error) {
	if len(rec) > MaxSize {
		return Session{}, fmt.Errorf("record: %d bytes", len(rec))
	}
	r := wire.Reader{B: rec}
	magic, id, host := r.U32(), r.U64(), r.U64()
	hostXUID, flags := r.U64(), r.U32()
	maxPublic, maxPrivate, filledPublic, filledPrivate := r.U32(), r.U32(), r.U32(), r.U32()
	info := r.Bytes(infoSize)
	nCtx := r.U32()
	if r.Err() != nil || magic != Magic || nCtx > MaxEntries {
		return Session{}, errBad
	}
	contexts := make(map[uint32]uint32, nCtx)
	for i := uint32(0); i < nCtx; i++ {
		id, value := r.U32(), r.U32()
		contexts[id] = value
	}
	nProp := r.U32()
	if r.Err() != nil || nProp > MaxEntries {
		return Session{}, errBad
	}
	for i := uint32(0); i < nProp; i++ {
		r.U32()
		size := r.U32()
		if r.Err() != nil || size > MaxValue {
			return Session{}, errBad
		}
		r.Bytes(int(size))
	}
	if r.Err() != nil {
		return Session{}, errBad
	}
	switch len(r.B) {
	case 0, 4: // the optional netcode word
	default:
		return Session{}, errBad
	}
	switch {
	case id == 0:
		return Session{}, errors.New("record: no session id")
	case host != sender:
		return Session{}, fmt.Errorf("record: host %016X is not the sender %016X", host, sender)
	case binary.BigEndian.Uint64(info[0:8]) != id:
		return Session{}, errors.New("record: session info is for another session")
	case binary.BigEndian.Uint64(info[infoHostOff:infoHostOff+8]) != sender:
		return Session{}, errors.New("record: session info names another host")
	case maxPublic > MaxSlots || maxPrivate > MaxSlots || filledPublic > maxPublic || filledPrivate > maxPrivate:
		return Session{}, errors.New("record: bad slot counts")
	}
	return Session{ID: id, Host: host, HostXUID: hostXUID, Flags: flags, Filled: filledPublic + filledPrivate, Max: maxPublic + maxPrivate,
		OpenPublic: filledPublic < maxPublic, Contexts: contexts}, nil
}
