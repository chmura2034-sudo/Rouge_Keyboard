// ============================================================ BRAMKA (nadajnik)
// ESP32 z Wi-Fi: strona WWW do edycji makr, wysylka makr do klawiatury, zdalne uruchamianie makr
// (po numerze makra na liscie - dziala tez dla makr bez fizycznego triggera), odbior wsadowego
// logu klikniec (raz na ok. minute, dziala tez jako heartbeat) przez ESP-NOW oraz podglad tego
// logu na stronie WWW (endpoint /keylog, odpytywany z przegladarki co kilka sekund).
// Arduino-ESP32 core 2.x (PlatformIO), biblioteka ArduinoJson 7
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "proto.h"

// ====== DO ZMIANY ======
const char *WIFI_SSID = "Kernel_301";
const char *WIFI_PASS = "Gdzie_Jest_Asia?";
uint8_t kbMac[6] = {0x3C, 0x0F, 0x02, 0xDA, 0x3D, 0x20};   // MAC KLAWIATURY (z jej logu startowego)
const char *WWW_USER = "admin";
const char *WWW_PASS = "haslo";
// =======================

#define RADIO_CMD_MARKER 0xFE      // pakiet "uruchom makro": [0xFE][numer makra na liscie, od 0]

WebServer server(80);
Preferences prefs;

uint16_t macroVersion = 0;
volatile int32_t  lastAck = -2;
volatile bool     doneFlag = false;
volatile uint32_t doneValue = 0;
volatile bool     helloPending = false;
volatile uint16_t kbVersion = 0;
volatile int      sendStatus = -1;   // wynik ostatniego esp_now_send: -1 czeka, 0 dostarczono, 1 blad
bool pendingPush = false;

