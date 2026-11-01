# Programowalna klawiatura USB z mostem CH375 i zdalnym zarządzaniem przez ESP-NOW

Dokumentacja projektu: co się gdzie znajduje, jak działa komunikacja między urządzeniami,
kiedy co się wysyła, oraz pełna składnia makr.

---

## 1. Przegląd systemu

Projekt składa się z dwóch niezależnych urządzeń ESP32, połączonych bezprzewodowo przez
**ESP-NOW** (nie wymaga routera, choć może współistnieć z siecią Wi-Fi).

```
Fizyczna klawiatura USB
        │ (USB, przez CH375B)
        ▼
┌─────────────────────┐         ESP-NOW          ┌─────────────────────┐
│   KLAWIATURA         │◄───────────────────────►│      BRAMKA          │
│   (ESP32-S3)          │                          │   (ESP32 zwykłe)      │
│                       │                          │                       │
│ - CH375 jako host USB │                          │ - Wi-Fi (router)      │
│ - natywne USB jako    │                          │ - serwer WWW          │
│   urządzenie HID      │                          │ - LittleFS (plik makr)│
└──────────┬────────────┘                          └──────────┬────────────┘
           │ USB (natywne, TinyUSB)                            │ Wi-Fi
           ▼                                                   ▼
      Komputer                                        Router / Internet
   (odbiera klawisze                                  (panel WWW, curl)
    i makra jak z
    normalnej klawiatury)
```

**Klawiatura** (ESP32-S3) pośredniczy między fizyczną klawiaturą USB (podłączoną przez układ
CH375B pracujący w trybie hosta USB) a komputerem (do którego podłącza się przez natywne
USB jako zwykłe urządzenie HID). Dodatkowo wykonuje makra i komunikuje się radiowo z bramką.

**Bramka** (zwykłe ESP32 z Wi-Fi) wystawia stronę WWW do edycji makr, wysyła je do
klawiatury, odbiera jej log kliknięć i pozwala zdalnie uruchamiać makra.

---

## 2. Pliki projektu

| Plik | Urządzenie | Rola |
|---|---|---|
| `keyboard/main.cpp` | ESP32-S3 | Obsługa CH375, HID, makra, log, komunikacja ESP-NOW |
| `gateway/main.cpp` | ESP32 (zwykłe) | Serwer WWW, przekazywanie makr, odbiór logu |
| `proto.h` | **oba** | Wspólna definicja protokołu — **musi być identyczny** na obu płytkach |

### `proto.h` — dlaczego musi być identyczny

Oba urządzenia komunikują się strukturą `Msg` (typ wiadomości, numer porcji, CRC, dane) oraz
wspólnymi stałymi (`CHUNK`, `ACK_BEGIN`, `LOG_ACK_BEGIN`, `LOG_ACK_BIT`, numery typów
wiadomości). Jeśli plik różni się choćby jedną liczbą między urządzeniami, wiadomości będą
się błędnie interpretować po drugiej stronie (np. ACK dla jednego protokołu zostanie wzięty
za coś innego).

---

## 3. Konfiguracja — co zmienić przed wgraniem

### W `keyboard/main.cpp`
```cpp
static uint8_t gwMac[6] = {0x8C, 0x94, 0xDF, 0x4C, 0xD8, 0x64};   // MAC BRAMKI
```
Podstaw rzeczywisty adres MAC bramki (wypisywany w jej logu przy starcie jako `MAC bramki: ...`).

Piny CH375 i inne stałe sprzętowe:
```cpp
#define CH375_TX_PIN     17
#define CH375_RX_PIN     16
#define INT_PIN          4
#define RSTI_PIN         5
#define CH375_BAUD       9600
```

### W `gateway/main.cpp`
```cpp
const char *WIFI_SSID = "...";
const char *WIFI_PASS = "...";
uint8_t kbMac[6] = {0x3C, 0x0F, 0x02, 0xDA, 0x3D, 0x20};   // MAC KLAWIATURY
const char *WWW_USER = "admin";
const char *WWW_PASS = "...";       // ZMIEŃ na własne hasło przed użyciem produkcyjnym
```

