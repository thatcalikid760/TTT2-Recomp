package main

import (
	"bufio"
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"log"
	"net"
	"os"
	"path/filepath"
	"strings"
	"time"

	"ttt2-server/internal/config"
)

// The admin port: the console's commands for a server that has no console (a systemd service, a container without
// a terminal). At start the server listens on 127.0.0.1 (a random port: loopback only) and writes
// <data dir>/admin.token - one line "127.0.0.1:<port> <token>", readable by the server's account only, the token 32
// random bytes in hex, new at every start, removed at a clean stop. `ttt2-server admin <command>` reads the file,
// connects, sends the token line and one command line, and prints the answer. Whoever can read the file can run every
// console command, as whoever can type in the console can.

const (
	adminTokenFile = "admin.token"
	maxAdminConns  = 4    // admin connections at once
	maxAdminLine   = 4096 // the token line and the command line
)

var (
	adminTimeout      = 10 * time.Second // reading the request, and each write of the answer
	adminRefusalDelay = time.Second      // before a wrong token is answered (guessing stays slow)
)

// startAdmin opens the admin port and writes its token file at path (stop: what quit does).
func startAdmin(path string, cmds *commands, stop func(), logger *log.Logger) error {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return err
	}
	token, err := newAdminToken()
	if err == nil {
		err = writeAdminToken(path, ln.Addr().String(), token)
	}
	if err != nil {
		ln.Close()
		return err
	}
	go serveAdmin(ln, token, cmds, stop, logger)
	return nil
}

func newAdminToken() (string, error) {
	var b [32]byte
	if _, err := rand.Read(b[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(b[:]), nil
}

// writeAdminToken writes "<addr> <token>" to path, the owner's only, at once (a reader sees the old file or the new).
func writeAdminToken(path, addr, token string) error {
	tmp := path + ".tmp"
	os.Remove(tmp) // left by a crash: never written through (it could have other permissions)
	f, err := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		return err
	}
	_, err = fmt.Fprintf(f, "%s %s\n", addr, token)
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, path)
}

// readAdminToken reads the admin port's address and token.
func readAdminToken(path string) (addr, token string, err error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return "", "", err
	}
	f := strings.Fields(string(b))
	if len(f) != 2 {
		return "", "", errors.New("damaged (restart the server to write it again)")
	}
	return f[0], f[1], nil
}

// serveAdmin answers admin connections, a few at a time, until the listener closes.
func serveAdmin(ln net.Listener, token string, cmds *commands, stop func(), logger *log.Logger) {
	slots := make(chan struct{}, maxAdminConns)
	for {
		c, err := ln.Accept()
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			logger.Printf("admin port: %v", err)
			time.Sleep(100 * time.Millisecond)
			continue
		}
		select {
		case slots <- struct{}{}:
			go func() {
				defer func() { <-slots }()
				handleAdmin(c, token, cmds, stop, logger)
			}()
		default:
			go adminBusy(c)
		}
	}
}

// adminBusy turns a connection away (all slots taken). Its request is read first, as everywhere: closing a socket
// with unread input resets it, and the reset can take the answer with it.
func adminBusy(c net.Conn) {
	defer c.Close()
	c.SetDeadline(time.Now().Add(time.Second))
	r := bufio.NewReaderSize(c, maxAdminLine)
	if _, err := adminLine(r); err == nil {
		adminLine(r)
	}
	fmt.Fprintln(c, "the admin port is busy: try again")
}

// handleAdmin: the token line, then one command, answered on the connection.
func handleAdmin(c net.Conn, token string, cmds *commands, stop func(), logger *log.Logger) {
	defer c.Close()
	c.SetDeadline(time.Now().Add(adminTimeout))
	r := bufio.NewReaderSize(c, maxAdminLine)
	got, err := adminLine(r)
	if err != nil {
		return
	}
	line, lineErr := adminLine(r)
	if subtle.ConstantTimeCompare([]byte(got), []byte(token)) != 1 {
		logger.Printf("admin port: a connection with a wrong token refused")
		time.Sleep(adminRefusalDelay)
		fmt.Fprintln(c, "wrong admin token")
		return
	}
	if lineErr != nil {
		return
	}
	logger.Printf("admin: %q (admin port)", line)
	out := deadlineWriter{c}
	if cmds.run(line, out) {
		fmt.Fprintln(out, "stopping")
		c.Close()
		stop()
	}
}

// adminLine reads one line (at most maxAdminLine bytes) without its line end; the last one may lack it.
func adminLine(r *bufio.Reader) (string, error) {
	line, err := r.ReadSlice('\n')
	if err != nil && !(errors.Is(err, io.EOF) && len(line) > 0) {
		return "", err
	}
	return strings.TrimRight(string(line), "\r\n"), nil
}

// deadlineWriter gives each write of an answer adminTimeout: a long command (backup) still answers, a client that
// stopped reading does not hold the connection.
type deadlineWriter struct{ c net.Conn }

func (w deadlineWriter) Write(b []byte) (int, error) {
	w.c.SetWriteDeadline(time.Now().Add(adminTimeout))
	return w.c.Write(b)
}

// adminRequest sends one command to the admin port at addr and copies the answer to out.
func adminRequest(addr, token, command string, out io.Writer) error {
	c, err := net.DialTimeout("tcp", addr, 3*time.Second)
	if err != nil {
		return err
	}
	defer c.Close()
	c.SetDeadline(time.Now().Add(10 * time.Minute)) // a backup of a large database takes a while
	if _, err := fmt.Fprintf(c, "%s\n%s\n", token, command); err != nil {
		return err
	}
	if cw, ok := c.(interface{ CloseWrite() error }); ok {
		cw.CloseWrite() // the whole request is sent
	}
	_, err = io.Copy(out, c)
	return err
}

// adminClient: `ttt2-server [-config server.toml] admin <command...>` runs one console command in the server running
// with that configuration (no words: help). The configuration is read only for its data folder. Exit status 1: it
// could not (the server is not running, or this account cannot read its token).
func adminClient(configPath string, words []string, stdout, stderr io.Writer) int {
	command := strings.Join(words, " ")
	if strings.TrimSpace(command) == "" {
		command = "help"
	}
	if strings.ContainsAny(command, "\r\n") {
		fmt.Fprintln(stderr, "one command, on one line")
		return 2
	}
	if _, err := os.Stat(configPath); err != nil {
		fmt.Fprintf(stderr, "no configuration %s (name the server's: ttt2-server -config <file> admin ...)\n", configPath)
		return 1
	}
	cfg, err := config.Load(configPath)
	if err != nil {
		fmt.Fprintf(stderr, "configuration: %v\n", err)
		return 1
	}
	path := filepath.Join(dataDirOf(configPath, cfg), adminTokenFile)
	addr, token, err := readAdminToken(path)
	switch {
	case errors.Is(err, fs.ErrNotExist):
		fmt.Fprintf(stderr, "the server is not running (no %s)\n", path)
		return 1
	case errors.Is(err, fs.ErrPermission):
		fmt.Fprintf(stderr, "cannot read %s: run this as the server's user (sudo -u <user> ...)\n", path)
		return 1
	case err != nil:
		fmt.Fprintf(stderr, "%s: %v\n", path, err)
		return 1
	}
	if err := adminRequest(addr, token, command, stdout); err != nil {
		var op *net.OpError
		if errors.As(err, &op) && op.Op == "dial" {
			fmt.Fprintf(stderr, "the server is not running (%s was left by a server that stopped)\n", path)
		} else {
			fmt.Fprintf(stderr, "admin port: %v\n", err)
		}
		return 1
	}
	return 0
}