// CRC32 - identyczny jak na klawiaturze
static uint32_t crc32Update(uint32_t crc, const uint8_t *d, size_t n) {
  crc = ~crc;
  while (n--) {
    crc ^= *d++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

// ---------------------------------------------------------------- Podglad logu klikniec na stronie WWW
//
// Bramka trzyma krotka, zawijana historie ostatnio ODEBRANYCH (juz potwierdzonych) zdarzen
// jako gotowe do wyswietlenia linie tekstu. Zasila ja TA SAMA funkcja, ktora wypisuje log
// w konsoli (decodeAndPrintLog) - zero dodatkowego ryzyka utraty danych, bo zrodlem jest juz
// skladowy, zweryfikowany CRC-iem wsadowy transfer, a nie pojedyncze pakiety "na zywo".

#define KEYLOG_LINES    300
#define KEYLOG_LINE_LEN 40
static char     keylogRing[KEYLOG_LINES][KEYLOG_LINE_LEN];
static uint16_t keylogHead  = 0;   // indeks najstarszej linii
static uint16_t keylogCount = 0;

static void keylogPush(const char *line) {
  uint16_t slot = (keylogHead + keylogCount) % KEYLOG_LINES;
  strncpy(keylogRing[slot], line, KEYLOG_LINE_LEN - 1);
  keylogRing[slot][KEYLOG_LINE_LEN - 1] = 0;
  if (keylogCount < KEYLOG_LINES) keylogCount++;
  else                            keylogHead = (keylogHead + 1) % KEYLOG_LINES;
}

void handleKeylog() {
  if (!server.authenticate(WWW_USER, WWW_PASS)) return server.requestAuthentication();
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint16_t i = 0; i < keylogCount; i++) arr.add(keylogRing[(keylogHead + i) % KEYLOG_LINES]);
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ---------------------------------------------------------------- Odbior wsadowego logu klikniec
//
// Klawiatura raz na ok. minute wysyla caly bufor klikniec jednym niezawodnym transferem
// (LOG_BEGIN -> LOG_DATA x N -> LOG_DONE), tym samym wzorcem co aktualizacja makr.
// onRecv tylko sklada dane i wysyla ACK/LOG_DONE (krotkie esp_now_send, szybkie) -
// dekodowanie na tekst i Serial.printf/keylogPush dzieje sie DOPIERO w loop(), gdy cala paczka
// jest juz potwierdzona, wiec nie ma ryzyka blokowania odbioru w trakcie samej transmisji.

#define LOG_RX_MAX_BYTES 6200   // >= LOG_ENTRY_CAP*sizeof(LogEntry) po stronie klawiatury, z zapasem

static uint8_t  logRxBuf[LOG_RX_MAX_BYTES];
static uint16_t logTotalBytes = 0, logTotalChunks = 0, logExpectedChunk = 0;
static uint32_t logCrcWanted = 0, logCrcRunning = 0;
static bool     logReceiving = false;

static volatile bool     logReady      = false;   // ustawiane w onRecv, obslugiwane w loop()
static volatile uint32_t logReadyBytes = 0;
static volatile bool     logReadyOk    = false;

static void logSendAck(uint16_t idx) { Msg a{}; a.type = MSG_ACK; a.idx = idx; esp_now_send(kbMac, (uint8_t *)&a, sizeof(a)); }

static void handleLogMsg(const Msg *m) {
  if (m->type == MSG_LOG_BEGIN) {
    logTotalBytes    = m->idx;
    logTotalChunks   = (logTotalBytes + CHUNK - 1) / CHUNK;
    logExpectedChunk = 0;
    logCrcWanted     = m->value;
    logCrcRunning    = 0;
    logReceiving     = (logTotalChunks > 0);
    logSendAck(LOG_ACK_BEGIN);

    if (logTotalChunks == 0) {                      // heartbeat: brak nowych klikniec
      bool ok = (logCrcRunning == logCrcWanted);     // CRC pustych danych = 0 po obu stronach
      Msg done{}; done.type = MSG_LOG_DONE; done.value = ok ? 1 : 0;
      esp_now_send(kbMac, (uint8_t *)&done, sizeof(done));
      logReadyBytes = 0; logReadyOk = ok; logReady = true;
    }
    return;
  }

  if (m->type == MSG_LOG_DATA && logReceiving) {
    if (m->len > CHUNK) return;

    if (m->idx < logExpectedChunk) { logSendAck((uint16_t)(m->idx | LOG_ACK_BIT)); return; }  // duplikat
    if (m->idx != logExpectedChunk) return;                                                   // luka - powtorzy

    uint32_t off = (uint32_t)logExpectedChunk * CHUNK;
    if (off + m->len <= LOG_RX_MAX_BYTES) {
      memcpy(logRxBuf + off, m->data, m->len);
      logCrcRunning = crc32Update(logCrcRunning, m->data, m->len);
    }
    logExpectedChunk++;
    logSendAck((uint16_t)(m->idx | LOG_ACK_BIT));

    if (logExpectedChunk == logTotalChunks) {
      logReceiving = false;
      bool ok = (logCrcRunning == logCrcWanted);
      Msg done{}; done.type = MSG_LOG_DONE; done.value = ok ? 1 : 0;
      esp_now_send(kbMac, (uint8_t *)&done, sizeof(done));
      logReadyBytes = logTotalBytes; logReadyOk = ok; logReady = true;
    }
  }
}

// Wolane WYLACZNIE z loop() - dekoduje odebrany log na tekst, wypisuje w konsoli i dopisuje
// do historii widocznej na stronie WWW (/keylog).
static char hidToChar(uint8_t k) {
  if (k >= 0x04 && k <= 0x1D) return 'a' + (k - 0x04);   // litery a-z
  if (k >= 0x1E && k <= 0x26) return '1' + (k - 0x1E);   // cyfry 1-9
  if (k == 0x27) return '0';
  return 0;
}

static void decodeAndPrintLog() {
  uint32_t totalBytes = logReadyBytes;
  bool ok = logReadyOk;
  uint16_t n = (uint16_t)(totalBytes / sizeof(LogEntry));

  if (!ok) {
    Serial.println("[LOG] odebrano paczke, ale CRC sie nie zgadza - dane odrzucone");
    keylogPush("!! blad CRC - paczka odrzucona");
    return;
  }
  if (n == 0) {
    Serial.println("[LOG] heartbeat - klawiatura online, brak nowych klikniec");
    char line[KEYLOG_LINE_LEN];
    snprintf(line, sizeof(line), "-- heartbeat (%lus) --", millis() / 1000);
    keylogPush(line);
    return;
  }

  Serial.printf("[LOG] odebrano paczke: %u zdarzen\n", n);
  char hdr[KEYLOG_LINE_LEN];
  snprintf(hdr, sizeof(hdr), "== paczka %u zdarzen (%lus) ==", n, millis() / 1000);
  keylogPush(hdr);

  uint32_t cum = 0;
  for (uint16_t i = 0; i < n; i++) {
    LogEntry e;
    memcpy(&e, logRxBuf + (size_t)i * sizeof(LogEntry), sizeof(e));
    cum += e.dtMs;

    char mods[20] = "";
    if (e.mod & 0x11) strcat(mods, "Ctrl+");
    if (e.mod & 0x22) strcat(mods, "Shift+");
    if (e.mod & 0x44) strcat(mods, "Alt+");
    if (e.mod & 0x88) strcat(mods, "Win+");

    char line[KEYLOG_LINE_LEN];
    if (e.key == 0) {
      if (mods[0]) mods[strlen(mods) - 1] = 0;
      snprintf(line, sizeof(line), "+%ums %s", cum, mods[0] ? mods : "?");
    } else {
      char c = hidToChar(e.key);
      if (c) snprintf(line, sizeof(line), "+%ums %s%c", cum, mods, c);
      else   snprintf(line, sizeof(line), "+%ums %s0x%02X", cum, mods, e.key);
    }
    Serial.printf("  [%s]\n", line);
    keylogPush(line);
  }
}

// ---------------------------------------------------------------- ESP-NOW: odbior (rozdzielacz)
void onRecv(const uint8_t *mac, const uint8_t *d, int len) {
  if (memcmp(mac, kbMac, 6) != 0) return;
  if (len != (int)sizeof(Msg)) return;
  const Msg *m = (const Msg *)d;

  switch (m->type) {
    case MSG_HELLO: kbVersion = m->version; helloPending = true; break;
    case MSG_ACK:   lastAck = m->idx; break;
    case MSG_DONE:  doneValue = m->value; doneFlag = true; break;
    case MSG_LOG_BEGIN:
    case MSG_LOG_DATA:
      handleLogMsg(m);
      break;
    default: break;
  }
}

// ---------------------------------------------------------------- ESP-NOW: wysylka (makra)
void onSent(const uint8_t *mac, esp_now_send_status_t st) {
  sendStatus = (st == ESP_NOW_SEND_SUCCESS) ? 0 : 1;
}

bool sendAndWait(Msg &m, int32_t expectAck) {
  for (int attempt = 0; attempt < 6; attempt++) {
    lastAck = -2;
    esp_now_send(kbMac, (uint8_t *)&m, sizeof(m));
    uint32_t t = millis();
    while (millis() - t < 150) {
      if (lastAck == expectAck) return true;
      delay(1);
    }
  }
  return false;
}

// Wysyla /macros.json do klawiatury (z ACK, powtorkami i weryfikacja CRC)
bool pushMacros() {
  File f = LittleFS.open("/macros.json", "r");
  if (!f) return false;

  uint32_t crc = 0;
  uint8_t buf[CHUNK];
  size_t n;
  while ((n = f.read(buf, CHUNK)) > 0) crc = crc32Update(crc, buf, n);
  uint16_t total = (f.size() + CHUNK - 1) / CHUNK;
  f.seek(0);

  doneFlag = false;
  Msg m{};
  m.type = MSG_BEGIN; m.version = macroVersion; m.idx = total; m.value = crc;
  if (!sendAndWait(m, ACK_BEGIN)) { f.close(); Serial.println("Brak ACK na BEGIN"); return false; }

  delay(30);
  if (doneFlag && doneValue == 2) {                  // klawiatura ma juz te wersje
    f.close();
    Serial.println("Klawiatura juz ma aktualne makra");
    return true;
  }

  for (uint16_t i = 0; i < total; i++) {
    Msg d{};
    d.type = MSG_DATA; d.version = macroVersion; d.idx = i;
    d.len = f.read(d.data, CHUNK);
    if (!sendAndWait(d, i)) {
      f.close();
      Serial.printf("Brak ACK na kawalek %u\n", i);
      return false;
    }
  }
  f.close();

  uint32_t t = millis();
  while (!doneFlag && millis() - t < 2000) delay(5);
  bool ok = doneFlag && doneValue == 1;
  Serial.println(ok ? "Makra wyslane i zweryfikowane" : "Klawiatura zglosila blad CRC");
  return ok;
}

void saveMacros(const String &json) {
  File f = LittleFS.open("/macros.json", "w");
  f.print(json);
  f.close();
  // wersja musi byc wyzsza niz ta, ktora zna klawiatura (dziala tez po wyczyszczeniu pamieci bramki)
  uint16_t base = macroVersion > kbVersion ? macroVersion : (uint16_t)kbVersion;
  macroVersion = base + 1;
  prefs.putUShort("ver", macroVersion);
  pendingPush = true;
  Serial.printf("Zapisano makra, wersja %u\n", macroVersion);
}

// Zdalne uruchomienie makra na klawiaturze PO NUMERZE NA LISCIE (0-based)
bool sendRun(uint8_t idx) {
  uint8_t pkt[2] = { RADIO_CMD_MARKER, idx };
  sendStatus = -1;
  if (esp_now_send(kbMac, pkt, 2) != ESP_OK) return false;
  uint32_t t = millis();
  while (sendStatus == -1 && millis() - t < 150) delay(1);
  return sendStatus == 0;
}

// ---------------------------------------------------------------- WWW
const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Makra</title>
<style>
button{padding:10px 14px;margin:4px;font-size:15px}
.remote{opacity:0.75}
#keylog{font-family:monospace;font-size:13px;background:#f3f3f3;padding:10px;
  max-height:260px;overflow-y:auto;white-space:pre-wrap;border-radius:6px}
#keylog .hdr{color:#888;margin-top:6px}
.hint{color:#777;font-size:13px;margin:4px 0 10px}
</style></head>
<body style="font-family:sans-serif;max-width:700px;margin:20px auto;padding:0 10px">
 
<h2>Uruchom makro</h2>
<div>%BTNS%</div>
<p>
  Albo wpisz numer makra (0, 1, 2…): <input id="lt" size="3" style="font-size:16px">
  <button onclick="runByNumber()">Uruchom</button>
</p>
<p id="st" style="min-height:1.5em;color:#555"></p>
 
<h2>Historia kliknięć</h2>
<p class="hint">Aktualizacja raz na minutę (paczka potwierdzona, bez strat) nie na żywo.</p>
<div id="keylog">(czekam na pierwszą paczkę…)</div>
 
<h2>Edycja makr</h2>
<form method="POST" action="/macros">
<textarea name="json" rows="18" style="width:100%;font-family:monospace">%JSON%</textarea><br>
<button type="submit">Zapisz i wyslij</button>
</form>
<p>Wersja: %VER%</p>
 
<script>
function run(idx){
  document.getElementById('st').textContent='Wysylanie...';
  fetch('/run',{method:'POST',body:new URLSearchParams({idx:idx})})
    .then(function(r){return r.text();})
    .then(function(t){document.getElementById('st').textContent=t;})
    .catch(function(){document.getElementById('st').textContent='Blad polaczenia z bramka';});
}
function runByNumber(){
  var v = document.getElementById('lt').value;
  if (v===''||isNaN(v)) { document.getElementById('st').textContent='Wpisz numer makra (liczba)'; return; }
  run(parseInt(v,10));
}
 
function escapeHtml(s){
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}
function pollKeylog(){
  fetch('/keylog').then(function(r){return r.json();}).then(function(lines){
    var box = document.getElementById('keylog');
    if (!lines.length) { box.textContent = '(czekam na pierwsza paczke...)'; return; }
    var html = lines.map(function(l){
      var isHdr = l.indexOf('==') === 0 || l.indexOf('--') === 0 || l.indexOf('!!') === 0;
      return '<div' + (isHdr ? ' class="hdr"' : '') + '>' + escapeHtml(l) + '</div>';
    }).join('');
    var wasAtBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 10;
    box.innerHTML = html;
    if (wasAtBottom) box.scrollTop = box.scrollHeight;
  }).catch(function(){});
}
setInterval(pollKeylog, 5000);
pollKeylog();
</script>
</body></html>
)HTML";

String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  return s;
}

