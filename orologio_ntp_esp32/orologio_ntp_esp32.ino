/*
  OROLOGIO_ESP32_16x64
  Orologio NTP su 2 matrici WS2812 8x32 (TOP = ora 5x7, BOTTOM = data 3x5)
  Web UI + API JSON + OTA + preferenze in NVS

  v2 - Modifiche rispetto alla versione precedente:
  - WiFi NON bloccante: niente piu' freeze di 15 s di display/web quando cade la rete
  - Ora letta con time() (nessun getLocalTime bloccante da 500 ms nel loop)
  - Ridisegno solo al cambio del secondo o di un'impostazione (meno carico, niente flicker)
  - Anteprima notte funziona anche con "Abilita Notte" disattivato
  - "--:--" e "--/--/--" finche' NTP non e' sincronizzato
  - Opzione lampeggio ":" configurabile
  - Pulsanti Accendi/Spegni, valori slider aggiornati in tempo reale
  - Pagina Info estesa (uptime, heap, ultimo sync NTP, MAC, firmware) con auto-refresh
  - API per Home Assistant: GET /api/status (JSON), GET/POST /set con aggiornamento parziale
  - OTA avviato alla prima connessione WiFi, password OTA opzionale
  v2.1:
  - Pagina /dispositivi: link fissi (Home Assistant, Raspberry Pi 5) + scansione mDNS
    dei dispositivi in rete (ESP con OTA, ESPHome, web server). Scansione in un task separato
    (non blocca il display), ripetuta ogni 5 minuti o a richiesta
  - L'orologio si annuncia in mDNS come servizio http (visibile agli altri dispositivi)
*/

#include <WiFi.h>
#include <time.h>
#include <esp_sntp.h>
#include <Adafruit_NeoPixel.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <vector>

// ======================= CONFIGURAZIONE =======================
const char *WIFI_SSID    = "LA_TUA_RETE_WIFI";   // <-- nome della tua rete
const char *WIFI_PASS    = "LA_TUA_PASSWORD";    // <-- password della tua rete
const char *HOSTNAME     = "Orologio-16x32";   // anche nome OTA e mDNS (http://orologio-16x32.local)
const char *OTA_PASSWORD = "";                 // "" = nessuna password OTA
const char *TZ_INFO      = "CET-1CEST,M3.5.0/2,M10.5.0/3";
const char *NTP1         = "it.pool.ntp.org";
const char *NTP2         = "pool.ntp.org";
const char *NTP3         = "time.google.com";

#define PIN_TOP    5
#define PIN_BOTTOM 18

const uint8_t  W = 32;
const uint8_t  H = 8;
const uint16_t NUMPIX = W * H;

const unsigned long WIFI_RETRY_MS    = 30000UL;  // nuovo tentativo WiFi ogni 30 s se scollegato
const unsigned long PREVIEW_NIGHT_MS = 8000UL;
const uint8_t       BRIGHT_NOSYNC    = 10;
const time_t        VALID_EPOCH      = 1700000000; // prima di questa data l'ora non e' valida
const uint16_t      MIN_PER_DAY      = 24 * 60;

