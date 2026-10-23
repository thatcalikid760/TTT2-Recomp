// Package steamauth checks a player's Steam Web API ticket (ISteamUser::GetAuthTicketForWebApi on the game's side)
// with Steam: ISteamUserAuth/AuthenticateUserTicket on api.steampowered.com with the host's own Web API key.
package steamauth

import (
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"strconv"
	"time"
)

// WebAPI verifies tickets with a Web API key.
type WebAPI struct {
	Key    string
	AppID  int
	Client *http.Client
	URL    string // default: https://api.steampowered.com/ISteamUserAuth/AuthenticateUserTicket/v1/
}

type response struct {
	Response struct {
		Params *struct {
			Result          string `json:"result"`
			SteamID         string `json:"steamid"`
			OwnerSteamID    string `json:"ownersteamid"`
			VACBanned       bool   `json:"vacbanned"`
			PublisherBanned bool   `json:"publisherbanned"`
		} `json:"params"`
		Error *struct {
			Code int    `json:"errorcode"`
			Desc string `json:"errordesc"`
		} `json:"error"`
	} `json:"response"`
}

// Verify returns the Steam ID the ticket (hex) belongs to, if it was made for `identity`. An identity is required:
// Steam passes a ticket checked without one whatever it was made for (MEASURED 2026-10-21), so a check without
// one would let any server's tickets in.
func (w WebAPI) Verify(ticketHex, identity string) (uint64, error) {
	if identity == "" {
		return 0, errors.New("steam: a ticket is only checked against this server's identity")
	}
	return w.verify(ticketHex, identity)
}

// verify asks Steam; an empty identity omits the parameter (the live test documents what Steam does then).
func (w WebAPI) verify(ticketHex, identity string) (uint64, error) {
	client := w.Client
	if client == nil {
		client = &http.Client{Timeout: 8 * time.Second}
	}
	endpoint := w.URL
	if endpoint == "" {
		endpoint = "https://api.steampowered.com/ISteamUserAuth/AuthenticateUserTicket/v1/"
	}
	q := url.Values{}
	q.Set("key", w.Key)
	q.Set("appid", strconv.Itoa(w.AppID))
	q.Set("ticket", ticketHex)
	if identity != "" {
		q.Set("identity", identity)
	}
	resp, err := client.Get(endpoint + "?" + q.Encode())
	if err != nil {
		// a *url.Error carries the whole request URL - with the key in it: never let it reach a log
		var ue *url.Error
		if errors.As(err, &ue) {
			return 0, fmt.Errorf("steam: %s request failed: %v", ue.Op, ue.Err)
		}
		return 0, errors.New("steam: request failed")
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return 0, fmt.Errorf("steam: HTTP %d", resp.StatusCode)
	}
	var r response
	if err := json.NewDecoder(resp.Body).Decode(&r); err != nil {
		return 0, err
	}
	if e := r.Response.Error; e != nil {
		return 0, fmt.Errorf("steam: %d %s", e.Code, e.Desc)
	}
	p := r.Response.Params
	if p == nil || p.Result != "OK" {
		return 0, fmt.Errorf("steam: no result")
	}
	if p.PublisherBanned {
		return 0, fmt.Errorf("steam: account banned")
	}
	return strconv.ParseUint(p.SteamID, 10, 64)
}