### W `proto.h`
```cpp
constexpr uint8_t WIFI_CH = 6;      // startowy kanał radiowy (zwykle = kanał routera bramki)
#define USE_ENCRYPT 0                // 1 = włącz szyfrowanie ESP-NOW (patrz sekcja Bezpieczeństwo)
```

---

## 4. Protokoły komunikacji

System używa **trzech niezależnych protokołów** nad ESP-NOW, rozróżnianych przez pole
`type` w strukturze `Msg` (lub przez pierwszy bajt dla komend jednobajtowych).

### 4.1. Aktualizacja makr (bramka → klawiatura)

Uruchamiana, gdy ktoś zapisze nowe makra na stronie WWW.

| Krok | Nadawca | Wiadomość | Odbiorca odpowiada |
|---|---|---|---|
| 1 | Bramka | `MSG_BEGIN` (idx=liczba porcji, value=CRC32 całości) | `MSG_ACK` (idx=`ACK_BEGIN`) |
| 2 | Bramka | `MSG_DATA` × N (po jednej porcji, max 200 B każda) | `MSG_ACK` (idx=numer porcji) |
| 3 | Klawiatura | po odebraniu wszystkich porcji i weryfikacji CRC | `MSG_DONE` (value=1 sukces / 0 błąd CRC / 2 już aktualne) |

- Bramka wysyła kolejną porcję dopiero po ACK poprzedniej (do 6 prób retransmisji, 150 ms
  timeout na próbę).
- Klawiatura zapisuje dane do pliku tymczasowego `/macros.tmp`, dopiero po pełnej
  weryfikacji CRC podmienia go na właściwy `/macros.json` — przerwany transfer nigdy nie
  nadpisuje działającej konfiguracji.
- **Wersjonowanie**: każdy zapis na stronie WWW podnosi `macroVersion`. Klawiatura zna
  swoją `curVersion` (zapisaną trwale w pamięci Preferences) i ignoruje aktualizację, jeśli
  ma już tę samą lub nowszą wersję (`MSG_DONE` z value=2).

**Kiedy to się dzieje:**
- Natychmiast po zapisaniu makr na stronie (`handleSave()` → `saveMacros()` → flaga `pendingPush`).
- Bramka sprawdza flagę `pendingPush` w `loop()` co najwyżej raz na 10 s i ponawia próbę,
  dopóki się nie uda.
- Dodatkowo: gdy klawiatura się uruchamia lub traci i odzyskuje łączność, wysyła `MSG_HELLO`
  ze swoją wersją; jeśli bramka ma nowszą wersję makr, od razu wysyła aktualizację.

### 4.2. Log kliknięć (klawiatura → bramka)

**Ewolucja rozwiązania** — pierwotnie każde naciśnięcie klawisza wysyłało osobny pakiet
radiowy "na żywo". Powodowało to znaczące straty (zarówno przy transmisji broadcast, jak i
unicast przy większym natężeniu ruchu) oraz — gdy wysyłka działa się w tej samej pętli co
obsługa UART do CH375 — zakłócenia w komunikacji z fizyczną klawiaturą. Finalne rozwiązanie
zbiera kliknięcia lokalnie i wysyła je **wsadowo, raz na minutę**, tym samym niezawodnym
wzorcem ACK+retransmisja co aktualizacja makr.

| Krok | Nadawca | Wiadomość | Odbiorca odpowiada |
|---|---|---|---|
| 1 | Klawiatura | `MSG_LOG_BEGIN` (idx=rozmiar danych w bajtach, value=CRC32) | `MSG_ACK` (idx=`LOG_ACK_BEGIN`) |
| 2 | Klawiatura | `MSG_LOG_DATA` × N | `MSG_ACK` (idx = numer porcji \| `LOG_ACK_BIT`) |
| 3 | Bramka | po odebraniu wszystkich porcji i weryfikacji CRC | `MSG_LOG_DONE` (value=1 sukces / 0 błąd CRC) |

- Każde zdarzenie w logu to 4 bajty: `dtMs` (czas od poprzedniego zdarzenia, ms, uint16),
  `mod` (maska modyfikatorów), `key` (kod HID klawisza, 0 = sam modyfikator).
- Bufor na klawiaturze mieści do **1500 zdarzeń** (`LOG_ENTRY_CAP`), co przy typowym pisaniu
  z zapasem starcza na minutę.
