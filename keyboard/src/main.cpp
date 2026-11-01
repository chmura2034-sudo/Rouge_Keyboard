#include <Arduino.h>
#include "driver/uart.h"
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <vector>
#include "proto.h"

#if defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE
#error "Ustaw Narzedzia -> USB Mode -> USB-OTG (TinyUSB), inaczej klawiatura HID nie bedzie dzialac."
#endif

// ====== DO ZMIANY ======
static uint8_t gwMac[6] = {0x8C, 0x94, 0xDF, 0x4C, 0xD8, 0x64};   // MAC BRAMKI
// =======================

// ------------------------------------------------------------ KONFIGURACJA CH375
#define CH375_TX_PIN     17      // ESP32 TX -> CH375 RXD (pin 6)
#define CH375_RX_PIN     16      // ESP32 RX <- CH375 TXD (pin 5)
#define INT_PIN          4       // CH375 INT# (pin 1) -> GPIO4 + rezystor 4.7k do 3.3V (-1 = bez INT#)
#define RSTI_PIN         5       // CH375 RSTI (pin 2, aktywny wysoki) -> GPIO5 (-1 = bez RSTI)
#define CH375_BAUD       9600
#define FRAME_MODE       3       // 0=auto, 1=8N1, 2=8N1+prefiks 57 AB, 3=9-bit (u Ciebie dziala)
#define USB_SPEED        0       // 0=auto (full, potem low), 1=low-speed, 2=full-speed
#define DEV_ADDR         1       // adres nadawany klawiaturze
#define SHOW_RAW_REPORT  1       // 1 = wypisuj surowe 8-bajtowe raporty HID
#define SET_BOOT_PROTOCOL 1      // 1 = wyslij SET_PROTOCOL(boot) i SET_IDLE(0)
#define SHOW_KEYS        1       // 1 = wypisuj wcisniete klawisze tez w konsoli (diagnostyka)

#define MACRO_CHAR_DELAY_MS 10   // przerwa miedzy wpisywanymi znakami
#define RADIO_ENABLE   1         // 1 = zdalne makra + aktualizacja makr + wsadowy log klikniec przez ESP-NOW

// ============================================================ MAKRA (dynamiczne, z /macros.json)
//
// Format pliku /macros.json (wysylany z bramki):
// {"macros":[
//   {"id":"kornel","trigger":"ctrl+i","text":"Kornel"},            // dowolna kombinacja jako trigger
//   {"id":"odswiez","trigger":"f5","url":"https://example.com"},   // sam klawisz, bez modyfikatora
//   {"id":"super","trigger":"win","text":"..."},                   // sam modyfikator jako trigger
//   {"id":"wdroz","steps":[{"type":"./deploy.sh\n"}]}              // BRAK "trigger" -> tylko zdalnie
// ]}
//
// "id": opcjonalna nazwa makra - etykieta na przycisku bramki i w logu (bez wplywu na dzialanie).
// "trigger": dowolna kombinacja klawiszy fizycznej klawiatury, ta sama skladnia co "combo" w krokach
//   (patrz nizej): ctrl, shift, alt, win; litery, cyfry, enter, esc, tab, space, backspace, delete,
//   home, end, pageup, pagedown, up, down, left, right, f1..f12, printscreen, insert, menu - laczone
//   plusem, np. "ctrl+alt+t", "shift+f2", "win". Jesli pole "trigger" jest pominiete, makro NIE
//   reaguje na zadna fizyczna kombinacje - mozna je uruchomic WYLACZNIE zdalnie (przycisk na stronie
//   bramki albo POST /run).
// "key": stary format sprzed wprowadzenia "trigger" (litera a-z albo kod HID) - nadal dziala,
//   traktowany jak "trigger":"ctrl+<litera>", zachowany dla zgodnosci wstecz.
// Pola "text", "url", "action":"youtube" i "steps" mozna laczyc - wykonuja sie w tej kolejnosci.
//
// Zdalne wywolanie (z bramki) adresuje makro PO NUMERZE NA LISCIE (liczac od 0), nie po kodzie
// klawisza - dzieki temu dziala takze dla makr bez "trigger".

#define MAX_MACROS   24
#define MAX_STEPS    32
#define MAX_DELAY_MS 10000

enum StepType : uint8_t { STEP_TYPE = 1, STEP_COMBO, STEP_DELAY };

struct Step {
  uint8_t  type = 0;
  uint8_t  mod  = 0;      // COMBO: maska modyfikatorow HID (1=Ctrl 2=Shift 4=Alt 8=Win) - jeden bit na strone (Left)
  uint8_t  key  = 0;      // COMBO: kod HID klawisza (0 = sam modyfikator)
  uint16_t ms   = 0;      // DELAY
  String   text;          // TYPE
};

struct MacroDef {
  String  id;              // etykieta (opcjonalna) - tylko do wyswietlania, bez wplywu na dzialanie
  bool    hasTrigger = false;
  uint8_t triggerMod  = 0;  // znormalizowana maska: 1=Ctrl 2=Shift 4=Alt 8=Win (patrz normalizeMods)
  uint8_t triggerKey  = 0;  // 0 = sam modyfikator jako trigger (np. "win")
  std::vector<Step> steps;
};

static std::vector<MacroDef> macros;

static Preferences macroPrefs;
static uint16_t    curVersion = 0;               // wersja makr zapisana w pamieci
static volatile bool macrosDirty = false;        // ustawiane po udanej aktualizacji

static void pushStep(MacroDef &m, const Step &s) {
  if (m.steps.size() < MAX_STEPS) m.steps.push_back(s);
}

static void addStepType(MacroDef &m, const char *t) {
  if (!t || !t[0]) return;
  Step s; s.type = STEP_TYPE; s.text = t;
  pushStep(m, s);
}

static void addStepDelay(MacroDef &m, int ms) {
  if (ms < 0) ms = 0;
  if (ms > MAX_DELAY_MS) ms = MAX_DELAY_MS;
  Step s; s.type = STEP_DELAY; s.ms = (uint16_t)ms;
  pushStep(m, s);
}

static uint8_t keyNameToHid(String n) {
  n.toLowerCase();
  if (n.length() == 1) {
    char c = n[0];
    if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
    if (c >= '1' && c <= '9') return 0x1E + (c - '1');
    if (c == '0') return 0x27;
    switch (c) {
      case '-': return 0x2D;  case '=': return 0x2E;  case '[': return 0x2F;  case ']': return 0x30;
      case '\\': return 0x31; case ';': return 0x33;  case '\'': return 0x34; case '`': return 0x35;
      case ',': return 0x36;  case '.': return 0x37;  case '/': return 0x38;
    }
    return 0;
  }
  if (n[0] == 'f') {
    int f = n.substring(1).toInt();
    if (f >= 1 && f <= 12) return 0x3A + f - 1;
  }
  static const struct { const char *name; uint8_t k; } T[] = {
    {"enter", 0x28}, {"return", 0x28}, {"esc", 0x29}, {"escape", 0x29}, {"backspace", 0x2A},
    {"tab", 0x2B}, {"space", 0x2C}, {"capslock", 0x39}, {"printscreen", 0x46}, {"insert", 0x49},
    {"home", 0x4A}, {"pageup", 0x4B}, {"delete", 0x4C}, {"del", 0x4C}, {"end", 0x4D},
    {"pagedown", 0x4E}, {"right", 0x4F}, {"left", 0x50}, {"down", 0x51}, {"up", 0x52}, {"menu", 0x65},
  };
  for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) if (n == T[i].name) return T[i].k;
  return 0;
}

// "ctrl+alt+t" -> znormalizowane modyfikatory (1=Ctrl 2=Shift 4=Alt 8=Win, jeden bit na strone) + kod klawisza
// Zwraca true takze dla samego modyfikatora bez klawisza (np. "win"), wtedy key=0.
static bool parseCombo(const char *s, uint8_t &mod, uint8_t &key) {
  mod = 0; key = 0;
  String str(s);
  str.toLowerCase();
  int start = 0;
  while (start <= (int)str.length()) {
    int p = str.indexOf('+', start);
    if (p < 0) p = str.length();
    String tok = str.substring(start, p);
    tok.trim();
    start = p + 1;
    if (!tok.length()) continue;
    if      (tok == "ctrl" || tok == "control")                                              mod |= 0x01;
    else if (tok == "shift")                                                                 mod |= 0x02;
    else if (tok == "alt")                                                                   mod |= 0x04;
    else if (tok == "gui" || tok == "win" || tok == "super" || tok == "meta" || tok == "cmd") mod |= 0x08;
    else {
      uint8_t k = keyNameToHid(tok);
      if (!k) return false;
      key = k;
    }
  }
  return mod || key;
}

