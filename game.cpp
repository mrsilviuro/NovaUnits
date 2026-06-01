#include "game.h"
#include <string.h>

// ============================================================
// MODELUL JOCULUI — definitii + valori initiale
// (deocamdata fara logica; o adaugam la pasii urmatori)
// ============================================================

// --- Identitate rol unitate ---
int8_t selectedMode = -1;

// --- Scoruri ---
int32_t  liveScore[4]        = {0, 0, 0, 0};
uint16_t teamKills[4]        = {0, 0, 0, 0};
int32_t  appliedPenalties[4] = {0, 0, 0, 0};

// --- Sector ---
Team     sectorOwner          = TEAM_NEUTRAL;
uint32_t captureStartTime     = 0;
uint32_t currentCapturePoints = 0;

// --- Bomb ---
bool     isBombArmed      = false;
Team     bombOwner        = TEAM_NEUTRAL;
uint32_t bombPlantTime    = 0;
uint32_t bombTimerMs      = 15 * 60000UL;  // 15 min
bool     isCooldownActive = false;
uint32_t cooldownStartTime= 0;
uint32_t cooldownMs       = 20 * 60000UL;  // 20 min
uint32_t bombPointsExplode= 600;
uint32_t bombPointsDefuse = 300;

// --- Respawn ---
Team     respawnTeam          = TEAM_NEUTRAL;
uint32_t respawnTimeMs        = 300000;     // 5 min
uint16_t respawnPenaltyPoints = 25;
uint16_t teamMaxRespawns[4]   = {0, 0, 0, 0};  // 0 = nelimitat
uint8_t  queueCount           = 0;
uint8_t  queueHead            = 0;
uint8_t  queueTail            = 0;
uint32_t respawnQueue[100]    = {0};

// --- Flux joc ---
uint32_t     gameTimeLeftSeconds = 0;
bool         isGameTimerRunning  = false;
bool         isTimeOut           = false;
Team         conquestWinner      = TEAM_NEUTRAL;
WinCondition currentWinCondition = WIN_BY_POINTS;
uint32_t     bonusIntervalMinutes= 30;
uint32_t     actionTimeMs        = 15000;   // 15 sec
bool         isGamePaused        = false;
uint32_t     pauseStartTime      = 0;

// --- Date agregate retea (toate offline / zero pana la Faza 2) ---
uint8_t  globalUnitMode[MAX_UNITS]   = {0};
Team     globalUnitStatus[MAX_UNITS] = {TEAM_NEUTRAL};
uint32_t lastSeenTime[MAX_UNITS]     = {0};
uint32_t globalEventTime[MAX_UNITS]  = {0};
uint8_t  globalBattery[MAX_UNITS]    = {0};

// ============================================================
// buildContext()
// ============================================================
void buildContext(PageContext& ctx, uint8_t currentPage, uint8_t batteryPercent,
                  uint8_t page4Scroll, uint8_t page5Scroll) {
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

    // Sector
    ctx.sectorOwner          = sectorOwner;
    ctx.captureStartTime     = captureStartTime;
    ctx.currentCapturePoints = currentCapturePoints;
    ctx.bonusIntervalMinutes = bonusIntervalMinutes;

    // Bomb
    ctx.isBombArmed      = isBombArmed;
    ctx.isCooldownActive = isCooldownActive;
    ctx.bombPlantTime    = bombPlantTime;
    ctx.bombTimerMs      = bombTimerMs;
    ctx.cooldownStartTime= cooldownStartTime;
    ctx.cooldownMs       = cooldownMs;

    // Respawn
    ctx.respawnTeam          = respawnTeam;
    ctx.queueCount           = queueCount;
    ctx.queueHead            = queueHead;
    ctx.respawnPenaltyPoints = respawnPenaltyPoints;
    ctx.respawnQueue[0]      = respawnQueue[queueHead];
    memcpy(ctx.teamMaxRespawns, teamMaxRespawns, sizeof(teamMaxRespawns));
    memcpy(ctx.teamKills, teamKills, sizeof(teamKills));

    // Scoruri
    for (uint8_t i = 0; i < 4; i++) ctx.liveScore[i] = liveScore[i];
    memset(ctx.globalKills, 0, sizeof(ctx.globalKills));
    for (uint8_t i = 0; i < 4; i++) ctx.globalKills[0][i] = teamKills[i];
    for (uint8_t i = 0; i < 4; i++) ctx.appliedPenalties[i] = appliedPenalties[i];

    // --- Datele unitatii locale (reale) ---
    uint8_t myMode = 0;
    if (selectedMode == 0)      myMode = 1;
    else if (selectedMode == 1) myMode = 2;
    else if (selectedMode == 2) myMode = 3;

    ctx.globalUnitMode[UNIT_ID - 1] = myMode;
    ctx.lastSeenTime[UNIT_ID - 1]   = lastSeenTime[UNIT_ID - 1];

    uint8_t localBars = 0;
    if (batteryPercent >= 80)      localBars = 4;
    else if (batteryPercent >= 60) localBars = 3;
    else if (batteryPercent >= 40) localBars = 2;
    else if (batteryPercent >= 20) localBars = 1;
    ctx.globalBattery[UNIT_ID - 1] = localBars;

    if (myMode == 1) {
        ctx.globalUnitStatus[UNIT_ID - 1] = sectorOwner;
        ctx.globalEventTime[UNIT_ID - 1]  = captureStartTime;
    } else if (myMode == 2) {
        ctx.globalUnitStatus[UNIT_ID - 1] = isBombArmed ? TEAM_PLANTED : TEAM_NEUTRAL;
        ctx.globalEventTime[UNIT_ID - 1]  = isBombArmed ? bombPlantTime
                                          : (isCooldownActive ? cooldownStartTime : 0);
    } else if (myMode == 3) {
        ctx.globalUnitStatus[UNIT_ID - 1] = respawnTeam;
        ctx.globalEventTime[UNIT_ID - 1]  = 0;
    }

    // --- Restul unitatilor (offline pana la Faza 2) ---
    for (uint8_t i = 0; i < MAX_UNITS; i++) {
        if (i == UNIT_ID - 1) continue;
        ctx.globalUnitMode[i]   = globalUnitMode[i];
        ctx.globalUnitStatus[i] = globalUnitStatus[i];
        ctx.lastSeenTime[i]     = lastSeenTime[i];
        ctx.globalEventTime[i]  = globalEventTime[i];
        ctx.globalBattery[i]    = globalBattery[i];
    }

    // Scroll persistent (vine din controller) — corectie fata de varianta veche,
    // unde era resetat la 0 in fiecare frame.
    ctx.page4ScrollIndex = page4Scroll;
    ctx.page5ScrollIndex = page5Scroll;
}
