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

## Reguli de operare pe teren (NU sunt bug-uri — sunt procedura)

Codul depinde de ordinea asta; mai multe „ciudatenii" din el se explica doar prin ea.

1. **Intai SYNC, apoi alegerea modurilor.** `PKT_MODE` e ignorat de o unitate
   nesincronizata (`if (!isSynced) return;` in `loraPoll`), deci un mod ales inainte de
   sync nu ajunge nicaieri. Daca s-a intamplat: **Admin Mode → Change Mode** si se alege
   modul din nou, iar unitatea il retransmite.
2. **SYNC transmite doar setarile + `localTime` + `sessionId`** — deliberat. Tabelul cu
   scoruri/moduri **nu** circula prin radio: la SF10 ar tine aerul ocupat prea mult.
   O unitate repornita in mijlocul jocului isi ia setarile prin sync si restul starii
   prin **Export/Import pe card de admin**.
3. **Jocul nu incepe pana cand staff-ul nu confirma ca toate unitatile sunt pornite si
   sincronizate.**
4. **RESET de ceas se da din pauza**, intentionat: pe pauza nicio unitate nu emite, deci
   alerta de reset nu se ciocneste de alt trafic (modulele nu au listen-before-talk si nu
   pot emite si asculta simultan). Foloseste la meciuri rapide/SQB: runda noua cu ceas
   resetat, dar cu scorurile pastrate. **Resetul atinge doar ceasul, nu si starea de joc** —
   sectoarele raman cucerite, bombele raman armate, dar toate cronometrele stau pe loc
   pana la urmatorul START.
5. **Import de pe card = unitatea preia si rolul.** Randul propriu din tabelul de pe card
   spune ce mod avea unitatea in retea si, pentru sector/bomba, daca era cucerita/armata
   si de cine — deci dupa import intra direct in rol. Coada de respawn nu circula pe card,
   deci porneste goala. Daca pe card unitatea n-are niciun rol, se cere selectia de mod.

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
  călătorește gratis în orice pachet. Nivelul se calculează în `updateBattery()` (la fiecare
  10s din `loop()`, în orice stare) — **nu** în `buildContext()`, care rulează doar cât timp
  se desenează paginile de joc: acolo, pachetele trimise din meniu/admin (SYNC, MODE) plecau
  cu bateria 0 și unitatea apărea descărcată pe pagina 5 a celorlalți.
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
  Copia a doua e marcată cu **bitul 7 al octetului 2** (`PKT_DUP_FLAG`) și resigilată; toți
  receptorii maschează deja unit (biți 3:0) și baterie (biți 6:4), deci bitul nu deranjează
  nicio decodare. Cine vede **doar** dublura știe că acțiunea s-a petrecut acum
  `loraEvtAgeMs()` = `UNIT_ID_emițător * 700ms + 100`, și **antidatează** cronometrele:
  `applyCapture`/`applyNeutralize` primesc `ageMs`, iar BOMB_PLANT/BOMB_DEFUSE folosesc
  `backdated()`. Fără asta, o unitate care ratează prima copie pornea tick-ul de 10s mai
  târziu decât sursa și rămânea permanent cu un tick în urmă. Preferă un octet de **secvență**
  (`PKT_CARDPTS`, `PKT_PTSRESET`) în locul unei ferestre de timp: fereastra trebuie să fie
  mai lată decât slotul copiei a doua (~8.4s) și atunci respinge și două acțiuni legitime
  apropiate — exact ce se întâmplă azi la `PKT_TIME_*` și `PKT_KILLRESET`.
- **Pattern important — „aplică după ce a plecat pachetul"**: pentru acțiuni de timp și de
  sector, emițătorul pune alerta în coadă, armează un flag (`emitterApplyArmed`,
  `sectorApplyArmed`) și își aplică efectul local abia când `loraTxIdle()` devine true. Așa
  emițătorul și receptoarele văd schimbarea aproximativ simultan.