- **Pusty bufor też się wysyła** — działa jako heartbeat (bramka wie, że klawiatura żyje,
  nawet gdy nikt nic nie pisał).
- Bufor jest czyszczony **tylko po potwierdzonej wysyłce** — jeśli transfer się nie powiedzie
  (np. kolizja z wyszukiwaniem kanału), dane zostają i próba powtarza się przy następnej
  okazji, zamiast przepaść.
- Cała logika logu działa na osobnym zadaniu FreeRTOS (`logTask`, rdzeń 0), niezależnie od
  pętli obsługującej CH375 (rdzeń 1) — zero wpływu na stabilność połączenia z fizyczną
  klawiaturą.

**Kiedy to się dzieje:**
- Co `LOG_FLUSH_MS` = **60 000 ms (1 minuta)** od poprzedniej wysyłki.
- Wcześniej, jeśli bufor zbliży się do limitu (`LOG_FORCE_AT` = 1450 zdarzeń).
- Pierwsza wysyłka: 3 sekundy po starcie zadania `logTask` (żeby dać czas na pierwszy
  kontakt z bramką przez `macroTask`).

### 4.3. Zdalne uruchomienie makra (bramka → klawiatura)

Jednorazowy, nieniezawodny (bez ACK+retry) pakiet 2-bajtowy:
```
[0xFE][numer makra na liście, 0-based]
```
- Wysyłany przez `sendRun()` na bramce, w odpowiedzi na kliknięcie przycisku na stronie
  lub żądanie `POST /run`.
- Numer makra to jego **pozycja w tablicy JSON**, licząc od 0 — nie kod klawisza. Dzięki
  temu działa identycznie dla makr z fizycznym skrótem i bez niego.
- Klawiatura odbiera w `onEspNowRecvCmd()`, ustawia flagę `pendingMacroFlag`, a wykonanie
  (`checkRemoteMacro()`) dzieje się w głównej pętli — nigdy w callbacku radiowym.
- Jeśli pakiet zginie (brak potwierdzenia na tym poziomie), trzeba kliknąć ponownie — to
  świadomy kompromis: rzadkie, ręczne akcje nie potrzebują pełnego protokołu ACK+retry.

---

## 5. Format zapisu makr (JSON)

Plik `/macros.json` na klawiaturze, edytowany przez stronę WWW bramki.

### Minimalny przykład
```json
[{"trigger":"ctrl+t","text":"test_t"}]
```

### Pełna struktura jednego makra

| Pole | Wymagane | Opis |
|---|---|---|
| `id` | nie | Nazwa makra — etykieta na przycisku bramki i w logu klawiatury. Bez niej etykietą jest `trigger` albo skrócony podgląd treści. |
| `trigger` | nie | Kombinacja klawiszy fizycznej klawiatury uruchamiająca makro. **Brak tego pola = makro tylko-zdalne** (nie reaguje na żadną fizyczną kombinację). |
| `key` | nie | **Stary format** (zgodność wsteczna) — litera a-z lub kod HID; traktowany jak `trigger:"ctrl+<litera>"`. |
| `text` | nie | Tekst wpisywany na komputerze (jak ręczne pisanie, 10 ms/znak). |
| `url` | nie | Otwiera adres w przeglądarce: Ctrl+Alt+T → `xdg-open 'URL' && exit` → Enter (zakłada Ubuntu/GNOME). |
| `action` | nie | `"youtube"` — skrót do `url:"https://www.youtube.com"`. |
| `steps` | nie | Lista własnych kroków wykonywanych po kolei (patrz niżej). |

Pola `text`, `url`/`action` i `steps` **można łączyć w jednym makrze** — wykonują się
dokładnie w tej kolejności (najpierw `text`, potem `url`/`action`, na końcu `steps`).

### Składnia `trigger`

Dowolna kombinacja, człony łączone `+`:
- modyfikatory: `ctrl`, `shift`, `alt`, `win` (aliasy: `control`, `gui`, `super`, `meta`, `cmd`)
- litery `a`-`z`, cyfry `0`-`9`
- `enter`/`return`, `esc`/`escape`, `tab`, `space`, `backspace`
- `delete`/`del`, `insert`, `home`, `end`, `pageup`, `pagedown`
- strzałki: `up`, `down`, `left`, `right`
- `f1`...`f12`, `printscreen`, `capslock`, `menu`
- znaki: `-` `=` `[` `]` `\` `;` `'` `` ` `` `,` `.` `/`

