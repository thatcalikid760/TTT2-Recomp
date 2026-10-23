package panel

import (
	"crypto/hmac"
	"crypto/pbkdf2"
	"crypto/rand"
	"crypto/sha1"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base32"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"strings"
	"time"
)

// The panel's login, in <data dir>/admin_panel.json (never in server.toml; readable by the server's account only,
// written at once): the user "admin", the password as a PBKDF2-HMAC-SHA256 hash (600000 iterations, a 16-byte
// random salt, a 32-byte key) and the secret of the authenticator app's codes (TOTP, RFC 6238: 20 random bytes,
// base32; HMAC-SHA1, 6 digits, 30 s steps). The admin commands panel-setup and panel-password make them; the
// password is shown once, by that command, and only its hash is kept.

const (
	loginFile      = "admin_panel.json"
	loginUser      = "admin"
	saltBytes      = 16
	keyBytes       = 32
	secretBytes    = 20
	passwordLength = 20
	// passwordAlphabet: letters and digits that cannot be misread (no 0 O, 1 l I)
	passwordAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789"
	totpStep         = 30 * time.Second
	totpDigits       = 6
	totpIssuer       = "TTT2 Server"
	kdfName          = "pbkdf2-sha256"
	maxLoginFile     = 64 << 10
)

// pbkdf2Iterations is the work of a new password hash (a variable for the tests; a kept hash has its own count).
var pbkdf2Iterations = 600000

// ErrNoLogin: no login has been made yet (panel-setup).
var ErrNoLogin = errors.New("no login yet: make one with ttt2admin panel-setup (or ttt2-server admin panel-setup)")

// ErrLoginExists: panel-setup without force, and a login exists.
var ErrLoginExists = errors.New("a login exists already: panel-setup force replaces it (a new password and a new " +
	"authenticator secret; every session ends), panel-password makes only a new password")

var secretEncoding = base32.StdEncoding.WithPadding(base32.NoPadding)

// credentials is admin_panel.json.
type credentials struct {
	User        string       `json:"user"`
	Password    passwordHash `json:"password"`
	TOTPSecret  string       `json:"totp_secret"` // base32, no padding
	Created     time.Time    `json:"created"`
	PasswordSet time.Time    `json:"password_set"`
}

// passwordHash is a PBKDF2-HMAC-SHA256 hash (salt and key base64 in the file).
type passwordHash struct {
	KDF        string `json:"kdf"`
	Iterations int    `json:"iterations"`
	Salt       []byte `json:"salt"`
	Key        []byte `json:"key"`
}

// hashPassword hashes a password with a new random salt.
func hashPassword(password string) (passwordHash, error) {
	h := passwordHash{KDF: kdfName, Iterations: pbkdf2Iterations, Salt: make([]byte, saltBytes)}
	if _, err := rand.Read(h.Salt); err != nil {
		return passwordHash{}, err
	}
	var err error
	h.Key, err = pbkdf2.Key(sha256.New, password, h.Salt, h.Iterations, keyBytes)
	return h, err
}

// verify reports whether password is the hashed one (the keys compared in constant time).
func (h passwordHash) verify(password string) bool {
	key, err := pbkdf2.Key(sha256.New, password, h.Salt, h.Iterations, keyBytes)
	return err == nil && subtle.ConstantTimeCompare(key, h.Key) == 1
}

// secret is the TOTP secret's bytes.
func (c *credentials) secret() []byte {
	b, _ := secretEncoding.DecodeString(c.TOTPSecret) // checked when the file was read or made
	return b
}

// check validates a login read from the file.
func (c *credentials) check() error {
	b, err := secretEncoding.DecodeString(c.TOTPSecret)
	switch {
	case c.User != loginUser:
		return fmt.Errorf("user %q (only %q)", c.User, loginUser)
	case c.Password.KDF != kdfName || c.Password.Iterations < 1 || c.Password.Iterations > 10_000_000:
		return fmt.Errorf("password hash: %s with 1..10000000 iterations expected", kdfName)
	case len(c.Password.Salt) != saltBytes || len(c.Password.Key) != keyBytes:
		return fmt.Errorf("password hash: a %d-byte salt and a %d-byte key expected", saltBytes, keyBytes)
	case err != nil || len(b) != secretBytes:
		return fmt.Errorf("totp_secret: %d bytes in base32 expected", secretBytes)
	}
	return nil
}