- **Heartbeat**: fereastră aleatoare de **10-30 min**, cu două regimuri, pentru că cele două
  încărcături se comportă diferit. Pe o unitate obișnuită **orice transmisie amână
  heartbeat-ul** — corect, fiindcă nivelul bateriei călătorește în byte 2 al oricărui pachet,
  deci o alertă de cucerire l-a dus deja mai departe. Pe **maestrul de timp** scadența se mută
  **doar** când chiar trimite el (`heartbeatReschedule()` iese devreme dacă `isTimeMaster`):
  corecția de ceas (`PKT_TIME_SYNC`) nu circulă în alte pachete, deci traficul celorlalți nu e
  un motiv s-o amâni. Pe regula veche, într-un joc activ corecția era împinsă la nesfârșit și
  nu pleca niciodată. `loraHeartbeatDue()` consumă scadența când întoarce true, deci în `loop()`
  verificarea de pauză trebuie să rămână **înaintea** ei.
- **Ceasul se realiniază și la RESUME**: `PKT_TIME_RESUME` poartă secundele rămase, iar
  receptorul face snap înainte să aplice. Deci orice pauză→resume resincronizează rețeaua.

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
  `buildExportBlob()` avertizează pe serial dacă lungimea scrisă nu e egală cu constanta.
  Blob-ul ocupă blocurile de date din sectoarele 2..11; `EXP_BLOCKS` are rezervă până la
  sectorul 15 (672 B), deci mai poate crește fără schimbări în `rfid.cpp`.

## UI

