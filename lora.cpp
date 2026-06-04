// ============================================================
// lora.cpp — Strat transport LoRa (DX-LR03) + ceas comun
// ============================================================
#include "lora.h"
#include "game.h"   // valorile model (gameTimeLimitSeconds, bombTimerMs, ...)

HardwareSerial LoRaSerial(1);   // UART1

// --- Ceas + stare retea ---
uint32_t localTime       = 0;
uint32_t lastLocalTick   = 0;
bool     localTimePaused = false;
bool     isSynced        = false;
uint8_t  syncedByUnit    = 0;

// Pachet SYNC: [NET][TYPE][UNIT][lt2][lt1][lt0][set x6][CRC] = 13 octeti
#define SYNC_PKT_LEN 13
#define RESTART_PKT_LEN 4   // [NET][TYPE][UNIT][CRC]
#define MODE_PKT_LEN    6   // [NET][TYPE][UNIT][mode][team][CRC]

// ============================================================
// Bit-cursor: scriem/citim cate nbits, MSB-first, pe un buffer
// ============================================================
static void putBits(uint8_t* buf, uint16_t& bitpos, uint32_t val, uint8_t nbits) {
    for (int8_t b = nbits - 1; b >= 0; b--) {
        uint16_t idx = bitpos >> 3;
        uint8_t  off = 7 - (bitpos & 7);
        if ((val >> b) & 1) buf[idx] |= (1 << off);
        else                buf[idx] &= ~(1 << off);
        bitpos++;
    }
}
static uint32_t getBits(const uint8_t* buf, uint16_t& bitpos, uint8_t nbits) {
    uint32_t val = 0;
    for (uint8_t b = 0; b < nbits; b++) {
        uint16_t idx = bitpos >> 3;
        uint8_t  off = 7 - (bitpos & 7);
        val = (val << 1) | ((buf[idx] >> off) & 1);
        bitpos++;
    }
    return val;
}

// ============================================================
// Coada TX non-blocant — alertele se trimit in background
// ============================================================
#define TX_QUEUE_SIZE 6
#define TX_MAX_LEN    16
static uint8_t  txQueue[TX_QUEUE_SIZE][TX_MAX_LEN];
static uint8_t  txQueueLen[TX_QUEUE_SIZE];
static uint8_t  txHead = 0, txTail = 0, txCount = 0;
enum TxState { TX_IDLE, TX_START, TX_SENDING, TX_WAIT_DONE };
static TxState  txState = TX_IDLE;
static uint32_t txTimer = 0;

static void loraQueueSend(const uint8_t* buf, uint8_t len) {
    if (len > TX_MAX_LEN || txCount >= TX_QUEUE_SIZE) return;
    memcpy(txQueue[txTail], buf, len);
    txQueueLen[txTail] = len;
    txTail = (txTail + 1) % TX_QUEUE_SIZE;
    txCount++;
}

// nivelul bateriei locale impachetat cu UNIT_ID: bits[6:4]=baterie(0-4), bits[3:0]=unit
static uint8_t unitByte() {
    return ((globalBattery[UNIT_ID - 1] & 0x07) << 4) | (UNIT_ID & 0x0F);
}

void loraTxUpdate() {
    switch (txState) {
        case TX_IDLE:
            if (txCount > 0) { txState = TX_START; txTimer = millis(); }
            break;
        case TX_START:
            if (digitalRead(PIN_LORA_AUX) == LOW) {
                LoRaSerial.write(txQueue[txHead], txQueueLen[txHead]);
                txTimer = millis();
                txState = TX_SENDING;
            } else if (millis() - txTimer > 3000) {   // modulul nu se elibereaza -> renuntam
                txHead = (txHead + 1) % TX_QUEUE_SIZE; txCount--; txState = TX_IDLE;
            }
            break;
        case TX_SENDING:
            if (digitalRead(PIN_LORA_AUX) == HIGH || millis() - txTimer > 200) {
                txTimer = millis(); txState = TX_WAIT_DONE;
            }
            break;
        case TX_WAIT_DONE:
            if (digitalRead(PIN_LORA_AUX) == LOW || millis() - txTimer > 2000) {
                lastSeenTime[UNIT_ID - 1] = millis();   // am transmis -> "ultimul semnal" local (pag.5)
                txHead = (txHead + 1) % TX_QUEUE_SIZE; txCount--; txState = TX_IDLE;
            }
            break;
    }
}

