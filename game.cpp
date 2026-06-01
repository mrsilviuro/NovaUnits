#include "game.h"
#include <string.h>

// ============================================================
// TABELUL + valori initiale (totul in RAM, zero flash)
// ============================================================
UnitRow unitTable[MAX_UNITS] = {};
UnitRow& myRow() { return unitTable[UNIT_ID - 1]; }

uint32_t liveCapturePoints = 0;
uint32_t lastPointTick     = 0;

int8_t  selectedMode        = -1;
int32_t appliedPenalties[4] = {0, 0, 0, 0};

// Setari Bomba
uint32_t bombTimerMs       = 15 * 60000UL;
uint32_t cooldownMs        = 20 * 60000UL;
uint32_t bombPointsExplode = 600;
uint32_t bombPointsDefuse  = 300;

// Setari Respawn + coada
uint32_t respawnTimeMs        = 300000;
uint16_t respawnPenaltyPoints = 25;
uint16_t teamMaxRespawns[4]   = {0, 0, 0, 0};
uint8_t  queueCount = 0, queueHead = 0, queueTail = 0;
uint32_t respawnQueue[100]    = {0};

// Flux joc
uint32_t     gameTimeLeftSeconds = 0;
bool         isGameTimerRunning  = false;
bool         isTimeOut           = false;
Team         conquestWinner      = TEAM_NEUTRAL;
WinCondition currentWinCondition = WIN_BY_POINTS;
uint32_t     bonusIntervalMinutes= 30;
uint32_t     actionTimeMs        = 15000;
bool         isGamePaused        = false;
uint32_t     pauseStartTime      = 0;

// Comunicatii
uint8_t  globalBattery[MAX_UNITS] = {0};
uint32_t lastSeenTime[MAX_UNITS]  = {0};

// ============================================================
// Scor prin INSUMARE peste tabel
// ============================================================
int32_t teamScore(uint8_t team) {
    int32_t total = 0;
    for (uint8_t u = 0; u < MAX_UNITS; u++) total += unitTable[u].savedPoints[team];
    // + acumularea LIVE a sectorului local detinut acum de aceasta echipa
    if (myRow().mode == 1 && myRow().status == SEC_CAPTURED &&
        myRow().team == (Team)(team + 1))
        total += (int32_t)liveCapturePoints;
    return total;
}

uint16_t teamKillTotal(uint8_t team) {
    uint32_t total = 0;
    for (uint8_t u = 0; u < MAX_UNITS; u++) total += unitTable[u].kills[team];
    return (total > 65535) ? 65535 : (uint16_t)total;
}

// ============================================================
// buildContext() — totul din tabel
// ============================================================
void buildContext(PageContext& ctx, uint8_t currentPage, uint8_t batteryPercent,
                  uint8_t page4Scroll, uint8_t page5Scroll) {
    UnitRow& r = myRow();

    ctx.batteryPercent     = batteryPercent;
    ctx.currentPage        = currentPage;
    ctx.selectedMode       = selectedMode;
    ctx.isTimeOut          = isTimeOut;
    ctx.conquestWinner     = conquestWinner;
    ctx.winCondition       = currentWinCondition;
    ctx.isGameTimerRunning = isGameTimerRunning;
    ctx.gameTimeLeftSeconds= gameTimeLeftSeconds;
    ctx.isGamePaused       = isGamePaused;
    ctx.pauseStartTime     = pauseStartTime;

    // Sector (din randul propriu)
    ctx.sectorOwner          = (r.mode == 1 && r.status == SEC_CAPTURED) ? r.team : TEAM_NEUTRAL;
    ctx.captureStartTime     = r.actionTime;
    ctx.currentCapturePoints = liveCapturePoints;
    ctx.bonusIntervalMinutes = bonusIntervalMinutes;

    // Bomb (din randul propriu)
    ctx.isBombArmed      = (r.mode == 2 && r.status == BOMB_ARMED);
    ctx.isCooldownActive = (r.mode == 2 && r.status == BOMB_COOLDOWN);
    ctx.bombPlantTime    = r.actionTime;
    ctx.bombTimerMs      = bombTimerMs;
    ctx.cooldownStartTime= r.actionTime;
    ctx.cooldownMs       = cooldownMs;

    // Respawn (din randul propriu)
    ctx.respawnTeam          = (r.mode == 3) ? r.team : TEAM_NEUTRAL;
    ctx.queueCount           = queueCount;
    ctx.queueHead            = queueHead;
    ctx.respawnPenaltyPoints = respawnPenaltyPoints;
    ctx.respawnQueue[0]      = respawnQueue[queueHead];
    memcpy(ctx.teamMaxRespawns, teamMaxRespawns, sizeof(teamMaxRespawns));

    // Scoruri si kill-uri (INSUMARE peste tabel)
    for (uint8_t t = 0; t < 4; t++) {
        ctx.liveScore[t]        = teamScore(t);
        ctx.appliedPenalties[t] = appliedPenalties[t];
        ctx.teamKills[t]        = teamKillTotal(t);
    }
    memset(ctx.globalKills, 0, sizeof(ctx.globalKills));
    for (uint8_t t = 0; t < 4; t++) ctx.globalKills[0][t] = teamKillTotal(t);

    // Meta unitate locala
    uint8_t localBars = 0;
    if (batteryPercent >= 80)      localBars = 4;
    else if (batteryPercent >= 60) localBars = 3;
    else if (batteryPercent >= 40) localBars = 2;
    else if (batteryPercent >= 20) localBars = 1;
    globalBattery[UNIT_ID - 1] = localBars;
    lastSeenTime[UNIT_ID - 1]  = millis();   // unitatea proprie e mereu "online"

    // Datele tuturor unitatilor pentru paginile 4/5 (din tabel)
    for (uint8_t u = 0; u < MAX_UNITS; u++) {
        UnitRow& ur = unitTable[u];
        ctx.globalUnitMode[u]  = ur.mode;
        ctx.globalEventTime[u] = ur.actionTime;
        ctx.globalBattery[u]   = globalBattery[u];
        ctx.lastSeenTime[u]    = lastSeenTime[u];
        // status -> Team (compatibil cu randarea paginilor de unitati)
        if (ur.mode == 1)
            ctx.globalUnitStatus[u] = (ur.status == SEC_CAPTURED) ? ur.team : TEAM_NEUTRAL;
        else if (ur.mode == 2)
            ctx.globalUnitStatus[u] = (ur.status == BOMB_ARMED) ? TEAM_PLANTED : TEAM_NEUTRAL;
        else if (ur.mode == 3)
            ctx.globalUnitStatus[u] = ur.team;
        else
            ctx.globalUnitStatus[u] = TEAM_NEUTRAL;
    }

    ctx.page4ScrollIndex = page4Scroll;
    ctx.page5ScrollIndex = page5Scroll;
}
