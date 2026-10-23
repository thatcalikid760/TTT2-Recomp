package tlsid

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestCreateThenReload(t *testing.T) {
	dir := t.TempDir()
	a, err := LoadOrCreate(dir)
	if err != nil {
		t.Fatal(err)
	}
	b, err := LoadOrCreate(dir)
	if err != nil {
		t.Fatal(err)
	}
	if a.Fingerprint != b.Fingerprint {
		t.Fatal("the key must survive a restart")
	}
	if !strings.HasPrefix(a.SteamIdentity(), "ttt2-") || len(a.SteamIdentity()) != MaxSteamIdentity || strings.Contains(a.SteamIdentity(), ":") {
		t.Fatal(a.SteamIdentity())
	}
	if len(a.FingerprintText()) != 39 {
		t.Fatal(a.FingerprintText())
	}
	other, _ := LoadOrCreate(t.TempDir())
	if other.Fingerprint == a.Fingerprint {
		t.Fatal("two servers share a key")
	}
}

func TestHalfIdentityRefused(t *testing.T) {
	dir := t.TempDir()
	if _, err := LoadOrCreate(dir); err != nil {
		t.Fatal(err)
	}
	os.Remove(filepath.Join(dir, "tls.crt"))
	if _, err := LoadOrCreate(dir); err == nil {
		t.Fatal("a key without its certificate must not be replaced silently")
	}
}
