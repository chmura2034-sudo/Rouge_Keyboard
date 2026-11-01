#pragma once
// Wspolny plik - ma byc IDENTYCZNY w projekcie bramki i klawiatury
#include <Arduino.h>

// Protokol aktualizacji makr (bramka -> klawiatura): HELLO, BEGIN, DATA, ACK, DONE
// Protokol wsadowego logu klikniec (klawiatura -> bramka): LOG_BEGIN, LOG_DATA, LOG_DONE
// ACK jest wspolny dla obu kierunkow, odrozniany przez wartosc idx (patrz LOG_ACK_BEGIN nizej
// i offset 0x8000 uzywany dla potwierdzen kawalkow logu).
enum : uint8_t { MSG_HELLO = 1, MSG_BEGIN, MSG_DATA, MSG_ACK, MSG_DONE,
                  MSG_LOG_BEGIN, MSG_LOG_DATA, MSG_LOG_DONE };

constexpr size_t   CHUNK        = 200;
constexpr uint16_t ACK_BEGIN    = 0xFFFF;   // potwierdzenie MSG_BEGIN (protokol makr)
constexpr uint16_t LOG_ACK_BEGIN = 0xFFFE;  // potwierdzenie MSG_LOG_BEGIN (protokol logu)
constexpr uint16_t LOG_ACK_BIT  = 0x8000;   // potwierdzenie kawalka logu: idx = numer_kawalka | LOG_ACK_BIT
constexpr uint8_t  WIFI_CH      = 6;        // kanal Twojego routera (2,4 GHz) - startowy kanal klawiatury

// Szyfrowanie ESP-NOW: 1 = wlaczone, 0 = wylaczone (oba urzadzenia musza miec to samo i te same klucze)
#define USE_ENCRYPT 0
static uint8_t PMK[16] = {'m','o','j','_','p','m','k','_','0','1','_','x','y','z','1','2'};
static uint8_t LMK[16] = {'m','o','j','_','l','m','k','_','0','1','_','x','y','z','3','4'};

struct __attribute__((packed)) Msg {
  uint8_t  type;
  uint16_t version;
  uint16_t idx;      // DATA: nr kawalka | BEGIN: liczba kawalkow | ACK: potwierdzany nr
                      // LOG_BEGIN: calkowity rozmiar danych w bajtach | LOG_DATA: nr kawalka
  uint32_t value;    // BEGIN/LOG_BEGIN: CRC32 | DONE: 1=ok,2=juz aktualne,0=blad | LOG_DONE: 1=ok,0=blad CRC
  uint8_t  len;
  uint8_t  data[CHUNK];
};

// Jedno zdarzenie w logu klikniec: czas od poprzedniego zdarzenia (ms, max 65535),
// modyfikatory HID (1=Ctrl 2=Shift 4=Alt 8=Win), kod klawisza HID (0 = sam modyfikator).
// WAZNE: uzywane identycznie po obu stronach (odczyt/zapis przez memcpy) - ten sam ESP32,
// wiec endianness i wyrownanie sa zgodne, ale zostaje "packed" dla pewnosci.
struct __attribute__((packed)) LogEntry {
  uint16_t dtMs;
  uint8_t  mod;
  uint8_t  key;
};