- **6 pagini de joc** (Roșu = înapoi, Albastru = înainte): 1 gameplay (se adaptează după mod),
  2 scoruri, 3 kill-uri, 4 status unități, 5 ping/radar + baterii, 6 info joc + start/pauză.
  Verde are funcții contextuale: scroll pe 4/5, **eliberare unități pe 1**, **reset scoruri
  pe 2**, reset kill-uri pe 3, reset ceas pe 6. Cele patru resetări cer cardul de admin
  (fereastră de 3s) și trimit alertă în rețea; sunt independente între ele.
  Toate confirmările cu card trec prin `drawAdminTagRequest(action)` din `display.cpp`,
  care afișează pe primul rând ce urmează să se întâmple („Pause game", „Release all
  units", ...) — maxim 21 de caractere, cât încape pe un rând la `textSize(1)`.
  **Fiecare resetare refuză un no-op** cu ton de fail, fără să deschidă fereastra de card:
  scoruri deja pe 0 (`scoresAreZero()`), kill-uri deja pe 0 (`killsAreZero()`), ceas deja la
  valoarea inițială. Motivul e că orice resetare pune radioul la treabă. Eliberarea de pe
  pagina 1 **nu** are un astfel de filtru: cozile de respawn sunt stare locală pe fiecare
  unitate, deci de aici nu se poate ști dacă altcineva mai are jucători în așteptare.
- **Change Mode** distinge două motive de refuz: jocul rulează → „Can't do that while
  playing"; unitatea e liberă de joc dar încă în rol (sector cucerit / bombă plantată /
  coadă nevidă) → ecranul `UNIT BUSY`, care spune exact ce o ține ocupată.
  **Eliberarea unităților** (`PKT_FIELDRESET`) pregătește terenul pentru o rundă nouă:
  sectoarele cucerite devin neutre, bombele armate sau în cooldown revin la IDLE (gata de
  plantat), coada de respawn se golește pe fiecare unitate. **Rolurile rămân** — o unitate de
  bombă rămâne unitate de bombă, una de respawn își păstrează echipa (echipa e rolul ei, nu
  o stare) — iar scorurile și kill-urile nu se ating. Blocată doar cât timp jocul rulează. **Resetul de scoruri e blocat doar cât timp jocul chiar rulează** — merge nepornit,
  pe pauză, la time out și la game over (altfel „Can't do that while playing")
  și atinge doar punctele — `savedPoints`, `liveCapture` și `appliedPenalties` (acestea din
  urmă ca pagina 2 să nu arate scoruri negative); sectoarele rămân cucerite, bombele armate,
  kill-urile neatinse.
- **Admin** (card de admin sau combo Roșu+Albastru 3s): Game Settings, Bomb Parameters, Respawn
  Rules, Sync Units, TAG Writer, Imp./Exp. Data, Change Mode, **Game Reset**, System Restart,
  Power Off. Cele mai multe sunt **blocate cât timp jocul rulează** → `STATE_ADMIN_BLOCKED`.
  **Game Reset** (`PKT_GAMERESET`) repune jocul la starea de dinainte de primul START:
  sectoare neutre, bombe dezamorsate, coadă goală, puncte și kill-uri pe zero, ceasul la
  limită, `isTimeOut`/`conquestWinner` șterse. **Păstrează modurile unităților** (respawn-ul
  își ține echipa), **setările** și **sincronizarea**. E singurul traseu care stinge
  `isTimeOut`, deci singurul mod de a porni o rundă nouă după game over fără reboot.
  Receptoarele pulsează releul 2s (sirena confirmă pe teren că alerta a ajuns); emițătorul
  nu — cine a apăsat știe deja. După ecranul de 2s, emițătorul revine în Admin Mode, iar
  receptoarele în joc (`gameResetReturnState`).
- **Game Reset și System Restart cer confirmare** — `STATE_CONFIRM_ACTION` cu
  `drawConfirmActionScreen(action)`, același tipar ca la Sync Units: ROȘU = No (înapoi în
  Admin Mode), ALBASTRU = Yes. Sunt singurele acțiuni ireversibile din meniu (una șterge
  jocul în rețea, cealaltă repornește toate unitățile și pierde tot ce e în RAM).
- **Paginile de joc cer un rol.** Un invariant din `loop()` mută unitatea din `STATE_PAGES`
  în `STATE_MENU` dacă `selectedMode == -1`. Verificarea e centrală tocmai ca să acopere
  orice traseu, inclusiv unele adăugate ulterior.
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
- **`millis()` nu se oprește niciodată — cronometrele „îngheață" prin compensare.**
  `gameplayRunning()` (`isGameTimerRunning && !isGamePaused && !isTimeOut`) spune dacă
  sectorul/bomba/respawn-ul au voie să avanseze. Un singur **detector de front** în `loop()`
  reține `gameFreezeStart` când jocul se oprește și, la START/RESUME, împinge înainte cu
  exact acel interval toate reperele de gameplay (`shiftGameplayTimers()`). Prinde orice
  traseu — buton, alertă radio, import — deci **nu adăuga apeluri de dezghețare pe trasee
  noi**; e de ajuns ca `gameplayRunning()` să reflecte realitatea.
- **Pentru desen folosește `ctx.timeRef`, nu `millis()`.** `gameTimeRef()` întoarce
  `millis()` cât timp jocul rulează și momentul înghețării în rest, deci
  `timeRef - actionTime` rămâne fix cu jocul oprit. `pauseStartTime` a rămas doar pentru
  lucrurile legate de radio (`lastSeenTime[]`, fereastra de 15s de la Exp./Imp.).
- `unfreezeAfterPause()` acoperă acum **doar** partea de pauză (ceasul de joc și reperele
  radio); cronometrele de gameplay sunt treaba detectorului de front.

## Lucruri de știut înainte să modifici

- `FW_VERSION` se bumpează la fiecare build și se afișează pe serial la boot. **Toate cele 12
  unități trebuie să ruleze aceeași versiune** — formatul pachetelor LoRa și al blob-ului de
  card diferă între versiuni, iar o unitate rămasă în urmă fie respinge cardul, fie
  interpretează greșit pachetele.
- WiFi și Bluetooth sunt oprite explicit în `setup()` (consum + stabilitate radio).
- Power latch pe GPIO17: unitatea își ține singură alimentarea; „Power Off" pulsează pinul LOW.
- Releul e `OUTPUT_OPEN_DRAIN`, **activ pe LOW**.

- # NOVA Units — ghid de proiect (CLAUDE.md)

Sistem electronic de management al jocurilor de airsoft, construit pe ESP32.
Fiecare unitate e o „cutie" autonomă (Pelican-style) care poate fi sector de
capturat, bombă sau punct de respawn. Unitățile comunică între ele prin LoRa
(433 MHz), fără infrastructură centrală — fiecare unitate ține o copie a stării
întregii rețele. Proiect personal, cod scris de la zero.

- **Repo:** https://github.com/mrsilviuro/NovaUnits (branch `main`)
- **Platformă:** ESP32 + Arduino / C++
- **Unități fizice de test:** ID 1, 2, 4 (din maxim `MAX_UNITS = 12`)

---

## Convenții de cod (OBLIGATORII)

- **Comentarii în română, FĂRĂ diacritice.** Tot codul nou respectă asta.
- **RAM-only.** Nu se scrie în flash/EEPROM în timpul jocului (uzura flash-ului).
- **Fără alocare dinamică, fără `String`.** Doar buffere statice, tipuri fixe.
- **Non-blocking peste tot.** Mașini de stare, nu `delay()`. Excepție unică
  istorică: trimiterea blocantă de SYNC (`loraSendSyncBlocking`), care îngheață
  intenționat ceasul local cât ține transmisia.
- **WiFi + Bluetooth dezactivate în `setup()`** (`esp_wifi_stop()`,
  `esp_bt_controller_disable()`) — economie de energie + curățenie RF.
- Preferă tipuri explicite (`uint8_t`, `int32_t`) și verifică `sizeof` la orice
  `memcpy`.

---

## Structura fișierelor

| Fișier          | Rol |
|-----------------|-----|
| `NovaUnits.ino` | Loop principal, mașina de stare a UI, handlerele de butoane, evenimentele LoRa consumate, baterie, export/import blob, globale. |
| `config.h`      | Constante hardware (pini), `MAX_UNITS`, coduri de pachet `PKT_*`, enum de stări `STATE_*`, nume echipe/unități, versiune+lungime blob. |
| `game.h/.cpp`   | Modelul de joc: `UnitRow`, `unitTable`, `teamScore`, `buildContext`, ecrane utilitare (`drawBlockedScreen`, etc.), globale de stare (`isGamePaused`, `savedPoints`...). |
| `lora.h/.cpp`   | Protocolul LoRa: build + parse pachete, `sealPacket`, coada de TX (`loraQueueSend`/`Dup`), heartbeat, sesiune. |
| `display.h/.cpp`| Randarea paginilor (SSD1309), iconițe, layout. |
| `buttons.h/.cpp`| Debounce + short/long press. |
| `rfid.h/.cpp`   | PN532 (SPI): admin tag, carduri de puncte, export/import stare pe card. |

---

## Model de bază (stare de joc)

```c
struct UnitRow { uint8_t mode; uint8_t status; Team team;
                 uint32_t actionTime; int32_t savedPoints[4]; uint16_t kills[4]; };
UnitRow unitTable[MAX_UNITS];        // starea INTREGII retele, replicata local
#define myRow() unitTable[UNIT_ID-1] // randul acestei unitati
```

- `mode`: 0 = none, 1 = sector, 2 = bomba, 3 = respawn.
  (Atentie: `selectedMode` din meniu e 0/1/2 → se mapeaza la `mode` 1/2/3.)
- `status`: sector → `SEC_NEUTRAL=0` / `SEC_CAPTURED=1`; bomba → `BOMB_IDLE=0` /
  `BOMB_ARMED=1` / `BOMB_COOLDOWN=2`.
- `Team`: `TEAM_NEUTRAL=0`, apoi echipele 1-4 (`Phantoms`, `Sentinels`,
  `Falcons`, `Nemesis`).

Globale importante (în `NovaUnits.ino` / `game.cpp`): `savedPoints` (in `UnitRow`),
`appliedPenalties[4]`, `liveCapture[12]`, `lastPointTick[12]`, `lastSeenTime[12]`
(uint32), `globalBattery[12]` (uint8, 0-4 bare), `cardSeq[12]` (dedup puncte-card),
`sessionId`, `batteryPercent` (0-100).

---

## Scorare

- `teamScore(t)` = **suma** `savedPoints[t]` din toate rândurile `unitTable`
  (+ captura live locală). `liveScore[t]` e **derivat** din `teamScore` în
  `buildContext` — nu se stochează separat.
- **Nu există reconciliere periodică de scor.** Scorurile se propagă EXCLUSIV
  prin pachetele de eveniment care actualizează `unitTable` (CAPTURE, NEUTRALIZE,
  BOMB, RESPAWN, KILLRESET, CARDPTS). Dacă un eveniment nu e difuzat, ceilalți
  nu-l văd niciodată.
- **Fiecare unitate face „tick" local pe TOATE sectoarele capturate** (nu doar
  ale ei). Deci `savedPoints` pentru o unitate ar trebui sa fie identic pe toate
  device-urile, cu conditia ca toate evenimentele non-deterministe (capturi,
  neutralizari, puncte de card) sa fie difuzate.