// ======================= OGGETTI =======================
Adafruit_NeoPixel topM(NUMPIX, PIN_TOP, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel botM(NUMPIX, PIN_BOTTOM, NEO_GRB + NEO_KHZ800);
WebServer server(80);
Preferences prefs;

// ======================= IMPOSTAZIONI =======================
struct Settings {
  bool     displayOn   = true;
  uint8_t  brightDay   = 40;
  uint8_t  brightNight = 10;
  uint32_t colorTime   = 0x0096FF;
  uint32_t colorDate   = 0x00FF50;
  bool     nightEnable = true;
  bool     nightOff    = false;
  uint16_t nightStart  = 23 * 60;
  uint16_t nightEnd    = 7 * 60;
  bool     blinkColon  = true;
} cfg;

// ======================= STATO RUNTIME =======================
bool          previewActive   = false;
unsigned long previewStart    = 0;
int16_t       lastBrightness  = -1;
bool          lastBlanked     = false;
bool          forceRedraw     = true;
time_t        lastRenderEpoch = 0;
bool          otaStarted      = false;
bool          wasConnected    = false;
unsigned long wifiLastAttempt = 0;
volatile time_t lastNtpSync   = 0;

// ======================= DISPOSITIVI IN RETE =======================
// Link fissi mostrati in /dispositivi (modifica liberamente)
struct FixedLink {
  const char *name;
  const char *url;
};

const FixedLink FIXED_LINKS[] = {
  {"Home Assistant", "http://homeassistant.local:8123"},  // <-- esempi: metti i tuoi link
  {"Router", "http://192.168.1.1"},
};

// Servizi mDNS cercati in rete
struct MdnsService {
  const char *service;
  const char *label;
};

const MdnsService MDNS_SERVICES[] = {
  {"http", "Web"},
  {"arduino", "Arduino OTA"},
  {"esphomelib", "ESPHome"},
};

struct NetDevice {
  String name;
  IPAddress ip;
  uint16_t port;   // porta web (80 se non annunciata)
  String types;
};

const unsigned long SCAN_EVERY_MS = 300000UL;  // nuova scansione automatica ogni 5 minuti

std::vector<NetDevice> netDevices;
SemaphoreHandle_t devMutex = nullptr;
volatile bool scanRunning = false;
unsigned long lastScanDone = 0;
bool scanEverDone = false;

// ======================= FONT =======================
struct Glyph {
  char c;
  uint8_t col[5];
};

const Glyph font5x7[] = {
  {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
  {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
  {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
  {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
  {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
  {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
  {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
  {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
  {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
  {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
  {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
  {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
  {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
};

struct Glyph3x5 {
  char c;
  uint8_t col[3];
};

const Glyph3x5 font3x5[] = {
  {' ', {0x00, 0x00, 0x00}},
  {'/', {0x10, 0x08, 0x04}},
  {'-', {0x04, 0x04, 0x04}},
  {'0', {0x1F, 0x11, 0x1F}},
  {'1', {0x00, 0x1F, 0x00}},
  {'2', {0x1D, 0x15, 0x17}},
  {'3', {0x15, 0x15, 0x1F}},
  {'4', {0x07, 0x04, 0x1F}},
  {'5', {0x17, 0x15, 0x1D}},
  {'6', {0x1F, 0x15, 0x1D}},
  {'7', {0x01, 0x01, 0x1F}},
  {'8', {0x1F, 0x15, 0x1F}},
  {'9', {0x17, 0x15, 0x1F}},
};

const Glyph *getGlyph(char c) {
  for (const Glyph &g : font5x7)
    if (g.c == c) return &g;
  return &font5x7[0];
}

const Glyph3x5 *getGlyph3x5(char c) {
  for (const Glyph3x5 &g : font3x5)
    if (g.c == c) return &g;
  return &font3x5[0];
}

// ======================= UTILITA' COLORE / TEMPO =======================
uint32_t toNeo(Adafruit_NeoPixel &m, uint32_t rgb) {
  return m.Color((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

String rgbToHex(uint32_t rgb) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%06X", (unsigned int)(rgb & 0xFFFFFF));
  return String(buf);
}

bool parseHexColor(const String &hex, uint32_t &outRgb) {
  String s = hex;
  s.trim();
  if (s.startsWith("#")) s = s.substring(1);
  if (s.length() != 6) return false;
  for (uint8_t i = 0; i < 6; i++)
    if (!isxdigit((unsigned char)s[i])) return false;
  outRgb = (uint32_t)strtoul(s.c_str(), nullptr, 16);
  return true;
}

uint16_t parseTimeToMin(const String &hhmm, uint16_t fallback) {
  int colon = hhmm.indexOf(':');
  if (colon < 1) return fallback;
  int hh = constrain(hhmm.substring(0, colon).toInt(), 0, 23);
  int mm = constrain(hhmm.substring(colon + 1).toInt(), 0, 59);
  return (uint16_t)(hh * 60 + mm);
}

String minToTime(uint16_t m) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(m / 60), (unsigned)(m % 60));
  return String(buf);
}

// true se l'ora di sistema e' valida (NTP sincronizzato almeno una volta)
bool getTimeNow(struct tm &t) {
  time_t now = time(nullptr);
  if (now < VALID_EPOCH) return false;
  localtime_r(&now, &t);
  return true;
}

String fmtTime(const struct tm &t) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

String fmtDate(const struct tm &t) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%02d/%02d/%04d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
  return String(buf);
}

String uptimeStr() {
  unsigned long s = millis() / 1000UL;
  char buf[24];
  snprintf(buf, sizeof(buf), "%lug %02lu:%02lu:%02lu", s / 86400UL, (s / 3600UL) % 24UL, (s / 60UL) % 60UL, s % 60UL);
  return String(buf);
}

void onNtpSync(struct timeval *tv) {
  lastNtpSync = tv->tv_sec;
}

// ======================= NOTTE =======================
bool previewOn() {
  return previewActive && (millis() - previewStart < PREVIEW_NIGHT_MS);
}

bool isNightMinutes(uint16_t nowMin) {
  if (cfg.nightStart < cfg.nightEnd) return nowMin >= cfg.nightStart && nowMin < cfg.nightEnd;
  if (cfg.nightStart > cfg.nightEnd) return nowMin >= cfg.nightStart || nowMin < cfg.nightEnd;
  return true;  // inizio == fine -> notte sempre
}

bool nightActive(const struct tm *t) {
  if (previewOn()) return true;
  if (!cfg.nightEnable || t == nullptr) return false;
  return isNightMinutes((uint16_t)(t->tm_hour * 60 + t->tm_min));
}

// ======================= WIFI =======================
int wifiQualityPercent(int rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}

String wifiStatusText() {
  switch (WiFi.status()) {
    case WL_CONNECTED:      return "CONNESSO";
    case WL_NO_SSID_AVAIL:  return "SSID NON TROVATO";
    case WL_CONNECT_FAILED: return "CONNESSIONE FALLITA";
    case WL_IDLE_STATUS:    return "IDLE";
    case WL_DISCONNECTED:   return "DISCONNESSO";
    default:                return "SCONOSCIUTO";
  }
}

// ======================= DISPLAY =======================
uint16_t XY(uint8_t x, uint8_t y) {
  if (x & 1) return (uint16_t)x * H + (H - 1 - y);  // matrice a serpentina per colonne
  return (uint16_t)x * H + y;
}

void setPix(Adafruit_NeoPixel &m, int x, int y, uint32_t c) {
  if (x < 0 || y < 0 || x >= W || y >= H) return;
  m.setPixelColor(XY((uint8_t)x, (uint8_t)y), c);
}

void blankNow() {
  topM.clear();
  botM.clear();
  topM.show();
  botM.show();
  lastBlanked = true;
}

void blankOnce() {
  if (!lastBlanked) blankNow();
}

void applyBrightness(uint8_t b) {
  if (lastBrightness == b) return;
  topM.setBrightness(b);
  botM.setBrightness(b);
  lastBrightness = b;
}

int textWidth(const char *s, uint8_t advance) {
  size_t n = strlen(s);
  return n ? (int)(n * advance) - 1 : 0;
}

int centerX(const char *s, uint8_t advance) {
  int x = (W - textWidth(s, advance)) / 2;
  return x < 0 ? 0 : x;
}

void draw5x7(Adafruit_NeoPixel &m, int x, int y, const char *s, uint32_t c) {
  for (; *s; s++, x += 6) {
    const Glyph *g = getGlyph(*s);
    for (uint8_t col = 0; col < 5; col++)
      for (uint8_t row = 0; row < 7; row++)
        if (g->col[col] & (1 << row)) setPix(m, x + col, y + row, c);
  }
}

void draw3x5(Adafruit_NeoPixel &m, int x, int y, const char *s, uint32_t c) {
  for (; *s; s++, x += 4) {
    const Glyph3x5 *g = getGlyph3x5(*s);
    for (uint8_t col = 0; col < 3; col++)
      for (uint8_t row = 0; row < 5; row++)
        if (g->col[col] & (1 << row)) setPix(m, x + col, y + row, c);
  }
}

void renderClock() {
  if (!cfg.displayOn) {
    blankOnce();
    return;
  }

  struct tm t;
  bool synced = getTimeNow(t);
  char timeStr[12];
  char dateStr[16];
  uint32_t cTime, cDate;

  if (!synced) {
    applyBrightness(BRIGHT_NOSYNC);
    bool colon = (millis() / 1000UL) % 2 == 0;
    snprintf(timeStr, sizeof(timeStr), "--%c--", colon ? ':' : ' ');
    snprintf(dateStr, sizeof(dateStr), "--/--/--");
    cTime = cDate = topM.Color(60, 60, 60);
  } else {
    bool night = nightActive(&t);
    if (night && cfg.nightOff) {
      blankOnce();
      return;
    }
    applyBrightness(night ? cfg.brightNight : cfg.brightDay);
    char sep = (!cfg.blinkColon || (t.tm_sec % 2 == 0)) ? ':' : ' ';
    snprintf(timeStr, sizeof(timeStr), "%02d%c%02d", t.tm_hour, sep, t.tm_min);
    snprintf(dateStr, sizeof(dateStr), "%02d/%02d/%02d", t.tm_mday, t.tm_mon + 1, (t.tm_year + 1900) % 100);
    cTime = toNeo(topM, cfg.colorTime);
    cDate = toNeo(botM, cfg.colorDate);
  }

  topM.clear();
  draw5x7(topM, centerX(timeStr, 6), 1, timeStr, cTime);
  botM.clear();
  draw3x5(botM, centerX(dateStr, 4), 1, dateStr, cDate);
  topM.show();
  botM.show();
  lastBlanked = false;
}

// ======================= PREFERENZE =======================
void loadPrefs() {
  prefs.begin("clock", true);
  cfg.brightDay   = prefs.getUChar("bday", 40);
  cfg.brightNight = prefs.getUChar("bnit", 10);
  cfg.colorTime   = prefs.getUInt("ct", 0x0096FF) & 0xFFFFFF;
  cfg.colorDate   = prefs.getUInt("cd", 0x00FF50) & 0xFFFFFF;
  cfg.displayOn   = prefs.getBool("on", true);
  cfg.nightEnable = prefs.getBool("nen", true);
  cfg.nightOff    = prefs.getBool("noff", false);
  cfg.nightStart  = prefs.getUShort("nsta", 23 * 60) % MIN_PER_DAY;
  cfg.nightEnd    = prefs.getUShort("nend", 7 * 60) % MIN_PER_DAY;
  cfg.blinkColon  = prefs.getBool("blk", true);
  prefs.end();
}

void savePrefs() {
  prefs.begin("clock", false);
  prefs.putUChar("bday", cfg.brightDay);
  prefs.putUChar("bnit", cfg.brightNight);
  prefs.putUInt("ct", cfg.colorTime);
  prefs.putUInt("cd", cfg.colorDate);
  prefs.putBool("on", cfg.displayOn);
  prefs.putBool("nen", cfg.nightEnable);
  prefs.putBool("noff", cfg.nightOff);
  prefs.putUShort("nsta", cfg.nightStart);
  prefs.putUShort("nend", cfg.nightEnd);
  prefs.putBool("blk", cfg.blinkColon);
  prefs.end();
}

// ======================= WEB =======================
const char CSS[] =
  "<style>"
  "body{font-family:system-ui,Arial;margin:16px;max-width:820px;background:#fafafa;color:#111}"
  ".card{padding:14px;border:1px solid #ddd;border-radius:12px;margin:12px 0;background:#fff}"
  "label{display:flex;justify-content:space-between;align-items:center;margin:10px 0;gap:12px;flex-wrap:wrap}"
  "input[type=color]{width:72px;height:40px;border:0;background:none}"
  "input[type=range]{width:100%;max-width:420px}"
  "input[type=checkbox]{width:22px;height:22px}"
  ".row{display:flex;gap:10px;flex-wrap:wrap;margin-top:12px}"
  "button,a.btn{padding:10px 14px;border-radius:10px;border:1px solid #333;background:#111;color:#fff;cursor:pointer;text-decoration:none;display:inline-block;font-size:1em}"
  "button.alt{background:#fff;color:#111}"
  ".muted{color:#555;font-size:.95em}"
  ".pill{padding:4px 10px;border:1px solid #ccc;border-radius:999px;display:inline-block;background:#fff}"
  ".k{color:#555;font-size:.95em}.v{font-weight:600}"
  ".grid{display:grid;grid-template-columns:1fr;gap:10px}"
  "@media(min-width:720px){.grid{grid-template-columns:1fr 1fr}}"
  "hr{border:none;border-top:1px solid #eee;margin:14px 0}"
  "#msg{margin-top:10px;min-height:1.2em}"
  "table{width:100%;border-collapse:collapse;margin-top:10px}"
  "th,td{text-align:left;padding:8px 6px;border-bottom:1px solid #eee;vertical-align:top}"
  "th{color:#555;font-weight:600}"
  "a{color:#0a58ca}"
  "</style>";

const char *checked(bool v) { return v ? "checked" : ""; }

String headHtml(const char *title, uint8_t refreshSec) {
  String s;
  s.reserve(1500);
  s += F("<!doctype html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>");
  if (refreshSec) {
    s += F("<meta http-equiv='refresh' content='");
    s += refreshSec;
    s += F("'>");
  }
  s += F("<title>");
  s += title;
  s += F("</title>");
  s += CSS;
  s += F("</head><body>");
  return s;
}

String htmlPage() {
  struct tm t;
  bool synced = getTimeNow(t);
  bool night = nightActive(synced ? &t : nullptr);
  String ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("non connesso");

  String page = headHtml("Orologio ESP32", 0);
  page.reserve(6500);

  page += F("<h2>Orologio NTP - ESP32</h2><div class='muted'>IP <span class='pill'>");
  page += ip;
  page += F("</span> WiFi <span class='pill'>");
  page += wifiStatusText();
  page += F("</span> Display <span class='pill'>");
  page += cfg.displayOn ? F("ACCESO") : F("SPENTO");
  page += F("</span> Notte <span class='pill'>");
  page += night ? F("SI") : F("NO");
  page += F("</span></div>");

  page += F("<form method='POST' action='/set'><input type='hidden' name='form' value='1'><div class='card'>");

  page += F("<h3>Display</h3><label>Acceso <input type='checkbox' name='on' value='1' ");
  page += checked(cfg.displayOn);
  page += F("></label><label>Lampeggio due punti <input type='checkbox' name='blk' value='1' ");
  page += checked(cfg.blinkColon);
  page += F("></label><label>Luminosit&agrave; giorno <span class='pill' id='vbd'>");
  page += cfg.brightDay;
  page += F("</span></label><input type='range' min='0' max='255' name='bday' value='");
  page += cfg.brightDay;
  page += F("' oninput=\"document.getElementById('vbd').textContent=this.value\">");

  page += F("<hr><h3>Colori</h3><label>Colore ora (TOP) <input type='color' name='ct' value='");
  page += rgbToHex(cfg.colorTime);
  page += F("'></label><label>Colore data (BOTTOM) <input type='color' name='cd' value='");
  page += rgbToHex(cfg.colorDate);
  page += F("'></label>");

  page += F("<hr><h3>Modalit&agrave; notte</h3><label>Abilita notte <input type='checkbox' name='nen' value='1' ");
  page += checked(cfg.nightEnable);
  page += F("></label><label>Inizio <input type='time' name='nsta' value='");
  page += minToTime(cfg.nightStart);
  page += F("'></label><label>Fine <input type='time' name='nend' value='");
  page += minToTime(cfg.nightEnd);
  page += F("'></label><label>Spegni di notte <input type='checkbox' name='noff' value='1' ");
  page += checked(cfg.nightOff);
  page += F("></label><label>Luminosit&agrave; notte <span class='pill' id='vbn'>");
  page += cfg.brightNight;
  page += F("</span></label><input type='range' min='0' max='255' name='bnit' value='");
  page += cfg.brightNight;
  page += F("' oninput=\"document.getElementById('vbn').textContent=this.value\">");

  page += F("<div class='row'><button type='submit'>Salva</button>"
            "<button type='button' class='alt' onclick=\"fetch('/previewNight').then(()=>{msg.textContent='Anteprima notte per 8 secondi'})\">Anteprima notte</button>"
            "<button type='button' class='alt' onclick=\"fetch('/on').then(()=>location.reload())\">Accendi</button>"
            "<button type='button' class='alt' onclick=\"fetch('/off').then(()=>location.reload())\">Spegni</button>"
            "<a class='btn' href='/dispositivi'>Dispositivi in rete</a>"
            "<a class='btn' href='/info'>Info</a></div><div id='msg' class='muted'></div>"
            "</div></form></body></html>");
  return page;
}

void addInfoRow(String &p, const __FlashStringHelper *k, const String &v) {
  p += F("<div><div class='k'>");
  p += k;
  p += F("</div><div class='v'>");
  p += v;
  p += F("</div></div>");
}

String infoPage() {
  struct tm t;
  bool synced = getTimeNow(t);
  bool conn = WiFi.status() == WL_CONNECTED;
  int rssi = conn ? WiFi.RSSI() : -127;

  String ntp = "non sincronizzato";
  if (lastNtpSync > 0) {
    struct tm ts;
    time_t ls = lastNtpSync;
    localtime_r(&ls, &ts);
    ntp = "ultimo sync " + fmtDate(ts) + " " + fmtTime(ts);
  } else if (synced) {
    ntp = "sincronizzato";
  }

  String page = headHtml("Info", 5);
  page.reserve(4500);
  page += F("<h2>Info</h2><div class='card'><div class='grid'>");
  addInfoRow(page, F("Ora"), synced ? fmtTime(t) : String("--:--:--"));
  addInfoRow(page, F("Data"), synced ? fmtDate(t) : String("-"));
  addInfoRow(page, F("NTP"), ntp);
  addInfoRow(page, F("Notte attiva"), nightActive(synced ? &t : nullptr) ? "SI" : "NO");
  addInfoRow(page, F("Display"), cfg.displayOn ? "ACCESO" : "SPENTO");
  addInfoRow(page, F("Luminosit&agrave; attuale"), lastBrightness < 0 ? String("-") : String(lastBrightness));
  addInfoRow(page, F("WiFi SSID"), conn ? WiFi.SSID() : String("-"));
  addInfoRow(page, F("WiFi stato"), wifiStatusText());
  addInfoRow(page, F("IP"), conn ? WiFi.localIP().toString() : String("-"));
  addInfoRow(page, F("RSSI"), conn ? String(rssi) + " dBm (" + String(wifiQualityPercent(rssi)) + "%)" : String("-"));
  addInfoRow(page, F("MAC"), WiFi.macAddress());
  addInfoRow(page, F("Hostname"), String(HOSTNAME));
  addInfoRow(page, F("Uptime"), uptimeStr());
  addInfoRow(page, F("Heap libero"), String(ESP.getFreeHeap() / 1024) + " kB");
  addInfoRow(page, F("Firmware"), String(__DATE__) + " " + String(__TIME__));
  addInfoRow(page, F("SDK"), String(ESP.getSdkVersion()));
  page += F("</div></div><div class='row'><a class='btn' href='/'>Home</a><a class='btn' href='/dispositivi'>Dispositivi in rete</a><a class='btn' href='/info'>Aggiorna</a></div>"
            "<div class='muted'>Aggiornamento automatico ogni 5 s</div></body></html>");
  return page;
}

String statusJson() {
  struct tm t;
  bool synced = getTimeNow(t);
  bool conn = WiFi.status() == WL_CONNECTED;

  String j;
  j.reserve(420);
  j += F("{\"on\":");          j += cfg.displayOn ? "true" : "false";
  j += F(",\"synced\":");      j += synced ? "true" : "false";
  j += F(",\"night\":");       j += nightActive(synced ? &t : nullptr) ? "true" : "false";
  j += F(",\"time\":\"");      j += synced ? fmtTime(t) : String("");
  j += F("\",\"date\":\"");    j += synced ? fmtDate(t) : String("");
  j += F("\",\"bday\":");      j += cfg.brightDay;
  j += F(",\"bnit\":");        j += cfg.brightNight;
  j += F(",\"ct\":\"");        j += rgbToHex(cfg.colorTime);
  j += F("\",\"cd\":\"");      j += rgbToHex(cfg.colorDate);
  j += F("\",\"nen\":");       j += cfg.nightEnable ? "true" : "false";
  j += F(",\"noff\":");        j += cfg.nightOff ? "true" : "false";
  j += F(",\"nsta\":\"");      j += minToTime(cfg.nightStart);
  j += F("\",\"nend\":\"");    j += minToTime(cfg.nightEnd);
  j += F("\",\"blk\":");       j += cfg.blinkColon ? "true" : "false";
  j += F(",\"ip\":\"");        j += conn ? WiFi.localIP().toString() : String("");
  j += F("\",\"rssi\":");      j += conn ? WiFi.RSSI() : 0;
  j += F(",\"uptime\":");      j += (uint32_t)(millis() / 1000UL);
  j += F(",\"heap\":");        j += (uint32_t)ESP.getFreeHeap();
  j += F("}");
  return j;
}

void sendJsonStatus() {
  server.send(200, "application/json", statusJson());
}

void handleRoot() { server.send(200, "text/html; charset=utf-8", htmlPage()); }
void handleInfo() { server.send(200, "text/html; charset=utf-8", infoPage()); }

// Form web (campo "form"): checkbox assente = false.
// Chiamata API (senza "form"): si modificano solo i parametri presenti (es. /set?on=0&bday=80).
void handleSet() {
  const bool isForm = server.hasArg("form");

  auto argBool = [&](const char *name, bool current) -> bool {
    if (isForm) return server.hasArg(name);
    if (!server.hasArg(name)) return current;
    String v = server.arg(name);
    v.toLowerCase();
    return v == "1" || v == "true" || v == "on";
  };

  cfg.displayOn   = argBool("on", cfg.displayOn);
  cfg.blinkColon  = argBool("blk", cfg.blinkColon);
  cfg.nightEnable = argBool("nen", cfg.nightEnable);
  cfg.nightOff    = argBool("noff", cfg.nightOff);

  if (server.hasArg("bday")) cfg.brightDay   = (uint8_t)constrain(server.arg("bday").toInt(), 0, 255);
  if (server.hasArg("bnit")) cfg.brightNight = (uint8_t)constrain(server.arg("bnit").toInt(), 0, 255);

  uint32_t rgb;
  if (server.hasArg("ct") && parseHexColor(server.arg("ct"), rgb)) cfg.colorTime = rgb;
  if (server.hasArg("cd") && parseHexColor(server.arg("cd"), rgb)) cfg.colorDate = rgb;

  if (server.hasArg("nsta")) cfg.nightStart = parseTimeToMin(server.arg("nsta"), cfg.nightStart);
  if (server.hasArg("nend")) cfg.nightEnd   = parseTimeToMin(server.arg("nend"), cfg.nightEnd);

  savePrefs();
  forceRedraw = true;

  if (isForm) {
    server.sendHeader("Location", "/");
    server.send(303);
  } else {
    sendJsonStatus();
  }
}

void handleOn() {
  cfg.displayOn = true;
  savePrefs();
  forceRedraw = true;
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleOff() {
  cfg.displayOn = false;
  savePrefs();
  forceRedraw = true;
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handlePreviewNight() {
  previewActive = true;
  previewStart = millis();
  forceRedraw = true;
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleNotFound() {
  server.send(404, "text/plain; charset=utf-8", "Pagina non trovata");
}

// ======================= SCANSIONE mDNS =======================
void addOrMerge(std::vector<NetDevice> &list, const String &name, IPAddress ip, uint16_t port, const char *label, bool isHttp) {
  for (NetDevice &d : list) {
    if (d.ip == ip) {
      if (d.types.indexOf(label) < 0) d.types += String(", ") + label;
      if (isHttp) d.port = port;
      if (d.name.length() == 0) d.name = name;
      return;
    }
  }
  NetDevice nd;
  nd.name = name;
  nd.ip = ip;
  nd.port = isHttp ? port : 80;
  nd.types = label;
  list.push_back(nd);
}

void scanTask(void *) {
  std::vector<NetDevice> found;
  for (const MdnsService &s : MDNS_SERVICES) {
    int n = MDNS.queryService(s.service, "tcp");
    for (int i = 0; i < n; i++) {
      IPAddress ip = MDNS.address(i);
      if (ip == IPAddress(0, 0, 0, 0)) continue;
      addOrMerge(found, MDNS.hostname(i), ip, MDNS.port(i), s.label, strcmp(s.service, "http") == 0);
    }
  }
  std::sort(found.begin(), found.end(), [](const NetDevice &a, const NetDevice &b) {
    return (uint32_t)a.ip[3] < (uint32_t)b.ip[3];
  });

  if (xSemaphoreTake(devMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    netDevices = found;
    xSemaphoreGive(devMutex);
  }
  Serial.printf("mDNS: trovati %u dispositivi\n", (unsigned)found.size());
  lastScanDone = millis();
  scanEverDone = true;
  scanRunning = false;
  vTaskDelete(nullptr);
}

void startScan() {
  if (scanRunning || !otaStarted || WiFi.status() != WL_CONNECTED) return;
  scanRunning = true;
  if (xTaskCreate(scanTask, "mdnsScan", 8192, nullptr, 1, nullptr) != pdPASS) scanRunning = false;
}

String devicesPage() {
  String page = headHtml("Dispositivi in rete", scanRunning ? 3 : 0);
  page.reserve(5000);
  page += F("<h2>Dispositivi in rete</h2>");

  page += F("<div class='card'><h3>Collegamenti</h3><div class='grid'>");
  for (const FixedLink &l : FIXED_LINKS) {
    page += F("<div><div class='k'>");
    page += l.url;
    page += F("</div><div class='v'><a href='");
    page += l.url;
    page += F("' target='_blank'>");
    page += l.name;
    page += F("</a></div></div>");
  }
  page += F("</div></div>");

  page += F("<div class='card'><h3>Trovati in rete (mDNS)</h3>");
  if (scanRunning) {
    page += F("<div class='muted'>Scansione in corso...</div>");
  } else if (!scanEverDone) {
    page += F("<div class='muted'>Nessuna scansione ancora eseguita.</div>");
  } else {
    page += F("<div class='muted'>Ultima scansione ");
    page += (millis() - lastScanDone) / 1000UL;
    page += F(" s fa</div>");
  }

  IPAddress self = WiFi.localIP();
  page += F("<table><tr><th>Nome</th><th>IP</th><th>Tipo</th></tr>");
  size_t count = 0;
  if (xSemaphoreTake(devMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    for (const NetDevice &d : netDevices) {
      String url = "http://" + d.ip.toString();
      if (d.port != 80) url += ":" + String(d.port);
      page += F("<tr><td><a href='");
      page += url;
      page += F("' target='_blank'>");
      page += d.name.length() ? d.name : d.ip.toString();
      page += F("</a>");
      if (d.ip == self) page += F(" <span class='pill'>questo</span>");
      page += F("</td><td>");
      page += d.ip.toString();
      if (d.port != 80) {
        page += ':';
        page += d.port;
      }
      page += F("</td><td>");
      page += d.types;
      page += F("</td></tr>");
      count++;
    }
    xSemaphoreGive(devMutex);
  }
  if (count == 0 && !scanRunning) page += F("<tr><td colspan='3' class='muted'>Nessun dispositivo trovato</td></tr>");
  page += F("</table></div>");

  page += F("<div class='row'><a class='btn' href='/'>Home</a><a class='btn' href='/dispositivi?scan=1'>Nuova scansione</a>"
            "<a class='btn' href='/info'>Info</a></div>"
            "<div class='muted'>Vengono trovati i dispositivi che si annunciano in mDNS (ESP32/ESP8266 con OTA, ESPHome, stampanti, NAS...).</div>"
            "</body></html>");
  return page;
}

void handleDevices() {
  if (server.hasArg("scan")) {
    startScan();
    server.sendHeader("Location", "/dispositivi");
    server.send(303);
    return;
  }
  server.send(200, "text/html; charset=utf-8", devicesPage());
}

void startWeb() {
  server.on("/dispositivi", HTTP_GET, handleDevices);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/info", HTTP_GET, handleInfo);
  server.on("/set", HTTP_ANY, handleSet);
  server.on("/on", HTTP_GET, handleOn);
  server.on("/off", HTTP_GET, handleOff);
  server.on("/previewNight", HTTP_GET, handlePreviewNight);
  server.on("/api/status", HTTP_GET, sendJsonStatus);
  server.onNotFound(handleNotFound);
  server.begin();
}

// ======================= OTA =======================
void setupOTA() {
  ArduinoOTA.setHostname(HOSTNAME);
  if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    Serial.println("OTA: inizio");
    blankNow();
  });
  ArduinoOTA.onEnd([]() { Serial.println("\nOTA: fine"); });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    Serial.printf("OTA: %u%%\r", total ? (done * 100U / total) : 0U);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA errore %u\n", (unsigned)error);
    forceRedraw = true;
    lastBlanked = true;
  });
  ArduinoOTA.begin();          // avvia anche mDNS con HOSTNAME
  MDNS.addService("http", "tcp", 80);
  otaStarted = true;
  Serial.printf("OTA pronto: %s\n", HOSTNAME);
}

// ======================= WIFI (NON BLOCCANTE) =======================
void wifiStart() {
  WiFi.persistent(false);
  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);         // risposta web/OTA piu' reattiva
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  wifiLastAttempt = millis();
  Serial.printf("WiFi: connessione a %s...\n", WIFI_SSID);
}

void wifiLoop() {
  bool conn = (WiFi.status() == WL_CONNECTED);

  if (conn && !wasConnected) {
    wasConnected = true;
    Serial.printf("WiFi connesso - IP: %s  RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    if (!otaStarted) setupOTA();
    startScan();
  } else if (!conn && wasConnected) {
    wasConnected = false;
    wifiLastAttempt = millis();  // lascia lavorare l'autoreconnect prima di forzare
    Serial.println("WiFi perso");
  }

  if (!conn && (millis() - wifiLastAttempt >= WIFI_RETRY_MS)) {
    Serial.println("WiFi: nuovo tentativo");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    wifiLastAttempt = millis();
  }
}

// ======================= SETUP / LOOP =======================
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\nOrologio ESP32 v2.1");

  loadPrefs();
  devMutex = xSemaphoreCreateMutex();

  topM.begin();
  botM.begin();
  applyBrightness(cfg.brightDay);
  blankNow();

  wifiStart();
  sntp_set_time_sync_notification_cb(onNtpSync);
  configTzTime(TZ_INFO, NTP1, NTP2, NTP3);
  startWeb();

  renderClock();
}

void loop() {
  server.handleClient();
  if (otaStarted) ArduinoOTA.handle();
  wifiLoop();

  if (previewActive && !previewOn()) {
    previewActive = false;
    forceRedraw = true;
  }

  time_t now = time(nullptr);
  if (forceRedraw || now != lastRenderEpoch) {
    forceRedraw = false;
    lastRenderEpoch = now;
    renderClock();
  }

  if (scanEverDone && millis() - lastScanDone >= SCAN_EVERY_MS) startScan();

  delay(2);
}
