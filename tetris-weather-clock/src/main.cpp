/*******************************************************************
  Tetris Weather Clock
  ESP32 WROOM + pannello HUB75 64x32 (piedinatura ESP32-Trinity)

  - Ora grande (cifre 12x28 px, tratto spesso) fatta di tetramini veri
    che cadono, ruotano e si impilano; velocita' regolabile dal web
  - Citta' multiple: la prima mostra ora grande + meteo, dalla seconda in
    poi ogni citta' ha una sola schermata con meteo e ora locale
  - Pagine con messaggi scorrevoli, aggiungibili e rimovibili dal web
  - Scorrimento tra le pagine con velocita' regolabile
  - Configurazione WiFi con portale captive (WiFiManager)
  - Ora sincronizzata via NTP, fuso orario preso dalla prima citta'
  - Meteo da Open-Meteo (nessuna chiave API necessaria)
*******************************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Adafruit_GFX.h>
#include <time.h>
#include <ctype.h>
#include <vector>
#include "TetrisEngine.h"
#include "TextUtil.h"

// ===================== Piedinatura ESP32-Trinity =====================
// Se il tuo cablaggio e' diverso, cambia solo questi valori.
#define PIN_R1 25
#define PIN_G1 26
#define PIN_B1 27
#define PIN_R2 14
#define PIN_G2 12
#define PIN_B2 13
#define PIN_A 23
#define PIN_B 19
#define PIN_C 5
#define PIN_D 17
#define PIN_E 18
#define PIN_LAT 4
#define PIN_OE 15
#define PIN_CLK 16
#define PIN_LDR 35  // sensore di luce della Trinity (opzionale)

#define PANEL_W 64
#define PANEL_H 32

// Fuso orario usato finche' non e' nota la citta' (Italia)
#define DEFAULT_TZ "CET-1CEST,M3.5.0,M10.5.0/3"
#define AP_NAME "TetrisClock"
#define HOSTNAME "tetrisclock"
#define WEATHER_REFRESH_MS 600000UL  // 10 minuti

#define MAXC 6  // citta' massime
#define MAXM 6  // messaggi massimi
#define LANG_N 5  // lingue: it, en, es, fr, de

// Posizione x (pixel) delle 4 cifre dell'ora: HH:MM (cifre larghe 12, due punti a x=30..33)
static const int SLOT_X[4] = {2, 16, 36, 50};

// ===================== Tipi (tutti prima della prima funzione) =====================
struct Settings {
  bool fahrenheit = false;
  bool h12 = false;
  uint8_t brightness = 90;
  bool autoBright = false;
  uint16_t timeSecs = 20;  // secondi pagina ora
  uint16_t wxSecs = 8;     // secondi pagina meteo della citta' principale
  uint16_t citySecs = 8;   // secondi di ogni altra citta'
  uint8_t speed = 6;       // caduta blocchi: 1 lenta ... 10 velocissima
  uint8_t trSpeed = 5;     // scorrimento tra pagine: 1 lento ... 10 veloce
  uint8_t msgSpeed = 5;    // scorrimento messaggi: 1 lento ... 10 veloce
  bool nightOn = false;    // luminosita' a orari (giorno/notte)
  uint16_t nightStart = 22 * 60;  // inizio notte, minuti dalla mezzanotte
  uint16_t nightEnd = 7 * 60;     // fine notte
  uint8_t nightBr = 15;           // luminosita' di notte
  uint8_t lang = 0;               // 0 it, 1 en, 2 es, 3 fr, 4 de
};

struct CityCfg {
  String name;
  String cc;  // codice ISO a 2 lettere, facoltativo
};

struct MsgCfg {
  String text;  // testo originale (UTF-8)
  String disp;  // testo mostrato (solo ASCII)
  uint32_t rgb = 0xFFFFFF;
  uint8_t size = 1;  // 1 = piccolo, 2 = grande
};

struct Weather {
  bool valid = false;
  float temp = 0;
  int hum = 0;
  int code = 0;
  bool isDay = true;
  char name[48] = "";  // nome citta' (solo ASCII)
  char cc[4] = "";     // codice paese
  char err[40] = "";
  int utcOffset = 0;
  bool haveOffset = false;
  uint32_t updated = 0;
};

struct GeoEnt {
  String key;
  float lat = 0, lon = 0;
  char name[48] = "";
  char cc[4] = "";
  bool bad = false;
};

enum PType { PG_TIME, PG_WX, PG_CITY, PG_MSG };
struct Page {
  PType t;
  int i;
};
enum Scene { S_CLEAR, S_PARTLY, S_CLOUDY, S_FOG, S_RAIN, S_DRIZZLE, S_SNOW, S_STORM };

struct WifiJob {
  bool pending = false;
  String ssid;
  String pass;
};

struct Trans {
  bool on;
  Page from, to;
  uint32_t start;
};

// ===================== Stato globale =====================
MatrixPanel_I2S_DMA *display = nullptr;
GFXcanvas16 cvA(PANEL_W, PANEL_H), cvB(PANEL_W, PANEL_H);  // le due pagine durante lo scorrimento
Adafruit_GFX *g = nullptr;                                  // dove disegnano le pagine (pannello o canvas)
WebServer server(80);
Preferences prefs;
SemaphoreHandle_t mtx;

Settings cfg;
CityCfg cities[MAXC];
int nCity = 0;
MsgCfg msgs[MAXM];
int nMsg = 0;
Weather wxs[MAXC];

static const char *LANG_CODE[LANG_N] = {"it", "en", "es", "fr", "de"};
bool langKnown = false;  // false al primo avvio: la lingua si sceglie nel portale WiFi
volatile bool geoClear = false;
volatile bool wxRefresh = true;
volatile bool replayFlag = false;
bool forceIntro = true;
bool needDraw = true;

Page cur = {PG_TIME, 0};
uint32_t pageSince = 0;
Trans tr = {false, {PG_TIME, 0}, {PG_TIME, 0}, 0};

WifiJob wifiJob;
volatile int wifiState = 0;  // 0 nessuna richiesta, 1 in corso, 2 collegato, 3 fallito
String wifiMsg = "";

tet::Engine engine;
uint32_t tickNow = 0, tickAcc = 0, lastClockMs = 0;

// ===================== Utilita' =====================
static inline uint16_t RGB(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static inline uint16_t rgb24(uint32_t v) { return RGB((v >> 16) & 255, (v >> 8) & 255, v & 255); }

void flip() { display->flipDMABuffer(); }

static inline void put(int x, int y, uint16_t c) {
  if ((unsigned)x < PANEL_W && (unsigned)y < PANEL_H) g->drawPixel(x, y, c);
}

void showLines(const char *l1, const char *l2 = "", const char *l3 = "", const char *l4 = "") {
  display->clearScreen();
  display->setTextSize(1);
  display->setTextWrap(false);
  display->setTextColor(RGB(255, 255, 255));
  const char *L[4] = {l1, l2, l3, l4};
  for (int i = 0; i < 4; i++) {
    display->setCursor(-1, i * 8);
    display->print(L[i]);
  }
  flip();
}

void marquee(const String &msg, uint16_t col, int y) {
  display->setTextSize(1);
  display->setTextWrap(false);
  int w = msg.length() * 6;
  for (int x = PANEL_W; x > -w; x--) {
    display->clearScreen();
    display->setTextColor(col);
    display->setCursor(x, y);
    display->print(msg);
    flip();
    delay(22);
  }
}

String urlEncode(const String &s) {
  String o;
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      o += (char)c;
    } else {
      o += '%';
      o += hex[c >> 4];
      o += hex[c & 15];
    }
  }
  return o;
}

// ===================== Impostazioni =====================
void foldMsg(MsgCfg &m) {
  char buf[200];
  asciiFold(m.text.c_str(), buf, sizeof(buf));
  m.disp = buf;
}

void saveCities() {
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < nCity; i++) {
    JsonObject o = a.add<JsonObject>();
    o["n"] = cities[i].name;
    o["c"] = cities[i].cc;
  }
  String out;
  serializeJson(d, out);
  prefs.putString("cities", out);
}

void saveMsgs() {
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < nMsg; i++) {
    JsonObject o = a.add<JsonObject>();
    o["t"] = msgs[i].text;
    o["c"] = msgs[i].rgb;
    o["s"] = msgs[i].size;
  }
  String out;
  serializeJson(d, out);
  prefs.putString("msgs", out);
}

void loadSettings() {
  prefs.begin("clock", false);
  cfg.fahrenheit = prefs.getBool("f", false);
  cfg.h12 = prefs.getBool("h12", false);
  cfg.brightness = prefs.getUChar("br", 90);
  cfg.autoBright = prefs.getBool("auto", false);
  cfg.timeSecs = prefs.getUShort("tsec", 20);
  cfg.wxSecs = prefs.getUShort("wsec", 8);
  cfg.citySecs = prefs.getUShort("csec", 8);
  cfg.speed = constrain(prefs.getUChar("spd", 6), 1, 10);
  cfg.trSpeed = constrain(prefs.getUChar("trspd", 5), 1, 10);
  cfg.msgSpeed = constrain(prefs.getUChar("mspd", 5), 1, 10);
  cfg.nightOn = prefs.getBool("non", false);
  cfg.nightStart = constrain(prefs.getUShort("nst", 22 * 60), 0, 1439);
  cfg.nightEnd = constrain(prefs.getUShort("nen", 7 * 60), 0, 1439);
  cfg.nightBr = constrain(prefs.getUChar("nbr", 15), 1, 255);
  langKnown = prefs.isKey("lang");
  cfg.lang = constrain(prefs.getUChar("lang", 0), 0, LANG_N - 1);

  nCity = 0;
  String cj = prefs.getString("cities", "");
  if (cj.length() > 0) {
    JsonDocument d;
    if (!deserializeJson(d, cj)) {
      for (JsonObject o : d.as<JsonArray>()) {
        if (nCity >= MAXC) break;
        cities[nCity].name = o["n"] | "";
        cities[nCity].cc = o["c"] | "";
        if (cities[nCity].name.length() > 0) nCity++;
      }
    }
  } else {
    // versione precedente: una sola citta'
    String c = prefs.getString("city", "");
    if (c.length() > 0) {
      cities[0].name = c;
      cities[0].cc = prefs.getString("country", "");
      nCity = 1;
    }
  }

  nMsg = 0;
  String mj = prefs.getString("msgs", "");
  if (mj.length() > 0) {
    JsonDocument d;
    if (!deserializeJson(d, mj)) {
      for (JsonObject o : d.as<JsonArray>()) {
        if (nMsg >= MAXM) break;
        msgs[nMsg].text = o["t"] | "";
        msgs[nMsg].rgb = o["c"] | 0xFFFFFFu;
        msgs[nMsg].size = constrain((int)(o["s"] | 1), 1, 2);
        foldMsg(msgs[nMsg]);
        if (msgs[nMsg].disp.length() > 0) nMsg++;
      }
    }
  }
}

void saveSettings() {
  prefs.putBool("f", cfg.fahrenheit);
  prefs.putBool("h12", cfg.h12);
  prefs.putUChar("br", cfg.brightness);
  prefs.putBool("auto", cfg.autoBright);
  prefs.putUShort("tsec", cfg.timeSecs);
  prefs.putUShort("wsec", cfg.wxSecs);
  prefs.putUShort("csec", cfg.citySecs);
  prefs.putUChar("spd", cfg.speed);
  prefs.putUChar("trspd", cfg.trSpeed);
  prefs.putUChar("mspd", cfg.msgSpeed);
  prefs.putBool("non", cfg.nightOn);
  prefs.putUShort("nst", cfg.nightStart);
  prefs.putUShort("nen", cfg.nightEnd);
  prefs.putUChar("nbr", cfg.nightBr);
  prefs.putUChar("lang", cfg.lang);
}

// ===================== Meteo (task su core 0) =====================
bool httpsGet(const String &url, String &body) {
  WiFiClientSecure client;
  client.setInsecure();  // dati meteo pubblici: niente verifica del certificato
  client.setTimeout(8);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code == HTTP_CODE_OK) body = http.getString();
  http.end();
  return code == HTTP_CODE_OK;
}

// Memoria delle coordinate gia' trovate (evita di richiedere ogni volta la geocodifica)
GeoEnt geoCache[MAXC + 2];
int geoCount = 0, geoNext = 0;

GeoEnt *geoFind(const String &key) {
  for (int i = 0; i < geoCount; i++)
    if (geoCache[i].key == key) return &geoCache[i];
  return nullptr;
}

GeoEnt *geoAdd() {
  int idx;
  if (geoCount < MAXC + 2) {
    idx = geoCount++;
  } else {
    idx = geoNext;
    geoNext = (geoNext + 1) % (MAXC + 2);
  }
  geoCache[idx] = GeoEnt();
  return &geoCache[idx];
}

// Scrive un errore sulla citta' i, solo se in quel posto c'e' ancora la stessa citta'
void setWxError(int i, const String &city, const String &cc, const char *msg, bool invalidate) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (i < nCity && cities[i].name == city && cities[i].cc == cc) {
    strlcpy(wxs[i].err, msg, sizeof(wxs[i].err));
    if (invalidate) wxs[i].valid = false;
  }
  xSemaphoreGive(mtx);
}

bool fetchCity(int i, const String &city, const String &cc) {
  if (city.length() == 0) return false;
  String key = city + "|" + cc + "|" + LANG_CODE[cfg.lang];
  GeoEnt *ge = geoFind(key);
  if (!ge) {
    String url = "https://geocoding-api.open-meteo.com/v1/search?name=" + urlEncode(city) +
                 "&count=1&language=" + String(LANG_CODE[cfg.lang]) + "&format=json";
    if (cc.length() == 2) url += "&countryCode=" + cc;
    String body;
    if (!httpsGet(url, body)) {
      setWxError(i, city, cc, "e_net", false);
      return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      setWxError(i, city, cc, "e_resp", false);
      return false;
    }
    ge = geoAdd();
    ge->key = key;
    if (!doc["results"][0].is<JsonObject>()) {
      ge->bad = true;  // evita di richiedere di continuo una citta' inesistente
    } else {
      JsonObject r = doc["results"][0];
      ge->lat = r["latitude"] | 0.0f;
      ge->lon = r["longitude"] | 0.0f;
      asciiFold(r["name"] | "", ge->name, sizeof(ge->name));
      asciiFold(r["country_code"] | "", ge->cc, sizeof(ge->cc));
    }
  }
  if (ge->bad) {
    setWxError(i, city, cc, "e_geo", true);
    return false;
  }

  char url[220];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code,is_day&timezone=auto",
           ge->lat, ge->lon);
  String body;
  if (!httpsGet(String(url), body)) {
    setWxError(i, city, cc, "e_wx", false);
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body) || !doc["current"].is<JsonObject>()) {
    setWxError(i, city, cc, "e_resp", false);
    return false;
  }
  JsonObject c = doc["current"];
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (i < nCity && cities[i].name == city && cities[i].cc == cc) {
    Weather &w = wxs[i];
    w.temp = c["temperature_2m"] | 0.0f;
    w.hum = c["relative_humidity_2m"] | 0;
    w.code = c["weather_code"] | 0;
    w.isDay = (c["is_day"] | 1) == 1;
    w.utcOffset = doc["utc_offset_seconds"] | 0;
    w.haveOffset = true;
    strlcpy(w.name, ge->name, sizeof(w.name));
    strlcpy(w.cc, ge->cc, sizeof(w.cc));
    w.err[0] = 0;
    w.updated = millis();
    w.valid = true;
  }
  xSemaphoreGive(mtx);
  return true;
}

void weatherTask(void *) {
  uint32_t nextDue = 0;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && (wxRefresh || (int32_t)(millis() - nextDue) >= 0)) {
      wxRefresh = false;
      if (geoClear) {
        geoClear = false;
        geoCount = 0;
        geoNext = 0;
      }
      String cn[MAXC], cc[MAXC];
      int n;
      xSemaphoreTake(mtx, portMAX_DELAY);
      n = nCity;
      for (int i = 0; i < n; i++) {
        cn[i] = cities[i].name;
        cc[i] = cities[i].cc;
      }
      xSemaphoreGive(mtx);
      bool allOk = true;
      for (int i = 0; i < n; i++) {
        if (!fetchCity(i, cn[i], cc[i])) allOk = false;
        if (wxRefresh) break;  // l'elenco e' cambiato: ricomincio subito
      }
      nextDue = millis() + (allOk ? WEATHER_REFRESH_MS : 60000UL);
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

bool getWx(int i, Weather &out) {
  bool ok = false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (i >= 0 && i < nCity) {
    out = wxs[i];
    ok = out.valid;
  }
  xSemaphoreGive(mtx);
  return ok;
}

// ===================== Ora locale =====================
// Ora della citta' principale (o dell'Italia finche' non e' nota)
bool getLocal(struct tm &tmv) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;  // NTP non ancora sincronizzato
  bool have = false;
  int off = 0;
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (nCity > 0) {
    have = wxs[0].haveOffset;
    off = wxs[0].utcOffset;
  }
  xSemaphoreGive(mtx);
  if (have) {
    time_t l = now + off;
    gmtime_r(&l, &tmv);
  } else {
    localtime_r(&now, &tmv);
  }
  return true;
}

// ===================== Ora con blocchi Tetris =====================
// Durata di un "tick" dell'animazione in ms, in base alla velocita' 1..10
uint32_t tickMs() {
  static const uint8_t T[10] = {140, 105, 80, 62, 48, 38, 30, 23, 17, 12};
  int s = constrain((int)cfg.speed, 1, 10);
  return T[s - 1];
}

// Durata dello scorrimento tra le pagine, in base alla velocita' 1..10
uint32_t trMs() {
  static const uint16_t T[10] = {1600, 1300, 1050, 850, 700, 580, 470, 370, 280, 200};
  return T[constrain((int)cfg.trSpeed, 1, 10) - 1];
}

// Pixel al secondo dei messaggi scorrevoli
uint32_t msgPps() {
  static const uint16_t T[10] = {18, 26, 36, 48, 62, 78, 96, 118, 142, 170};
  return T[constrain((int)cfg.msgSpeed, 1, 10) - 1];
}

void advanceTicks(uint32_t now) {
  uint32_t dt = now - lastClockMs;
  lastClockMs = now;
  if (dt > 200) dt = 200;  // dopo una pausa lunga non far scattare tanti tick insieme
  tickAcc += dt;
  uint32_t tm = tickMs();
  while (tickAcc >= tm) {
    tickAcc -= tm;
    tickNow++;
  }
}

// Colori vivi dei pezzi: rosso, arancio, giallo, verde, ciano, blu, viola
static const uint8_t KC[7][3] = {{255, 40, 40}, {255, 140, 0}, {255, 230, 0}, {40, 235, 70},
                                 {0, 225, 255}, {50, 100, 255}, {200, 60, 255}};
uint16_t kBase[7], kDark[7], kLight[7];

void initColors() {
  for (int k = 0; k < 7; k++) {
    int r = KC[k][0], g2 = KC[k][1], b = KC[k][2];
    kBase[k] = RGB(r, g2, b);
    kDark[k] = RGB(r * 33 / 100, g2 * 33 / 100, b * 33 / 100);
    kLight[k] = RGB(r + (255 - r) * 6 / 10, g2 + (255 - g2) * 6 / 10, b + (255 - b) * 6 / 10);
  }
}

// Cella 4x4 con rilievo marcato: lato chiaro in alto a sinistra, scuro in basso a destra
void drawCell(int x, int y, int kind) {
  uint16_t b = kBase[kind], l = kLight[kind], d = kDark[kind];
  for (int j = 0; j < 4; j++)
    for (int i = 0; i < 4; i++) {
      uint16_t c = b;
      if (j == 0 || i == 0) c = l;
      if (j == 3 || i == 3) c = d;
      if ((j == 0 && i == 3) || (j == 3 && i == 0)) c = b;
      put(x + i, y + j, c);
    }
}

// Cifre piccole 3x5 (disegnate a scala 2 nelle pagine delle altre citta')
static const uint8_t D35[10][5] = {{7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
                                   {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}};

void tinyDigit2(int d, int x, int y, uint16_t c) {
  if (d < 0 || d > 9) return;
  for (int r = 0; r < 5; r++)
    for (int b = 0; b < 3; b++)
      if (D35[d][r] & (4 >> b)) g->fillRect(x + b * 2, y + r * 2, 2, 2, c);
}

void computeDigits(const struct tm &t, int d[4]) {
  int h = t.tm_hour;
  if (cfg.h12) {
    h %= 12;
    if (h == 0) h = 12;
  }
  d[0] = (cfg.h12 && h < 10) ? -1 : h / 10;
  d[1] = h % 10;
  d[2] = t.tm_min / 10;
  d[3] = t.tm_min % 10;
}

void paintTime(bool colon, bool pm) {
  g->fillScreen(0);
  engine.render(tickNow, SLOT_X, [](int x, int y, int k) { drawCell(x, y, k); });
  if (colon) {
    drawCell(30, 10, 2);
    drawCell(30, 18, 2);
  }
  if (cfg.h12 && pm) g->fillRect(31, 27, 2, 2, RGB(0, 225, 255));  // puntino ciano = PM
}

// ===================== Icone meteo animate =====================
const int CX = 11, CY = 20;  // centro dell'icona (occupa x 0..22, y 9..31)

Scene sceneOf(int code) {
  if (code <= 1) return S_CLEAR;
  if (code == 2) return S_PARTLY;
  if (code == 3) return S_CLOUDY;
  if (code == 45 || code == 48) return S_FOG;
  if (code >= 51 && code <= 57) return S_DRIZZLE;
  if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return S_RAIN;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return S_SNOW;
  if (code >= 95) return S_STORM;
  return S_CLOUDY;
}

void drawCloud(int x, int y, uint16_t col) {
  g->fillCircle(x - 5, y + 1, 4, col);
  g->fillCircle(x + 1, y - 2, 5, col);
  g->fillCircle(x + 6, y + 1, 3, col);
  g->fillRect(x - 5, y + 1, 12, 4, col);
}

void drawSun(int cx, int cy, int r, float r1, float r2, uint32_t t) {
  float base = t / 1800.0f;
  uint16_t ray = RGB(255, 160, 0);
  for (int i = 0; i < 8; i++) {
    float a = base + i * 0.7853982f;
    g->drawLine(cx + lroundf(cosf(a) * r1), cy + lroundf(sinf(a) * r1),
                cx + lroundf(cosf(a) * r2), cy + lroundf(sinf(a) * r2), ray);
  }
  g->fillCircle(cx, cy, r, RGB(255, 210, 0));
}

void drawMoon(int cx, int cy, int r) {
  g->fillCircle(cx, cy, r, RGB(235, 235, 190));
  g->fillCircle(cx + r / 2 + 1, cy - r / 3, r - 1, 0);
}

void drawStars(uint32_t t) {
  const int sx[3] = {CX + 7, CX - 8, CX + 8};
  const int sy[3] = {CY - 8, CY + 6, CY + 7};
  for (int i = 0; i < 3; i++) {
    float k = 0.5f + 0.5f * sinf(t / 300.0f + i * 2.1f);
    uint8_t v = 70 + (uint8_t)(185 * k);
    uint16_t c = RGB(v, v, (uint8_t)(v * 0.8f));
    put(sx[i], sy[i], c);
    if (k > 0.75f) {
      put(sx[i] - 1, sy[i], c);
      put(sx[i] + 1, sy[i], c);
      put(sx[i], sy[i] - 1, c);
      put(sx[i], sy[i] + 1, c);
    }
  }
}

void drawRain(int n, uint32_t t, uint16_t col) {
  int sp = 16 / (n - 1);
  for (int i = 0; i < n; i++) {
    int x = CX - 8 + i * sp;
    int y = CY + 2 + ((t / 55 + i * 5) % 10);
    g->drawFastVLine(x, y, 2, col);
    put(x, y - 1, RGB(30, 70, 140));
  }
}

void drawSnow(uint32_t t) {
  for (int i = 0; i < 5; i++) {
    int x = CX - 8 + i * 4 + lroundf(sinf(t / 400.0f + i) * 1.5f);
    int y = CY + 2 + ((t / 110 + i * 5) % 10);
    g->fillRect(x, y, 2, 2, RGB(230, 240, 255));
  }
}

void drawScene(int code, bool isDay, uint32_t t) {
  int drift = lroundf(sinf(t / 1500.0f) * 2);
  switch (sceneOf(code)) {
    case S_CLEAR:
      if (isDay) {
        drawSun(CX, CY, 5, 7.5f, 10.5f, t);
      } else {
        drawMoon(CX, CY, 8);
        drawStars(t);
      }
      break;
    case S_PARTLY:
      if (isDay) {
        drawSun(CX - 3, CY - 5, 4, 5.5f, 8.0f, t);
      } else {
        drawMoon(CX - 3, CY - 5, 6);
        drawStars(t);
      }
      drawCloud(CX + 2 + drift, CY + 3, RGB(205, 210, 220));
      break;
    case S_CLOUDY:
      drawCloud(CX - 3 - drift, CY - 4, RGB(120, 130, 145));
      drawCloud(CX + 2 + drift, CY + 2, RGB(200, 205, 215));
      break;
    case S_FOG:
      for (int i = 0; i < 4; i++) {
        int y = CY - 8 + i * 5;
        int x = CX - 7 + lroundf(sinf(t / 900.0f + i * 1.3f) * 3);
        g->drawFastHLine(x, y, 14, RGB(150, 160, 170));
        g->drawFastHLine(x + 4, y + 2, 8, RGB(85, 92, 100));
      }
      break;
    case S_DRIZZLE:
      drawCloud(CX + drift, CY - 5, RGB(150, 160, 175));
      drawRain(3, t, RGB(90, 170, 255));
      break;
    case S_RAIN:
      drawCloud(CX + drift, CY - 5, RGB(105, 115, 135));
      drawRain(5, t, RGB(60, 140, 255));
      break;
    case S_SNOW:
      drawCloud(CX + drift, CY - 5, RGB(170, 180, 195));
      drawSnow(t);
      break;
    case S_STORM: {
      bool flash = ((t / 120) % 10) < 2;
      drawCloud(CX + drift, CY - 5, flash ? RGB(170, 175, 190) : RGB(80, 85, 100));
      drawRain(3, t, RGB(60, 140, 255));
      if (flash) {
        uint16_t y = RGB(255, 230, 0);
        g->fillTriangle(CX + 2, CY - 1, CX - 3, CY + 6, CX + 1, CY + 6, y);
        g->fillTriangle(CX + 1, CY + 4, CX + 5, CY + 4, CX - 1, CY + 11, y);
      }
      break;
    }
  }
}

uint16_t tempColor(float tc) {
  if (tc < 0) return RGB(130, 190, 255);
  if (tc < 10) return RGB(60, 160, 255);
  if (tc < 18) return RGB(0, 205, 205);
  if (tc < 25) return RGB(90, 225, 90);
  if (tc < 31) return RGB(255, 170, 0);
  return RGB(255, 70, 40);
}

uint16_t humColor(int h) {
  if (h < 30) return RGB(255, 150, 0);
  if (h < 60) return RGB(0, 205, 255);
  return RGB(40, 120, 255);
}

// Piccolo termometro (x 26..30, y 10..20): il mercurio sale e scende con la temperatura
void drawMiniThermo(float tc, uint32_t t, uint16_t col) {
  g->drawRect(27, 10, 3, 7, RGB(120, 120, 135));
  int lv = constrain((int)lroundf((tc + 10.0f) * 4.0f / 50.0f) + 1, 1, 5);
  g->drawFastVLine(28, 16 - lv, lv, col);
  g->fillCircle(28, 18, 2, col);
  if ((t / 250) % 2) put(28, 16 - lv, RGB(255, 255, 255));
}

// Piccola goccia (x 25..31, y 21..31) che ondeggia e brilla
void drawMiniDrop(uint32_t t, uint16_t col) {
  int yb = 26 + lroundf(sinf(t / 380.0f));
  g->fillTriangle(28, yb - 4, 25, yb, 31, yb, col);
  g->fillCircle(28, yb + 1, 3, col);
  if (sinf(t / 260.0f) > 0) {
    put(26, yb + 1, RGB(255, 255, 255));
    put(26, yb + 2, RGB(255, 255, 255));
  }
}

// Valore in carattere piccolo (5x7) con eventuale gradi e unita'. Ritorna la x di fine.
int drawValue(const char *s, int x, int y, uint16_t col, char unit, bool degree) {
  g->setTextSize(1);
  g->setTextWrap(false);
  g->setTextColor(col);
  g->setCursor(x, y);
  g->print(s);
  int xe = x + (int)strlen(s) * 6;
  if (degree) {
    g->drawRect(xe, y, 3, 3, col);
    xe += 4;
  }
  if (unit) {
    g->setCursor(xe, y);
    g->print(unit);
    xe += 6;
  }
  return xe;
}

// Nome citta' e paese in alto; se e' troppo lungo scorre avanti e indietro
void drawCity(const Weather &w, uint32_t t) {
  int nameLen = (int)strlen(w.name);
  int ccLen = (int)strlen(w.cc);
  int chars = nameLen + (ccLen ? ccLen + 1 : 0);
  int wpx = chars * 6 - 1;
  int x;
  if (wpx <= PANEL_W) {
    x = (PANEL_W - wpx) / 2;
  } else {
    int steps = wpx - (PANEL_W - 2), pause = 20;
    int cycle = 2 * (steps + pause);
    int i = (int)((t / 50) % cycle), off;
    if (i < pause) off = 0;
    else if (i < pause + steps) off = i - pause;
    else if (i < 2 * pause + steps) off = steps;
    else off = steps - (i - (2 * pause + steps));
    x = 1 - off;
  }
  g->setTextSize(1);
  g->setTextWrap(false);
  g->setCursor(x, 0);
  g->setTextColor(RGB(235, 240, 255));
  g->print(w.name);
  if (ccLen) {
    g->setTextColor(RGB(110, 150, 210));
    g->print(' ');
    g->print(w.cc);
  }
}

// Pagina meteo della citta' principale (icona, termometro, goccia)
void paintWeather(const Weather &w, uint32_t t) {
  g->fillScreen(0);
  drawScene(w.code, w.isDay, t);
  drawCity(w, t);

  float shown = cfg.fahrenheit ? (w.temp * 9.0f / 5.0f + 32.0f) : w.temp;
  uint16_t tc = tempColor(w.temp);
  drawMiniThermo(w.temp, t, tc);
  char buf[12];
  snprintf(buf, sizeof(buf), "%d", (int)lroundf(shown));
  drawValue(buf, 34, 12, tc, cfg.fahrenheit ? 'F' : 'C', true);

  int hum = constrain(w.hum, 0, 99);
  uint16_t hc = humColor(hum);
  drawMiniDrop(t, hc);
  snprintf(buf, sizeof(buf), "%d", hum);
  drawValue(buf, 34, 23, hc, '%', false);
}

// Pagina delle altre citta': meteo e ora locale insieme
void paintCity(const Weather &w, uint32_t t) {
  g->fillScreen(0);
  drawScene(w.code, w.isDay, t);
  drawCity(w, t);

  // ora locale della citta' (cifre 6x10)
  time_t l = time(nullptr) + w.utcOffset;
  struct tm tt;
  gmtime_r(&l, &tt);
  int h = tt.tm_hour;
  if (cfg.h12) {
    h %= 12;
    if (h == 0) h = 12;
  }
  uint16_t tcol = RGB(255, 225, 80);
  const int x0 = 28, y0 = 10;
  if (!(cfg.h12 && h < 10)) tinyDigit2(h / 10, x0, y0, tcol);
  tinyDigit2(h % 10, x0 + 7, y0, tcol);
  if (((t / 500) & 1) == 0) {
    g->fillRect(x0 + 14, y0 + 2, 2, 2, RGB(0, 225, 255));
    g->fillRect(x0 + 14, y0 + 6, 2, 2, RGB(0, 225, 255));
  }
  tinyDigit2(tt.tm_min / 10, x0 + 18, y0, tcol);
  tinyDigit2(tt.tm_min % 10, x0 + 25, y0, tcol);
  if (cfg.h12 && tt.tm_hour >= 12) g->fillRect(x0 + 31, y0, 1, 2, RGB(0, 225, 255));

  // temperatura e umidita' sulla stessa riga
  float shown = cfg.fahrenheit ? (w.temp * 9.0f / 5.0f + 32.0f) : w.temp;
  char buf[12];
  snprintf(buf, sizeof(buf), "%d", (int)lroundf(shown));
  uint16_t tc = tempColor(w.temp);
  int xe = drawValue(buf, 24, 22, tc, 0, true);  // temperatura con gradi, senza lettera
  int hum = constrain(w.hum, 0, 99);
  snprintf(buf, sizeof(buf), "%d", hum);
  int wh = (int)strlen(buf) * 6 + 6;  // cifre + simbolo %
  int xh = max(xe + 2, PANEL_W - wh);
  drawValue(buf, xh, 22, humColor(hum), xh + wh <= PANEL_W ? '%' : 0, false);
}

// Pagina con messaggio scorrevole da destra a sinistra
uint32_t msgDurationMs(int i) {
  uint32_t wpx = 0;
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (i >= 0 && i < nMsg) wpx = (uint32_t)msgs[i].disp.length() * 6u * msgs[i].size;
  xSemaphoreGive(mtx);
  if (wpx == 0) return 0;
  return (uint32_t)(PANEL_W + wpx) * 1000u / msgPps() + 400;
}

void paintMsg(int i, uint32_t el) {
  g->fillScreen(0);
  MsgCfg m;
  xSemaphoreTake(mtx, portMAX_DELAY);
  bool ok = i >= 0 && i < nMsg;
  if (ok) m = msgs[i];
  xSemaphoreGive(mtx);
  if (!ok) return;
  int x = PANEL_W - (int)(el * msgPps() / 1000u);
  g->setTextSize(m.size);
  g->setTextWrap(false);
  g->setTextColor(rgb24(m.rgb));
  g->setCursor(x, (PANEL_H - 8 * m.size) / 2);
  g->print(m.disp);
  g->setTextSize(1);
}

// ===================== Luminosita' =====================
void updateBrightness(uint32_t now) {
  static uint32_t last = 0;
  static float smooth = -1;
  static int appliedB = -1;
  if (now - last < 400) return;
  last = now;
  int target = cfg.brightness;
  struct tm lt;
  bool night = false;
  if (cfg.nightOn && getLocal(lt)) {
    int m = lt.tm_hour * 60 + lt.tm_min;
    if (cfg.nightStart == cfg.nightEnd) night = false;
    else if (cfg.nightStart < cfg.nightEnd) night = m >= cfg.nightStart && m < cfg.nightEnd;
    else night = m >= cfg.nightStart || m < cfg.nightEnd;  // fascia a cavallo della mezzanotte
  }
  if (night) {
    target = cfg.nightBr;  // di notte vale solo la luminosita' notturna
  } else if (cfg.autoBright) {
    int raw = analogRead(PIN_LDR);
    smooth = (smooth < 0) ? raw : smooth * 0.9f + raw * 0.1f;
    target = constrain((int)map((long)smooth, 100, 3000, 12, 255), 12, 255);
  }
  if (target != appliedB) {
    display->setBrightness8(target);
    appliedB = target;
  }
}

// ===================== Interfaccia web =====================
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="it"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tetris Clock</title>
<style>
:root{color-scheme:dark;--bg:#0e1014;--card:#171a21;--line:#2a2f3a;--fg:#e9ebf1;--mut:#8f97ab;--acc:#4da3ff}
*{box-sizing:border-box}
body{margin:0;padding:16px;max-width:560px;margin-inline:auto;background:var(--bg);color:var(--fg);font:16px/1.45 system-ui,-apple-system,Segoe UI,sans-serif}
h1{font-size:20px;margin:4px 0 14px;display:flex;align-items:center;gap:8px}
h2{font-size:16px;margin:0 0 10px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;margin-bottom:14px}
.mut{color:var(--mut);font-size:13px}
label{display:block;margin:12px 0 4px;font-size:14px;color:var(--mut)}
input,select{width:100%;padding:10px 12px;border-radius:10px;border:1px solid var(--line);background:#0e1014;color:var(--fg);font-size:16px}
input[type=range]{padding:0}
input[type=color]{padding:2px;height:42px}
.row{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.row3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:12px}
.chk{display:flex;gap:10px;align-items:center;margin-top:14px}
.chk input{width:auto}
button{width:100%;margin-top:16px;padding:12px;border:0;border-radius:10px;background:var(--acc);color:#04121f;font-weight:700;font-size:16px;cursor:pointer}
button.sec{background:transparent;border:1px solid var(--line);color:var(--fg);font-weight:500;margin-top:10px}
button.sm{width:auto;margin:0;padding:7px 11px;font-size:14px}
.item{display:flex;justify-content:space-between;align-items:center;gap:10px;padding:10px 0;border-top:1px solid var(--line)}
.item:first-of-type{border-top:0}
.btns{display:flex;gap:6px;flex-shrink:0}
.tag{font-size:11px;background:#233044;color:#9cc7ff;border-radius:6px;padding:1px 6px;margin-left:6px}
.big{font-weight:700}
.err{color:#ff7b7b}.ok{color:#6fdc8c}
.msgs{min-height:20px;margin-top:10px;font-size:14px}
</style></head><body>
<h1>&#9632; Tetris Clock</h1>

<div class="card">
  <h2 data-i="t_cities"></h2>
  <div class="mut" style="margin-bottom:6px" data-i="cities_help"></div>
  <div id="cl"></div>
  <div class="row" style="margin-top:6px">
    <div><label for="ncity" data-i="new_city"></label><input id="ncity" data-ph="ph_city" autocomplete="off" maxlength="40"></div>
    <div><label for="ncc" data-i="country"></label><input id="ncc" maxlength="2" placeholder="IT" style="text-transform:uppercase"></div>
  </div>
  <button type="button" id="cadd" data-i="add_city"></button>
  <div class="msgs" id="cmsg"></div>
</div>

<div class="card">
  <h2 data-i="t_msgs"></h2>
  <div class="mut" style="margin-bottom:6px" data-i="msgs_help"></div>
  <div id="ml"></div>
  <label for="mt" data-i="text"></label>
  <input id="mt" maxlength="160" data-ph="ph_msg" autocomplete="off">
  <div class="row">
    <div><label for="mc" data-i="color"></label><input id="mc" type="color" value="#ffd23c"></div>
    <div><label for="ms" data-i="size"></label><select id="ms"><option value="2" data-i="big"></option><option value="1" data-i="small"></option></select></div>
  </div>
  <button type="button" id="madd" data-i="add_msg"></button>
  <div class="msgs" id="mmsg"></div>
</div>

<form class="card" id="f">
  <h2 data-i="t_set"></h2>
  <div class="mut" id="net">--</div>
  <div class="mut"><span data-i="disp_time"></span>: <span id="time">--:--:--</span></div>
  <label for="lang">Lingua / Language / Idioma / Langue / Sprache</label>
  <select id="lang"><option value="0">Italiano</option><option value="1">English</option><option value="2">Español</option><option value="3">Français</option><option value="4">Deutsch</option></select>
  <div class="row">
    <div><label for="units" data-i="units"></label><select id="units"><option value="C">&deg;C</option><option value="F">&deg;F</option></select></div>
    <div><label for="fmt" data-i="fmt"></label><select id="fmt"><option value="24" data-i="h24"></option><option value="12" data-i="h12"></option></select></div>
  </div>
  <label for="br" data-i="br_day"></label>
  <input id="br" type="range" min="5" max="255">
  <div class="chk"><input id="auto" type="checkbox"><label for="auto" style="margin:0" data-i="auto"></label></div>
  <div class="chk"><input id="non" type="checkbox"><label for="non" style="margin:0" data-i="night_on"></label></div>
  <div id="nbox">
    <div class="row">
      <div><label for="nst" data-i="n_start"></label><input id="nst" type="time" value="22:00"></div>
      <div><label for="nen" data-i="n_end"></label><input id="nen" type="time" value="07:00"></div>
    </div>
    <label for="nbr" data-i="br_night"></label>
    <input id="nbr" type="range" min="1" max="255">
  </div>
  <label for="spd"><span data-i="spd"></span>: <b id="spdv">6</b> / 10</label>
  <input id="spd" type="range" min="1" max="10" step="1">
  <label for="trspd"><span data-i="trspd"></span>: <b id="trv">5</b> / 10</label>
  <input id="trspd" type="range" min="1" max="10" step="1">
  <label for="mspd"><span data-i="mspd"></span>: <b id="mspv">5</b> / 10</label>
  <input id="mspd" type="range" min="1" max="10" step="1">
  <div class="row3">
    <div><label for="tsec" data-i="s_time"></label><input id="tsec" type="number" min="5" max="300"></div>
    <div><label for="wsec" data-i="s_wx"></label><input id="wsec" type="number" min="3" max="120"></div>
    <div><label for="csec" data-i="s_city"></label><input id="csec" type="number" min="3" max="120"></div>
  </div>
  <button type="submit" data-i="save"></button>
  <button type="button" class="sec" id="rp" data-i="save_replay"></button>
  <button type="button" class="sec" id="rf" data-i="refresh"></button>
  <button type="button" class="sec" id="wr" data-i="wifi_reset"></button>
  <div class="msgs" id="msg"></div>
</form>

<div class="card" id="wf">
  <h2 data-i="t_wifi"></h2>
  <div class="mut" id="wcur" style="margin-bottom:8px">--</div>
  <div style="display:grid;grid-template-columns:1fr auto;gap:8px">
    <select id="wl"></select>
    <button type="button" class="sec" id="ws" style="margin-top:0;width:auto;padding:10px 14px" data-i="scan"></button>
  </div>
  <input id="wo" data-ph="ph_ssid" maxlength="32" style="display:none;margin-top:8px" autocomplete="off">
  <input id="wp" type="password" data-ph="ph_pass" maxlength="63" style="margin-top:8px" autocomplete="off">
  <button type="button" id="wc" data-i="connect"></button>
  <div id="wmsg" class="mut" style="margin-top:10px;min-height:20px"></div>
</div>
<script>
const $=id=>document.getElementById(id);
const D={
t_cities:["Città","Cities","Ciudades","Villes","Städte"],
cities_help:["La prima città mostra ora grande e meteo in due pagine; dalla seconda in poi ogni città ha una sola pagina con meteo e ora locale.","The first city shows the big clock and the weather on two pages; from the second on, each city has a single page with weather and local time.","La primera ciudad muestra la hora grande y el tiempo en dos páginas; desde la segunda, cada ciudad tiene una sola página con tiempo y hora local.","La première ville affiche la grande heure et la météo sur deux pages ; à partir de la deuxième, chaque ville a une seule page avec météo et heure locale.","Die erste Stadt zeigt die große Uhr und das Wetter auf zwei Seiten; ab der zweiten hat jede Stadt eine Seite mit Wetter und Ortszeit."],
no_city:["Nessuna città: aggiungine una qui sotto.","No cities: add one below.","Ninguna ciudad: añade una abajo.","Aucune ville : ajoutez-en une ci-dessous.","Keine Stadt: unten eine hinzufügen."],
new_city:["Nuova città","New city","Nueva ciudad","Nouvelle ville","Neue Stadt"],
ph_city:["es. Bologna","e.g. Boston","p. ej. Sevilla","ex. Lyon","z. B. Köln"],
country:["Paese (facoltativo)","Country (optional)","País (opcional)","Pays (facultatif)","Land (optional)"],
add_city:["Aggiungi città","Add city","Añadir ciudad","Ajouter la ville","Stadt hinzufügen"],
main_tag:["principale","main","principal","principale","Haupt"],
make_main:["Principale","Make main","Principal","Principale","Hauptstadt"],
remove:["Rimuovi","Remove","Quitar","Supprimer","Entfernen"],
confirm_rm:["Rimuovere {0}?","Remove {0}?","¿Quitar {0}?","Supprimer {0} ?","{0} entfernen?"],
waiting:["In attesa dei dati meteo…","Waiting for weather data…","Esperando datos del tiempo…","En attente des données météo…","Warte auf Wetterdaten…"],
min_ago:["{0} min fa","{0} min ago","hace {0} min","il y a {0} min","vor {0} Min."],
city_added:["Città aggiunta. Cerco il meteo…","City added. Fetching weather…","Ciudad añadida. Buscando el tiempo…","Ville ajoutée. Recherche de la météo…","Stadt hinzugefügt. Hole Wetter…"],
enter_city:["Scrivi il nome della città","Enter the city name","Escribe el nombre de la ciudad","Saisissez le nom de la ville","Stadtnamen eingeben"],
t_msgs:["Messaggi scorrevoli","Scrolling messages","Mensajes desplazables","Messages défilants","Laufende Nachrichten"],
msgs_help:["Ogni messaggio è una pagina che scorre da destra a sinistra. Vengono dopo le città.","Each message is a page that scrolls from right to left. They come after the cities.","Cada mensaje es una página que se desplaza de derecha a izquierda. Van después de las ciudades.","Chaque message est une page qui défile de droite à gauche. Ils viennent après les villes.","Jede Nachricht ist eine Seite, die von rechts nach links läuft. Sie folgen auf die Städte."],
no_msg:["Nessun messaggio.","No messages.","Ningún mensaje.","Aucun message.","Keine Nachrichten."],
text:["Testo","Text","Texto","Texte","Text"],
ph_msg:["es. Buon compleanno!","e.g. Happy birthday!","p. ej. ¡Feliz cumpleaños!","ex. Joyeux anniversaire !","z. B. Alles Gute!"],
color:["Colore","Color","Color","Couleur","Farbe"],
size:["Dimensione","Size","Tamaño","Taille","Größe"],
big:["Grande","Large","Grande","Grand","Groß"],
small:["Piccolo","Small","Pequeño","Petit","Klein"],
add_msg:["Aggiungi messaggio","Add message","Añadir mensaje","Ajouter le message","Nachricht hinzufügen"],
msg_added:["Messaggio aggiunto","Message added","Mensaje añadido","Message ajouté","Nachricht hinzugefügt"],
enter_msg:["Scrivi il messaggio","Enter the message","Escribe el mensaje","Saisissez le message","Nachricht eingeben"],
t_set:["Impostazioni","Settings","Ajustes","Réglages","Einstellungen"],
disp_time:["Ora sul display","Time on display","Hora en pantalla","Heure affichée","Zeit auf dem Display"],
units:["Unità","Units","Unidades","Unités","Einheit"],
fmt:["Formato ora","Time format","Formato de hora","Format de l'heure","Zeitformat"],
h24:["24 ore","24 hours","24 horas","24 heures","24 Stunden"],
h12:["12 ore (puntino = PM)","12 hours (dot = PM)","12 horas (punto = PM)","12 heures (point = PM)","12 Stunden (Punkt = PM)"],
br_day:["Luminosità di giorno","Daytime brightness","Brillo de día","Luminosité de jour","Helligkeit tagsüber"],
auto:["Luminosità automatica (sensore LDR)","Automatic brightness (LDR sensor)","Brillo automático (sensor LDR)","Luminosité automatique (capteur LDR)","Automatische Helligkeit (LDR-Sensor)"],
night_on:["Fascia notturna a orari","Night mode on a schedule","Modo noche con horario","Mode nuit programmé","Nachtmodus nach Zeitplan"],
n_start:["Inizio notte","Night starts","Inicio de la noche","Début de la nuit","Nachtbeginn"],
n_end:["Fine notte","Night ends","Fin de la noche","Fin de la nuit","Nachtende"],
br_night:["Luminosità di notte","Night brightness","Brillo de noche","Luminosité de nuit","Helligkeit nachts"],
spd:["Velocità caduta blocchi","Block fall speed","Velocidad de caída","Vitesse de chute","Fallgeschwindigkeit"],
trspd:["Velocità scorrimento tra le pagine","Page slide speed","Velocidad entre páginas","Vitesse entre les pages","Seitenwechsel-Tempo"],
mspd:["Velocità messaggi scorrevoli","Scrolling message speed","Velocidad de los mensajes","Vitesse des messages","Lauftext-Tempo"],
s_time:["Sec. ora","Clock secs","Seg. hora","Sec. heure","Sek. Uhr"],
s_wx:["Sec. meteo","Weather secs","Seg. tiempo","Sec. météo","Sek. Wetter"],
s_city:["Sec. altre città","Other cities secs","Seg. otras ciudades","Sec. autres villes","Sek. andere Städte"],
save:["Salva","Save","Guardar","Enregistrer","Speichern"],
save_replay:["Salva e rigioca l'animazione","Save and replay the animation","Guardar y repetir la animación","Enregistrer et rejouer l'animation","Speichern und Animation wiederholen"],
refresh:["Aggiorna meteo adesso","Refresh weather now","Actualizar el tiempo ahora","Actualiser la météo","Wetter jetzt aktualisieren"],
wifi_reset:["Reimposta WiFi (riapre il portale)","Reset WiFi (reopens the portal)","Restablecer WiFi (reabre el portal)","Réinitialiser le WiFi (rouvre le portail)","WLAN zurücksetzen (öffnet das Portal)"],
saving:["Salvataggio…","Saving…","Guardando…","Enregistrement…","Speichere…"],
saved:["Salvato","Saved","Guardado","Enregistré","Gespeichert"],
save_err:["Errore di salvataggio","Save error","Error al guardar","Erreur d'enregistrement","Fehler beim Speichern"],
anim_started:["Animazione avviata","Animation started","Animación iniciada","Animation lancée","Animation gestartet"],
error:["Errore","Error","Error","Erreur","Fehler"],
req_sent:["Richiesta inviata","Request sent","Solicitud enviada","Demande envoyée","Anfrage gesendet"],
confirm_reset:["Cancello la rete WiFi salvata? Il display riavvierà il portale di configurazione.","Delete the saved WiFi network? The display will reopen the setup portal.","¿Borrar la red WiFi guardada? La pantalla reabrirá el portal de configuración.","Effacer le réseau WiFi enregistré ? L'afficheur rouvrira le portail de configuration.","Gespeichertes WLAN löschen? Das Display öffnet wieder das Einrichtungsportal."],
restarting:["Riavvio…","Restarting…","Reiniciando…","Redémarrage…","Neustart…"],
t_wifi:["Rete WiFi","WiFi network","Red WiFi","Réseau WiFi","WLAN-Netz"],
connected_to:["Collegato a {0} ({1} dBm), indirizzo {2}","Connected to {0} ({1} dBm), address {2}","Conectado a {0} ({1} dBm), dirección {2}","Connecté à {0} ({1} dBm), adresse {2}","Verbunden mit {0} ({1} dBm), Adresse {2}"],
press_scan:["Premi «Cerca reti»","Press “Scan networks”","Pulsa «Buscar redes»","Appuyez sur « Chercher »","„Netze suchen“ drücken"],
scan:["Cerca reti","Scan networks","Buscar redes","Chercher","Netze suchen"],
scanning:["Cerco reti…","Scanning…","Buscando redes…","Recherche…","Suche läuft…"],
prot:["protetta","secured","protegida","protégé","geschützt"],
other_net:["Altra rete (nascosta)…","Other network (hidden)…","Otra red (oculta)…","Autre réseau (masqué)…","Anderes Netz (versteckt)…"],
ph_ssid:["Nome rete (SSID)","Network name (SSID)","Nombre de la red (SSID)","Nom du réseau (SSID)","Netzwerkname (SSID)"],
ph_pass:["Password (vuota se la rete è aperta)","Password (empty if the network is open)","Contraseña (vacía si la red es abierta)","Mot de passe (vide si le réseau est ouvert)","Passwort (leer bei offenem Netz)"],
connect:["Collega a questa rete","Connect to this network","Conectar a esta red","Se connecter à ce réseau","Mit diesem Netz verbinden"],
choose_net:["Scegli una rete","Choose a network","Elige una red","Choisissez un réseau","Netz auswählen"],
pass_min:["La password deve avere almeno 8 caratteri","The password must have at least 8 characters","La contraseña debe tener al menos 8 caracteres","Le mot de passe doit comporter au moins 8 caractères","Das Passwort braucht mindestens 8 Zeichen"],
net_err:["Errore di rete","Network error","Error de red","Erreur réseau","Netzwerkfehler"],
no_result:["Nessun risultato, riprova","No results, try again","Sin resultados, inténtalo de nuevo","Aucun résultat, réessayez","Keine Treffer, nochmal versuchen"],
connecting:["Collegamento in corso… Se la pagina smette di rispondere, collega il telefono alla nuova rete e apri http://tetrisclock.local oppure l'indirizzo mostrato sul display.","Connecting… If the page stops responding, join the new network with your phone and open http://tetrisclock.local or the address shown on the display.","Conectando… Si la página deja de responder, conecta el móvil a la nueva red y abre http://tetrisclock.local o la dirección mostrada en la pantalla.","Connexion en cours… Si la page ne répond plus, connectez le téléphone au nouveau réseau et ouvrez http://tetrisclock.local ou l'adresse affichée.","Verbinde… Reagiert die Seite nicht mehr, verbinde das Handy mit dem neuen Netz und öffne http://tetrisclock.local oder die angezeigte Adresse."],
connected_ok:["Collegato a {0} · {1}","Connected to {0} · {1}","Conectado a {0} · {1}","Connecté à {0} · {1}","Verbunden mit {0} · {1}"],
w_fail:["Non riesco a collegarmi a {0}. Ho rimesso la rete precedente.","Cannot connect to {0}. The previous network was restored.","No puedo conectar a {0}. He restaurado la red anterior.","Impossible de se connecter à {0}. Le réseau précédent a été rétabli.","Verbindung zu {0} fehlgeschlagen. Das vorherige Netz wurde wiederhergestellt."],
c_clear:["Sereno","Clear","Despejado","Dégagé","Klar"],
c_partly:["Parzialmente nuvoloso","Partly cloudy","Parcialmente nublado","Partiellement nuageux","Teils bewölkt"],
c_cloudy:["Coperto","Overcast","Cubierto","Couvert","Bedeckt"],
c_fog:["Nebbia","Fog","Niebla","Brouillard","Nebel"],
c_drizzle:["Pioggerella","Drizzle","Llovizna","Bruine","Nieselregen"],
c_rain:["Pioggia","Rain","Lluvia","Pluie","Regen"],
c_snow:["Neve","Snow","Nieve","Neige","Schnee"],
c_storm:["Temporale","Thunderstorm","Tormenta","Orage","Gewitter"],
e_net:["e_net","Network unreachable","Red no disponible","Réseau injoignable","Netz nicht erreichbar"],
e_geo:["Città non trovata","City not found","Ciudad no encontrada","Ville introuvable","Stadt nicht gefunden"],
e_resp:["Risposta non valida","Invalid response","Respuesta no válida","Réponse invalide","Ungültige Antwort"],
e_wx:["e_wx","Weather unreachable","Tiempo no disponible","Météo injoignable","Wetter nicht erreichbar"],
s_badcity:["Nome città non valido","Invalid city name","Nombre de ciudad no válido","Nom de ville invalide","Ungültiger Stadtname"],
s_maxcity:["Massimo 6 città","Maximum 6 cities","Máximo 6 ciudades","6 villes maximum","Höchstens 6 Städte"],
s_nocity:["Città inesistente","No such city","La ciudad no existe","Ville inexistante","Stadt existiert nicht"],
s_msglen:["s_msglen","The message must be 1 to 160 characters","El mensaje debe tener de 1 a 160 caracteres","Le message doit faire de 1 à 160 caractères","Die Nachricht braucht 1 bis 160 Zeichen"],
s_maxmsg:["Massimo 6 messaggi","Maximum 6 messages","Máximo 6 mensajes","6 messages maximum","Höchstens 6 Nachrichten"],
s_nomsg:["s_nomsg","No such message","El mensaje no existe","Message inexistant","Nachricht existiert nicht"],
s_noprint:["s_noprint","The message has no displayable characters","El mensaje no tiene caracteres visibles","Le message n'a pas de caractères affichables","Die Nachricht enthält keine darstellbaren Zeichen"],
s_ssid:["s_ssid","Invalid network name","Nombre de red no válido","Nom de réseau invalide","Ungültiger Netzname"],
s_wpass:["s_wpass","The password must be 8 to 63 characters","La contraseña debe tener de 8 a 63 caracteres","Le mot de passe doit faire de 8 à 63 caractères","Das Passwort braucht 8 bis 63 Zeichen"],
s_busy:["Collegamento già in corso","Already connecting","Ya se está conectando","Connexion déjà en cours","Verbindung läuft bereits"]
};
let lang=0,filled=false,F=false,last=null;
function t(k,...a){const e=D[k];let s=e?(e[lang]||e[0]):k;a.forEach((v,i)=>{s=s.split("{"+i+"}").join(v)});return s;}
function applyLang(){
  document.documentElement.lang=["it","en","es","fr","de"][lang];
  document.querySelectorAll("[data-i]").forEach(e=>{e.textContent=t(e.dataset.i)});
  document.querySelectorAll("[data-ph]").forEach(e=>{e.placeholder=t(e.dataset.ph)});
  const w=$("wl");if(!w.options.length||!w.dataset.scanned){w.innerHTML="";const o=document.createElement("option");o.value="";o.textContent=t("press_scan");w.appendChild(o);}
  if(last){renderCities(last.cities);renderMsgs(last.msgs);}
}
function condOf(c){return c<=1?"c_clear":c==2?"c_partly":c==3?"c_cloudy":(c==45||c==48)?"c_fog":(c>=51&&c<=57)?"c_drizzle":((c>=61&&c<=67)||(c>=80&&c<=82))?"c_rain":((c>=71&&c<=77)||c==85||c==86)?"c_snow":c>=95?"c_storm":"c_cloudy";}
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
function el(tg,c,x){const e=document.createElement(tg);if(c)e.className=c;if(x!==undefined)e.textContent=x;return e;}
function say(id,x,c){const m=$(id);m.textContent=x;m.className="msgs "+(c||"");}
async function post(u,o){const r=await fetch(u,{method:"POST",body:new URLSearchParams(o||{})});if(!r.ok)throw new Error(t(await r.text()));return r;}
function btn(x,fn){const b=el("button","sec sm",x);b.type="button";b.onclick=fn;return b;}
function renderCities(a){
  const box=$("cl");box.innerHTML="";
  if(!a.length){box.appendChild(el("div","mut",t("no_city")));return;}
  a.forEach((c,i)=>{
    const it=el("div","item"),l=el("div");
    const h=el("div");h.appendChild(el("span","big",c.place));
    if(i==0)h.appendChild(el("span","tag",t("main_tag")));
    l.appendChild(h);
    let d;
    if(c.ok){const x=F?c.temp*9/5+32:c.temp;d=Math.round(x)+"°"+(F?"F":"C")+" · "+c.hum+"% · "+t(condOf(c.code))+" · "+t("min_ago",Math.round(c.age/60));}
    else d=c.err?t(c.err):t("waiting");
    l.appendChild(el("div","mut",d));
    it.appendChild(l);
    const bs=el("div","btns");
    if(i>0)bs.appendChild(btn(t("make_main"),async()=>{try{await post("/api/city/main",{i:i});load();}catch(e){say("cmsg",e.message,"err");}}));
    bs.appendChild(btn(t("remove"),async()=>{if(!confirm(t("confirm_rm",c.n)))return;try{await post("/api/city/del",{i:i});load();}catch(e){say("cmsg",e.message,"err");}}));
    it.appendChild(bs);box.appendChild(it);
  });
}
function renderMsgs(a){
  const box=$("ml");box.innerHTML="";
  if(!a.length){box.appendChild(el("div","mut",t("no_msg")));return;}
  a.forEach((m,i)=>{
    const it=el("div","item"),l=el("div");
    const s=el("div","big",m.t);s.style.color=m.c;s.style.wordBreak="break-word";l.appendChild(s);
    l.appendChild(el("div","mut",m.s==2?t("big"):t("small")));
    it.appendChild(l);
    it.appendChild(btn(t("remove"),async()=>{try{await post("/api/msg/del",{i:i});load();}catch(e){say("mmsg",e.message,"err");}}));
    box.appendChild(it);
  });
}
function hm(m){return String(Math.floor(m/60)).padStart(2,"0")+":"+String(m%60).padStart(2,"0");}
function mn(v){const p=(v||"0:0").split(":");return (+p[0])*60+(+p[1]);}
function nvis(){$("nbox").style.display=$("non").checked?"block":"none";}
async function load(){
  try{
    const s=await (await fetch("/api/status")).json();
    last=s;F=s.cfg.f;
    if(!filled){lang=s.cfg.lang;}
    $("time").textContent=s.time;
    $("net").textContent=s.ssid+" · "+s.ip+" · "+s.rssi+" dBm";
    $("wcur").textContent=t("connected_to",s.ssid,s.rssi,s.ip);
    renderCities(s.cities);renderMsgs(s.msgs);
    if(!filled){
      const c=s.cfg;
      $("lang").value=c.lang;applyLang();
      $("units").value=c.f?"F":"C";$("fmt").value=c.h12?"12":"24";
      $("br").value=c.br;$("auto").checked=c.auto;$("br").disabled=c.auto;
      $("non").checked=c.non;$("nst").value=hm(c.nst);$("nen").value=hm(c.nen);$("nbr").value=c.nbr;nvis();
      $("spd").value=c.spd;$("spdv").textContent=c.spd;
      $("trspd").value=c.trspd;$("trv").textContent=c.trspd;
      $("mspd").value=c.mspd;$("mspv").textContent=c.mspd;
      $("tsec").value=c.tsec;$("wsec").value=c.wsec;$("csec").value=c.csec;
      filled=true;
    }
  }catch(e){}
}
$("lang").onchange=()=>{lang=+$("lang").value;applyLang();};
$("non").onchange=nvis;
$("auto").onchange=()=>{$("br").disabled=$("auto").checked};
$("spd").oninput=()=>{$("spdv").textContent=$("spd").value};
$("trspd").oninput=()=>{$("trv").textContent=$("trspd").value};
$("mspd").oninput=()=>{$("mspv").textContent=$("mspd").value};
$("cadd").onclick=async()=>{
  const city=$("ncity").value.trim();if(!city){say("cmsg",t("enter_city"),"err");return;}
  try{await post("/api/city/add",{city:city,country:$("ncc").value.trim().toUpperCase()});
    $("ncity").value="";$("ncc").value="";say("cmsg",t("city_added"),"ok");load();setTimeout(load,4000);}
  catch(e){say("cmsg",e.message,"err");}
};
$("madd").onclick=async()=>{
  const x=$("mt").value.trim();if(!x){say("mmsg",t("enter_msg"),"err");return;}
  try{await post("/api/msg/add",{text:x,color:$("mc").value,size:$("ms").value});
    $("mt").value="";say("mmsg",t("msg_added"),"ok");load();}
  catch(e){say("mmsg",e.message,"err");}
};
async function save(){
  await post("/api/config",{lang:$("lang").value,units:$("units").value,fmt:$("fmt").value,br:$("br").value,auto:$("auto").checked?"1":"0",
    non:$("non").checked?"1":"0",nst:mn($("nst").value),nen:mn($("nen").value),nbr:$("nbr").value,
    spd:$("spd").value,trspd:$("trspd").value,mspd:$("mspd").value,tsec:$("tsec").value,wsec:$("wsec").value,csec:$("csec").value});
}
$("f").onsubmit=async e=>{
  e.preventDefault();say("msg",t("saving"));
  try{await save();say("msg",t("saved"),"ok");load();}catch(x){say("msg",t("save_err"),"err");}
};
$("rp").onclick=async()=>{
  try{await save();await post("/api/replay");say("msg",t("anim_started"),"ok");}catch(x){say("msg",t("error"),"err");}
};
$("rf").onclick=async()=>{await post("/api/refresh");say("msg",t("req_sent"),"ok");setTimeout(load,3500);};
$("wr").onclick=async()=>{
  if(!confirm(t("confirm_reset")))return;
  post("/api/wifi-reset");say("msg",t("restarting"));
};
function wmsg(x,c){const m=$("wmsg");m.textContent=x;m.className="mut "+(c||"");}
function wsel(){$("wo").style.display=$("wl").value==="__other__"?"block":"none";}
async function wscan(){
  const sel=$("wl");sel.innerHTML="";sel.dataset.scanned="1";
  const o=document.createElement("option");o.value="";o.textContent=t("scanning");sel.appendChild(o);
  wmsg("");
  for(let i=0;i<15;i++){
    let r;
    try{r=await (await fetch("/api/wifi/scan")).json();}catch(e){wmsg(t("net_err"),"err");return;}
    if(!r.scanning){
      sel.innerHTML="";
      r.nets.forEach(n=>{const op=document.createElement("option");op.value=n.ssid;op.textContent=n.ssid+" ("+n.rssi+" dBm"+(n.open?"":", "+t("prot"))+")";sel.appendChild(op);});
      const x=document.createElement("option");x.value="__other__";x.textContent=t("other_net");sel.appendChild(x);
      wsel();return;
    }
    await sleep(1000);
  }
  wmsg(t("no_result"),"err");
}
$("wl").onchange=wsel;$("ws").onclick=wscan;
function wtxt(m){if(m.indexOf("w_fail|")==0)return t("w_fail",m.slice(7));return m;}
$("wc").onclick=async()=>{
  const ssid=$("wl").value==="__other__"?$("wo").value:$("wl").value;
  if(!ssid){wmsg(t("choose_net"),"err");return;}
  const pass=$("wp").value;
  if(pass&&pass.length<8){wmsg(t("pass_min"),"err");return;}
  let r;
  try{r=await fetch("/api/wifi/connect",{method:"POST",body:new URLSearchParams({ssid:ssid,pass:pass})});}catch(e){wmsg(t("net_err"),"err");return;}
  if(!r.ok){wmsg(t(await r.text()),"err");return;}
  wmsg(t("connecting"));
  for(let i=0;i<20;i++){
    await sleep(2000);
    try{
      const s=await (await fetch("/api/wifi/state")).json();
      if(s.state===2){wmsg(t("connected_ok",s.ssid,s.ip),"ok");$("wp").value="";return;}
      if(s.state===3){wmsg(wtxt(s.msg),"err");return;}
    }catch(e){}
  }
};
applyLang();load();setInterval(load,5000);
</script></body></html>
)rawliteral";

void handleStatus() {
  JsonDocument d;
  struct tm t;
  char tb[12] = "--:--:--";
  if (getLocal(t)) strftime(tb, sizeof(tb), "%H:%M:%S", &t);
  d["ip"] = WiFi.localIP().toString();
  d["ssid"] = WiFi.SSID();
  d["rssi"] = WiFi.RSSI();
  d["time"] = tb;
  xSemaphoreTake(mtx, portMAX_DELAY);
  JsonArray ca = d["cities"].to<JsonArray>();
  for (int i = 0; i < nCity; i++) {
    const Weather &w = wxs[i];
    JsonObject o = ca.add<JsonObject>();
    char place[64];
    if (w.valid && w.cc[0])
      snprintf(place, sizeof(place), "%s, %s", w.name, w.cc);
    else if (w.valid)
      snprintf(place, sizeof(place), "%s", w.name);
    else
      snprintf(place, sizeof(place), "%s", cities[i].name.c_str());
    o["n"] = cities[i].name;
    o["place"] = place;
    o["ok"] = w.valid;
    o["temp"] = w.temp;
    o["hum"] = w.hum;
    o["code"] = w.code;
    o["err"] = w.err;
    o["age"] = w.valid ? (int)((millis() - w.updated) / 1000) : -1;
  }
  JsonArray ma = d["msgs"].to<JsonArray>();
  for (int i = 0; i < nMsg; i++) {
    JsonObject o = ma.add<JsonObject>();
    char hx[10];
    snprintf(hx, sizeof(hx), "#%06X", (unsigned)(msgs[i].rgb & 0xFFFFFF));
    o["t"] = msgs[i].text;
    o["c"] = hx;
    o["s"] = msgs[i].size;
  }
  JsonObject c = d["cfg"].to<JsonObject>();
  c["f"] = cfg.fahrenheit;
  c["h12"] = cfg.h12;
  c["br"] = cfg.brightness;
  c["auto"] = cfg.autoBright;
  c["spd"] = cfg.speed;
  c["trspd"] = cfg.trSpeed;
  c["mspd"] = cfg.msgSpeed;
  c["tsec"] = cfg.timeSecs;
  c["wsec"] = cfg.wxSecs;
  c["csec"] = cfg.citySecs;
  c["non"] = cfg.nightOn;
  c["nst"] = cfg.nightStart;
  c["nen"] = cfg.nightEnd;
  c["nbr"] = cfg.nightBr;
  c["lang"] = cfg.lang;
  xSemaphoreGive(mtx);
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleConfig() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  cfg.fahrenheit = server.arg("units") == "F";
  cfg.h12 = server.arg("fmt") == "12";
  cfg.autoBright = server.arg("auto") == "1";
  cfg.brightness = constrain(server.arg("br").toInt(), 5, 255);
  cfg.speed = constrain(server.arg("spd").toInt(), 1, 10);
  cfg.trSpeed = constrain(server.arg("trspd").toInt(), 1, 10);
  cfg.msgSpeed = constrain(server.arg("mspd").toInt(), 1, 10);
  cfg.timeSecs = constrain(server.arg("tsec").toInt(), 5, 300);
  cfg.wxSecs = constrain(server.arg("wsec").toInt(), 3, 120);
  cfg.citySecs = constrain(server.arg("csec").toInt(), 3, 120);
  cfg.nightOn = server.arg("non") == "1";
  cfg.nightStart = constrain(server.arg("nst").toInt(), 0, 1439);
  cfg.nightEnd = constrain(server.arg("nen").toInt(), 0, 1439);
  cfg.nightBr = constrain(server.arg("nbr").toInt(), 1, 255);
  if (server.hasArg("lang")) {
    uint8_t nl = constrain(server.arg("lang").toInt(), 0, LANG_N - 1);
    if (nl != cfg.lang) {
      cfg.lang = nl;
      geoClear = true;  // i nomi delle citta' arrivano nella nuova lingua
      wxRefresh = true;
    }
  }
  saveSettings();
  xSemaphoreGive(mtx);
  needDraw = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

// ----- Citta' -----
void handleCityAdd() {
  String city = server.arg("city");
  city.trim();
  String cc = server.arg("country");
  cc.trim();
  cc.toUpperCase();
  if (cc.length() != 2) cc = "";
  if (city.length() == 0 || city.length() > 40) {
    server.send(400, "text/plain", "s_badcity");
    return;
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (nCity >= MAXC) {
    xSemaphoreGive(mtx);
    server.send(400, "text/plain", "s_maxcity");
    return;
  }
  cities[nCity].name = city;
  cities[nCity].cc = cc;
  wxs[nCity] = Weather();
  nCity++;
  saveCities();
  xSemaphoreGive(mtx);
  wxRefresh = true;
  needDraw = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleCityDel() {
  int i = server.arg("i").toInt();
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (i < 0 || i >= nCity) {
    xSemaphoreGive(mtx);
    server.send(400, "text/plain", "s_nocity");
    return;
  }
  for (int k = i; k < nCity - 1; k++) {
    cities[k] = cities[k + 1];
    wxs[k] = wxs[k + 1];
  }
  nCity--;
  cities[nCity] = CityCfg();
  wxs[nCity] = Weather();
  saveCities();
  xSemaphoreGive(mtx);
  wxRefresh = true;
  needDraw = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

// Porta la citta' scelta in prima posizione (la principale): cambia anche il fuso dell'ora grande
void handleCityMain() {
  int i = server.arg("i").toInt();
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (i <= 0 || i >= nCity) {
    xSemaphoreGive(mtx);
    server.send(400, "text/plain", "s_nocity");
    return;
  }
  std::swap(cities[0], cities[i]);
  std::swap(wxs[0], wxs[i]);
  saveCities();
  xSemaphoreGive(mtx);
  wxRefresh = true;
  needDraw = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

// ----- Messaggi -----
void handleMsgAdd() {
  String text = server.arg("text");
  text.trim();
  if (text.length() == 0 || text.length() > 160) {
    server.send(400, "text/plain", "s_msglen");
    return;
  }
  String col = server.arg("color");
  uint32_t rgb = 0xFFD23C;
  if (col.length() == 7 && col[0] == '#') rgb = (uint32_t)strtoul(col.c_str() + 1, nullptr, 16);
  int size = server.arg("size").toInt() == 1 ? 1 : 2;
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (nMsg >= MAXM) {
    xSemaphoreGive(mtx);
    server.send(400, "text/plain", "s_maxmsg");
    return;
  }
  msgs[nMsg].text = text;
  msgs[nMsg].rgb = rgb;
  msgs[nMsg].size = (uint8_t)size;
  foldMsg(msgs[nMsg]);
  if (msgs[nMsg].disp.length() == 0) {
    xSemaphoreGive(mtx);
    server.send(400, "text/plain", "s_noprint");
    return;
  }
  nMsg++;
  saveMsgs();
  xSemaphoreGive(mtx);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleMsgDel() {
  int i = server.arg("i").toInt();
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (i < 0 || i >= nMsg) {
    xSemaphoreGive(mtx);
    server.send(400, "text/plain", "s_nomsg");
    return;
  }
  for (int k = i; k < nMsg - 1; k++) msgs[k] = msgs[k + 1];
  nMsg--;
  msgs[nMsg] = MsgCfg();
  saveMsgs();
  xSemaphoreGive(mtx);
  server.send(200, "application/json", "{\"ok\":true}");
}

// ===================== Cambio WiFi dal web =====================
// La scansione parte in background: la pagina richiama finche' "scanning" e' true
void handleWifiScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) {
    WiFi.scanNetworks(true);
    server.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  if (n == WIFI_SCAN_RUNNING) {
    server.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  JsonDocument d;
  d["scanning"] = false;
  JsonArray a = d["nets"].to<JsonArray>();
  std::vector<String> seen;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    bool dup = false;
    for (size_t k = 0; k < seen.size(); k++)
      if (seen[k] == s) dup = true;
    if (dup) continue;
    seen.push_back(s);
    JsonObject o = a.add<JsonObject>();
    o["ssid"] = s;
    o["rssi"] = WiFi.RSSI(i);
    o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleWifiConnect() {
  String ssid = server.arg("ssid"), pass = server.arg("pass");
  if (ssid.length() == 0 || ssid.length() > 32) {
    server.send(400, "text/plain", "s_ssid");
    return;
  }
  if (pass.length() > 63 || (pass.length() > 0 && pass.length() < 8)) {
    server.send(400, "text/plain", "s_wpass");
    return;
  }
  if (wifiState == 1) {
    server.send(409, "text/plain", "s_busy");
    return;
  }
  wifiJob.ssid = ssid;
  wifiJob.pass = pass;
  wifiJob.pending = true;
  wifiState = 1;
  wifiMsg = "";
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleWifiState() {
  JsonDocument d;
  d["state"] = (int)wifiState;
  d["msg"] = wifiMsg;
  d["ssid"] = WiFi.SSID();
  d["ip"] = WiFi.localIP().toString();
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

// Eseguita dal loop dopo aver risposto alla pagina. Se la nuova rete non risponde
// entro 15 secondi, rimette quella precedente.
void processWifiJob() {
  if (!wifiJob.pending) return;
  wifiJob.pending = false;
  String oldSsid = WiFi.SSID(), oldPass = WiFi.psk();
  String l1 = wifiJob.ssid.substring(0, 10), l2 = wifiJob.ssid.length() > 10 ? wifiJob.ssid.substring(10, 20) : String("");
  showLines("WIFI", "-> ", l1.c_str(), l2.c_str());
  WiFi.begin(wifiJob.ssid.c_str(), wifiJob.pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    MDNS.end();
    if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
    wifiMsg = "Collegato a " + wifiJob.ssid;
    wifiState = 2;
    wxRefresh = true;
    marquee("Config: http://" + WiFi.localIP().toString() + " o http://" HOSTNAME ".local", RGB(0, 220, 255), 12);
  } else {
    WiFi.disconnect();
    if (oldSsid.length() > 0) {
      WiFi.begin(oldSsid.c_str(), oldPass.c_str());
      uint32_t t1 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t1 < 10000) delay(200);
    }
    wifiMsg = "w_fail|" + wifiJob.ssid;
    wifiState = 3;
  }
  wifiJob.pass = "";
  lastClockMs = millis();
  needDraw = true;
}

void setupWeb() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); });
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi/connect", HTTP_POST, handleWifiConnect);
  server.on("/api/wifi/state", HTTP_GET, handleWifiState);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/config", HTTP_POST, handleConfig);
  server.on("/api/city/add", HTTP_POST, handleCityAdd);
  server.on("/api/city/del", HTTP_POST, handleCityDel);
  server.on("/api/city/main", HTTP_POST, handleCityMain);
  server.on("/api/msg/add", HTTP_POST, handleMsgAdd);
  server.on("/api/msg/del", HTTP_POST, handleMsgDel);
  server.on("/api/refresh", HTTP_POST, []() {
    wxRefresh = true;
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/replay", HTTP_POST, []() {
    replayFlag = true;
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/wifi-reset", HTTP_POST, []() {
    server.send(200, "application/json", "{\"ok\":true}");
    delay(300);
    WiFiManager wm;
    wm.resetSettings();
    ESP.restart();
  });
  server.onNotFound([]() { server.send(404, "text/plain", "Non trovato"); });
  server.begin();
}

// ===================== Setup / loop =====================
void setupDisplay() {
  HUB75_I2S_CFG::i2s_pins pins = {PIN_R1, PIN_G1, PIN_B1, PIN_R2, PIN_G2, PIN_B2, PIN_A,
                                  PIN_B,  PIN_C,  PIN_D,  PIN_E,  PIN_LAT, PIN_OE, PIN_CLK};
  HUB75_I2S_CFG mxconfig(PANEL_W, PANEL_H, 1, pins);
  mxconfig.double_buff = true;
  // Se i colori sono strani o compaiono disturbi, prova una di queste opzioni:
  // mxconfig.clkphase = false;
  // mxconfig.driver = HUB75_I2S_CFG::FM6124;
  display = new MatrixPanel_I2S_DMA(mxconfig);
  display->begin();
  g = display;
  display->setBrightness8(cfg.brightness);
  display->clearScreen();
  flip();
}

// Il server web gira su un task a parte (core 0), cosi' non aspetta il disegno del pannello
void webTask(void *) {
  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void setup() {
  Serial.begin(115200);
  mtx = xSemaphoreCreateMutex();
  setenv("TZ", DEFAULT_TZ, 1);
  tzset();
  loadSettings();
  initColors();
  setupDisplay();
  showLines("Avvio...");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFiManager wm;
  // Scelta della lingua nel portale: un menu a tendina che riempie un campo nascosto
  static char langHtml[700];
  {
    static const char *NAMES[LANG_N] = {"Italiano", "English", "Espa&ntilde;ol", "Fran&ccedil;ais", "Deutsch"};
    String h = "<br/><label for='lsel'>Lingua / Language / Idioma / Langue / Sprache</label>"
               "<select id='lsel' onchange=\"document.getElementById('lang').value=this.value\">";
    for (int i = 0; i < LANG_N; i++) {
      h += "<option value='" + String(LANG_CODE[i]) + "'" + (i == cfg.lang ? " selected" : "") + ">" + NAMES[i] + "</option>";
    }
    h += "</select>";
    strlcpy(langHtml, h.c_str(), sizeof(langHtml));
  }
  WiFiManagerParameter langSelect(langHtml);
  WiFiManagerParameter langField("lang", "", LANG_CODE[cfg.lang], 2, "type='hidden'", WFM_NO_LABEL);
  wm.addParameter(&langSelect);
  wm.addParameter(&langField);
  wm.setConnectTimeout(20);
  wm.setAPCallback([](WiFiManager *) { showLines("SETUP WIFI", "WiFi:", AP_NAME, "192.168.4.1"); });
  if (!wm.autoConnect(AP_NAME)) {
    ESP.restart();
  }
  WiFi.setAutoReconnect(true);
  {
    // lingua scelta nel portale (se e' stato mostrato); altrimenti resta quella salvata
    String l = langField.getValue();
    for (int i = 0; i < LANG_N; i++)
      if (l == LANG_CODE[i] && (i != cfg.lang || !langKnown)) {
        cfg.lang = i;
        prefs.putUChar("lang", cfg.lang);
        langKnown = true;
      }
  }

  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
  setupWeb();

  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  showLines("WiFi OK", "NTP", "...");
  for (int i = 0; i < 40 && time(nullptr) < 1700000000; i++) delay(250);

  WiFi.setSleep(false);  // niente risparmio energetico: la pagina web risponde molto piu' in fretta
  xTaskCreatePinnedToCore(weatherTask, "weather", 12288, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(webTask, "web", 8192, nullptr, 2, nullptr, 0);

  String ip = WiFi.localIP().toString();
  marquee("Config: http://" + ip + " o http://" HOSTNAME ".local", RGB(0, 220, 255), 12);
  lastClockMs = millis();
}

void maintainWifi(uint32_t now) {
  static uint32_t last = 0;
  if (WiFi.status() == WL_CONNECTED || now - last < 30000) return;
  last = now;
  WiFi.reconnect();
}

// ===================== Pagine e scorrimento =====================
// Elenco delle pagine in questo momento: ora, meteo della principale, altre citta', messaggi
int buildPages(Page *pg) {
  int n = 0;
  bool ok[MAXC];
  xSemaphoreTake(mtx, portMAX_DELAY);
  int nc = nCity, nm = nMsg;
  for (int i = 0; i < nc; i++) ok[i] = wxs[i].valid && wxs[i].haveOffset;
  xSemaphoreGive(mtx);
  pg[n++] = Page{PG_TIME, 0};
  if (nc > 0 && ok[0]) pg[n++] = Page{PG_WX, 0};
  for (int i = 1; i < nc; i++)
    if (ok[i]) pg[n++] = Page{PG_CITY, i};
  for (int i = 0; i < nm; i++) pg[n++] = Page{PG_MSG, i};
  return n;
}

int findPage(const Page *pg, int n, const Page &p) {
  for (int k = 0; k < n; k++)
    if (pg[k].t == p.t && pg[k].i == p.i) return k;
  return -1;
}

bool pageDone(const Page &p, uint32_t now) {
  uint32_t el = now - pageSince;
  switch (p.t) {
    case PG_TIME: return !engine.animating(tickNow) && el >= cfg.timeSecs * 1000UL;
    case PG_WX: return el >= cfg.wxSecs * 1000UL;
    case PG_CITY: return el >= cfg.citySecs * 1000UL;
    case PG_MSG: return el >= msgDurationMs(p.i);
  }
  return false;
}

// Mette le cifre gia' composte (senza animazione), per quando l'ora rientra scorrendo
void snapDigits(const struct tm &tmv) {
  int d[4];
  computeDigits(tmv, d);
  engine.reset();
  engine.seed(esp_random());
  for (int i = 0; i < 4; i++) {
    engine.setDigit(i, d[i], tickNow, 0);
    engine.slot[i].old.clear();
    engine.slot[i].anim = false;
  }
}

// Disegna una pagina su "g" (pannello o canvas). el = ms dall'inizio della pagina
void paintPage(const Page &p, uint32_t now, uint32_t el, const struct tm &tmv) {
  Weather w;
  switch (p.t) {
    case PG_TIME:
      paintTime(((now / 500) & 1) == 0, tmv.tm_hour >= 12);
      break;
    case PG_WX:
      if (getWx(p.i, w)) paintWeather(w, now);
      else g->fillScreen(0);
      break;
    case PG_CITY:
      if (getWx(p.i, w)) paintCity(w, now);
      else g->fillScreen(0);
      break;
    case PG_MSG:
      paintMsg(p.i, el);
      break;
  }
}

// Il contenuto scorre verso sinistra: A esce a sinistra, B entra da destra
void blitSlide(int off) {
  const uint16_t *a = cvA.getBuffer(), *b = cvB.getBuffer();
  for (int y = 0; y < PANEL_H; y++)
    for (int x = 0; x < PANEL_W; x++) {
      int sx = x + off;
      uint16_t c = sx < PANEL_W ? a[y * PANEL_W + sx] : b[y * PANEL_W + sx - PANEL_W];
      display->drawPixel(x, y, c);
    }
}

void loop() {
  processWifiJob();
  uint32_t now = millis();
  maintainWifi(now);
  updateBrightness(now);
  advanceTicks(now);
  g = display;

  struct tm tmv;
  if (!getLocal(tmv)) {
    static uint32_t l = 0;
    if (now - l > 1000) {
      l = now;
      showLines("NTP", "Sync...");
    }
    delay(5);
    return;
  }

  Page pg[2 + MAXC + MAXM];
  int np = buildPages(pg);

  if (replayFlag) {
    replayFlag = false;
    tr.on = false;
    cur = Page{PG_TIME, 0};
    pageSince = now;
    forceIntro = true;
  }

  // la pagina corrente e' stata rimossa: si torna all'ora
  if (!tr.on && findPage(pg, np, cur) < 0) {
    cur = Page{PG_TIME, 0};
    pageSince = now;
    snapDigits(tmv);
    needDraw = true;
  }

  if (!tr.on && np > 1 && pageDone(cur, now)) {
    int ix = findPage(pg, np, cur);
    Page nx = pg[(ix + 1) % np];
    if (nx.t == PG_TIME) snapDigits(tmv);
    tr.on = true;
    tr.from = cur;
    tr.to = nx;
    tr.start = now;
  }

  if (tr.on) {
    float p = (float)(now - tr.start) / (float)trMs();
    if (p > 1.0f) p = 1.0f;
    float e = p < 0.5f ? 2 * p * p : 1 - 2 * (1 - p) * (1 - p);
    g = &cvA;
    paintPage(tr.from, now, now - pageSince, tmv);
    g = &cvB;
    paintPage(tr.to, now, 0, tmv);
    g = display;
    blitSlide((int)lroundf(e * PANEL_W));
    flip();
    if (p >= 1.0f) {
      cur = tr.to;
      pageSince = now;
      tr.on = false;
      needDraw = true;
    }
    delay(1);
    return;
  }

  if (cur.t == PG_TIME) {
    int d[4];
    computeDigits(tmv, d);
    if (forceIntro) {
      forceIntro = false;
      engine.reset();
      engine.seed(esp_random());
      for (int i = 0; i < 4; i++) engine.setDigit(i, d[i], tickNow, i * 4);  // le cifre arrivano una dopo l'altra
      needDraw = true;
    } else {
      for (int i = 0; i < 4; i++) engine.setDigit(i, d[i], tickNow, 0);  // cambia solo cio' che e' cambiato
    }
    bool animating = engine.animating(tickNow);

    static uint32_t lastDrawTick = 0xFFFFFFFFu;
    static bool wasAnim = false, lastColon = false;
    bool colon = ((now / 500) & 1) == 0;
    bool tickChanged = tickNow != lastDrawTick;
    if (needDraw || colon != lastColon || ((animating || wasAnim) && tickChanged)) {
      paintTime(colon, tmv.tm_hour >= 12);
      flip();
      lastDrawTick = tickNow;
      lastColon = colon;
      needDraw = false;
    }
    wasAnim = animating;
  } else {
    static uint32_t lastDraw = 0;
    if (now - lastDraw >= (cur.t == PG_MSG ? 16u : 33u)) {
      lastDraw = now;
      paintPage(cur, now, now - pageSince, tmv);
      flip();
    }
  }
  delay(1);
}