- Afișaj (pagina 2): `liveScore - appliedPenalties` (poate fi negativ, `%ld`).

---

## Timer & Pauză

- `gameTimeLeftSeconds` numărat în jos, gated pe `!isGamePaused && !isTimeOut`.
- **Disciplina timerului:** `lastTimerTick += 1000`, NICIODATĂ `= now`
  (altfel drift/drenaj rapid pe tick învechit).
- `gameActive = isGameTimerRunning && !isGamePaused && !isTimeOut` — „jocul chiar
  rulează".
- `applyGamePause()` doar setează flag-ul + `pauseStartTime` (NU decalează timpi).
- `applyGameResume()` decalează TOȚI timpii absoluți cu `pauseDuration`
  (`lastTimerTick`, `actionTime`, `lastPointTick`, `respawnQueue`, `lastSeenTime`).
- RESUME poartă `gameTimeLeftSeconds` → receptoarele fac **snap** la valoare
  (corectează drift-ul acumulat în fereastra de propagare a pauzei).
- Pe pauză: `lastSeenTime` se scrie cu `pauseStartTime` (nu `millis()`), iar
  paginile 4/5 afișează timpul scurs față de `pauseStartTime` → îngheață.
  Heartbeat-ul se amână (nu se trimite pe pauză).

---