// ============================================================
// applySettingsFromIndices() — converteste indecsii in valori
// (aceleasi tabele de optiuni ca la "CONFIRM" din admin)
// ============================================================
static void applySettingsFromIndices() {
    const uint32_t tl[] = {0, 900, 1800, 3600, 7200, 10800, 14400, 18000, 21600, 25200, 28800, 32400, 36000, 39600, 43200, 86400};
    const uint16_t bn[] = {0, 15, 30, 60, 120, 180, 240};
    const uint32_t at[] = {5000, 10000, 15000, 20000};
    const uint32_t tv[] = {5, 10, 15, 20, 30, 45, 60, 120};
    const uint32_t pv[] = {50, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1500, 2000, 2500, 3000};
    const uint32_t ts[] = {10, 30, 60, 120, 180, 240, 300, 600, 900, 1200, 1500, 1800};
    const uint16_t pp[] = {0, 5, 10, 25, 50, 75, 100};
    const uint16_t lm[] = {0, 10, 25, 50, 75, 100, 200, 300, 400, 500, 1000};

    gameTimeLimitSeconds = tl[gsTimeLimit];
    bonusIntervalMinutes = bn[gsBonus];
    currentWinCondition  = (WinCondition)gsWinCond;
    actionTimeMs         = at[gsActionIdx];
    bombTimerMs          = tv[bsTimerIdx] * 60000UL;
    cooldownMs           = tv[bsCooldownIdx] * 60000UL;
    bombPointsExplode    = pv[bsExpPtsIdx];
    bombPointsDefuse     = pv[bsDefPtsIdx];
    respawnTimeMs        = ts[rsTimeIdx] * 1000UL;
    respawnPenaltyPoints = pp[rsPenaltyIdx];
    for (uint8_t i = 0; i < 4; i++) teamMaxRespawns[i] = lm[rsLimitIdx[i]];

    // ceasul live preia limita doar daca jocul nu e in desfasurare
    if (!isGameTimerRunning && !isGamePaused && !isTimeOut)
        gameTimeLeftSeconds = gameTimeLimitSeconds;
}

// ============================================================
// loraInit()
// ============================================================
void loraInit() {
    pinMode(PIN_LORA_AUX, INPUT);
    LoRaSerial.begin(LORA_BAUD_RATE, SERIAL_8N1, PIN_LORA_RX, PIN_LORA_TX);
    localTime     = 0;
    lastLocalTick = millis();
    Serial.println("[LORA] Init UART.");
}

// ============================================================
// loraTick() — +1 secunda la localTime (daca nu e pe pauza)
// ============================================================
void loraTick() {
    if (localTimePaused) return;
    uint32_t now = millis();
    if (now - lastLocalTick >= 1000) {
        localTime++;
        lastLocalTick += 1000;
    }
}

// ============================================================
// buildSyncPacket()
// ============================================================
static void buildSyncPacket(uint8_t* buf) {
    buf[0] = (uint8_t)NETWORK_ID;
    buf[1] = PKT_SYNC;
    buf[2] = UNIT_ID;
    buf[3] = (localTime >> 16) & 0xFF;
    buf[4] = (localTime >> 8) & 0xFF;
    buf[5] =  localTime        & 0xFF;

    for (uint8_t i = 6; i < 12; i++) buf[i] = 0;
    uint16_t bit = 0;
    putBits(&buf[6], bit, gsTimeLimit,   4);
    putBits(&buf[6], bit, gsWinCond,     2);
    putBits(&buf[6], bit, gsActionIdx,   2);
    putBits(&buf[6], bit, gsBonus,       3);
    putBits(&buf[6], bit, bsTimerIdx,    3);
    putBits(&buf[6], bit, bsCooldownIdx, 3);
    putBits(&buf[6], bit, bsExpPtsIdx,   4);
    putBits(&buf[6], bit, bsDefPtsIdx,   4);
    putBits(&buf[6], bit, rsTimeIdx,     4);
    putBits(&buf[6], bit, rsPenaltyIdx,  3);
    putBits(&buf[6], bit, rsLimitIdx[0], 4);
    putBits(&buf[6], bit, rsLimitIdx[1], 4);
    putBits(&buf[6], bit, rsLimitIdx[2], 4);
    putBits(&buf[6], bit, rsLimitIdx[3], 4);

    uint8_t cs = 0;
    for (uint8_t i = 0; i < SYNC_PKT_LEN - 1; i++) cs ^= buf[i];
    buf[SYNC_PKT_LEN - 1] = cs;
}