Przykłady: `"ctrl+alt+t"`, `"f5"` (sam klawisz, bez modyfikatora), `"win"` (sam modyfikator,
bez żadnego klawisza — uruchamia się tylko, gdy w tym samym momencie nie jest wciśnięty
żaden inny "zwykły" klawisz, żeby np. Win+R nie wyzwalało przypadkiem triggera samego Win).

Dopasowanie jest **dokładne**: trigger `"ctrl+i"` nie zareaguje, jeśli równocześnie trzymany
jest też Shift. Fizyczne wciśnięcie dopasowanego triggera **nie jest przekazywane** do
komputera — konsumuje je makro.

### Krok w `steps`

| Zapis | Działanie |
|---|---|
| `{"type":"tekst"}` | wpisuje tekst (`\n`=Enter, `\t`=Tab, `\"`=cudzysłów, `\\`=ukośnik) |
| `{"combo":"ctrl+shift+esc"}` | naciska skrót (ta sama składnia co `trigger`), trzyma 40 ms, puszcza |
| `{"delay":500}` | czeka tyle milisekund (maks. 10000) |

### Pełny przykład
```json
[
  {"id":"kornel","trigger":"ctrl+i","text":"Kornel"},
  {"id":"odswiez","trigger":"f5","url":"https://example.com"},
  {"id":"super","trigger":"win","text":"sam modyfikator jako skrot"},
  {"id":"wdroz","steps":[
    {"combo":"ctrl+alt+t"},
    {"delay":1000},
    {"type":"./deploy.sh\n"}
  ]}
]
```
Czwarte makro (`wdroz`) nie ma `trigger` → dostępne wyłącznie zdalnie (przycisk `[zdalne]`
na stronie, pole z numerem, albo `curl`).

### Zapis z kluczem `"macros"` — równoważny

```json
{"macros":[{"trigger":"ctrl+t","text":"test_t"}]}
```

### Limity

| Co | Wartość | Stała w kodzie |
|---|---|---|
| Liczba makr | 24 | `MAX_MACROS` |
| Liczba kroków w jednym makrze | 32 | `MAX_STEPS` |
| Maksymalne opóźnienie `delay` | 10 000 ms | `MAX_DELAY_MS` |

### Najczęstsze błędy składni

- apostrofy `'` zamiast cudzysłowów `"`
- brak przecinka między makrami / przecinek po ostatnim makrze
- brak `[` na początku lub `]` na końcu
- nazwa pola bez cudzysłowów (np. `{trigger:"ctrl+t"}`)
- polskie litery w `text` (klawiatura wpisuje tylko ASCII, układ US)

Przy błędzie składni bramka odpowiada „Niepoprawny JSON” i **nic nie wysyła** — zapis na
klawiaturze pozostaje niezmieniony. Każdy udany zapis **zastępuje całą** listę makr, więc
trzeba w nim zostawić również te makra, które mają nadal działać.

---

## 6. Interfejs WWW bramki

Strona (chroniona hasłem HTTP Basic Auth, `WWW_USER`/`WWW_PASS`) ma trzy panele:

### Panel 1 — Uruchom makro
Przyciski generowane z aktualnej treści `/macros.json`, w tej samej kolejności. Makra
bez `trigger` oznaczone jako `[zdalne]`. Obok: pole do ręcznego wpisania numeru makra.

### Panel 2 — Historia kliknięć
Lista ostatnich zdarzeń z logu (patrz 4.2), odświeżana z przeglądarki co 5 s przez
`GET /keylog`. Dane realnie zmieniają się maks. raz na minutę — odświeżanie co 5 s tylko
szybciej pokazuje to, co już nadeszło. Bramka trzyma w pamięci RAM (ring buffer) ostatnie
300 linii; restart bramki czyści historię (nie jest zapisywana na dysk).