// Zamienia surowy bajt modyfikatorow z raportu HID (bity L/R osobno) na znormalizowana maske
// (1=Ctrl 2=Shift 4=Alt 8=Win, bez rozroznienia strony) - ta sama konwencja co parseCombo().
static uint8_t normalizeMods(uint8_t raw) {
  uint8_t n = 0;
  if (raw & 0x11) n |= 0x01;   // LCtrl | RCtrl
  if (raw & 0x22) n |= 0x02;   // LShift | RShift
  if (raw & 0x44) n |= 0x04;   // LAlt | RAlt
  if (raw & 0x88) n |= 0x08;   // LGui | RGui
  return n;
}

static void addStepCombo(MacroDef &m, const char *combo) {
  Step s; s.type = STEP_COMBO;
  if (!parseCombo(combo, s.mod, s.key)) {
    Serial.printf("[MAKRA] nieznany skrot \"%s\" - pomijam krok\n", combo);
    return;
  }
  pushStep(m, s);
}

// Ubuntu: Ctrl+Alt+T -> xdg-open 'URL' && exit -> Enter
static void addUrlSteps(MacroDef &m, const char *url) {
  if (!url || !url[0]) return;
  if (strchr(url, '\'') || strchr(url, '\n') || strchr(url, '\r')) {
    Serial.println("[MAKRA] url zawiera niedozwolone znaki - pomijam");
    return;
  }
  addStepCombo(m, "ctrl+alt+t");
  addStepDelay(m, 1000);
  String cmd = String("xdg-open '") + url + "' && exit";
  addStepType(m, cmd.c_str());
  addStepCombo(m, "enter");
}

static void parseStep(MacroDef &m, JsonObject s) {
  const char *t = s["type"] | "";
  if (t[0]) { addStepType(m, t); return; }
  const char *c = s["combo"] | "";
  if (c[0]) { addStepCombo(m, c); return; }
  c = s["press"] | "";
  if (c[0]) { addStepCombo(m, c); return; }
  if (!s["delay"].isNull()) { addStepDelay(m, s["delay"] | 0); return; }
}

// Stary format pola "key" (litera a-z albo kod HID liczbowy) - uzywany tylko dla zgodnosci wstecz,
// gdy w makrze nie ma nowego pola "trigger".
static uint8_t parseLegacyKey(JsonVariant v) {
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    if (s && s[0] && !s[1]) {
      char c = tolower((unsigned char)s[0]);
      if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
    }
    return 0;
  }
  if (v.is<int>()) {
    int k = v.as<int>();
    return (k >= 0x04 && k <= 0xFF) ? (uint8_t)k : 0;
  }
  return 0;
}

static void loadDefaultMacros() {
  macros.clear();
  MacroDef a; a.id = "kornel"; a.hasTrigger = true; a.triggerMod = 0x01; a.triggerKey = 0x0C;   // Ctrl+I
  addStepType(a, "Kornel"); macros.push_back(a);
  MacroDef b; b.id = "knop";   b.hasTrigger = true; b.triggerMod = 0x01; b.triggerKey = 0x11;   // Ctrl+N
  addStepType(b, "Knop"); macros.push_back(b);
  MacroDef c; c.id = "youtube"; c.hasTrigger = true; c.triggerMod = 0x01; c.triggerKey = 0x1C;  // Ctrl+Y
  addUrlSteps(c, "https://www.youtube.com"); macros.push_back(c);
}