// Przyciski "uruchom" generowane z aktualnej listy makr. Kazdy przycisk wysyla swoj numer
// na liscie (0-based), niezaleznie od tego, czy makro ma fizyczny trigger.
String macroButtons(const String &json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return "<i>Brak poprawnych makr</i>";
  JsonArray arr = doc.is<JsonArray>() ? doc.as<JsonArray>() : doc["macros"].as<JsonArray>();
  String out;
  int idx = 0;
  for (JsonObject o : arr) {
    const char *idc  = o["id"]  | "";
    const char *trig = o["trigger"] | "";
    bool legacyKey = !o["key"].isNull();
    bool remoteOnly = (trig[0] == 0) && !legacyKey;

    String label = idc;
    if (!label.length()) {
      if (trig[0]) {
        label = trig;
      } else if (legacyKey) {
        JsonVariant kv = o["key"];
        if (kv.is<const char *>()) label = String("Ctrl+") + kv.as<const char *>();
        else                       label = String("Ctrl+0x") + String((int)kv.as<int>(), HEX);
      } else {
        String d = o["text"] | "";
        if (!d.length()) d = o["url"] | "";
        if (!d.length()) d = "makro";
        if (d.length() > 16) d = d.substring(0, 16) + "...";
        label = d;
      }
    }

    out += "<button class=\"" + String(remoteOnly ? "remote" : "") + "\" onclick=\"run(" + String(idx) + ")\">"
         + (remoteOnly ? "[zdalne] " : "") + "#" + String(idx) + " " + htmlEscape(label) + "</button>";
    idx++;
  }
  return out.length() ? out : "<i>Brak makr</i>";
}

