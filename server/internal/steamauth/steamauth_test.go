package steamauth

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func fake(t *testing.T, status int, body string, delay time.Duration, check func(*http.Request)) WebAPI {
	t.Helper()
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if check != nil {
			check(r)
		}
		time.Sleep(delay)
		w.WriteHeader(status)
		w.Write([]byte(body))
	}))
	t.Cleanup(srv.Close)
	return WebAPI{Key: "KEY", AppID: 480, URL: srv.URL + "/", Client: &http.Client{Timeout: 500 * time.Millisecond}}
}

func TestVerify(t *testing.T) {
	ok := `{"response":{"params":{"result":"OK","steamid":"76561197960287930","ownersteamid":"76561197960287930","vacbanned":false,"publisherbanned":false}}}`
	w := fake(t, 200, ok, 0, func(r *http.Request) {
		q := r.URL.Query()
		if q.Get("key") != "KEY" || q.Get("appid") != "480" || q.Get("ticket") != "ABCD" || q.Get("identity") != "ttt2-server:00" {
			t.Errorf("query %v", q)
		}
	})
	id, err := w.Verify("ABCD", "ttt2-server:00")
	if err != nil || id != 76561197960287930 {
		t.Fatal(id, err)
	}
}

func TestVerifyFailures(t *testing.T) {
	for name, w := range map[string]WebAPI{
		"steam error": fake(t, 200, `{"response":{"error":{"errorcode":101,"errordesc":"Invalid ticket"}}}`, 0, nil),
		"banned":      fake(t, 200, `{"response":{"params":{"result":"OK","steamid":"1","publisherbanned":true}}}`, 0, nil),
		"no result":   fake(t, 200, `{"response":{}}`, 0, nil),
		"not json":    fake(t, 200, `<html>`, 0, nil),
		"http 403":    fake(t, 403, ``, 0, nil),
		"bad id":      fake(t, 200, `{"response":{"params":{"result":"OK","steamid":"x"}}}`, 0, nil),
		"slow":        fake(t, 200, `{}`, time.Second, nil),
	} {
		if id, err := w.Verify("ABCD", "id"); err == nil {
			t.Errorf("%s: accepted (%d)", name, id)
		}
	}
}

func TestErrorsNeverCarryTheKey(t *testing.T) {
	// a server that is not there: the transport error must not include the request URL (key, ticket)
	w := WebAPI{Key: "SECRETKEY123", AppID: 480, URL: "http://127.0.0.1:1/", Client: &http.Client{Timeout: 300 * time.Millisecond}}
	_, err := w.Verify("ABCDEF0123456789ABCDEF0123456789", "id")
	if err == nil || strings.Contains(err.Error(), "SECRETKEY123") || strings.Contains(err.Error(), "ABCDEF0123456789") {
		t.Fatalf("error leaks the request: %v", err)
	}
}

func TestIdentityRequired(t *testing.T) {
	w := fake(t, 200, `{"response":{"params":{"result":"OK","steamid":"1"}}}`, 0, nil)
	if _, err := w.Verify("ABCD", ""); err == nil {
		t.Fatal("a ticket was checked without an identity")
	}
}