// Wczytuje /macros.json. Przy bledzie zostawia dotychczasowe makra. Wolane tylko z watku glownego.
static bool loadMacrosFromFile() {
  File f = LittleFS.open("/macros.json", "r");
  if (!f) { Serial.println("[MAKRA] brak macros.json - uzywam domyslnych"); return false; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { Serial.printf("[MAKRA] blad JSON: %s - zostaja dotychczasowe\n", err.c_str()); return false; }

  // akceptuj {"macros":[...]} oraz sama tablice [...]
  JsonArray arr = doc.is<JsonArray>() ? doc.as<JsonArray>() : doc["macros"].as<JsonArray>();
  if (arr.isNull()) {
    Serial.println("[MAKRA] brak tablicy makr - tresc pliku:");
    File d = LittleFS.open("/macros.json", "r");
    if (d) { Serial.println(d.readString()); d.close(); }
    return false;
  }

  std::vector<MacroDef> fresh;
  for (JsonObject o : arr) {
    if (fresh.size() >= MAX_MACROS) break;

    MacroDef m;
    const char *idc = o["id"] | "";
    m.id = idc;

    const char *trig = o["trigger"] | "";
    if (trig[0]) {
      uint8_t tm = 0, tk = 0;
      if (parseCombo(trig, tm, tk)) { m.hasTrigger = true; m.triggerMod = tm; m.triggerKey = tk; }
      else Serial.printf("[MAKRA] nieznany trigger \"%s\" - makro bedzie dostepne tylko zdalnie\n", trig);
    } else {
      uint8_t legacy = parseLegacyKey(o["key"]);           // zgodnosc wstecz ze starym "key"
      if (legacy >= 0x04) { m.hasTrigger = true; m.triggerMod = 0x01; m.triggerKey = legacy; }
    }
    // brak "trigger" i brak poprawnego starego "key" -> m.hasTrigger zostaje false = makro tylko zdalne

    addStepType(m, o["text"] | "");
    const char *act = o["action"] | "";
    if (!strcmp(act, "youtube")) addUrlSteps(m, "https://www.youtube.com");
    addUrlSteps(m, o["url"] | "");
    JsonArray st = o["steps"].as<JsonArray>();
    for (JsonObject s : st) parseStep(m, s);

    if (m.steps.empty()) continue;
    if (!m.id.length()) m.id = trig[0] ? trig : "(makro)";

    Serial.printf("[MAKRA]  #%u %s%s -> %u krokow\n", (unsigned)fresh.size(), m.id.c_str(),
                  m.hasTrigger ? "" : " [tylko zdalnie]", (unsigned)m.steps.size());
    fresh.push_back(m);
  }
  macros = fresh;
  Serial.printf("[MAKRA] wczytano %u makr\n", (unsigned)macros.size());
  return true;
}

// Wolane z watku glownego (checkRemoteMacro) - tu bezpiecznie podmieniamy tablice makr
static void applyMacroUpdateIfAny() {
  if (macrosDirty) {
    macrosDirty = false;
    loadMacrosFromFile();
  }
}

// Prosty CRC32 (identyczny wynik jak zlib / esp_rom_crc32_le) - uzyj tej samej funkcji na bramce
static uint32_t crc32Update(uint32_t crc, const uint8_t *d, size_t n) {
  crc = ~crc;
  while (n--) {
    crc ^= *d++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

// ============================================================ ESP-NOW (zdalne makra + aktualizacja makr)
#if RADIO_ENABLE
#define RADIO_CMD_MARKER 0xFE     // pierwszy bajt pakietu-komendy: [0xFE][numer makra na liscie, od 0]

static uint8_t peerBroadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static volatile bool espNowReady = false;
static volatile bool pendingMacroFlag = false;
static volatile uint8_t pendingMacroIdx = 0;   // numer makra na liscie (nie kod HID)

// Ustawiane na czas skanowania kanalow w findGateway() - logTask wstrzymuje wtedy wysylke,
// zeby nie wysylac na kanale, ktory w tej samej chwili zmienia sie pod skanowaniem.
static volatile bool channelScanning = false;

// --- stan protokolu logu (klawiatura -> bramka), uzywane w onEspNowRecvCmd ---
static volatile int32_t  logLastAck   = -2;
static volatile bool     logDoneFlag  = false;
static volatile uint32_t logDoneValue = 0;

// --- stan odbioru aktualizacji makr (bramka -> klawiatura) ---
static QueueHandle_t macroQ = nullptr;
static volatile bool gwAnswered = false;

static File     tmpF;
static uint16_t mExpected = 0, mTotal = 0, mRxVersion = 0;
static uint32_t mCrcWanted = 0, mCrcRunning = 0;
static bool     mReceiving = false;
static uint32_t mLastPkt = 0;

static void macroSend(const Msg &a) { esp_now_send(gwMac, (const uint8_t *)&a, sizeof(a)); }
static void sendAck(uint16_t idx)   { Msg a{}; a.type = MSG_ACK;  a.idx = idx; macroSend(a); }
static void sendDone(uint32_t v)    { Msg a{}; a.type = MSG_DONE; a.value = v; a.version = mRxVersion; macroSend(a); }

// Wolane z callbacku radiowego. Zwraca true, jesli pakiet nalezal do protokolu aktualizacji makr.
static bool macroSyncRx(const uint8_t *mac, const uint8_t *d, int len) {
  if (len != (int)sizeof(Msg)) return false;
  if (memcmp(mac, gwMac, 6) != 0) return false;               // tylko od naszej bramki
  const Msg *m = (const Msg *)d;
  if (m->type < MSG_HELLO || m->type > MSG_DONE) return false;   // poza tym zakresem to log, nie makra

  if (m->type == MSG_ACK && m->idx == 0) { gwAnswered = true; return true; }  // odpowiedz na HELLO
  if (macroQ) xQueueSend(macroQ, m, 0);
  return true;
}

static void macroAbortReceive() {
  if (mReceiving) { tmpF.close(); LittleFS.remove("/macros.tmp"); mReceiving = false; }
}

static void macroHandle(const Msg &m) {
  if (m.type == MSG_BEGIN) {
    sendAck(ACK_BEGIN);
    if (m.version <= curVersion) { mRxVersion = m.version; sendDone(2); return; }   // juz aktualne
    macroAbortReceive();
    tmpF = LittleFS.open("/macros.tmp", "w");
    if (!tmpF) { Serial.println("[MAKRA] nie moge otworzyc macros.tmp"); return; }
    mReceiving = true;
    mExpected = 0; mCrcRunning = 0;
    mTotal = m.idx; mCrcWanted = m.value; mRxVersion = m.version;
    mLastPkt = millis();
    Serial.printf("[MAKRA] start odbioru v%u (%u kawalkow)\n", mRxVersion, mTotal);
  }
  else if (m.type == MSG_DATA && mReceiving) {
    if (m.len > CHUNK) return;
    mLastPkt = millis();
    if (m.idx < mExpected) { sendAck(m.idx); return; }   // duplikat (zgubil sie ACK)
    if (m.idx != mExpected) return;                      // luka - nadawca powtorzy

    tmpF.write(m.data, m.len);
    mCrcRunning = crc32Update(mCrcRunning, m.data, m.len);
    mExpected++;
    sendAck(m.idx);

    if (mExpected == mTotal) {
      tmpF.close();
      mReceiving = false;
      if (mCrcRunning == mCrcWanted) {
        LittleFS.remove("/macros.json");
        LittleFS.rename("/macros.tmp", "/macros.json");
        curVersion = mRxVersion;
        macroPrefs.putUShort("ver", curVersion);
        macrosDirty = true;                              // przeladuje watek glowny
        Serial.println("[MAKRA] nowe makra zapisane");
        sendDone(1);
      } else {
        LittleFS.remove("/macros.tmp");
        Serial.println("[MAKRA] blad CRC - stare makra zostaja");
        sendDone(0);
      }
    }
  }
}

static bool pingGateway(uint32_t waitMs) {
  Msg h{}; h.type = MSG_HELLO; h.version = curVersion;
  gwAnswered = false;
  macroSend(h);
  uint32_t t = millis();
  while (millis() - t < waitMs) {
    if (gwAnswered) return true;
    vTaskDelay(1);
  }
  return false;
}

// Najpierw probuje biezacy kanal, potem skanuje 1-13. Przy porazce wraca na poprzedni kanal.
static bool findGateway() {
  channelScanning = true;             // logTask wstrzyma wysylke na czas skanowania
  uint8_t cur = WIFI_CH;
  wifi_second_chan_t sc;
  esp_wifi_get_channel(&cur, &sc);
  if (pingGateway(200)) { channelScanning = false; return true; }

  for (uint8_t ch = 1; ch <= 13; ch++) {
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    if (pingGateway(250)) {
      Serial.printf("[MAKRA] bramka na kanale %u\n", ch);
      channelScanning = false;
      return true;
    }
  }
  esp_wifi_set_channel(cur, WIFI_SECOND_CHAN_NONE);
  Serial.println("[MAKRA] bramka nie odpowiada");
  channelScanning = false;
  return false;
}

static void macroTask(void *) {
  vTaskDelay(pdMS_TO_TICKS(2000));
  findGateway();
  uint32_t lastPing = millis();
  Msg m;
  for (;;) {
    if (xQueueReceive(macroQ, &m, pdMS_TO_TICKS(500))) macroHandle(m);

    if (mReceiving && millis() - mLastPkt > 5000) {      // przerwany transfer
      macroAbortReceive();
      Serial.println("[MAKRA] timeout odbioru - porzucam");
    }
    // co 2 min HELLO: bramka dowie sie o naszej wersji i dosle zalegle makra
    if (!mReceiving && millis() - lastPing > 120000UL) {
      lastPing = millis();
      findGateway();
    }
  }
}

static void queueKeyPress(uint8_t mod, uint8_t key);   // definicja nizej - wolane z handleReport

// Arduino-ESP32 core 2.x
static void onEspNowRecvCmd(const uint8_t *mac, const uint8_t *data, int len) {
  if (len == (int)sizeof(Msg) && memcmp(mac, gwMac, 6) == 0) {
    const Msg *m = (const Msg *)data;
    if (m->type == MSG_ACK && (m->idx & LOG_ACK_BIT)) { logLastAck = m->idx; return; }
    if (m->type == MSG_LOG_DONE) { logDoneValue = m->value; logDoneFlag = true; return; }
  }
  if (macroSyncRx(mac, data, len)) return;               // pakiet aktualizacji makr (bramka -> klawiatura)
  if (len >= 2 && data[0] == RADIO_CMD_MARKER) {
    pendingMacroIdx = data[1];
    pendingMacroFlag = true;        // wykonanie makra odlozone do petli glownej
  }
}
#endif
// ---------------------------------------------------------------------------

#define UART_PORT UART_NUM_2     // Serial2 (CH375)

// komendy CH375
#define CMD_GET_IC_VER    0x01
#define CMD_SET_USB_SPEED 0x04   // 00H=12Mbps, 02H=1.5Mbps
#define CMD_RESET_ALL     0x05
#define CMD_CHECK_EXIST   0x06
#define CMD_SET_RETRY     0x0B
#define CMD_SET_USB_ADDR  0x13
#define CMD_SET_USB_MODE  0x15
#define CMD_TEST_CONNECT  0x16
#define CMD_GET_STATUS    0x22
#define CMD_RD_USB_DATA   0x28
#define CMD_WR_USB_DATA7  0x2B
#define CMD_CLR_STALL     0x41
#define CMD_SET_ADDRESS   0x45
#define CMD_GET_DESCR     0x46
#define CMD_SET_CONFIG    0x49
#define CMD_ISSUE_TKN_X   0x4E
// statusy
#define RET_SUCCESS       0x51
#define INT_SUCCESS       0x14
#define INT_CONNECT       0x15
#define INT_DISCONNECT    0x16
#define INT_BUF_OVER      0x17
#define INT_USB_READY     0x18
// PID
#define PID_SETUP         0x0D
#define PID_IN            0x09

enum Frame { F_8N1 = 0, F_PREFIX, F_9BIT };
static Frame frame = F_8N1;

// ============================================================ USB HID (natywne USB ESP32-S3)

static USBHIDKeyboard Keyboard;
static USBHIDConsumerControl Consumer;
static volatile uint8_t hostLeds = 0;   // stan diod z komputera: bit0 Num, bit1 Caps, bit2 Scroll

static void usbEventCb(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base == ARDUINO_USB_HID_KEYBOARD_EVENTS && id == ARDUINO_USB_HID_KEYBOARD_LED_EVENT) {
    arduino_usb_hid_keyboard_event_data_t *d = (arduino_usb_hid_keyboard_event_data_t *)data;
    hostLeds = (d->numlock ? 1 : 0) | (d->capslock ? 2 : 0) | (d->scrolllock ? 4 : 0);
  }
}

static void hidSendKeys(uint8_t mod, const uint8_t keys[6]) {
  KeyReport kr;
  kr.modifiers = mod;
  kr.reserved = 0;
  for (int i = 0; i < 6; i++) kr.keys[i] = keys[i];
  Keyboard.sendReport(&kr);
}

static void hidReleaseAll() {
  const uint8_t none[6] = {0, 0, 0, 0, 0, 0};
  hidSendKeys(0, none);
}

// ============================================================ CH375: niski poziom UART

static const char *frameName(Frame f) {
  switch (f) {
    case F_8N1:    return "8N1";
    case F_PREFIX: return "8N1 + prefiks 57 AB";
    default:       return "9-bit (emulacja parzystoscia)";
  }
}

static void setFrame(Frame f) {
  frame = f;
  uart_set_parity(UART_PORT, f == F_9BIT ? UART_PARITY_EVEN : UART_PARITY_DISABLE);
}

static void flushInput() { while (Serial2.available()) Serial2.read(); }

static void txByte(uint8_t b, bool isCmd) {
  if (frame == F_9BIT) {
    bool oddOnes = __builtin_popcount(b) & 1;        // 9. bit = bit parzystosci
    uart_set_parity(UART_PORT, (oddOnes == isCmd) ? UART_PARITY_EVEN : UART_PARITY_ODD);
    Serial2.write(b);
  } else if (isCmd && frame == F_PREFIX) {
    Serial2.write((uint8_t)0x57);
    Serial2.write((uint8_t)0xAB);
    Serial2.write(b);
  } else {
    Serial2.write(b);
  }
  Serial2.flush();
}

static void startCmd(uint8_t cmd) { flushInput(); txByte(cmd, true); }
static void txData(uint8_t b)     { txByte(b, false); }

static int rxByte(uint32_t timeoutMs) {              // -1 = timeout
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (Serial2.available()) return Serial2.read() & 0xFF;
    delay(1);
  }
  return -1;
}

// ------------------------------------------------------------ komendy CH375

static void hwReset() {
  if (RSTI_PIN >= 0) { digitalWrite(RSTI_PIN, HIGH); delay(10); digitalWrite(RSTI_PIN, LOW); }
  else               { startCmd(CMD_RESET_ALL); }
  delay(100);
  Serial2.updateBaudRate(CH375_BAUD);
  flushInput();
}

static int checkExist(uint8_t v) { startCmd(CMD_CHECK_EXIST); txData(v); return rxByte(50); }
static int getIcVer()            { startCmd(CMD_GET_IC_VER); return rxByte(50); }
static int setUsbMode(uint8_t m) { startCmd(CMD_SET_USB_MODE); txData(m); return rxByte(50); }
static int getStatus()           { startCmd(CMD_GET_STATUS); return rxByte(30); }

static int testConnect() {
  int r = -1;
  for (int i = 0; i < 5; i++) {                      // 0 = komenda jeszcze niezakonczona
    startCmd(CMD_TEST_CONNECT);
    r = rxByte(50);
    if (r != 0) break;
    delay(2);
  }
  return r;
}

static bool devicePresent() {
  int s = testConnect();
  return s == INT_CONNECT || s == 0x14 || s == INT_USB_READY;
}

static void setUsbSpeed(uint8_t s) { startCmd(CMD_SET_USB_SPEED); txData(s); delay(5); }

// czeka na przerwanie: INT# lub odpytywanie GET_STATUS; zwraca status (>=0x14) albo -1
static int waitInt(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (INT_PIN >= 0) {
      if (digitalRead(INT_PIN) == LOW) return getStatus();
      delay(1);
    } else {
      int s = getStatus();
      if (s >= 0x14) return s;
      delay(2);
    }
  }
  return -1;
}

