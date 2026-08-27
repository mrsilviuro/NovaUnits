# NOVA Units — ghid de proiect

Sketch Arduino (ESP32) pentru un set de **unități de teren pentru airsoft** (Airsoft Club
Roman). Fiecare unitate este o cutie autonomă cu OLED, 4 butoane/LED-uri colorate, cititor
RFID, releu de sirenă, buzzer și un modul LoRa. Până la **12 unități** joacă împreună într-o
rețea LoRa fără server: fiecare unitate ține o **replică locală completă** a stării jocului și
o menține sincronizată prin alerte broadcast.

## Build & flash

- Toolchain: **Arduino IDE / arduino-cli**, board ESP32 (sketch-ul se numește `NovaUnits.ino`,
  deci folderul trebuie să rămână `NovaUnits/`).
- Biblioteci externe: `Adafruit_GFX`, `Adafruit_SSD1306` (drive-uiește un panou **SSD1309**
  2.42"), `Adafruit_PN532`, `Wire`, `SPI`.
- **Nu există teste, CI sau build local în acest repo.** Verificarea = compilare în Arduino IDE
  + test pe hardware. Când modifici ceva, verifică mental/manual că nu strici build-ul; nu
  există niciun script de validare.
- ⚠️ **`UNIT_ID` din `config.h` se schimbă manual înainte de fiecare upload** (1..12), câte o
  valoare per cutie fizică. `NETWORK_ID` se schimbă per lot de unități.

## Fișiere

| Fișier | Rol |
|---|---|
| `config.h` | Pini, ID unitate/rețea, tipuri de pachete LoRa, enum-uri (`GameState`, `Team`, `UnitMode`, `ActionType`, `WinCondition`, `TagType`), nume echipe/unități, constante de timing. Tot ce e „configurabil la compilare". |
| `NovaUnits.ino` | Sketch-ul principal: variabile de stare globale, `setup()`, `loop()` (mașina de stări + toate timer-ele de gameplay), callback-urile de butoane (`onShortPress`/`onLongPress`/`onAdminCombo`), serializarea stării pentru card (`buildExportBlob`/`applyImportBlob`). ~2100 linii — aici e creierul. |
| `game.cpp/.h` | **Modelul**: `unitTable[12]` (un rând per unitate), setările de joc, coada de respawn, scorurile. `buildContext()` împachetează tot ce trebuie desenat într-un `PageContext`. Conține și câteva ecrane de sync (istoric — nu e locul lor logic, dar acolo sunt). |
| `display.cpp/.h` | Toată randarea OLED. `drawPages()` = cele 6 pagini de joc; `drawAdminPages()` = cele 4 sub-pagini de admin; plus ecrane de stare (boot, loading, success, boom, sync, export/import etc.). Zero logică de joc — desenează doar din `PageContext`/`AdminContext`. |
| `buttons.cpp/.h` | Debounce + short/long press + combo admin (Roșu+Albastru ținute 3s). Apelează callback-urile definite în `.ino`. |
| `rfid.cpp/.h` | PN532 pe SPI, carduri MIFARE Classic: citire/scriere card de puncte și card de admin, „arderea" cardului după folosire, export/import blob de stare pe sectoarele 2..7. |
| `lora.cpp/.h` | Transport: modul DX-LR03 pe UART1 (9600), coadă TX non-blocantă cu handshake pe pinul AUX, format de pachete, `loraPoll()` care decodează și aplică ce vine. |

## Modelul de joc

`unitTable[MAX_UNITS]` — **fiecare unitate ține tabelul întreg**, nu doar rândul propriu.
`myRow()` = `unitTable[UNIT_ID-1]`. Un rând:

```c
mode        // 0=unknown, 1=sector, 2=bomb, 3=respawn
status      // sector: SEC_NEUTRAL/SEC_CAPTURED; bomb: BOMB_IDLE/ARMED/COOLDOWN
team        // cine deține / cine a plantat
actionTime  // millis() al ultimei acțiuni (ceas LOCAL, chiar și pentru rândurile altor unități)
savedPoints[4]  // puncte commited per echipă
kills[4]        // kill-uri per echipă
```

Scorul unei echipe = **suma** `savedPoints` peste tot tabelul (`teamScore()`), minus
`appliedPenalties[]` la afișare. Nu există „master" al scorului — fiecare unitate îl
recalculează din replica ei.

### Cele 3 moduri de unitate

- **Sector** — se cucerește ținând un buton de echipă `actionTimeMs`. Cât timp e ținut,
  generează **3 puncte la fiecare 10s**, plus un bonus (`+1` per `bonusIntervalMinutes` de
  deținere, plafonat la +3). `liveCapture[u]` ține punctele episodului curent, ca la
  neutralizare să se poată corecta cu valoarea exactă de la sursă.
- **Bomb** — armare (orice echipă), apoi countdown `bombTimerMs` cu beep accelerat; explozie
  → puncte celui care a plantat + `cooldownMs`. Dezamorsarea (doar de altă echipă) →
  `bombPointsDefuse` + cooldown. Bombele **altor** unități rulează autonom în `loop()`, ca
  tabelul să rămână corect fără trafic radio.
- **Respawn** — long-press GALBEN înregistrează un kill: intră în coada `respawnQueue[100]`
  (`millis() + respawnTimeMs`), incrementează `kills[]` și adaugă `respawnPenaltyPoints`.
  O echipă nu poate avea două unități de respawn (verificat la setup).

### Condiții de victorie

`WIN_BY_POINTS` (expiră ceasul), `WIN_BY_CONQUEST` (`checkConquest()` — o echipă deține toate
sectoarele din tabel; ceasul nu curge), `WIN_BY_ANY` (ambele).

## Rețeaua LoRa

- Fără adresare — totul e **broadcast**. Filtrare pe trei niveluri: `NETWORK_ID` (byte 0),
  `sessionId` (penultimul byte, adoptat la SYNC) și checksum XOR (ultimul byte).
- Byte-ul 2 e `unitByte()`: bits[3:0] = UNIT_ID, bits[6:4] = nivel baterie (0-4) — bateria
  călătorește gratis în orice pachet.
- **Sync**: o unitate din admin trimite `PKT_SYNC` (blocant, cu pauză de ceas) — celelalte
  adoptă setările (indici pe biți), `localTime` și `sessionId`. Emițătorul devine
  `isTimeMaster` și trimite periodic `PKT_TIME_SYNC` ca să anuleze drift-ul între ESP-uri.
- `isSynced == false` ⇒ **OFFLINE**: nu transmite nicio alertă și ignoră alertele de joc
  primite (ascultă doar SYNC și RESTART).
- **Coada TX e non-blocantă** (`loraTxUpdate()` în `loop()`), cu stări `TX_IDLE → TX_START →
  TX_SENDING → TX_WAIT_DONE` pilotate de pinul AUX.
- **Fiabilitate prin duplicare**: `loraQueueSendDup()` trimite o copie imediat și a doua
  întârziat, într-un **slot determinist derivat din UNIT_ID** (`UNIT_ID * 700ms + jitter`),
  ca benzile celor 12 unități să nu se suprapună. De aceea **fiecare handler de recepție are
  un filtru de dublaj** — verificare de stare (`status != SEC_CAPTURED`), fereastră de timp
  (≥12s pentru TIME/KILLRESET) sau contor `seq` (CARDPTS). Dacă adaugi un tip de pachet nou,
  **trebuie să-i proiectezi și filtrul de dublaj**.
- **Pattern important — „aplică după ce a plecat pachetul"**: pentru acțiuni de timp și de
  sector, emițătorul pune alerta în coadă, armează un flag (`emitterApplyArmed`,
  `sectorApplyArmed`) și își aplică efectul local abia când `loraTxIdle()` devine true. Așa
  emițătorul și receptoarele văd schimbarea aproximativ simultan.
- **Heartbeat**: la 20-30 min de la ultima transmisie (orice transmisie resetează timerul).

## Carduri RFID (MIFARE Classic)

- Blocul 4 (sectorul 1), cheia custom `A1 B2 C3 D4 E5 F6`: `[magic 0x4E][type][pts_hi][pts_lo][checksum]`.
  `type` 1 = card de puncte, 2 = card de admin. Checksum-ul include UID-ul ⇒ cardul nu se poate clona.
- Cardurile noi se scriu cu cheia de fabrică (`FF...`), apoi trailer-ul sectorului e rescris cu
  cheia custom.
- Un card de puncte se **arde** (blocul se zerorizează) înainte de acordarea punctelor — dacă
  arderea eșuează, punctele NU se dau.
- Cardul de admin: deschide meniul Admin la scanare și **confirmă** acțiunile sensibile
  (start/pauză/resume/reset ceas, reset kill-uri).
- **Export/Import stare**: `buildExportBlob()` serializează întreaga stare de joc în
  `STATE_BLOB_LEN` (297) octeți — protejat cu magic + `STATE_BLOB_VERSION` + CRC16-CCITT — și
  o scrie în sectoarele 2..7 ale unui card de admin. Serveşte la înlocuirea unei unități moarte
  în mijlocul jocului. Timpii se serializează ca **durate scurse**, nu ca `millis()` absolut, și
  se reconstruiesc pe unitatea nouă.
  ⚠️ Dacă schimbi layout-ul blob-ului, **incrementează `STATE_BLOB_VERSION` și recalculează
  `STATE_BLOB_LEN`** (altfel `applyImportBlob` respinge cardul, ceea ce e comportamentul dorit).

## UI

- **6 pagini de joc** (Roșu = înapoi, Albastru = înainte): 1 gameplay (se adaptează după mod),
  2 scoruri, 3 kill-uri, 4 status unități, 5 ping/radar + baterii, 6 info joc + start/pauză.
  Verde are funcții contextuale: scroll pe 4/5, reset kill-uri pe 3, reset ceas pe 6.
- **Admin** (card de admin sau combo Roșu+Albastru 3s): Game Settings, Bomb Parameters, Respawn
  Rules, Sync Units, TAG Writer, Imp./Exp. Data, Change Mode, System Restart, Power Off. Cele
  mai multe sunt **blocate cât timp jocul rulează** → `STATE_ADMIN_BLOCKED`.
- Mașina de stări e un `switch (currentState)` uriaș în `loop()`; `needsDisplayUpdate` evită
  redesenarea inutilă. Auto-dim la 10% luminozitate + revenire la pagina 1 după 30s de
  inactivitate (`resetActivity()`).
- Setările admin trăiesc ca **indici** (`gsTimeLimit`, `bsTimerIdx`, ...) cu tabele de valori
  duplicate în trei locuri: la CONFIRM în `.ino`, în `syncAdminIndices()` și în
  `applySettingsFromIndices()` (lora.cpp). **Dacă schimbi o listă de opțiuni, schimb-o în toate
  trei** — altfel sync-ul și importul vor da alte valori decât UI-ul.

## Convenții de cod

- Comentarii și mesaje seriale în **română fără diacritice**; textele de pe OLED în **engleză**.
- Zero alocare dinamică, zero `String` — buffere fixe și `snprintf`. Ecranul are 21 caractere
  pe linie la `textSize(1)`; textele se centrează manual cu `(SCREEN_WIDTH - strlen(s)*6)/2`.
- Nimic nu blochează în `loop()` în afară de `loraSendSyncBlocking()` și `doReboot()` — restul
  sunt mașini de stări cu `millis()`.
- Comparațiile de timp folosesc diferențe cast-uite la `int32_t` acolo unde valorile pot fi
  „subcurse" după import — păstrează pattern-ul, nu-l simplifica la `a > b`.
- ⚠️ **Indentarea minte în mai multe locuri** (blocuri `if` fără acolade cu linii aliniate
  greșit dedesubt). Citește logica, nu indentarea, când modifici `loop()`.

## Lucruri de știut înainte să modifici

- `FW_VERSION` = `"NU-diag-1"` — se bumpează la fiecare build ca să verifici că toate unitățile
  au același cod. Există și un **dump de diagnostic pe serial la 60s** în `loop()` (blocul
  „DIAG SCOR"), marcat ca temporar.
- Limite de serializare: `savedPoints` se trunchiază la `int16_t` și `kills` la 255 în blob-ul
  de card — scoruri peste 32767 nu supraviețuiesc unui export.
- WiFi și Bluetooth sunt oprite explicit în `setup()` (consum + stabilitate radio).
- Power latch pe GPIO17: unitatea își ține singură alimentarea; „Power Off" pulsează pinul LOW.
- Releul e `OUTPUT_OPEN_DRAIN`, **activ pe LOW**.
