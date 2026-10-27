package main

import (
	"bufio"
	"bytes"
	"crypto/tls"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"math"
	"net/http"
	"net/textproto"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

// -----------------------------------------------------------------------------
// Config structures matching firmware/src/config.h and config.cpp
// -----------------------------------------------------------------------------

type SlotMapping struct {
	Slot            string `json:"slot"`
	Source          string `json:"source"`
	TTL             uint32 `json:"ttl"`
	DeltaSource     string `json:"delta_source"`
	DeltaIsPrevious bool   `json:"delta_is_previous"`
	HasHistory      bool   `json:"has_history"`
	HistorySource   string `json:"history_source"`
	HistoryItem     string `json:"history_item"`
}

type Connector struct {
	ID              string        `json:"id"`
	Kind            string        `json:"kind"`
	URL             string        `json:"url"`
	Interval        uint32        `json:"interval"`
	Token           string        `json:"token,omitempty"`
	RefreshToken    string        `json:"refresh_token,omitempty"`
	Username        string        `json:"username,omitempty"`
	TokenSet        bool          `json:"token_set"`
	RefreshTokenSet bool          `json:"refresh_token_set"`
	UsernameSet     bool          `json:"username_set"`
	Insecure        bool          `json:"insecure"`
	Map             []SlotMapping `json:"map"`
}

type WidgetInstance struct {
	Type    string `json:"type"`
	Size    string `json:"size,omitempty"`
	Divider bool   `json:"divider,omitempty"`
	Slot    string `json:"slot,omitempty"`
	Label   string `json:"label,omitempty"`
}

type Dashboard struct {
	Name string             `json:"name"`
	Rows [][]WidgetInstance `json:"rows"`
}

type Network struct {
	SSID        string `json:"ssid"`
	Password    string `json:"password,omitempty"`
	PasswordSet bool   `json:"password_set"`
}

type Settings struct {
	DeviceName      string      `json:"device_name"`
	Lang            string      `json:"lang"`
	TimezoneMinutes int         `json:"timezone_minutes"`
	City            string      `json:"city"`
	CityResolved    string      `json:"city_resolved"`
	CityLat         float64     `json:"city_lat"`
	CityLon         float64     `json:"city_lon"`
	APPassword      string      `json:"ap_password,omitempty"`
	Networks        []Network   `json:"networks"`
	Connectors      []Connector `json:"connectors"`
	ActiveDashboard int         `json:"active_dashboard"`
	Dashboards      []Dashboard `json:"dashboards"`
}

// -----------------------------------------------------------------------------
// Slot & Store definitions matching firmware/src/slots.h
// -----------------------------------------------------------------------------

type Slot struct {
	Text       string    `json:"text"`
	Number     float64   `json:"number"`
	Delta      float64   `json:"delta"`
	At         uint32    `json:"at"`
	TTL        uint32    `json:"ttl"`
	OK         bool      `json:"ok"`
	Error      string    `json:"error,omitempty"`
	History    []float64 `json:"history,omitempty"`
	Connector  string    `json:"connector,omitempty"`
	HistoryLen int       `json:"history_len,omitempty"`
}

type DeviceInfo struct {
	BatteryPct      int    `json:"battery_pct"`
	WifiRSSI        int    `json:"wifi_rssi"`
	Now             uint32 `json:"now"`
	NextUpdateAt    uint32 `json:"next_update_at"`
	TimezoneMinutes int    `json:"timezone_minutes"`
	Lang            string `json:"lang"`
}

type StorePayload struct {
	Device DeviceInfo        `json:"device"`
	Slots  map[string]Slot   `json:"slots"`
	Failed map[string]string `json:"failed"`
}

// -----------------------------------------------------------------------------
// Global Daemon State
// -----------------------------------------------------------------------------

type Daemon struct {
	mu           sync.Mutex
	cfgPath      string
	htmlPath     string
	rendererPath string
	imgPath      string
	rotMode      string
	port         int
	settings     Settings
	slots        map[string]Slot
	failed       map[string]string
	lastPolled   map[string]time.Time
	forceRedraw  bool
}

func formatNumber(val float64) string {
	if math.Abs(val) >= 1000 {
		intPart := int64(math.Round(val))
		sign := ""
		if intPart < 0 {
			sign = "-"
			intPart = -intPart
		}
		s := strconv.FormatInt(intPart, 10)
		var out []byte
		n := len(s)
		for i, c := range s {
			if i > 0 && (n-i)%3 == 0 {
				out = append(out, ' ')
			}
			out = append(out, byte(c))
		}
		return sign + string(out)
	}
	return fmt.Sprintf("%.2f", val)
}

func readBattery() int {
	matches, _ := filepath.Glob("/sys/class/power_supply/*/capacity")
	for _, p := range matches {
		data, err := os.ReadFile(p)
		if err == nil {
			v, err := strconv.Atoi(strings.TrimSpace(string(data)))
			if err == nil && v >= 0 && v <= 100 {
				return v
			}
		}
	}
	return 100
}

func readRSSI() int {
	data, err := os.ReadFile("/proc/net/wireless")
	if err == nil {
		lines := strings.Split(string(data), "\n")
		for _, line := range lines {
			fields := strings.Fields(line)
			if len(fields) >= 4 && strings.HasPrefix(fields[0], "wlan") {
				levelStr := strings.TrimSuffix(fields[3], ".")
				level, err := strconv.Atoi(levelStr)
				if err == nil {
					if level > 0 {
						return level - 256
					}
					return level
				}
			}
		}
	}
	return -60
}

// -----------------------------------------------------------------------------
// Connector Pollers
// -----------------------------------------------------------------------------

func (d *Daemon) pollBinanceTicker(c Connector) {
	client := &http.Client{Timeout: 5 * time.Second}
	resp, err := client.Get(c.URL)
	if err != nil {
		d.mu.Lock()
		d.failed[c.ID] = "source did not respond: " + err.Error()
		d.mu.Unlock()
		return
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		d.mu.Lock()
		d.failed[c.ID] = fmt.Sprintf("source HTTP %d", resp.StatusCode)
		d.mu.Unlock()
		return
	}

	var data struct {
		LastPrice          string `json:"lastPrice"`
		PriceChangePercent string `json:"priceChangePercent"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&data); err != nil {
		d.mu.Lock()
		d.failed[c.ID] = "invalid json"
		d.mu.Unlock()
		return
	}

	price, _ := strconv.ParseFloat(data.LastPrice, 64)
	delta, _ := strconv.ParseFloat(data.PriceChangePercent, 64)

	d.mu.Lock()
	defer d.mu.Unlock()
	delete(d.failed, c.ID)
	now := uint32(time.Now().Unix())

	sl := d.slots["btc"]
	sl.Number = price
	sl.Delta = delta
	sl.Text = formatNumber(price)
	sl.At = now
	sl.TTL = 900
	sl.OK = true
	sl.Connector = c.ID
	d.slots["btc"] = sl
}

func (d *Daemon) pollBinanceKlines(c Connector) {
	client := &http.Client{Timeout: 5 * time.Second}
	resp, err := client.Get(c.URL)
	if err != nil {
		return
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return
	}

	var raw [][]interface{}
	if err := json.NewDecoder(resp.Body).Decode(&raw); err != nil {
		return
	}

	var hist []float64
	for _, candle := range raw {
		if len(candle) > 4 {
			if closeStr, ok := candle[4].(string); ok {
				if v, err := strconv.ParseFloat(closeStr, 64); err == nil {
					hist = append(hist, v)
				}
			}
		}
	}

	if len(hist) > 0 {
		d.mu.Lock()
		defer d.mu.Unlock()
		sl := d.slots["btc"]
		sl.History = hist
		sl.HistoryLen = len(hist)
		d.slots["btc"] = sl
	}
}

func (d *Daemon) pollFiatRates(c Connector) {
	client := &http.Client{Timeout: 5 * time.Second}
	resp, err := client.Get(c.URL)
	if err != nil {
		d.mu.Lock()
		d.failed[c.ID] = "source did not respond: " + err.Error()
		d.mu.Unlock()
		return
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		d.mu.Lock()
		d.failed[c.ID] = fmt.Sprintf("source HTTP %d", resp.StatusCode)
		d.mu.Unlock()
		return
	}

	var data struct {
		Valute map[string]struct {
			Value    float64 `json:"Value"`
			Previous float64 `json:"Previous"`
		} `json:"Valute"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&data); err != nil {
		d.mu.Lock()
		d.failed[c.ID] = "invalid json"
		d.mu.Unlock()
		return
	}

	d.mu.Lock()
	defer d.mu.Unlock()
	delete(d.failed, c.ID)
	now := uint32(time.Now().Unix())

	if usd, ok := data.Valute["USD"]; ok && usd.Previous > 0 {
		delta := (usd.Value - usd.Previous) / usd.Previous * 100
		d.slots["usd_rub"] = Slot{
			Number:    usd.Value,
			Delta:     delta,
			Text:      fmt.Sprintf("%.2f", usd.Value),
			At:        now,
			TTL:       86400,
			OK:        true,
			Connector: c.ID,
		}
	}
	if eur, ok := data.Valute["EUR"]; ok && eur.Previous > 0 {
		delta := (eur.Value - eur.Previous) / eur.Previous * 100
		d.slots["eur_rub"] = Slot{
			Number:    eur.Value,
			Delta:     delta,
			Text:      fmt.Sprintf("%.2f", eur.Value),
			At:        now,
			TTL:       86400,
			OK:        true,
			Connector: c.ID,
		}
	}
}

func (d *Daemon) pollWeather(c Connector) {
	lat := d.settings.CityLat
	lon := d.settings.CityLon
	if lat == 0 && lon == 0 {
		lat = 56.8389  // default fallback if city not geocoded yet
		lon = 60.6057
	}
	url := fmt.Sprintf("https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min&timezone=auto", lat, lon)

	client := &http.Client{Timeout: 5 * time.Second}
	resp, err := client.Get(url)
	if err != nil {
		d.mu.Lock()
		d.failed[c.ID] = "weather source did not respond"
		d.mu.Unlock()
		return
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		d.mu.Lock()
		d.failed[c.ID] = fmt.Sprintf("weather HTTP %d", resp.StatusCode)
		d.mu.Unlock()
		return
	}

	var data struct {
		Current struct {
			Temperature2m float64 `json:"temperature_2m"`
			WeatherCode   int     `json:"weather_code"`
		} `json:"current"`
		Daily struct {
			Temperature2mMax []float64 `json:"temperature_2m_max"`
			Temperature2mMin []float64 `json:"temperature_2m_min"`
		} `json:"daily"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&data); err != nil {
		d.mu.Lock()
		d.failed[c.ID] = "weather invalid json"
		d.mu.Unlock()
		return
	}

	weatherNames := map[int]string{
		0: "CLEAR", 1: "MAINLY CLEAR", 2: "PARTLY CLOUDY", 3: "OVERCAST",
		45: "FOG", 48: "DEPOSITING RIME FOG", 51: "LIGHT DRIZZLE", 53: "DRIZZLE",
		55: "HEAVY DRIZZLE", 61: "SLIGHT RAIN", 63: "RAIN", 65: "HEAVY RAIN",
		71: "SLIGHT SNOW", 73: "SNOW", 75: "HEAVY SNOW", 80: "SHOWERS", 95: "THUNDERSTORM",
	}
	cond := "CLOUDY"
	if name, ok := weatherNames[data.Current.WeatherCode]; ok {
		cond = name
	}

	minT := 0.0
	maxT := 0.0
	if len(data.Daily.Temperature2mMin) > 0 {
		minT = data.Daily.Temperature2mMin[0]
	}
	if len(data.Daily.Temperature2mMax) > 0 {
		maxT = data.Daily.Temperature2mMax[0]
	}

	summary := fmt.Sprintf("%+d…%+d · %s", int(math.Round(minT)), int(math.Round(maxT)), cond)
	temp := data.Current.Temperature2m

	d.mu.Lock()
	defer d.mu.Unlock()
	delete(d.failed, c.ID)
	now := uint32(time.Now().Unix())

	d.slots["weather.temp"] = Slot{
		Number:    temp,
		Text:      fmt.Sprintf("%+d°", int(math.Round(temp))),
		At:        now,
		TTL:       3600,
		OK:        true,
		Connector: c.ID,
	}
	d.slots["weather.summary"] = Slot{
		Text:      summary,
		At:        now,
		TTL:       3600,
		OK:        true,
		Connector: c.ID,
	}
}

func (d *Daemon) pollHomeAssistant(c Connector) {
	if c.URL == "" || c.Token == "" {
		return
	}
	tr := &http.Transport{
		Proxy:           http.ProxyFromEnvironment,
		TLSClientConfig: &tls.Config{InsecureSkipVerify: c.Insecure},
	}
	client := &http.Client{Transport: tr, Timeout: 5 * time.Second}

	for _, m := range c.Map {
		if m.Source == "" {
			continue
		}
		url := strings.TrimRight(c.URL, "/") + "/api/states/" + m.Source
		req, err := http.NewRequest("GET", url, nil)
		if err != nil {
			continue
		}
		req.Header.Set("Authorization", "Bearer "+c.Token)
		resp, err := client.Do(req)
		if err != nil {
			d.mu.Lock()
			d.failed[c.ID] = "HA connection error: " + err.Error()
			d.mu.Unlock()
			continue
		}

		if resp.StatusCode != http.StatusOK {
			resp.Body.Close()
			d.mu.Lock()
			d.failed[c.ID] = fmt.Sprintf("HA HTTP %d", resp.StatusCode)
			d.mu.Unlock()
			continue
		}

		var st struct {
			State      string `json:"state"`
			Attributes struct {
				UnitOfMeasurement string `json:"unit_of_measurement"`
				FriendlyName      string `json:"friendly_name"`
			} `json:"attributes"`
		}
		json.NewDecoder(resp.Body).Decode(&st)
		resp.Body.Close()

		num, _ := strconv.ParseFloat(st.State, 64)
		text := st.State
		if st.Attributes.UnitOfMeasurement != "" {
			text += " " + st.Attributes.UnitOfMeasurement
		}

		d.mu.Lock()
		delete(d.failed, c.ID)
		now := uint32(time.Now().Unix())
		sl := d.slots[m.Slot]
		sl.Number = num
		sl.Text = text
		sl.At = now
		sl.TTL = m.TTL
		if sl.TTL == 0 {
			sl.TTL = 600
		}
		sl.OK = true
		sl.Connector = c.ID
		d.slots[m.Slot] = sl
		d.mu.Unlock()
	}
}

func (d *Daemon) pollClaudeLimits(c Connector) {
	if c.Token == "" {
		return
	}
	req, _ := http.NewRequest("GET", "https://api.anthropic.com/api/oauth/usage", nil)
	req.Header.Set("Authorization", "Bearer "+c.Token)
	req.Header.Set("User-Agent", "tablo/0.1.0")

	client := &http.Client{Timeout: 5 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		d.mu.Lock()
		d.failed[c.ID] = "Claude error: " + err.Error()
		d.mu.Unlock()
		return
	}
	defer resp.Body.Close()

	if resp.StatusCode == 401 || resp.StatusCode == 403 {
		d.mu.Lock()
		d.failed[c.ID] = "Claude token expired or unauthorized"
		d.mu.Unlock()
		return
	}
	if resp.StatusCode != http.StatusOK {
		d.mu.Lock()
		d.failed[c.ID] = fmt.Sprintf("Claude HTTP %d", resp.StatusCode)
		d.mu.Unlock()
		return
	}

	var data struct {
		FiveHour struct {
			Utilization float64 `json:"utilization"`
			ResetTime   string  `json:"resets_at"`
		} `json:"five_hour"`
		SevenDay struct {
			Utilization float64 `json:"utilization"`
			ResetTime   string  `json:"resets_at"`
		} `json:"seven_day"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&data); err != nil {
		return
	}

	d.mu.Lock()
	defer d.mu.Unlock()
	delete(d.failed, c.ID)
	now := uint32(time.Now().Unix())

	d.slots["limit.claude.5h"] = Slot{
		Number:    data.FiveHour.Utilization,
		Text:      fmt.Sprintf("%.0f%%", data.FiveHour.Utilization),
		At:        now,
		TTL:       300,
		OK:        true,
		Connector: c.ID,
	}
	d.slots["limit.claude.week"] = Slot{
		Number:    data.SevenDay.Utilization,
		Text:      fmt.Sprintf("%.0f%%", data.SevenDay.Utilization),
		At:        now,
		TTL:       300,
		OK:        true,
		Connector: c.ID,
	}
	d.slots["limit.claude.reset"] = Slot{
		Text:      "active",
		At:        now,
		TTL:       300,
		OK:        true,
		Connector: c.ID,
	}
}

func (d *Daemon) pollCodexLimits(c Connector) {
	if c.Token == "" {
		return
	}
	req, _ := http.NewRequest("GET", "https://chatgpt.com/backend-api/wham/usage", nil)
	req.Header.Set("Authorization", "Bearer "+c.Token)
	req.Header.Set("User-Agent", "tablo/0.1.0")

	client := &http.Client{Timeout: 5 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		d.mu.Lock()
		d.failed[c.ID] = "Codex error: " + err.Error()
		d.mu.Unlock()
		return
	}
	defer resp.Body.Close()

	if resp.StatusCode == 401 || resp.StatusCode == 403 {
		d.mu.Lock()
		d.failed[c.ID] = "Codex token expired — update in setup"
		d.mu.Unlock()
		return
	}
	if resp.StatusCode != http.StatusOK {
		d.mu.Lock()
		d.failed[c.ID] = fmt.Sprintf("Codex HTTP %d", resp.StatusCode)
		d.mu.Unlock()
		return
	}

	var data struct {
		RateLimit struct {
			Remaining int `json:"remaining"`
			Limit     int `json:"limit"`
		} `json:"rate_limit"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&data); err == nil && data.RateLimit.Limit > 0 {
		pct := float64(data.RateLimit.Remaining) / float64(data.RateLimit.Limit) * 100
		d.mu.Lock()
		defer d.mu.Unlock()
		delete(d.failed, c.ID)
		now := uint32(time.Now().Unix())
		d.slots["limit.codex"] = Slot{
			Number:    pct,
			Text:      fmt.Sprintf("%.0f%%", pct),
			At:        now,
			TTL:       300,
			OK:        true,
			Connector: c.ID,
		}
		d.slots["limit.codex.reset"] = Slot{
			Text:      "week",
			At:        now,
			TTL:       300,
			OK:        true,
			Connector: c.ID,
		}
	}
}

func (d *Daemon) pollIMAP(c Connector) {
	if c.Username == "" || c.Token == "" {
		return
	}
	host := c.URL
	if host == "" {
		host = "imap.gmail.com"
	}
	if !strings.Contains(host, ":") {
		host += ":993"
	}

	conn, err := tls.Dial("tcp", host, &tls.Config{ServerName: strings.Split(host, ":")[0]})
	if err != nil {
		d.mu.Lock()
		d.failed[c.ID] = "IMAP TLS error: " + err.Error()
		d.mu.Unlock()
		return
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(7 * time.Second))

	r := bufio.NewReader(conn)
	tp := textproto.NewReader(r)

	// Read greeting
	if _, err := tp.ReadLine(); err != nil {
		return
	}

	// Login
	fmt.Fprintf(conn, "a1 LOGIN %s %s\r\n", c.Username, c.Token)
	for {
		line, err := tp.ReadLine()
		if err != nil || strings.HasPrefix(line, "a1 NO") || strings.HasPrefix(line, "a1 BAD") {
			d.mu.Lock()
			d.failed[c.ID] = "IMAP login rejected — check app password"
			d.mu.Unlock()
			return
		}
		if strings.HasPrefix(line, "a1 OK") {
			break
		}
	}

	// Select INBOX
	fmt.Fprintf(conn, "a2 SELECT INBOX\r\n")
	for {
		line, err := tp.ReadLine()
		if err != nil || strings.HasPrefix(line, "a2 NO") || strings.HasPrefix(line, "a2 BAD") {
			return
		}
		if strings.HasPrefix(line, "a2 OK") {
			break
		}
	}

	// Search UNSEEN
	fmt.Fprintf(conn, "a3 SEARCH UNSEEN\r\n")
	var ids []string
	for {
		line, err := tp.ReadLine()
		if err != nil {
			return
		}
		if strings.HasPrefix(line, "* SEARCH") {
			parts := strings.Fields(line)
			if len(parts) > 2 {
				ids = parts[2:]
			}
		}
		if strings.HasPrefix(line, "a3 OK") {
			break
		}
	}

	unreadCount := len(ids)

	type EmailHeader struct {
		From    string
		Subject string
		Date    string
	}
	var headers []EmailHeader

	// Fetch headers of up to 3 most recent emails
	fetchCount := 3
	if unreadCount < fetchCount {
		fetchCount = unreadCount
	}
	for i := 0; i < fetchCount; i++ {
		msgID := ids[len(ids)-1-i]
		tag := fmt.Sprintf("h%d", i)
		fmt.Fprintf(conn, "%s FETCH %s (BODY.PEEK[HEADER.FIELDS (FROM SUBJECT DATE)])\r\n", tag, msgID)

		var cur EmailHeader
		for {
			line, err := tp.ReadLine()
			if err != nil || strings.HasPrefix(line, tag+" OK") || strings.HasPrefix(line, tag+" NO") {
				break
			}
			lower := strings.ToLower(line)
			if strings.HasPrefix(lower, "from:") {
				cur.From = strings.TrimSpace(line[5:])
			} else if strings.HasPrefix(lower, "subject:") {
				cur.Subject = strings.TrimSpace(line[8:])
			} else if strings.HasPrefix(lower, "date:") {
				cur.Date = strings.TrimSpace(line[5:])
			}
		}
		headers = append(headers, cur)
	}

	// Clean up from names (strip angle brackets)
	for i := range headers {
		if idx := strings.Index(headers[i].From, "<"); idx > 0 {
			headers[i].From = strings.TrimSpace(headers[i].From[:idx])
		}
		headers[i].From = strings.Trim(headers[i].From, `"'`)
	}

	d.mu.Lock()
	defer d.mu.Unlock()
	delete(d.failed, c.ID)
	now := uint32(time.Now().Unix())

	d.slots["mail.unread"] = Slot{
		Number:    float64(unreadCount),
		Text:      strconv.Itoa(unreadCount),
		At:        now,
		TTL:       900,
		OK:        true,
		Connector: c.ID,
	}

	for idx, h := range headers {
		keyFrom := fmt.Sprintf("mail.%d.from", idx+1)
		keySub := fmt.Sprintf("mail.%d.subject", idx+1)
		keyTime := fmt.Sprintf("mail.%d.time", idx+1)
		d.slots[keyFrom] = Slot{Text: h.From, At: now, TTL: 900, OK: true, Connector: c.ID}
		d.slots[keySub] = Slot{Text: h.Subject, At: now, TTL: 900, OK: true, Connector: c.ID}
		d.slots[keyTime] = Slot{Text: "today", At: now, TTL: 900, OK: true, Connector: c.ID}
	}
}

func (d *Daemon) pollAllDue() {
	d.mu.Lock()
	connectors := make([]Connector, len(d.settings.Connectors))
	copy(connectors, d.settings.Connectors)
	d.mu.Unlock()

	now := time.Now()
	for _, c := range connectors {
		d.mu.Lock()
		last, has := d.lastPolled[c.ID]
		d.mu.Unlock()

		interval := time.Duration(c.Interval) * time.Second
		if interval < 5*time.Second {
			interval = 5 * time.Second
		}
		if has && now.Sub(last) < interval {
			continue
		}

		d.mu.Lock()
		d.lastPolled[c.ID] = now
		d.mu.Unlock()

		switch c.Kind {
		case "http":
			if c.ID == "btc" {
				go d.pollBinanceTicker(c)
			} else if c.ID == "btc_history" {
				go d.pollBinanceKlines(c)
			} else if c.ID == "fiat" {
				go d.pollFiatRates(c)
			}
		case "weather":
			go d.pollWeather(c)
		case "homeassistant":
			go d.pollHomeAssistant(c)
		case "anthropic":
			go d.pollClaudeLimits(c)
		case "codex":
			go d.pollCodexLimits(c)
		case "imap":
			go d.pollIMAP(c)
		}
	}
}

// -----------------------------------------------------------------------------
// Render & Screen Output
// -----------------------------------------------------------------------------

func (d *Daemon) renderFrame() error {
	d.mu.Lock()
	payload := StorePayload{
		Device: DeviceInfo{
			BatteryPct:      readBattery(),
			WifiRSSI:        readRSSI(),
			Now:             uint32(time.Now().Unix()),
			NextUpdateAt:    uint32(time.Now().Unix() + 300),
			TimezoneMinutes: d.settings.TimezoneMinutes,
			Lang:            d.settings.Lang,
		},
		Slots:  d.slots,
		Failed: d.failed,
	}
	d.mu.Unlock()

	tmpSlotsFile := filepath.Join(os.TempDir(), "tablo_slots.json")
	data, err := json.Marshal(payload)
	if err != nil {
		return err
	}
	if err := os.WriteFile(tmpSlotsFile, data, 0644); err != nil {
		return err
	}

	cmd := exec.Command(d.rendererPath,
		"-config", d.cfgPath,
		"-slots", tmpSlotsFile,
		"-out", d.imgPath,
		"-rot", d.rotMode,
	)
	var outBuf, errBuf bytes.Buffer
	cmd.Stdout = &outBuf
	cmd.Stderr = &errBuf
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("renderer error: %v, out: %s, stderr: %s", err, outBuf.String(), errBuf.String())
	}

	// Trigger PocketBook image viewer display if running on PB
	if _, err := os.Stat("/ebrmain/bin/openbook"); err == nil {
		exec.Command("/ebrmain/bin/openbook", d.imgPath).Start()
	}

	log.Printf("Frame rendered successfully -> %s", d.imgPath)
	return nil
}