// kasuje zalegajace przerwanie (INT# niski), zwraca ostatni odczytany status lub -1
static int drainInt() {
  int last = -1;
  for (int i = 0; i < 4 && INT_PIN >= 0 && digitalRead(INT_PIN) == LOW; i++) last = getStatus();
  return last;
}

static int getDescr(uint8_t type, uint32_t timeoutMs) {          // 1=device, 2=config
  startCmd(CMD_GET_DESCR);
  txData(type);
  uint32_t t0 = millis();
  for (;;) {
    int32_t left = (int32_t)timeoutMs - (int32_t)(millis() - t0);
    if (left <= 0) return -1;
    int s = waitInt((uint32_t)left);
    if (s < 0) return -1;
    if (s == INT_CONNECT) continue;                              // pozostalosc po resecie
    return s;
  }
}

static int rdUsbData(uint8_t *buf, int maxLen) {
  startCmd(CMD_RD_USB_DATA);
  int len = rxByte(50);
  if (len < 0) return -1;
  for (int i = 0; i < len; i++) {
    int b = rxByte(50);
    if (b < 0) return -2;
    if (i < maxLen) buf[i] = (uint8_t)b;
  }
  return len;
}

static void wrUsbData(const uint8_t *buf, uint8_t len) {
  startCmd(CMD_WR_USB_DATA7);
  txData(len);
  for (uint8_t i = 0; i < len; i++) txData(buf[i]);
}

static int setAddress(uint8_t addr) {                            // SET_ADDRESS + SET_USB_ADDR
  startCmd(CMD_SET_ADDRESS); txData(addr);
  int s = waitInt(300);
  if (s == INT_SUCCESS) { startCmd(CMD_SET_USB_ADDR); txData(addr); }
  return s;
}

static int setConfig(uint8_t cfg) { startCmd(CMD_SET_CONFIG); txData(cfg); return waitInt(300); }

// transfer kontrolny bez fazy danych (np. SET_PROTOCOL, SET_IDLE); zwraca status
static int controlNoData(uint8_t bmReq, uint8_t bReq, uint16_t wValue, uint16_t wIndex) {
  uint8_t s[8] = { bmReq, bReq, (uint8_t)(wValue & 0xFF), (uint8_t)(wValue >> 8),
                   (uint8_t)(wIndex & 0xFF), (uint8_t)(wIndex >> 8), 0, 0 };
  wrUsbData(s, 8);
  startCmd(CMD_ISSUE_TKN_X); txData(0x00); txData((0 << 4) | PID_SETUP);   // SETUP, DATA0
  int st = waitInt(300);
  if (st != INT_SUCCESS) return st;
  startCmd(CMD_ISSUE_TKN_X); txData(0x80); txData((0 << 4) | PID_IN);      // status: IN, DATA1
  return waitInt(500);
}

// SET_REPORT(Output), 1 bajt danych: diody LED fizycznej klawiatury
static int setLeds(uint8_t iface, uint8_t leds) {
  uint8_t s[8] = { 0x21, 0x09, 0x00, 0x02, iface, 0x00, 0x01, 0x00 };
  wrUsbData(s, 8);
  startCmd(CMD_ISSUE_TKN_X); txData(0x00); txData((0 << 4) | PID_SETUP);   // SETUP, DATA0
  int st = waitInt(300);
  if (st != INT_SUCCESS) return st;
  wrUsbData(&leds, 1);
  startCmd(CMD_ISSUE_TKN_X); txData(0x40); txData((0 << 4) | 0x01);       // OUT (PID 0x01), DATA1
  st = waitInt(300);
  if (st != INT_SUCCESS) return st;
  startCmd(CMD_ISSUE_TKN_X); txData(0x80); txData((0 << 4) | PID_IN);     // status: IN, DATA1
  return waitInt(500);
}

