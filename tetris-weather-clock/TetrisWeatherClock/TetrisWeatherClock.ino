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
#include "FastServer.h"
#include "WebUI.h"

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
#define AP_PASS "tetrisclock"  // password dell'hotspot (minimo 8 caratteri)
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
  bool apOn = false;              // hotspot di sicurezza (spento di default)
};

struct CityCfg {
  String name;
  String cc;  // codice ISO a 2 lettere, facoltativo
  bool fc = true;  // mostra la pagina previsioni per questa citta'
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
  bool haveFc = false;
  int8_t fcMax[3] = {0, 0, 0};
  int8_t fcMin[3] = {0, 0, 0};
  uint8_t fcHum[3] = {0, 0, 0};
  uint8_t fcCode[3] = {0, 0, 0};
};

struct GeoEnt {
  String key;
  float lat = 0, lon = 0;
  char name[48] = "";
  char cc[4] = "";
  bool bad = false;
};

enum PType { PG_TIME, PG_WX, PG_CITY, PG_FC, PG_MSG };
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
FastServer server;
volatile bool apDirty = false;  // da applicare dal loop: accende/spegne l'hotspot
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
    o["f"] = cities[i].fc;
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
  cfg.apOn = prefs.getBool("ap2", false);
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
        cities[nCity].fc = o["f"] | true;
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
  prefs.putBool("ap2", cfg.apOn);
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

  char url[360];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code,is_day"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,relative_humidity_2m_mean"
           "&forecast_days=3&timezone=auto",
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
    JsonObject dd = doc["daily"];
    w.haveFc = dd["temperature_2m_max"].size() >= 3 && dd["temperature_2m_min"].size() >= 3 &&
               dd["weather_code"].size() >= 3;
    if (w.haveFc)
      for (int k = 0; k < 3; k++) {
        w.fcMax[k] = (int8_t)lroundf(dd["temperature_2m_max"][k] | 0.0f);
        w.fcMin[k] = (int8_t)lroundf(dd["temperature_2m_min"][k] | 0.0f);
        w.fcHum[k] = (uint8_t)constrain((int)lroundf(dd["relative_humidity_2m_mean"][k] | 0.0f), 0, 99);
        w.fcCode[k] = (uint8_t)(dd["weather_code"][k] | 0);
      }
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

