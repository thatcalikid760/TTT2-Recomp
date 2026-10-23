// Package tlsid is the server's TLS identity (PROTOCOL.md "Control connection"): a self-signed ECDSA P-256
// certificate created on first run, its key fingerprint (what players pin) and the Steam ticket identity
// derived from it.
package tlsid

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/base64"
	"encoding/hex"
	"encoding/pem"
	"errors"
	"fmt"
	"math/big"
	"os"
	"path/filepath"
	"time"
)

// Identity is a loaded certificate with its fingerprint.
type Identity struct {
	Cert        tls.Certificate
	Fingerprint [32]byte // SHA-256 of the SubjectPublicKeyInfo
}

// SteamIdentity is the identity string the game requests its Steam Web API ticket for.
func (id Identity) SteamIdentity() string { return SteamIdentityOf(id.Fingerprint) }

// SteamIdentityOf derives the ticket identity from a key fingerprint: "ttt2-" + 152 bits of it in base64url,
// 31 characters. MEASURED with Valve 2026-10-21: Steam binds at most 31 characters of a Web API ticket's identity
// (a longer one is cut and then matches nothing, not even itself), and an identity starting with a short "name:"
// prefix ("ttt2:...", even "ttt2:abc") never matches either - so no colon.
func SteamIdentityOf(fp [32]byte) string {
	return "ttt2-" + base64.RawURLEncoding.EncodeToString(fp[:19])
}

// MaxSteamIdentity is the longest identity Steam binds a ticket to.
const MaxSteamIdentity = 31

// FingerprintText is how hosts and players read a fingerprint: 8 groups of 4 hex digits (the first 16 bytes).
func (id Identity) FingerprintText() string {
	h := hex.EncodeToString(id.Fingerprint[:16])
	out := ""
	for i := 0; i < len(h); i += 4 {
		if i > 0 {
			out += "-"
		}
		out += h[i : i+4]
	}
	return out
}

// LoadOrCreate reads dir/tls.crt + dir/tls.key, creating them on first run (the key file readable by the owner
// only). A server keeps its key for life: players pin it.
func LoadOrCreate(dir string) (Identity, error) {
	certPath, keyPath := filepath.Join(dir, "tls.crt"), filepath.Join(dir, "tls.key")
	_, certErr := os.Stat(certPath)
	_, keyErr := os.Stat(keyPath)
	switch {
	case os.IsNotExist(certErr) && os.IsNotExist(keyErr):
		if err := create(certPath, keyPath); err != nil {
			return Identity{}, err
		}
	case certErr != nil || keyErr != nil:
		return Identity{}, fmt.Errorf("tls: %s and %s must both exist (or both be absent to make new ones)", certPath, keyPath)
	}
	cert, err := tls.LoadX509KeyPair(certPath, keyPath)
	if err != nil {
		return Identity{}, err
	}
	leaf, err := x509.ParseCertificate(cert.Certificate[0])
	if err != nil {
		return Identity{}, err
	}
	cert.Leaf = leaf
	return Identity{Cert: cert, Fingerprint: sha256.Sum256(leaf.RawSubjectPublicKeyInfo)}, nil
}

func create(certPath, keyPath string) error {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return err
	}
	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 127))
	if err != nil {
		return err
	}
	tmpl := &x509.Certificate{
		SerialNumber:          serial,
		Subject:               pkix.Name{CommonName: "ttt2-server"},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().AddDate(30, 0, 0),
		KeyUsage:              x509.KeyUsageDigitalSignature,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		return err
	}
	keyDER, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		return err
	}
	if err := writeNew(keyPath, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER}), 0o600); err != nil {
		return err
	}
	return writeNew(certPath, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), 0o644)
}

// writeNew creates a file that must not exist yet.
func writeNew(path string, data []byte, perm os.FileMode) error {
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, perm)
	if err != nil {
		if errors.Is(err, os.ErrExist) {
			return fmt.Errorf("tls: %s appeared while creating the identity", path)
		}
		return err
	}
	if _, err := f.Write(data); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}

// ServerConfig is the TLS configuration of the control port.
func (id Identity) ServerConfig() *tls.Config {
	return &tls.Config{
		Certificates: []tls.Certificate{id.Cert},
		MinVersion:   tls.VersionTLS12,
		// No resumption: a game makes one long connection, and TLS 1.3 tickets would arrive as post-handshake
		// messages, which the game's SChannel client treats as renegotiation and refuses.
		SessionTicketsDisabled: true,
		// TLS 1.2 only with forward-secret AEAD suites (TLS 1.3 suites are not configurable and all fine).
		CipherSuites: []uint16{
			tls.TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
			tls.TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
			tls.TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
		},
	}
}