static const char *statusStr(int st) {
  switch (st) {
    case 0x14: return "USB_INT_SUCCESS";
    case 0x15: return "USB_INT_CONNECT";
    case 0x16: return "USB_INT_DISCONNECT";
    case 0x17: return "USB_INT_BUF_OVER";
    case 0x18: return "USB_INT_USB_READY(?)";
    case 0x51: return "CMD_RET_SUCCESS";
    case 0x5F: return "CMD_RET_ABORT";
    case -1:   return "timeout";
  }
  if (st >= 0x20 && st <= 0x3F) {
    int pid = st & 0x0F;
    if (pid == 0x0A) return "FAIL: urzadzenie NAK";
    if (pid == 0x0E) return "FAIL: urzadzenie STALL";
    if ((pid & 3) == 0) return "FAIL: timeout / brak odpowiedzi urzadzenia";
    return "FAIL: blad USB (PID)";
  }
  return "?";
}

static void hexdump(const char *label, const uint8_t *b, int n) {
  Serial.printf("      %s (%d B):", label, n);
  for (int i = 0; i < n; i++) Serial.printf(" %02X", b[i]);
  Serial.print("\n");
}

static void report(bool ok, const char *name, const char *fmt, ...) {
  char msg[160];
  va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof(msg), fmt, ap); va_end(ap);
  Serial.printf("[%s] %-22s %s\n", ok ? "OK  " : "FAIL", name, msg);
}

// ------------------------------------------------------------ inicjalizacja modulu CH375

static bool probe() {
  for (int i = 0; i < 3; i++) {
    if (checkExist(0x57) == 0xA8) return true;
    delay(10);
  }
  return false;
}

static bool detectFrame() {
#if FRAME_MODE == 1
  const Frame order[] = { F_8N1 };
#elif FRAME_MODE == 2
  const Frame order[] = { F_PREFIX };
#elif FRAME_MODE == 3
  const Frame order[] = { F_9BIT };
#else
  const Frame order[] = { F_8N1, F_PREFIX, F_9BIT };
#endif
  for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
    setFrame(order[i]);
    hwReset();
    if (probe()) {
      report(true, "CH375", "odpowiada, ramka: %s, wersja 0x%02X", frameName(order[i]), getIcVer() & 0x3F);
      return true;
    }
  }
  return false;
}

// ------------------------------------------------------------ enumeracja klawiatury

struct HidEp { uint8_t ep, iface, proto, toggle; };
static HidEp eps[2];
static int epCount = 0;

static bool getDeviceDescriptor(uint8_t *buf, bool *low) {
  int attempts = (USB_SPEED == 0) ? 2 : 1;
  for (int a = 0; a < attempts; a++) {
    bool useLow = (USB_SPEED == 1) || (USB_SPEED == 0 && a == 1);
    setUsbMode(7); delay(50);                                    // reset magistrali
    setUsbMode(6); delay(150);                                   // tryb 6 przywraca full-speed
    if (useLow) setUsbSpeed(0x02);
    int s = getDescr(1, 500);
    if (s == INT_SUCCESS) {
      int len = rdUsbData(buf, 64);
      if (len >= 18 && buf[1] == 0x01) { *low = useLow; return true; }
      report(false, "GET_DESCR(device)", "RD_USB_DATA len=%d", len);
      return false;
    }
    Serial.printf("      %s: status 0x%02X (%s)\n", useLow ? "low-speed" : "full-speed", s & 0xFF, statusStr(s));
  }
  return false;
}

// zapamietuje oba endpointy IN typu interrupt interfejsow HID (klawiatura + klawisze specjalne)
static bool parseConfig(const uint8_t *b, int len) {
  epCount = 0;
  int off = b[0];
  bool inHid = false;
  uint8_t curIface = 0, curProto = 0;
  while (off + 2 <= len) {
    uint8_t dl = b[off], dt = b[off + 1];
    if (dl < 2 || off + dl > len) break;
    if (dt == 0x04 && dl >= 9) {
      curIface = b[off + 2];
      inHid = (b[off + 5] == 0x03);
      curProto = b[off + 7];
    } else if (dt == 0x05 && dl >= 7 && inHid) {
      bool in = b[off + 2] & 0x80, intr = (b[off + 3] & 3) == 3;
      if (in && intr && epCount < 2) {
        eps[epCount++] = { (uint8_t)(b[off + 2] & 0x0F), curIface, curProto, 0 };
        Serial.printf("      interfejs %u (protocol %u): endpoint IN %u, MaxPacket %u\n",
                      curIface, curProto, b[off + 2] & 0x0F, b[off + 4] | (b[off + 5] << 8));
      }
    }
    off += dl;
  }
  return epCount > 0;
}

static bool enumerateKeyboard() {
  uint8_t buf[64];
  bool low = false;

  if (!getDeviceDescriptor(buf, &low)) { report(false, "GET_DESCR(device)", "brak odpowiedzi"); return false; }
  report(true, "GET_DESCR(device)", "%s, USB %x.%02x, class 0x%02X, ep0=%u, VID:PID=%04X:%04X, konfiguracji: %u",
         low ? "low-speed" : "full-speed", buf[3], buf[2], buf[4], buf[7],
         (unsigned)(buf[9] << 8 | buf[8]), (unsigned)(buf[11] << 8 | buf[10]), buf[17]);

  int s = setAddress(DEV_ADDR);
  report(s == INT_SUCCESS, "SET_ADDRESS", "adres %u, status 0x%02X (%s)", DEV_ADDR, s & 0xFF, statusStr(s));
  if (s != INT_SUCCESS) return false;
  delay(10);

  s = getDescr(2, 500);
  if (s == INT_BUF_OVER) { report(false, "GET_DESCR(config)", "deskryptor > 64 B (BUF_OVER)"); return false; }
  if (s != INT_SUCCESS)  { report(false, "GET_DESCR(config)", "status 0x%02X (%s)", s & 0xFF, statusStr(s)); return false; }
  int len = rdUsbData(buf, 64);
  if (len < 9 || buf[1] != 0x02) { report(false, "GET_DESCR(config)", "RD_USB_DATA len=%d", len); return false; }
  report(true, "GET_DESCR(config)", "%d B, wTotalLength=%u, bConfigurationValue=%u",
         len, buf[2] | (buf[3] << 8), buf[5]);
  hexdump("config", buf, len);

  if (!parseConfig(buf, len)) {
    report(false, "Interfejs HID", "nie znaleziono endpointu IN HID");
    return false;
  }
  uint8_t cfgVal = buf[5];

  s = setConfig(cfgVal);
  report(s == INT_SUCCESS, "SET_CONFIG", "konfiguracja %u, status 0x%02X (%s)", cfgVal, s & 0xFF, statusStr(s));
  if (s != INT_SUCCESS) return false;

#if SET_BOOT_PROTOCOL
  for (int i = 0; i < epCount; i++) {
    if (eps[i].proto == 1) {                                     // boot tylko dla klawiatury
      s = controlNoData(0x21, 0x0B, 0, eps[i].iface);            // SET_PROTOCOL(0 = boot)
      report(s == INT_SUCCESS, "SET_PROTOCOL(boot)", "iface %u, status 0x%02X (%s)", eps[i].iface, s & 0xFF, statusStr(s));
    }
    s = controlNoData(0x21, 0x0A, 0, eps[i].iface);              // SET_IDLE(0)
    Serial.printf("      SET_IDLE(0) iface %u: status 0x%02X (%s) [STALL jest dopuszczalny]\n",
                  eps[i].iface, s & 0xFF, statusStr(s));
  }
#endif

  startCmd(CMD_SET_RETRY); txData(0x25); txData(0x05);
  return true;
}

// ------------------------------------------------------------ diagnostyka: klawisze -> tekst

#if SHOW_KEYS
static bool capsLock = false;

static const char kLow[]  = "abcdefghijklmnopqrstuvwxyz1234567890";   // 0x04..0x27
static const char kUp[]   = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()";
static const char pLow[]  = "-=[]\\#;'`,./";                           // 0x2D..0x38
static const char pUp[]   = "_+{}|~:\"~<>?";

static const char *specialName(uint8_t k) {
  static char fbuf[8];
  if (k >= 0x3A && k <= 0x45) { snprintf(fbuf, sizeof(fbuf), "F%u", k - 0x3A + 1); return fbuf; }
  switch (k) {
    case 0x29: return "Esc";       case 0x2A: return "Backspace"; case 0x39: return "CapsLock";
    case 0x46: return "PrtSc";     case 0x47: return "ScrollLock"; case 0x48: return "Pause";
    case 0x49: return "Insert";    case 0x4A: return "Home";       case 0x4B: return "PageUp";
    case 0x4C: return "Delete";    case 0x4D: return "End";        case 0x4E: return "PageDown";
    case 0x4F: return "Right";     case 0x50: return "Left";       case 0x51: return "Down";
    case 0x52: return "Up";        case 0x53: return "NumLock";    case 0x65: return "Menu";
  }
  return NULL;
}