// -----------------------------------------------------------------------------
// Web Setup Portal HTTP Handlers
// -----------------------------------------------------------------------------

func (d *Daemon) handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	data, err := os.ReadFile(d.htmlPath)
	if err != nil {
		http.Error(w, "index.html not found: "+err.Error(), 500)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write(data)
}

func (d *Daemon) handleGetConfig(w http.ResponseWriter, r *http.Request) {
	d.mu.Lock()
	defer d.mu.Unlock()

	// Return config matching to_json(include_secrets = false)
	cfgCopy := d.settings
	maskedConnectors := make([]Connector, len(cfgCopy.Connectors))
	for i, c := range cfgCopy.Connectors {
		c.TokenSet = len(c.Token) > 0
		c.RefreshTokenSet = len(c.RefreshToken) > 0
		c.UsernameSet = len(c.Username) > 0
		c.Token = ""
		c.RefreshToken = ""
		c.Username = ""
		maskedConnectors[i] = c
	}
	cfgCopy.Connectors = maskedConnectors

	maskedNetworks := make([]Network, len(cfgCopy.Networks))
	for i, n := range cfgCopy.Networks {
		n.PasswordSet = len(n.Password) > 0
		n.Password = ""
		maskedNetworks[i] = n
	}
	cfgCopy.Networks = maskedNetworks

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(cfgCopy)
}