void handleRoot() {
  if (!server.authenticate(WWW_USER, WWW_PASS)) return server.requestAuthentication();
  String json = "{\"macros\":[\n  {\"id\":\"kornel\",\"trigger\":\"ctrl+i\",\"text\":\"Kornel\"},\n  {\"id\":\"youtube\",\"trigger\":\"ctrl+y\",\"url\":\"https://www.youtube.com\"},\n  {\"id\":\"wdroz\",\"steps\":[{\"type\":\"./deploy.sh\\n\"}]}\n]}";
  if (LittleFS.exists("/macros.json")) {
    File f = LittleFS.open("/macros.json", "r");
    if (f) { json = f.readString(); f.close(); }
  }
  String html = FPSTR(PAGE);
  html.replace("%BTNS%", macroButtons(json));
  html.replace("%JSON%", htmlEscape(json));
  html.replace("%VER%", String(macroVersion));
  server.send(200, "text/html", html);
}

void handleSave() {
  if (!server.authenticate(WWW_USER, WWW_PASS)) return server.requestAuthentication();
  String json = server.arg("json");
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    server.send(400, "text/plain", "Niepoprawny JSON");
    return;
  }
  if (!doc.is<JsonArray>() && !doc["macros"].is<JsonArray>()) {
    server.send(400, "text/plain", "Brak tablicy \"macros\" w JSON");
    return;
  }
  saveMacros(json);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleRun() {
  if (!server.authenticate(WWW_USER, WWW_PASS)) return server.requestAuthentication();
  if (!server.hasArg("idx")) { server.send(400, "text/plain", "Brak parametru idx"); return; }
  int idx = server.arg("idx").toInt();
  if (idx < 0 || idx > 254) { server.send(400, "text/plain", "Zly numer makra"); return; }
  bool ok = sendRun((uint8_t)idx);
  Serial.printf("Uruchom makro #%d: %s\n", idx, ok ? "OK" : "brak odpowiedzi");
  server.send(ok ? 200 : 502, "text/plain",
              ok ? "Wyslano do klawiatury" : "Klawiatura nie odpowiada (wylaczona lub inny kanal)");
}