static void printKey(uint8_t k, uint8_t mods) {
  bool shift = mods & 0x22, ctrl = mods & 0x11, alt = mods & 0x44, gui = mods & 0x88;
  char c = 0;
  if (k >= 0x04 && k <= 0x1D)      { bool up = shift ^ capsLock; c = up ? kUp[k - 4] : kLow[k - 4]; }
  else if (k >= 0x1E && k <= 0x27) { c = shift ? kUp[k - 4] : kLow[k - 4]; }
  else if (k >= 0x2D && k <= 0x38) { c = shift ? pUp[k - 0x2D] : pLow[k - 0x2D]; }
  else if (k == 0x2C) c = ' ';
  else if (k == 0x28 || k == 0x58) c = '\n';
  else if (k == 0x2B) c = '\t';
  else if (k >= 0x59 && k <= 0x61) c = '1' + (k - 0x59);         // klawiatura numeryczna
  else if (k == 0x62) c = '0';
  else if (k == 0x63) c = '.';
  else if (k == 0x54) c = '/';
  else if (k == 0x55) c = '*';
  else if (k == 0x56) c = '-';
  else if (k == 0x57) c = '+';

  if (k == 0x39) capsLock = !capsLock;

  if (c && !(ctrl || alt || gui)) { Serial.write((uint8_t)c); return; }

  Serial.print("[");
  if (ctrl) Serial.print("Ctrl+");
  if (alt)  Serial.print("Alt+");
  if (gui)  Serial.print("Gui+");
  if (c && c != '\n' && c != '\t' && c != ' ') { Serial.write((uint8_t)c); }
  else if (c == '\n') Serial.print("Enter");
  else if (c == '\t') Serial.print("Tab");
  else if (c == ' ')  Serial.print("Space");
  else {
    const char *n = specialName(k);
    if (n) Serial.print(n); else Serial.printf("0x%02X", k);
  }
  Serial.print("]");
}
#endif

// ------------------------------------------------------------ raporty HID -> komputer

static uint8_t prevMod = 0;
static uint8_t prevKeys[6];
static bool macroActive = false;   // po uruchomieniu makra ignorujemy klawiature do puszczenia wszystkich klawiszy
static uint32_t macroStart = 0;    // czas uruchomienia makra (limit czasu)

// Szuka makra, ktorego trigger (dowolna kombinacja) pasuje do nowo wcisnietego klawisza.
// mod - surowy bajt modyfikatorow z raportu HID (L/R osobno), keys - 6 kodow klawiszy z raportu.
static const MacroDef *findMacro(uint8_t mod, const uint8_t *keys) {
  uint8_t nmod = normalizeMods(mod);
  for (int i = 0; i < 6; i++) {
    uint8_t k = keys[i];
    if (k < 0x04) continue;
    bool wasDown = false;
    for (int j = 0; j < 6; j++) if (prevKeys[j] == k) wasDown = true;
    if (wasDown) continue;                       // interesuje nas tylko nowo wcisniety klawisz
    for (auto &m : macros)
      if (m.hasTrigger && m.triggerKey != 0 && m.triggerKey == k && m.triggerMod == nmod) return &m;
  }
  return nullptr;
}

// Trigger bedacy samym modyfikatorem (np. "win"), bez zadnego klawisza - tylko gdy zaden "zwykly"
// klawisz nie jest aktualnie wcisniety (inaczej np. Win+K bez pasujacego triggera zle wyzwolaloby "win").
static const MacroDef *findModOnlyMacro(uint8_t mod, uint8_t prevRawMod, const uint8_t *keys) {
  for (int i = 0; i < 6; i++) if (keys[i] >= 0x04) return nullptr;
  uint8_t nmod = normalizeMods(mod);
  uint8_t prevNmod = normalizeMods(prevRawMod);
  if (nmod == 0 || nmod == prevNmod) return nullptr;   // nic nowego nie zostalo wcisniete
  for (auto &m : macros)
    if (m.hasTrigger && m.triggerKey == 0 && m.triggerMod == nmod) return &m;
  return nullptr;
}

// Szuka makra po numerze na liscie (0-based) - uzywane przy zdalnym wyzwalaniu z bramki;
// dziala takze dla makr bez triggera ("tylko zdalnie").
static const MacroDef *findMacroByIndex(uint8_t idx) {
  if (idx < macros.size()) return &macros[idx];
  return nullptr;
}

// Wpisuje tekst na komputerze znak po znaku (uklad US/ASCII)
static void typeText(const char *text) {
  for (const char *p = text; *p; p++) {
    Keyboard.write((uint8_t)*p);
    delay(MACRO_CHAR_DELAY_MS);
  }
}

// Wykonuje kolejne kroki makra: wpisanie tekstu, skrot klawiszowy, pauza
static void runSteps(const MacroDef *macro) {
  for (const Step &s : macro->steps) {
    switch (s.type) {
      case STEP_TYPE:
        typeText(s.text.c_str());
        break;
      case STEP_DELAY:
        delay(s.ms);
        break;
      case STEP_COMBO: {
        uint8_t keys[6] = { s.key, 0, 0, 0, 0, 0 };
        hidSendKeys(s.mod, keys);
        delay(40);
        hidReleaseAll();
        delay(40);
        break;
      }
    }
  }
}

static void runMacro(const MacroDef *macro) {
  if (!macro) return;
#if SHOW_KEYS
  Serial.printf("[Makro %s]", macro->id.length() ? macro->id.c_str() : "?");
#endif
  hidReleaseAll();                 // zwolnij modyfikatory na komputerze przed makrem
  delay(20);
  runSteps(macro);
  Keyboard.releaseAll();           // wyzeruj stan biblioteki
  hidReleaseAll();                 // komputer nie ma zostac z wcisnietym klawiszem
  drainInt();                      // skasuj zalegle przerwanie CH375
  macroActive = true;
  macroStart = millis();
  prevMod = 0;
  memset(prevKeys, 0, sizeof(prevKeys));
}

// Wolane z petli glownej: podmienia makra po aktualizacji i obsluguje zdalne wyzwolenie makra
static void checkRemoteMacro() {
  applyMacroUpdateIfAny();
#if RADIO_ENABLE
  if (!pendingMacroFlag) return;
  pendingMacroFlag = false;
  uint8_t idx = pendingMacroIdx;
  const MacroDef *macro = findMacroByIndex(idx);
  if (macro) {
    Serial.printf("[Radio] zdalne wyzwolenie makra #%u (%s)\n", idx, macro->id.length() ? macro->id.c_str() : "?");
    runMacro(macro);
  } else {
    Serial.printf("[Radio] nieznany numer makra %u\n", idx);
  }
#endif
}