// ===================== Pagina previsioni a 3 giorni =====================
static const char *const DAYN[LANG_N][7] = {
    {"Dom", "Lun", "Mar", "Mer", "Gio", "Ven", "Sab"}, {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"},
    {"Dom", "Lun", "Mar", "Mie", "Jue", "Vie", "Sab"}, {"Dim", "Lun", "Mar", "Mer", "Jeu", "Ven", "Sam"},
    {"Son", "Mon", "Die", "Mit", "Don", "Fre", "Sam"}};

void d35(int d, int x, int y, uint16_t c) {
  for (int r = 0; r < 5; r++)
    for (int b = 0; b < 3; b++)
      if (D35[d][r] & (4 >> b)) put(x + b, y + r, c);
}

int n35w(int n) { return n >= 10 ? 7 : 3; }

void n35(int n, int x, int y, uint16_t c) {
  if (n >= 10) {
    d35((n / 10) % 10, x, y, c);
    d35(n % 10, x + 4, y, c);
  } else {
    d35(n % 10, x, y, c);
  }
}

void miniCloud(int x, int y, uint16_t c) {
  g->fillRect(x + 1, y + 2, 5, 3, c);
  g->fillRect(x + 2, y + 1, 3, 1, c);
  put(x, y + 3, c);
  put(x + 6, y + 3, c);
}

// Icona 7x7 (x,y = angolo alto-sinistra) in base al codice meteo
void miniIcon(int code, int x, int y, uint32_t t) {
  int f = (t / 400) % 2;
  const uint16_t Y = RGB(255, 210, 0), O = RGB(255, 160, 0), B = RGB(60, 140, 255);
  if (code == 0 || code == 1) {  // sereno
    g->fillRect(x + 2, y + 2, 3, 3, Y);
    put(x + 3, y, O); put(x + 3, y + 6, O); put(x, y + 3, O); put(x + 6, y + 3, O);
    if (f) { put(x + 1, y + 1, O); put(x + 5, y + 1, O); put(x + 1, y + 5, O); put(x + 5, y + 5, O); }
  } else if (code == 2) {  // parzialmente nuvoloso
    g->fillRect(x, y, 3, 3, Y);
    put(x + 3, y + 1, O); put(x + 1, y + 3, O);
    miniCloud(x, y + 1, RGB(205, 210, 220));
  } else if (code == 3 || code == 45 || code == 48) {  // nuvoloso / nebbia
    miniCloud(x, y + 1, RGB(200, 205, 215));
    g->fillRect(x + 1, y + 5, 5, 1, RGB(120, 130, 145));
  } else if (code >= 95) {  // temporale
    miniCloud(x, y, RGB(90, 95, 110));
    if (f) { put(x + 3, y + 5, RGB(255, 230, 0)); put(x + 2, y + 6, RGB(255, 230, 0)); put(x + 4, y + 6, RGB(255, 230, 0)); }
    else { put(x + 2, y + 5, B); put(x + 5, y + 6, B); }
  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {  // neve
    miniCloud(x, y, RGB(150, 160, 180));
    for (int dx = 1; dx <= 5; dx += 2) put(x + dx, y + 5 + ((f + dx) % 2), RGB(240, 245, 255));
  } else {  // pioggia / pioviggine
    miniCloud(x, y, RGB(110, 120, 140));
    for (int dx = 1; dx <= 5; dx += 2) put(x + dx, y + 5 + ((f + dx) % 2), B);
  }
}

void paintForecast(const Weather &w, uint32_t t) {
  g->fillScreen(0);
  // intestazione: citta' a sinistra (scorre se lunga), ora locale a destra
  {
    int nameLen = (int)strlen(w.name), ccLen = (int)strlen(w.cc);
    int wpx = (nameLen + (ccLen ? ccLen + 1 : 0)) * 6 - 1;
    const int AREA = 44;
    int off = 0;
    if (wpx > AREA) {
      int steps = wpx - AREA, pause = 20, cycle = 2 * (steps + pause);
      int i = (int)((t / 50) % cycle);
      if (i < pause) off = 0;
      else if (i < pause + steps) off = i - pause;
      else if (i < 2 * pause + steps) off = steps;
      else off = steps - (i - (2 * pause + steps));
    }
    g->setTextSize(1);
    g->setTextWrap(false);
    g->setCursor(1 - off, 0);
    g->setTextColor(RGB(235, 240, 255));
    g->print(w.name);
    if (ccLen) {
      g->setTextColor(RGB(110, 150, 210));
      g->print(' ');
      g->print(w.cc);
    }
    g->fillRect(AREA, 0, PANEL_W - AREA, 8, 0);
  }
  time_t l = time(nullptr) + w.utcOffset;
  struct tm tt;
  gmtime_r(&l, &tt);
  int h = tt.tm_hour;
  if (cfg.h12) {
    h %= 12;
    if (h == 0) h = 12;
  }
  uint16_t tcol = RGB(255, 225, 80);
  int tx = 46;
  if (h >= 10 || !cfg.h12) { d35(h / 10, tx, 1, tcol); }
  d35(h % 10, tx + 4, 1, tcol);
  if (((t / 500) & 1) == 0) { put(tx + 8, 2, RGB(0, 225, 255)); put(tx + 8, 4, RGB(0, 225, 255)); }
  d35(tt.tm_min / 10, tx + 10, 1, tcol);
  d35(tt.tm_min % 10, tx + 14, 1, tcol);

  int wd = tt.tm_wday;
  const int lg = constrain((int)cfg.lang, 0, LANG_N - 1);
  const uint16_t GRAY = RGB(120, 120, 135);
  for (int k = 0; k < 3; k++) {
    int x = k * 21;
    g->setTextColor(k == 0 ? RGB(255, 230, 0) : RGB(235, 240, 255));
    g->setCursor(x + 2, 8);
    g->print(DAYN[lg][(wd + k) % 7]);
    miniIcon(w.fcCode[k], x + 1, 17, t);
    uint16_t hc = humColor(w.fcHum[k]);
    n35(w.fcHum[k], x + 8, 18, hc);
    {  // simbolo % 4x5 subito dopo le cifre
      static const uint8_t PCT[5] = {9, 1, 6, 8, 9};
      int px = x + 8 + n35w(w.fcHum[k]) + 1;
      for (int r = 0; r < 5; r++)
        for (int b = 0; b < 4; b++)
          if (PCT[r] & (8 >> b)) put(px + b, 18 + r, hc);
    }
    int mx = w.fcMax[k], mn = w.fcMin[k];
    if (cfg.fahrenheit) {
      mx = (int)lroundf(mx * 9.0f / 5.0f + 32.0f);
      mn = (int)lroundf(mn * 9.0f / 5.0f + 32.0f);
    }
    // numeri negativi: segno meno e cifre a 1-2 posizioni; i valori a 3 cifre (F) si limitano a 99
    bool nmx = mx < 0, nmn = mn < 0;
    int amx = min(abs(mx), 99), amn = min(abs(mn), 99);
    int wmax = n35w(amx) + (nmx ? 2 : 0), wmin = n35w(amn) + (nmn ? 2 : 0);
    int tot = wmax + 1 + 3 + 1 + wmin;
    int sx = x + (21 - tot) / 2;
    if (sx < x) sx = x;
    int cx = sx;
    uint16_t cmax = tempColor(w.fcMax[k]);
    if (nmx) { g->fillRect(cx, 27, 2, 1, cmax); cx += 2; }
    n35(amx, cx, 25, cmax);
    int sl = sx + wmax + 1;  // barra
    put(sl + 2, 25, GRAY); put(sl + 1, 26, GRAY); put(sl + 1, 27, GRAY); put(sl, 28, GRAY); put(sl, 29, GRAY);
    cx = sl + 4;
    uint16_t cmin = RGB(90, 130, 220);
    if (nmn) { g->fillRect(cx, 27, 2, 1, cmin); cx += 2; }
    n35(amn, cx, 25, cmin);
    if (k > 0)
      for (int y = 9; y < 32; y += 2) put(x - 1, y, RGB(30, 32, 40));
  }
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


void handleStatus() {
  JsonDocument d;
  struct tm t;
  char tb[12] = "--:--:--";
  if (getLocal(t)) strftime(tb, sizeof(tb), "%H:%M:%S", &t);
  d["ip"] = WiFi.localIP().toString();
  d["heap"] = ESP.getFreeHeap() / 1024;
  d["up"] = (uint32_t)(millis() / 1000);
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
    o["fc"] = cities[i].fc;
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
  c["ap"] = cfg.apOn;
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
  {
    bool a = server.arg("ap") != "0";
    if (a != cfg.apOn) apDirty = true;
    cfg.apOn = a;
  }
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
  cities[nCity].fc = server.arg("fc") != "0";
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
void handleCityFc() {
  int i = server.arg("i").toInt();
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (i < 0 || i >= nCity) {
    xSemaphoreGive(mtx);
    server.send(400, "text/plain", "s_nocity");
    return;
  }
  cities[i].fc = server.arg("on") == "1";
  saveCities();
  xSemaphoreGive(mtx);
  needDraw = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

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
  server.on("/", HTTP_GET, []() { server.sendGz(INDEX_GZ, INDEX_GZ_LEN); });
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi/connect", HTTP_POST, handleWifiConnect);
  server.on("/api/wifi/state", HTTP_GET, handleWifiState);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/config", HTTP_POST, handleConfig);
  server.on("/api/city/add", HTTP_POST, handleCityAdd);
  server.on("/api/city/del", HTTP_POST, handleCityDel);
  server.on("/api/city/main", HTTP_POST, handleCityMain);
  server.on("/api/city/fc", HTTP_POST, handleCityFc);
  server.on("/api/reboot", HTTP_POST, []() {
    server.send(200, "application/json", "{\"ok\":true}");
    delay(400);  // il tempo di far arrivare la risposta al browser
    ESP.restart();
  });
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

void setup() {
  Serial.begin(9600);
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

  WiFi.setSleep(false);  // subito: il risparmio energetico fa perdere pacchetti con alcuni router
  // Prima l'ora da internet con la sola connessione al router, poi si accende l'hotspot
  Serial.printf("[WiFi] ip=%s gw=%s dns=%s ch=%d\n", WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str(), WiFi.channel());
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  showLines("WiFi OK", "NTP", "...");
  for (int i = 0; i < 40 && time(nullptr) < 1700000000; i++) delay(250);
  Serial.printf("[NTP] time=%ld\n", (long)time(nullptr));
  applyAp();

  WiFi.setSleep(false);  // niente risparmio energetico: la pagina web risponde molto piu' in fretta
  xTaskCreatePinnedToCore(weatherTask, "weather", 12288, nullptr, 1, nullptr, 0);

  String ip = WiFi.localIP().toString();
  marquee("Config: http://" + ip + " o http://" HOSTNAME ".local", RGB(0, 220, 255), 12);
  lastClockMs = millis();
}

// Hotspot di sicurezza: rete TetrisClock sempre disponibile (192.168.4.1), anche senza router
void applyAp() {
  if (cfg.apOn) {
    if (WiFi.getMode() != WIFI_AP_STA) WiFi.mode(WIFI_AP_STA);
    // stesso canale del router: cosi' la connessione al router non viene interrotta
    int ch = WiFi.status() == WL_CONNECTED ? WiFi.channel() : 1;
    WiFi.softAP(AP_NAME, AP_PASS, ch);
    Serial.printf("[AP] %s ch=%d sta=%d\n", AP_NAME, ch, (int)WiFi.status());
  } else {
    WiFi.softAPdisconnect(true);
    if (WiFi.getMode() != WIFI_STA) WiFi.mode(WIFI_STA);
  }
}

void maintainWifi(uint32_t now) {
  static uint32_t last = 0, downSince = 0;
  if (WiFi.status() == WL_CONNECTED) {
    if (downSince && !cfg.apOn) apDirty = true;  // la rete e' tornata: l'hotspot di emergenza si spegne
    downSince = 0;
    return;
  }
  if (!downSince) downSince = now ? now : 1;
  // rete di casa assente da oltre un minuto: si accende l'hotspot di emergenza (non salvato)
  if (now - downSince > 60000 && WiFi.getMode() != WIFI_AP_STA) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_NAME, AP_PASS, 1);
  }
  if (now - last < 30000) return;
  last = now;
  WiFi.reconnect();
}

// ===================== Pagine e scorrimento =====================
// Elenco delle pagine in questo momento: ora, meteo della principale, altre citta', messaggi
int buildPages(Page *pg) {
  int n = 0;
  bool ok[MAXC], fc[MAXC];
  xSemaphoreTake(mtx, portMAX_DELAY);
  int nc = nCity, nm = nMsg;
  for (int i = 0; i < nc; i++) {
    ok[i] = wxs[i].valid && wxs[i].haveOffset;
    fc[i] = ok[i] && wxs[i].haveFc && cities[i].fc;
  }
  xSemaphoreGive(mtx);
  pg[n++] = Page{PG_TIME, 0};
  if (nc > 0 && ok[0]) pg[n++] = Page{PG_WX, 0};
  if (nc > 0 && fc[0]) pg[n++] = Page{PG_FC, 0};
  for (int i = 1; i < nc; i++) {
    if (ok[i]) pg[n++] = Page{PG_CITY, i};
    if (fc[i]) pg[n++] = Page{PG_FC, i};
  }
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
    case PG_FC: return el >= (cfg.wxSecs < 8 ? 8 : cfg.wxSecs) * 1000UL;
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
    case PG_FC:
      if (getWx(p.i, w)) paintForecast(w, now);
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
  if (apDirty) {
    apDirty = false;
    applyAp();
  }
  processWifiJob();
  uint32_t now = millis();
  maintainWifi(now);
  updateBrightness(now);
  advanceTicks(now);
  g = display;

  struct tm tmv;
  if (!getLocal(tmv)) {
    static uint32_t l = 0, r = 0;
    if (now - l > 1000) {
      l = now;
      showLines("NTP", "Sync...");
    }
    if (now - r > 8000) {  // riprova: riconnette il WiFi se serve e richiede l'ora
      r = now;
      Serial.printf("[NTP] attesa: wifi=%d ip=%s dns=%s\n", (int)WiFi.status(), WiFi.localIP().toString().c_str(),
                    WiFi.dnsIP().toString().c_str());
      IPAddress nip;
      int dr = WiFi.hostByName("pool.ntp.org", nip);
      Serial.printf("[DNS] pool.ntp.org r=%d ip=%s rssi=%d gw=%s\n", dr, nip.toString().c_str(), WiFi.RSSI(),
                    WiFi.gatewayIP().toString().c_str());
      if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
      // dopo un po' si provano anche gli indirizzi IP dei server (se il DNS non risponde)
      if (now > 30000) configTime(0, 0, "162.159.200.1", "216.239.35.0", "pool.ntp.org");
      else configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    }
    delay(5);
    return;
  }

  Page pg[2 + 2 * MAXC + MAXM];
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