## Protocol LoRa

Modul **DX-LR03-433T30D** (UART, 9600 baud spre modul), 27 dBm, ~435.8 MHz.
Modulele **NU au LBT** (fără carrier sense). Antene: sleeve dipol (fără plan de masă).

### Structura pachetului
```
[NET][TYPE][UNIT][...payload...][SES][CRC]
```
- `NET` = `NETWORK_ID` (marker fix de flux, 1 octet). Nu-l varia.
- `UNIT` = `unitByte()` = `(bare_baterie<<4) | UNIT_ID`. **Bateria calatoreste in
  fiecare alerta** aici; se citeste la RX cu `(rxBuf[2]>>4)&0x07`.
- `SES` = `sessionId` (1 octet) — filtru de retea (vezi mai jos).
- `CRC` = XOR pe 1 octet peste tot restul, prin `sealPacket()`.

`sealPacket(buf, len)` scrie `SES` la `[len-2]` + CRC la `[len-1]`. Orice funcție
de build trebuie s-o folosească (nu scrie CRC de mână). Offset-urile payload-ului
NU se schimbă când adaugi/scoți câmpuri — `SES` și `CRC` stau mereu la coadă.

### Sesiune (izolarea rețelei)
- Maestrul generează `sessionId = random(1,256)` la primul sync (stabil apoi).
- La RX, după CRC: pachetele cu altă sesiune sunt **ignorate**, cu excepția
  SYNC (care poartă sesiunea de adoptat). SYNC acceptat → `sessionId = rxBuf[len-2]`.
- SYNC primit e respins dacă jocul local rulează (`gameActive`) — protejează
  împotriva unui sync accidental de pe o unitate proaspătă.

### Fiabilitate (dublaj + sloturi)
- Majoritatea evenimentelor se trimit **dublat** (`loraQueueSendDup`): o copie
  imediat + a doua în slotul unității: `now + UNIT_ID*TX_SLOT_MS + random(0,TX_SLOT_RAND)`
  (`TX_SLOT_MS=700`, `TX_SLOT_RAND=200`). Sloturile per-unitate nu se suprapun →
  copiile de recuperare nu se ciocnesc între ele.
- Trimise **simplu**: SYNC (blocant), TIME_SYNC (valoare proaspătă), HEARTBEAT-ping.
- Heartbeat automat (keep-alive / time-sync) la interval random 20-30 min
  (`HB_MIN_MS`/`HB_MAX_MS`). Orice transmisie reprogramează heartbeat-ul.
- Evenimentele aditive au **dedup** la copia dublă: RESPAWN/KILLRESET prin
  cumulativ/fereastră de timp; CARDPTS prin `seq` per-unitate.

### Pachete (cod tip / lungime cu sesiune)
`PKT_SYNC 0x01`(14) · `PKT_RESTART 0x02`(5) · `PKT_MODE 0x03`(7) ·
`PKT_TIME_START..RESET 0x04-0x07`(5, RESUME 7) · `PKT_CAPTURE 0x08`(6) ·
`PKT_NEUTRALIZE 0x09`(8) · `PKT_RESPAWN 0x0A`(8) · `PKT_BOMB_PLANT 0x0B`(6) ·
`PKT_BOMB_DEFUSE 0x0C`(6) · `PKT_KILLRESET 0x0D`(8) · `PKT_HEARTBEAT 0x0E`(5) ·
`PKT_TIME_SYNC 0x0F`(7) · `PKT_CARDPTS 0x10`(9).