func (d *Daemon) handlePostConfig(w http.ResponseWriter, r *http.Request) {
	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, `{"ok":false}`, 400)
		return
	}

	var newCfg Settings
	if err := json.Unmarshal(body, &newCfg); err != nil {
		http.Error(w, `{"ok":false}`, 400)
		return
	}

	d.mu.Lock()
	// Merge secrets from previous settings if not provided
	for i := range newCfg.Connectors {
		newC := &newCfg.Connectors[i]
		for _, prevC := range d.settings.Connectors {
			if prevC.ID == newC.ID {
				if newC.Token == "" {
					newC.Token = prevC.Token
				}
				if newC.RefreshToken == "" {
					newC.RefreshToken = prevC.RefreshToken
				}
				if newC.Username == "" {
					newC.Username = prevC.Username
				}
				if len(newC.Map) == 0 {
					newC.Map = prevC.Map
				}
				break
			}
		}
	}
	for i := range newCfg.Networks {
		newN := &newCfg.Networks[i]
		for _, prevN := range d.settings.Networks {
			if prevN.SSID == newN.SSID && newN.Password == "" {
				newN.Password = prevN.Password
				break
			}
		}
	}

	d.settings = newCfg
	// Save to config.json
	data, _ := json.MarshalIndent(d.settings, "", "  ")
	os.WriteFile(d.cfgPath, data, 0644)
	d.forceRedraw = true
	d.lastPolled = make(map[string]time.Time)
	d.mu.Unlock()

	go func() {
		d.pollAllDue()
		time.Sleep(500 * time.Millisecond)
		d.renderFrame()
	}()

	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(`{"ok":true}`))
}