static void handleReport(const uint8_t *r, int len) {
#if SHOW_RAW_REPORT
  hexdump("raport", r, len);
#endif
  if (len < 8) return;
  uint8_t mod = r[0];
  const uint8_t *keys = r + 2;
  for (int i = 0; i < 6; i++) if (keys[i] == 0x01) return;        // ErrorRollOver - ignoruj

  // przekaz tylko gdy raport sie zmienil
  if (mod == prevMod && memcmp(keys, prevKeys, 6) == 0) return;

  // trwa makro: nic nie przekazuj, dopoki wszystkie klawisze nie zostana puszczone (max 1,5 s)
  if (macroActive) {
    bool allUp = (mod == 0);
    for (int i = 0; i < 6; i++) if (keys[i]) allUp = false;
    if (allUp || millis() - macroStart > 1500) {
      macroActive = false;
      prevMod = 0;
      memset(prevKeys, 0, sizeof(prevKeys));
      if (allUp) return;           // przy timeoucie przetwarzaj raport normalnie ponizej
    } else {
      return;
    }
  }

  // trigger (dowolna kombinacja) -> makro; sam skrot NIE jest wysylany do komputera
  const MacroDef *macro = findMacro(mod, keys);
  if (!macro) macro = findModOnlyMacro(mod, prevMod, keys);
  if (macro) {
    runMacro(macro);
    return;
  }

#if SHOW_KEYS || RADIO_ENABLE
  {
    uint8_t newMods = mod & ~prevMod;          // modyfikator wcisniety samodzielnie (bez innego klawisza)
    bool anyRealKey = false;
    for (int i = 0; i < 6; i++) {
      uint8_t k = keys[i];
      if (k < 0x04) continue;
      bool wasDown = false;
      for (int j = 0; j < 6; j++) if (prevKeys[j] == k) wasDown = true;
      if (wasDown) continue;
      anyRealKey = true;
#if SHOW_KEYS
      printKey(k, mod);
#endif
#if RADIO_ENABLE
      queueKeyPress(mod, k);     // TYLKO xQueueSend (mikrosekundy) - do lokalnego logu, wysylka raz na minute
#endif
    }
#if RADIO_ENABLE
    if (newMods && !anyRealKey) queueKeyPress(newMods, 0);   // sam modyfikator wcisniety osobno
#endif
  }
#endif

  hidSendKeys(mod, keys);
  prevMod = mod;
  memcpy(prevKeys, keys, 6);
}

// ------------------------------------------------------------ odpytywanie klawiatury

// Pelny reset CH375 + ponowna inicjalizacja modulu
static void ch375Recover() {
  Serial.println("[CH375] reset i ponowna inicjalizacja modulu...");
  bool ok = false;
  for (int i = 0; i < 3 && !ok; i++) {
    ok = detectFrame();
    if (!ok) delay(200);
  }
  if (!ok) { Serial.println("[FAIL] CH375 nie odpowiada po resecie."); return; }
  drainInt();
  int r = setUsbMode(6);
  report(r == RET_SUCCESS, "SET_USB_MODE(6)", "odp=0x%02X (%s)", r & 0xFF, statusStr(r));
}

// Raport z drugiego interfejsu: [Report ID][usage lo][usage hi] (ID 1 = Consumer Control)
static void handleExtraReport(const uint8_t *r, int len) {
#if SHOW_RAW_REPORT
  hexdump("raport EP2", r, len);
#endif
  if (len >= 3 && r[0] == 0x01) {
    uint16_t usage = r[1] | (r[2] << 8);
    if (usage) Consumer.press(usage); else Consumer.release();
  }
}

// zwraca true przy normalnym odlaczeniu klawiatury, false po zbyt wielu bledach (potrzebny reset CH375)
static bool pollKeyboard() {
  Serial.println("\n>>> Klawiatura gotowa - klawisze sa przekazywane przez USB ESP32-S3 do komputera <<<\n");
  prevMod = 0;
  memset(prevKeys, 0, sizeof(prevKeys));
  macroActive = false;
  hidReleaseAll();
  Consumer.release();
  drainInt();
  int errors = 0;
  uint8_t buf[64];
  uint8_t sentLeds = 0xFF;          // wymusza wyslanie stanu diod na starcie
  int cur = 0;

  for (;;) {
    checkRemoteMacro();                                           // aktualizacja makr + zdalna komenda
                                                                    // (UWAGA: zadna wysylka radiowa nie dzieje sie tutaj -
                                                                    //  cala wysylka ESP-NOW jest w logTask na rdzeniu 0)
    HidEp *e = &eps[cur];
    cur = (cur + 1) % epCount;

    startCmd(CMD_ISSUE_TKN_X);
    txData(e->toggle ? 0x80 : 0x00);                             // bit7 = oczekiwany DATA1/DATA0
    txData((e->ep << 4) | PID_IN);
    int st = waitInt(100);

    if (st == INT_SUCCESS) {
      errors = 0;
      int len = rdUsbData(buf, sizeof(buf));
      if (len > 0) {
        e->toggle ^= 1;
        if (e->proto == 1) handleReport(buf, len);
        else               handleExtraReport(buf, len);
      }
    } else if (st >= 0 && (st & 0xEF) == 0x2A) {                 // NAK: brak nowych danych
      errors = 0;
    } else if (st == INT_DISCONNECT) {
      Serial.println("\n[USB] klawiatura odlaczona");
      hidReleaseAll();
      Consumer.release();
      return true;
    } else if (st >= 0 && (st & 0xEF) == 0x23) {                 // niezgodny DATA toggle
      e->toggle ^= 1;
    } else if (st >= 0 && (st & 0xEF) == 0x2E) {                 // STALL
      startCmd(CMD_CLR_STALL); txData(0x80 | e->ep); waitInt(200);
      e->toggle = 0;
    } else {
      if (++errors >= 20) {
        Serial.printf("\n[USB] zbyt wiele bledow (ostatni 0x%02X: %s) - ponowna enumeracja\n",
                      st & 0xFF, statusStr(st));
        hidReleaseAll();
        Consumer.release();
        return false;
      }
    }

    // diody LED z komputera -> fizyczna klawiatura
    uint8_t want = hostLeds;
    if (want != sentLeds && !macroActive) {
      for (int i = 0; i < epCount; i++) {
        if (eps[i].proto == 1) {
          int r = setLeds(eps[i].iface, want);
          Serial.printf("[LED] 0x%02X -> status 0x%02X (%s)\n", want, r & 0xFF, statusStr(r));
        }
      }
      sentLeds = want;
    }
    delay(1);
  }
}

// ============================================================ Log klikniec: wsadowa wysylka co ~1 min (rdzen 0)
//
// Klawiatura zbiera kliknieca lokalnie (tylko zapis do tablicy w pamieci, zero radia).
// Raz na LOG_FLUSH_MS (albo wczesniej, jesli bufor sie zapelnia) caly bufor jest wysylany
// do bramki JEDNYM niezawodnym transferem (ten sam wzorzec co aktualizacja makr: BEGIN/DATA/ACK/DONE,
// z retransmisjami na poziomie aplikacji). Pusty bufor tez jest wysylany - to dziala jako heartbeat:
// bramka wie, ze klawiatura zyje, nawet gdy nikt nic nie pisal.
#if RADIO_ENABLE

#define LOG_ENTRY_CAP   1500                    // 1500*4B = 6000 B bufora - z zapasem na typowa minute pisania
#define LOG_FORCE_AT    (LOG_ENTRY_CAP - 50)    // wczesniejszy flush, gdyby bufor sie zapelnial
#define LOG_FLUSH_MS    60000UL                 // co ile wysylamy log (takze jako heartbeat)

static QueueHandle_t radioKeyQ = nullptr;        // KeyEvt z handleReport (rdzen 1) -> logTask (rdzen 0)
struct KeyEvt { uint8_t mod; uint8_t key; };

static LogEntry logBuf[LOG_ENTRY_CAP];
static uint16_t logCount = 0;

static bool logSendAndWait(Msg &m, int32_t expectAck) {
  for (int attempt = 0; attempt < 6; attempt++) {
    while (channelScanning) vTaskDelay(1);        // nie koliduj z findGateway()
    logLastAck = -2;
    esp_now_send(gwMac, (uint8_t *)&m, sizeof(m));
    uint32_t t = millis();
    while (millis() - t < 150) {
      if (logLastAck == expectAck) return true;
      vTaskDelay(1);
    }
  }
  return false;
}