`rxBuf[16]` — pachetul maxim (SYNC=14) trebuie să încapă aici. La RX, `rxLen` se
alege după `PKT_*`; nu hardcoda lungimi (folosește macro-urile `*_PKT_LEN`).

---

## Export / Import stare pe card RFID

Alternativă la SYNC pentru a aduce o unitate la curent MID-game (transferă starea
completă: setări, timer, scoruri, kills, `lastSeenTime`, baterii, `sessionId`).

- **Versiune curentă:** `STATE_BLOB_VERSION 0x04`, `STATE_BLOB_LEN 297`.
- Buffer `stateBlob[336]` — trebuie **>= `STATE_BLOB_LEN`** (overflow-ul a fost
  un bug clasic; bump-uiește-l odată cu formatul).
- `EXP_BLOCKS` (rfid.cpp) = 21 blocuri de date (sectoare 2-8) = 336 octeti capacitate.
- Layout: magic, versiune, setări(14), timer, flags, conquestWinner, timerPhase,
  tabel 12×19, penalizări(8), `lastSeen[12]` uint16, baterii[12] uint8, `sessionId`, CRC16.
- **Bateria proprie NU se ia de pe card** (o măsoară unitatea singură) — importul
  sare peste `UNIT_ID-1` la `globalBattery`.
- **Schimbarea formatului → bump versiune → re-export toate cardurile.**
  Cardurile vechi dau „Import FAILED" (respins curat).

---

## RFID / OLED / Baterie — capcane cunoscute

- **RFID:** Elechouse PN532 V3 (SPI, `Adafruit_PN532`). IRQ/RSTO neconectate.
  Admin tag = bloc 4, magic + type==2, cheie custom `{A1B2C3D4E5F6}`; celelalte
  sectoare cu cheie factory `{FF×6}`. (Lotul vechi MFRC522 era defect — migrat.)
- **OLED SSD1309** (I2C 0x3C, 128×64): necesită `SSD1306_EXTERNALVCC`. Fix drift
  imagine: `Wire.setClock(100000)` după `begin()` + refresh registri periodic.
- **Baterie:** `updateBattery()` pe GPIO36, divizor 10k/3.3k, la 10s, smoothing
  80/20. `batteryPercent` (0-100) → bare 0-4 → `globalBattery[UNIT_ID-1]`
  (setat ACUM în `updateBattery`, ca orice alertă să ducă valoarea curentă,
  independent de afișaj).

---

## Workflow de livrare (IMPORTANT)

1. Modificările se aplică chirurgical (blocuri exacte GĂSEȘTE / ÎNLOCUIEȘTE),
   nu descrieri vagi de tip „în blocul de heartbeat".
2. După orice modificare: verifică **echilibrul acoladelor** per fișier și
   referințele încrucișate (grep) înainte de a declara gata.
3. Iterativ, feature cu feature; se confirmă că merge înainte de a continua.
4. **Schimbare de wire-format** (lungime pachet / câmp nou / sesiune) →
   **clean build + reflash la TOATE unitățile simultan** + re-sync. O unitate pe
   format vechi nu mai comunică cu cele noi.
5. **Schimbare de blob-format** → re-export toate cardurile.
6. Push pe GitHub după ce starea e confirmată funcțională.

---

## Reguli de aur (bug-uri deja plătite)

- `lastTimerTick += 1000`, niciodată `= now`.
- Verifică `sizeof` destinație la orice `memcpy` (un `memcpy` de 400B într-un
  destinatar de 4B a corupt globale ore la rând).
- `stateBlob` >= `STATE_BLOB_LEN` mereu; crește-le împreună.
- Reconstrucția de timpi pe **unitate proaspătă** trebuie să tolereze underflow
  `uint32` (`millis()` mic), iar gardienii de consum să fie wraparound-safe
  (diferență cu semn, NU `(a>b)?a-b:0`).
- Antene „433 MHz" din comerț aveau elementul greșit (~1500 MHz real) → rază
  ~490 m. Sleeve dipol e corect pentru cutii de plastic.
- Activează **CRC hardware pe modul, uniform pe toate unitățile** (altfel unele
  așteaptă un câmp pe care altele nu-l trimit → nu comunică).