// loadLogin reads the login (fs.ErrNotExist: none yet).
func loadLogin(path string) (*credentials, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	var c credentials
	dec := json.NewDecoder(io.LimitReader(f, maxLoginFile)) // a larger file is cut, and fails as damaged
	dec.DisallowUnknownFields()
	if err := dec.Decode(&c); err != nil {
		return nil, fmt.Errorf("%s: %v", path, err)
	}
	if err := c.check(); err != nil {
		return nil, fmt.Errorf("%s: %v", path, err)
	}
	return &c, nil
}

// saveLogin writes the login, the owner's only, at once (a reader sees the old file or the new).
func saveLogin(path string, c *credentials) error {
	b, err := json.MarshalIndent(c, "", "  ")
	if err != nil {
		return err
	}
	return writeFileAtomic(path, append(b, '\n'))
}

// writeFileAtomic writes a file readable by the owner only through a temporary file renamed over it.
func writeFileAtomic(path string, data []byte) error {
	tmp := path + ".tmp"
	os.Remove(tmp) // left by a crash: never written through (it could have other permissions)
	f, err := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		return err
	}
	_, err = f.Write(data)
	if serr := f.Sync(); err == nil {
		err = serr
	}
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, path)
}

// newLogin makes a login with a new password and a new TOTP secret; it returns the password (shown once).
func newLogin(now time.Time) (*credentials, string, error) {
	password, err := randomPassword()
	if err != nil {
		return nil, "", err
	}
	hash, err := hashPassword(password)
	if err != nil {
		return nil, "", err
	}
	secret := make([]byte, secretBytes)
	if _, err := rand.Read(secret); err != nil {
		return nil, "", err
	}
	return &credentials{User: loginUser, Password: hash, TOTPSecret: secretEncoding.EncodeToString(secret),
		Created: now.UTC(), PasswordSet: now.UTC()}, password, nil
}

// randomPassword is passwordLength characters of passwordAlphabet, uniformly random (about 116 bits).
func randomPassword() (string, error) {
	limit := 256 - 256%len(passwordAlphabet) // bytes from limit up would favour the first characters
	out := make([]byte, 0, passwordLength)
	var buf [64]byte
	for len(out) < passwordLength {
		if _, err := rand.Read(buf[:]); err != nil {
			return "", err
		}
		for _, b := range buf {
			if int(b) < limit && len(out) < passwordLength {
				out = append(out, passwordAlphabet[int(b)%len(passwordAlphabet)])
			}
		}
	}
	return string(out), nil
}

// OTPAuthURI is the authenticator apps' link for a base32 secret (what a QR code of the login holds).
func OTPAuthURI(secret string) string {
	return "otpauth://totp/TTT2%20Server:" + loginUser + "?secret=" + secret + "&issuer=TTT2%20Server"
}

// totp is the code of one time step (RFC 6238 over RFC 4226: HMAC-SHA1 of the step, dynamic truncation).
func totp(secret []byte, step int64, digits int) string {
	var msg [8]byte
	binary.BigEndian.PutUint64(msg[:], uint64(step))
	m := hmac.New(sha1.New, secret)
	m.Write(msg[:])
	sum := m.Sum(nil)
	off := sum[len(sum)-1] & 0x0F
	v := binary.BigEndian.Uint32(sum[off:off+4]) & 0x7FFFFFFF
	mod := uint32(1)
	for i := 0; i < digits; i++ {
		mod *= 10
	}
	return fmt.Sprintf("%0*d", digits, v%mod)
}

// matchCode finds the time step whose code is `code`: the step of `now` or the one before or after (clocks drift),
// each compared in constant time.
func matchCode(secret []byte, code string, now time.Time) (step int64, ok bool) {
	if len(code) != totpDigits || strings.Trim(code, "0123456789") != "" {
		return 0, false
	}
	at := now.Unix() / int64(totpStep/time.Second)
	for d := int64(-1); d <= 1; d++ {
		if subtle.ConstantTimeCompare([]byte(totp(secret, at+d, totpDigits)), []byte(code)) == 1 {
			step, ok = at+d, true
		}
	}
	return step, ok
}

// isNotExist: no login file.
func isNotExist(err error) bool { return errors.Is(err, fs.ErrNotExist) }