Liczby w nawiasach przy nagłówkach paczek (`== paczka N zdarzen (123s) ==`) to **sekundy od
uruchomienia bramki** (`millis()/1000`), nie zegar ścienny — ESP32 nie ma wbudowanego RTC.

### Panel 3 — Edycja makr
Pole tekstowe z pełną treścią `/macros.json` i przycisk zapisu, który waliduje JSON,
zapisuje plik, podnosi wersję i wysyła aktualizację do klawiatury (patrz 4.1).

### Endpointy HTTP

| Metoda | Adres | Parametry | Działanie |
|---|---|---|---|
| GET | `/` | — | strona główna |
| POST | `/macros` | `json` | zapisuje nowe makra |
| POST | `/run` | `idx` (numer makra, 0-254) | zdalnie uruchamia makro |
| GET | `/keylog` | — | zwraca JSON z historią kliknięć |

Przykład wywołania z linii poleceń:
```bash
curl -u admin:HASLO -X POST http://IP_BRAMKI/run -d idx=2
```

---

## 7. Harmonogram zdarzeń — co, kiedy i gdzie

| Zdarzenie | Częstotliwość / wyzwalacz | Gdzie w kodzie |
|---|---|---|
| Klawiatura szuka bramki (`findGateway`) | przy starcie (po 2 s) i co 2 min, jeśli nie trwa odbiór makr | `macroTask()` (keyboard) |
| Wysyłka logu kliknięć | co 60 s (lub wcześniej przy zapełnieniu bufora) | `logTask()` (keyboard) |
| Sprawdzenie, czy wysłać zaległą aktualizację makr | w każdej iteracji `loop()`, ale realna próba wysyłki nie częściej niż co 10 s | `loop()` (gateway) |
| Odpytywanie strony o historię kliknięć | co 5 s (z przeglądarki) | `pollKeylog()` w JS na stronie |
| Weryfikacja połączenia Wi-Fi bramki | co 15 s, jeśli rozłączona | `loop()` (gateway) |
| Timeout przerwanego odbioru aktualizacji makr | 5 s bez nowej porcji | `macroTask()` (keyboard) |

---

## 8. Bezpieczeństwo

- **Panel WWW** chroniony hasłem HTTP Basic Auth — zmień domyślne `WWW_USER`/`WWW_PASS`
  przed jakimkolwiek użyciem poza zamkniętą siecią testową.
- **Zdalne uruchamianie makr nie ma uwierzytelnienia na poziomie radia** — każdy w zasięgu,
  kto zna adres MAC klawiatury, może wysłać pakiet `[0xFE][numer]`. Ponieważ makro może
  wpisywać dowolny tekst (w tym polecenia powłoki z Enterem), jest to równoważne możliwości
  wstrzyknięcia polecenia na komputerze, do którego podłączona jest klawiatura (analogia do
  ataków typu BadUSB).
- **Szyfrowanie ESP-NOW** (`USE_ENCRYPT 1` w `proto.h`, wraz z własnymi kluczami `PMK`/`LMK`
  — **identycznymi na obu urządzeniach**) ogranicza możliwość podsłuchu i wstrzykiwania
  obcych pakietów radiowych, ale nie chroni przed kimś, kto zna klucze.
- **Filtrowanie po adresie MAC** jest już wbudowane (`onRecv`/`onEspNowRecvCmd` odrzucają
  pakiety spoza znanego MAC-a), co chroni przed przypadkowym odbiorem od innych urządzeń,
  ale nie przed celowym podszyciem się pod MAC (spoofing).

---

## 9. Znane ograniczenia

- Klawiatura wpisuje tylko znaki **ASCII w układzie US** — polskie litery (ą, ę, ł...) nie
  wpiszą się poprawnie.
- Pole `url` zakłada środowisko **Ubuntu z terminalem pod Ctrl+Alt+T**; na innym systemie
  trzeba użyć własnej sekwencji `steps`.
- Historia kliknięć na stronie **nie jest zapisywana trwale** — restart bramki ją czyści
  (sam plik `/macros.json` na klawiaturze przetrwa restart, bo jest w LittleFS).
- Zdalne uruchamianie makra (`/run`) nie ma potwierdzenia wykonania — odpowiedź „Wysłano do
  klawiatury” oznacza tylko dostarczenie pakietu radiowego, nie sukces samego makra.
