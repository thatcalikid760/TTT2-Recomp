package panel

import (
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

// RFC 6238 appendix B (SHA1, the 20-byte ASCII seed, 8 digits) and RFC 4226 appendix D (6 digits, counters 0..9).
func TestTOTPVectors(t *testing.T) {
	seed := []byte("12345678901234567890")
	for _, v := range []struct {
		unix int64
		code string
	}{
		{59, "94287082"}, {1111111109, "07081804"}, {1111111111, "14050471"},
		{1234567890, "89005924"}, {2000000000, "69279037"}, {20000000000, "65353130"},
	} {
		if got := totp(seed, v.unix/30, 8); got != v.code {
			t.Errorf("T=%d: %s, want %s", v.unix, got, v.code)
		}
	}
	for i, want := range []string{"755224", "287082", "359152", "969429", "338314", "254676", "287922", "162583", "399871", "520489"} {
		if got := totp(seed, int64(i), 6); got != want {
			t.Errorf("HOTP counter %d: %s, want %s", i, got, want)
		}
	}
}

// A code is accepted for its step and the steps before and after it, nothing else; only six digits.
func TestMatchCode(t *testing.T) {
	seed := []byte("12345678901234567890")
	now := time.Unix(1_800_000_015, 0)
	at := now.Unix() / 30
	for d := int64(-3); d <= 3; d++ {
		step, ok := matchCode(seed, totp(seed, at+d, 6), now)
		if want := d >= -1 && d <= 1; ok != want || (ok && step != at+d) {
			t.Errorf("step %+d: ok %v step %d", d, ok, step)
		}
	}
	code := totp(seed, at, 6)
	for _, bad := range []string{"", code[:5], code + "0", "12345a", " " + code[1:], "-12345"} {
		if _, ok := matchCode(seed, bad, now); ok {
			t.Errorf("%q accepted", bad)
		}
	}
}

// PBKDF2-HMAC-SHA256: RFC 7914 section 11 (P "passwd", S "salt", c 1: its first 32 bytes), and hashes made here.
func TestPasswordHash(t *testing.T) {
	want, _ := hex.DecodeString("55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc")
	known := passwordHash{KDF: kdfName, Iterations: 1, Salt: []byte("salt"), Key: want}
	if !known.verify("passwd") || known.verify("passwd ") || known.verify("Passwd") {
		t.Fatal("RFC 7914 vector")
	}
	old := pbkdf2Iterations
	pbkdf2Iterations = 1000
	defer func() { pbkdf2Iterations = old }()
	a, err := hashPassword("correct horse")
	if err != nil {
		t.Fatal(err)
	}
	b, _ := hashPassword("correct horse")
	if len(a.Salt) != 16 || len(a.Key) != 32 || a.Iterations != 1000 || a.KDF != "pbkdf2-sha256" || string(a.Salt) == string(b.Salt) {
		t.Fatalf("%+v %+v", a, b)
	}
	if !a.verify("correct horse") || a.verify("correct horse!") || a.verify("") {
		t.Fatal("verify")
	}
}

func TestDefaultIterations(t *testing.T) {
	if pbkdf2Iterations != 600000 {
		t.Fatalf("%d iterations", pbkdf2Iterations)
	}
	creds, password, err := newLogin(time.Now())
	if err != nil {
		t.Fatal(err)
	}
	if creds.Password.Iterations != 600000 || !creds.Password.verify(password) {
		t.Fatalf("%+v", creds.Password)
	}
}

func TestRandomPassword(t *testing.T) {
	seen := map[string]bool{}
	for i := 0; i < 50; i++ {
		p, err := randomPassword()
		if err != nil {
			t.Fatal(err)
		}
		if len(p) != 20 || strings.Trim(p, passwordAlphabet) != "" || seen[p] {
			t.Fatalf("%q", p)
		}
		seen[p] = true
	}
}

func TestOTPAuthURI(t *testing.T) {
	if got := OTPAuthURI("JBSWY3DPEHPK3PXP"); got != "otpauth://totp/TTT2%20Server:admin?secret=JBSWY3DPEHPK3PXP&issuer=TTT2%20Server" {
		t.Fatal(got)
	}
}

// The login file: written at once, the owner's only; a damaged or altered one is refused, not used.
func TestLoginFile(t *testing.T) {
	old := pbkdf2Iterations
	pbkdf2Iterations = 1000
	defer func() { pbkdf2Iterations = old }()
	path := filepath.Join(t.TempDir(), loginFile)
	if _, err := loadLogin(path); !isNotExist(err) {
		t.Fatalf("no file: %v", err)
	}
	creds, password, err := newLogin(time.Unix(1_800_000_000, 0))
	if err != nil {
		t.Fatal(err)
	}
	if len(creds.secret()) != 20 || len(creds.TOTPSecret) != 32 {
		t.Fatalf("secret %q", creds.TOTPSecret)
	}
	if err := saveLogin(path, creds); err != nil {
		t.Fatal(err)
	}
	if runtime.GOOS != "windows" {
		if st, _ := os.Stat(path); st.Mode().Perm() != 0o600 {
			t.Fatalf("mode %v", st.Mode().Perm())
		}
	}
	if _, err := os.Stat(path + ".tmp"); !errors.Is(err, os.ErrNotExist) {
		t.Fatal("the temporary file is left")
	}
	got, err := loadLogin(path)
	if err != nil || got.TOTPSecret != creds.TOTPSecret || !got.Password.verify(password) || got.User != "admin" {
		t.Fatalf("%+v %v", got, err)
	}
	raw, _ := os.ReadFile(path)
	if strings.Contains(string(raw), password) {
		t.Fatal("the password itself is in the file")
	}
	for _, change := range []func(m map[string]any){
		func(m map[string]any) { m["user"] = "root" },
		func(m map[string]any) { m["totp_secret"] = "AAAA" },
		func(m map[string]any) { m["password"].(map[string]any)["kdf"] = "md5" },
		func(m map[string]any) { m["password"].(map[string]any)["iterations"] = 0 },
		func(m map[string]any) { m["password"].(map[string]any)["salt"] = "" },
		func(m map[string]any) { m["extra"] = true },
	} {
		var m map[string]any
		json.Unmarshal(raw, &m)
		change(m)
		b, _ := json.Marshal(m)
		os.WriteFile(path, b, 0o600)
		if _, err := loadLogin(path); err == nil {
			t.Errorf("accepted %s", b)
		}
	}
	os.WriteFile(path, []byte("{"), 0o600)
	if _, err := loadLogin(path); err == nil || isNotExist(err) {
		t.Fatalf("a damaged file: %v", err)
	}
}