func (d *Daemon) handleGetWidgets(w http.ResponseWriter, r *http.Request) {
	out, err := exec.Command(d.rendererPath, "-print-widgets").Output()
	if err != nil {
		http.Error(w, "error retrieving widgets", 500)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Write(out)
}

func (d *Daemon) handleGetStatus(w http.ResponseWriter, r *http.Request) {
	d.mu.Lock()
	defer d.mu.Unlock()

	type SlotStatus struct {
		OK         bool   `json:"ok"`
		Error      string `json:"error,omitempty"`
		Age        uint32 `json:"age"`
		TTL        uint32 `json:"ttl"`
		HistoryLen int    `json:"history_len"`
	}

	now := uint32(time.Now().Unix())
	slotsStatus := make(map[string]SlotStatus)
	for k, sl := range d.slots {
		age := uint32(0)
		if sl.At > 0 && now >= sl.At {
			age = now - sl.At
		}
		slotsStatus[k] = SlotStatus{
			OK:         sl.OK,
			Error:      sl.Error,
			Age:        age,
			TTL:        sl.TTL,
			HistoryLen: len(sl.History),
		}
	}

	resp := map[string]interface{}{
		"time_synced": true,
		"slots":       slotsStatus,
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
}

func (d *Daemon) handleScan(w http.ResponseWriter, r *http.Request) {
	type NetResult struct {
		SSID   string `json:"ssid"`
		RSSI   int    `json:"rssi"`
		Secure bool   `json:"secure"`
	}
	// Return current network or scan list
	nets := []NetResult{
		{SSID: "Current Connection", RSSI: readRSSI(), Secure: true},
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{
		"scanning": false,
		"networks": nets,
	})
}

func (d *Daemon) handleReboot(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(`{"ok":true}`))
	go func() {
		d.pollAllDue()
		d.renderFrame()
	}()
}

func (d *Daemon) handlePreview(w http.ResponseWriter, r *http.Request) {
	data, err := os.ReadFile(d.imgPath)
	if err != nil {
		http.Error(w, "Image not generated yet: "+err.Error(), 404)
		return
	}
	w.Header().Set("Content-Type", "image/png")
	w.Header().Set("Cache-Control", "no-cache, no-store, must-revalidate")
	w.Write(data)
}

// -----------------------------------------------------------------------------
// Main Entry Point
// -----------------------------------------------------------------------------

func main() {
	port := flag.Int("port", 8080, "Web Portal HTTP port")
	cfgPath := flag.String("config", "config.json", "Path to config.json")
	htmlPath := flag.String("html", "index.html", "Path to firmware/data/index.html")
	rendererPath := flag.String("renderer", "./tablo_render", "Path to tablo_render binary")
	imgPath := flag.String("img", "tablo_live.png", "Path to output PNG image")
	rotMode := flag.String("rot", "right", "Rotation mode: right (600x800 PB625), left, or none (800x600)")
	once := flag.Bool("once", false, "Fetch once, render once, and exit")
	flag.Parse()

	// Locate files if defaults don't exist in current dir
	if _, err := os.Stat(*cfgPath); os.IsNotExist(err) {
		candidates := []string{
			"/mnt/ext1/tablo_config.json",
			"/mnt/ext1/applications/.tablo/config.json",
			"pocketbook/default_config.json",
		}
		for _, c := range candidates {
			if _, err := os.Stat(c); err == nil {
				*cfgPath = c
				break
			}
		}
	}

	if _, err := os.Stat(*htmlPath); os.IsNotExist(err) {
		candidates := []string{
			"firmware/data/index.html",
			"/mnt/ext1/applications/.tablo/data/index.html",
		}
		for _, c := range candidates {
			if _, err := os.Stat(c); err == nil {
				*htmlPath = c
				break
			}
		}
	}

	if _, err := os.Stat(*rendererPath); os.IsNotExist(err) {
		candidates := []string{
			"./pocketbook/tablo_render_host",
			"./pocketbook/tablo_render_arm",
			"/mnt/ext1/applications/.tablo/bin/tablo_render",
		}
		for _, c := range candidates {
			if _, err := os.Stat(c); err == nil {
				*rendererPath = c
				break
			}
		}
	}

	log.Printf("Starting Tablo PocketBook Daemon...")
	log.Printf("Config: %s | Renderer: %s | Output: %s (rot: %s)", *cfgPath, *rendererPath, *imgPath, *rotMode)

	// Load settings
	var settings Settings
	cfgBytes, err := os.ReadFile(*cfgPath)
	if err == nil {
		json.Unmarshal(cfgBytes, &settings)
	}

	d := &Daemon{
		cfgPath:      *cfgPath,
		htmlPath:     *htmlPath,
		rendererPath: *rendererPath,
		imgPath:      *imgPath,
		rotMode:      *rotMode,
		port:         *port,
		settings:     settings,
		slots:        make(map[string]Slot),
		failed:       make(map[string]string),
		lastPolled:   make(map[string]time.Time),
	}

	// Initial poll and frame render
	d.pollAllDue()
	time.Sleep(2 * time.Second)
	if err := d.renderFrame(); err != nil {
		log.Printf("Warning during initial render: %v", err)
	}

	if *once {
		return
	}

	// Setup Web Portal HTTP Server
	http.HandleFunc("/", d.handleIndex)
	http.HandleFunc("/api/config", func(w http.ResponseWriter, r *http.Request) {
		if r.Method == "POST" {
			d.handlePostConfig(w, r)
		} else {
			d.handleGetConfig(w, r)
		}
	})
	http.HandleFunc("/api/widgets", d.handleGetWidgets)
	http.HandleFunc("/api/status", d.handleGetStatus)
	http.HandleFunc("/api/scan", d.handleScan)
	http.HandleFunc("/api/reboot", d.handleReboot)
	http.HandleFunc("/api/preview.png", d.handlePreview)

	server := &http.Server{
		Addr:    fmt.Sprintf(":%d", *port),
		Handler: nil,
	}

	go func() {
		log.Printf("Tablo Web Setup Portal listening on http://0.0.0.0:%d", *port)
		if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Fatalf("HTTP server error: %v", err)
		}
	}()

	// Background ticker loop
	ticker := time.NewTicker(30 * time.Second)
	defer ticker.Stop()
	lastHourFull := time.Now()

	for range ticker.C {
		d.pollAllDue()
		d.mu.Lock()
		force := d.forceRedraw
		d.forceRedraw = false
		d.mu.Unlock()

		if force || time.Since(lastHourFull) >= 5*time.Minute {
			lastHourFull = time.Now()
			if err := d.renderFrame(); err != nil {
				log.Printf("Render error: %v", err)
			}
		}
	}
}