// ---------------------------------------------------------------- SETUP / LOOP
void setup() {
  Serial.begin(115200);
  delay(300);
  LittleFS.begin(true);
  prefs.begin("macros", false);
  macroVersion = prefs.getUShort("ver", 0);

  // 1. Najpierw Wi-Fi (radio ustawia sie na kanale routera)
  WiFi.mode(WIFI_STA);
  Serial.print("MAC bramki: ");
  Serial.println(WiFi.macAddress());
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Laczenie z Wi-Fi");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nIP: %s, kanal: %d\n", WiFi.localIP().toString().c_str(), WiFi.channel());
  } else {
    Serial.println("\nBrak Wi-Fi (ESP-NOW dziala dalej)");
  }

  // 2. ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Blad ESP-NOW");
    return;
  }
#if USE_ENCRYPT
  esp_now_set_pmk(PMK);
#endif
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t p{};
  memcpy(p.peer_addr, kbMac, 6);
  p.channel = 0;
#if USE_ENCRYPT
  memcpy(p.lmk, LMK, 16);
  p.encrypt = true;
#else
  p.encrypt = false;
#endif
  esp_now_add_peer(&p);

  // 3. Serwer WWW
  server.on("/", HTTP_GET, handleRoot);
  server.on("/macros", HTTP_POST, handleSave);
  server.on("/run", HTTP_POST, handleRun);
  server.on("/keylog", HTTP_GET, handleKeylog);
  server.begin();
}

void loop() {
  if (logReady) {                  // calosc logu potwierdzona - dekoduj i wypisz TERAZ, poza callbackiem
    logReady = false;
    decodeAndPrintLog();
  }

  server.handleClient();

  static uint32_t lastWifi = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifi > 15000) {
    lastWifi = millis();
    WiFi.reconnect();
  }

  if (helloPending) {
    helloPending = false;
    Serial.printf("HELLO od klawiatury, jej wersja: %u\n", kbVersion);
    Msg a{}; a.type = MSG_ACK; a.idx = 0;
    esp_now_send(kbMac, (uint8_t *)&a, sizeof(a));
    if (macroVersion > kbVersion) pendingPush = true;
  }

  static uint32_t lastTry = 0;
  if (pendingPush && millis() - lastTry > 10000) {
    lastTry = millis();
    if (pushMacros()) pendingPush = false;
  }
}