// ============================================================
// loraSendSyncBlocking() — pauza ceas -> trimite -> AUX LOW -> reia
// ============================================================
void loraSendSyncBlocking() {
    // golim coada TX inainte (timp curat pentru sincronizare)
    uint32_t t0 = millis();
    while ((txCount > 0 || txState != TX_IDLE) && millis() - t0 < 3000) loraTxUpdate();

    localTimePaused = true;             // inghetam ceasul cat dureaza transmisia
    uint8_t txBuf[SYNC_PKT_LEN];
    buildSyncPacket(txBuf);

    uint32_t t = millis();
    while (digitalRead(PIN_LORA_AUX) == HIGH && millis() - t < 2000) { }  // aer liber (AUX LOW)
    LoRaSerial.write(txBuf, SYNC_PKT_LEN);
    LoRaSerial.flush();
    t = millis();
    while (digitalRead(PIN_LORA_AUX) == LOW  && millis() - t < 200)  { }  // inceput transmisie (AUX HIGH)
    t = millis();
    while (digitalRead(PIN_LORA_AUX) == HIGH && millis() - t < 2000) { }  // final (trailing edge: AUX LOW)

    localTimePaused = false;            // livrat -> reluam numaratoarea
    lastLocalTick   = millis();
    isSynced        = true;
    lastSeenTime[UNIT_ID - 1] = millis();   // am transmis -> "ultimul semnal" local (pag.5)
    Serial.print("[LORA] SYNC trimis @localTime=");
    Serial.println(localTime);
}

// ============================================================
// loraSendRestartBlocking() — anunta reteaua, apoi unitatea da reboot
// ============================================================
void loraSendRestart() {
    if (!isSynced) return;   // OFFLINE: nu transmitem alerte
    uint8_t buf[RESTART_PKT_LEN];
    buf[0] = (uint8_t)NETWORK_ID;
    buf[1] = PKT_RESTART;
    buf[2] = unitByte();
    uint8_t cs = 0;
    for (uint8_t i = 0; i < RESTART_PKT_LEN - 1; i++) cs ^= buf[i];
    buf[RESTART_PKT_LEN - 1] = cs;
    loraQueueSend(buf, RESTART_PKT_LEN);
    loraQueueSend(buf, RESTART_PKT_LEN);   // de 2x pentru siguranta
    Serial.println("[LORA] RESTART pus in coada.");
}

// ============================================================
// loraSendModeBlocking() — anunta modul ales (sau 0 = UNKNOWN la change mode)
// ============================================================
void loraSendMode(uint8_t mode, uint8_t team) {
    if (!isSynced) return;   // OFFLINE: nu transmitem alerte
    uint8_t buf[MODE_PKT_LEN];
    buf[0] = (uint8_t)NETWORK_ID;
    buf[1] = PKT_MODE;
    buf[2] = unitByte();
    buf[3] = mode;
    buf[4] = team;
    uint8_t cs = 0;
    for (uint8_t i = 0; i < MODE_PKT_LEN - 1; i++) cs ^= buf[i];
    buf[MODE_PKT_LEN - 1] = cs;
    loraQueueSend(buf, MODE_PKT_LEN);
    loraQueueSend(buf, MODE_PKT_LEN);
    Serial.print("[LORA] MODE pus in coada: mode="); Serial.print(mode);
    Serial.print(" team="); Serial.println(team);
}