// Zwraca true, jesli wszystkie kawalki zostaly potwierdzone (dane bezpiecznie dotarly do bramki) -
// wtedy mozna bezpiecznie wyczyscic lokalny bufor. Przy false bufor NIE jest czyszczony, zeby nic
// nie zginelo - probujemy wyslac te same dane przy nastepnej okazji.
static bool sendLogBatch() {
  uint16_t n = logCount;
  uint16_t totalBytes  = n * (uint16_t)sizeof(LogEntry);
  uint16_t totalChunks = (totalBytes + CHUNK - 1) / CHUNK;
  uint32_t crc = crc32Update(0, (const uint8_t *)logBuf, totalBytes);

  Msg m{}; m.type = MSG_LOG_BEGIN; m.idx = totalBytes; m.value = crc;
  logDoneFlag = false;
  if (!logSendAndWait(m, LOG_ACK_BEGIN)) {
    Serial.println("[LOG] brak odpowiedzi bramki na LOG_BEGIN - probuje przy nastepnej wysylce");
    return false;
  }

  const uint8_t *bytes = (const uint8_t *)logBuf;
  for (uint16_t i = 0; i < totalChunks; i++) {
    Msg d{}; d.type = MSG_LOG_DATA; d.idx = i;
    uint32_t off = (uint32_t)i * CHUNK;
    uint32_t len = totalBytes - off; if (len > CHUNK) len = CHUNK;
    memcpy(d.data, bytes + off, len);
    d.len = (uint8_t)len;
    if (!logSendAndWait(d, (int32_t)(i | LOG_ACK_BIT))) {
      Serial.printf("[LOG] brak ACK na kawalek %u - probuje przy nastepnej wysylce\n", i);
      return false;
    }
  }

  uint32_t t0 = millis();
  while (!logDoneFlag && millis() - t0 < 1000) vTaskDelay(1);
  if (logDoneFlag) {
    Serial.printf("[LOG] wyslano %u zdarzen (%s)\n", n, logDoneValue ? "potwierdzone" : "blad CRC po stronie bramki");
  } else if (n == 0) {
    Serial.println("[LOG] heartbeat wyslany (brak potwierdzenia koncowego)");
  } else {
    Serial.printf("[LOG] wyslano %u zdarzen, dane dotarly (brak potwierdzenia koncowego)\n", n);
  }
  return true;    // wszystkie kawalki potwierdzone - dane bezpiecznie dotarly do bramki
}

static void logTask(void *) {
  vTaskDelay(pdMS_TO_TICKS(3000));      // daj czas findGateway() w macroTask na pierwszy kontakt
  uint32_t lastFlush = millis();

  for (;;) {
    KeyEvt k;
    if (xQueueReceive(radioKeyQ, &k, pdMS_TO_TICKS(200))) {
      if (logCount < LOG_ENTRY_CAP) {
        uint32_t now = millis();
        static uint32_t lastEventMs = 0;
        uint32_t dt = (lastEventMs == 0) ? 0 : now - lastEventMs;
        lastEventMs = now;
        logBuf[logCount++] = { (uint16_t)(dt > 65535 ? 65535 : dt), k.mod, k.key };
      }
    }

    bool dueTime = millis() - lastFlush >= LOG_FLUSH_MS;
    bool dueFull = logCount >= LOG_FORCE_AT;
    if (dueTime || dueFull) {
      if (sendLogBatch()) logCount = 0;   // bufor czyscimy TYLKO po potwierdzonej wysylce
      lastFlush = millis();
    }
  }
}

// Wolane z handleReport() na rdzeniu 1 (petla CH375) - TYLKO wklada do kolejki, nigdy nie wysyla
static void queueKeyPress(uint8_t mod, uint8_t key) {
  if (!radioKeyQ) return;
  KeyEvt e{ mod, key };
  xQueueSend(radioKeyQ, &e, 0);            // timeout 0: nigdy nie czeka, w razie przepelnienia po prostu gubi
}

#endif

// ------------------------------------------------------------ Arduino

void setup() {
  USB.VID(0x046D);                       // identyfikator producenta
  USB.PID(0xC31C);                       // identyfikator produktu
  USB.manufacturerName("Moja firma");
  USB.productName("Moja klawiatura");
  USB.serialNumber("12345");
  USB.begin();


  Serial.begin(115200);
  delay(1500);
  Serial.printf("\n=== CH375B: klawiatura USB -> natywne USB ESP32-S3 -> komputer ===\n"
                "CH375: TX=GPIO%d RX=GPIO%d INT=%d RSTI=%d baud=%d\n\n",
                CH375_TX_PIN, CH375_RX_PIN, INT_PIN, RSTI_PIN, CH375_BAUD);

  // makra: domyslne, a jesli jest plik z bramki - z pliku
  if (!LittleFS.begin(true)) Serial.println("[FAIL] LittleFS - sprawdz schemat partycji");
  macroPrefs.begin("macros", false);
  curVersion = macroPrefs.getUShort("ver", 0);
  loadDefaultMacros();
  loadMacrosFromFile();

  Keyboard.onEvent(usbEventCb);
  Keyboard.begin();
  Consumer.begin();


#if RADIO_ENABLE
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(WIFI_CH, WIFI_SECOND_CHAN_NONE);       // kanal startowy (potem findGateway)
  macroQ    = xQueueCreate(8, sizeof(Msg));
  radioKeyQ = xQueueCreate(128, sizeof(KeyEvt));

  if (esp_now_init() != ESP_OK) {
    Serial.println("[FAIL] esp_now_init() - zdalne makra/log przez radio nie beda dzialac");
  } else {
#if USE_ENCRYPT
    esp_now_set_pmk(PMK);
#endif
    esp_now_register_recv_cb(onEspNowRecvCmd);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, peerBroadcast, 6);
    peer.channel = 0;         // biezacy kanal WiFi
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) == ESP_OK) {
      espNowReady = true;
      Serial.printf("[OK  ] ESP-NOW gotowe (zdalne makra + log), MAC nadajnika: %s\n", WiFi.macAddress().c_str());
    } else {
      Serial.println("[FAIL] esp_now_add_peer() nieudane");
    }

    // peer bramki (unicast z ACK sprzetowym) - do aktualizacji makr i logu
    esp_now_peer_info_t gw = {};
    memcpy(gw.peer_addr, gwMac, 6);
    gw.channel = 0;
#if USE_ENCRYPT
    memcpy(gw.lmk, LMK, 16);
    gw.encrypt = true;
#else
    gw.encrypt = false;
#endif
    if (esp_now_add_peer(&gw) != ESP_OK) Serial.println("[FAIL] peer bramki nieudany");

    // zadanie odbioru makr (dziala niezaleznie od blokujacego pollKeyboard)
    xTaskCreatePinnedToCore(macroTask, "macroSync", 6144, nullptr, 1, nullptr, 0);
    // zadanie wysylki logu klikniec - raz na minute, cala wysylka ESP-NOW dzieje sie tutaj
    xTaskCreatePinnedToCore(logTask, "logTx", 4096, nullptr, 1, nullptr, 0);
  }
#endif

  Serial2.begin(CH375_BAUD, SERIAL_8N1, CH375_RX_PIN, CH375_TX_PIN);
  if (INT_PIN >= 0)  pinMode(INT_PIN, INPUT_PULLUP);
  if (RSTI_PIN >= 0) { pinMode(RSTI_PIN, OUTPUT); digitalWrite(RSTI_PIN, LOW); }
  delay(100);

  if (!detectFrame()) {
    Serial.println("[FAIL] CH375 nie odpowiada - sprawdz polaczenia TX/RX, mase, zasilanie, kwarc 12 MHz, pin 5 przy resecie.");
    while (true) delay(1000);
  }
#if INT_PIN >= 0
  {
    int lvl = digitalRead(INT_PIN);
    if (lvl == LOW) { int st = drainInt(); Serial.printf("      INT# byl niski - odczytano zalegly status 0x%02X\n", st & 0xFF); lvl = digitalRead(INT_PIN); }
    report(lvl == HIGH, "INT# w spoczynku", "poziom=%d (oczekiwane 1; jesli 0 - sprawdz podciagniecie i polaczenie)", lvl);
  }
#endif
  int r = setUsbMode(6);
  report(r == RET_SUCCESS, "SET_USB_MODE(6)", "odp=0x%02X (%s)", r & 0xFF, statusStr(r));
}

void loop() {
  checkRemoteMacro();                                             // dziala takze bez podlaczonej klawiatury
  static uint32_t lastMsg = 0;
  if (!devicePresent()) {
    if (millis() - lastMsg > 5000 || lastMsg == 0) { Serial.println("Podlacz klawiature USB..."); lastMsg = millis(); }
    delay(100);
    return;
  }
  lastMsg = 0;
  Serial.println("\n[USB] wykryto urzadzenie - enumeracja...");
  if (enumerateKeyboard()) {
    if (!pollKeyboard()) ch375Recover();     // zbyt wiele bledow -> zresetuj CH375
  } else {
    Serial.println("[USB] enumeracja nieudana - ponowna proba za 2 s");
    delay(2000);
  }
  setUsbMode(6);
  delay(300);
}