// ============================================================
// loraPoll() — citeste UART; intoarce evenimentul primit
// ============================================================
LoraEvent loraPoll() {
    static uint8_t  rxBuf[16];
    static uint8_t  rxCount = 0;
    static uint8_t  rxLen   = 0;   // lungime asteptata (0 = inca necunoscuta)
    static uint32_t rxStart = 0;
    uint32_t now = millis();

    // aruncam bytes pana la markerul NETWORK_ID
    while (LoRaSerial.available() && rxCount == 0 &&
        (uint8_t)LoRaSerial.peek() != (uint8_t)NETWORK_ID)
        LoRaSerial.read();

    while (LoRaSerial.available() && rxCount < sizeof(rxBuf) &&
        (rxLen == 0 || rxCount < rxLen)) {
        rxBuf[rxCount++] = LoRaSerial.read();
    if (rxCount == 1) rxStart = now;
    if (rxCount == 2) {            // tipul e cunoscut -> stim lungimea
        if      (rxBuf[1] == PKT_SYNC)    rxLen = SYNC_PKT_LEN;
        else if (rxBuf[1] == PKT_RESTART) rxLen = RESTART_PKT_LEN;
        else if (rxBuf[1] == PKT_MODE)    rxLen = MODE_PKT_LEN;
        else { rxCount = 0; rxLen = 0; break; }   // tip necunoscut -> resync
    }
        }

        if (rxCount > 0 && (rxLen == 0 || rxCount < rxLen) && now - rxStart > 500) { rxCount = 0; rxLen = 0; return LORA_EVT_NONE; }
        if (rxLen == 0 || rxCount < rxLen) return LORA_EVT_NONE;

        uint8_t type = rxBuf[1];
    uint8_t len  = rxLen;
    rxCount = 0; rxLen = 0;            // pachet complet — il consumam

    if (rxBuf[0] != (uint8_t)NETWORK_ID) return LORA_EVT_NONE;
    uint8_t cs = 0;
    for (uint8_t i = 0; i < len - 1; i++) cs ^= rxBuf[i];
    if (cs != rxBuf[len - 1]) return LORA_EVT_NONE;

    // --- RESTART ---
    if (type == PKT_RESTART) {
        // RESTART vine doar de la o unitate SINCRONIZATA (filtrul e la emisie);
        // toate unitatile, inclusiv cele nesincronizate, dau reboot.
        Serial.print("[LORA] RESTART primit de la unit "); Serial.println(rxBuf[2] & 0x0F);
        return LORA_EVT_RESTART;
    }

    // --- MODE (select / change mode) ---
    if (type == PKT_MODE) {
        if (!isSynced) return LORA_EVT_NONE;   // OFFLINE ignora alertele de joc
        uint8_t u = rxBuf[2] & 0x0F;
        uint8_t batt = (rxBuf[2] >> 4) & 0x07;
        uint8_t mode = rxBuf[3], team = rxBuf[4];
        if (u >= 1 && u <= MAX_UNITS && u != UNIT_ID) {
            unitTable[u - 1].mode       = mode;
            unitTable[u - 1].status     = 0;            // fara actiune inca (neutral/idle)
            unitTable[u - 1].team       = (Team)team;   // doar respawn trimite echipa la acest pas
            unitTable[u - 1].actionTime = 0;
            globalBattery[u - 1] = batt;
            lastSeenTime[u - 1] = millis();
            Serial.print("[LORA] MODE de la unit "); Serial.print(u);
            Serial.print(" -> mode="); Serial.print(mode);
            Serial.print(" team="); Serial.println(team);
        }
        return LORA_EVT_NONE;
    }

    // --- SYNC ---
    uint8_t  fromUnit = rxBuf[2];
    uint32_t lt = ((uint32_t)rxBuf[3] << 16) | ((uint32_t)rxBuf[4] << 8) | rxBuf[5];

    uint16_t bit = 0;
    gsTimeLimit   = getBits(&rxBuf[6], bit, 4);
    gsWinCond     = getBits(&rxBuf[6], bit, 2);
    gsActionIdx   = getBits(&rxBuf[6], bit, 2);
    gsBonus       = getBits(&rxBuf[6], bit, 3);
    bsTimerIdx    = getBits(&rxBuf[6], bit, 3);
    bsCooldownIdx = getBits(&rxBuf[6], bit, 3);
    bsExpPtsIdx   = getBits(&rxBuf[6], bit, 4);
    bsDefPtsIdx   = getBits(&rxBuf[6], bit, 4);
    rsTimeIdx     = getBits(&rxBuf[6], bit, 4);
    rsPenaltyIdx  = getBits(&rxBuf[6], bit, 3);
    rsLimitIdx[0] = getBits(&rxBuf[6], bit, 4);
    rsLimitIdx[1] = getBits(&rxBuf[6], bit, 4);
    rsLimitIdx[2] = getBits(&rxBuf[6], bit, 4);
    rsLimitIdx[3] = getBits(&rxBuf[6], bit, 4);

    applySettingsFromIndices();
    localTime       = lt;             // sincronizam ceasul 1:1
    lastLocalTick   = now;            // numaram imediat de la valoarea primita
    localTimePaused = false;
    isSynced        = true;
    syncedByUnit    = fromUnit;
    if (fromUnit >= 1 && fromUnit <= MAX_UNITS) lastSeenTime[fromUnit - 1] = now;
    Serial.print("[LORA] SYNC primit de la unit ");
    Serial.print(", localTime="); Serial.println(lt);
    return LORA_EVT_SYNC;
}
