#pragma once
#include <Arduino.h>

#include "config.h"
#include "display.h"  // pentru PageContext

// ============================================================
// game.h — MODELUL JOCULUI
// Variabilele sunt definite in game.cpp. Controller-ul (.ino) le
// modifica; view-ul (display.cpp) le citeste prin buildContext().
// ============================================================

// --- Identitate rol unitate (setat din meniu) ---
extern int8_t selectedMode;   // -1=nesetat, 0=Sector, 1=Bomb, 2=Respawn

// --- Scoruri ---
extern int32_t  liveScore[4];
extern uint16_t teamKills[4];
extern int32_t  appliedPenalties[4];

// --- Sector ---
extern Team     sectorOwner;
extern uint32_t captureStartTime;
extern uint32_t currentCapturePoints;

// --- Bomb ---
extern bool     isBombArmed;
extern Team     bombOwner;
extern uint32_t bombPlantTime;
extern uint32_t bombTimerMs;
extern bool     isCooldownActive;
extern uint32_t cooldownStartTime;
extern uint32_t cooldownMs;
extern uint32_t bombPointsExplode;
extern uint32_t bombPointsDefuse;

// --- Respawn ---
extern Team     respawnTeam;
extern uint32_t respawnTimeMs;
extern uint16_t respawnPenaltyPoints;
extern uint16_t teamMaxRespawns[4];
extern uint8_t  queueCount;
extern uint8_t  queueHead;
extern uint8_t  queueTail;
extern uint32_t respawnQueue[100];

// --- Flux joc ---
extern uint32_t     gameTimeLeftSeconds;
extern bool         isGameTimerRunning;
extern bool         isTimeOut;
extern Team         conquestWinner;
extern WinCondition currentWinCondition;
extern uint32_t     bonusIntervalMinutes;
extern uint32_t     actionTimeMs;   // durata actiunilor (capture/arm/defuse)
extern bool         isGamePaused;
extern uint32_t     pauseStartTime;

// --- Date agregate retea (placeholder pana la Faza 2 — LoRa le va popula) ---
extern uint8_t  globalUnitMode[MAX_UNITS];
extern Team     globalUnitStatus[MAX_UNITS];
extern uint32_t lastSeenTime[MAX_UNITS];
extern uint32_t globalEventTime[MAX_UNITS];
extern uint8_t  globalBattery[MAX_UNITS];

// ============================================================
// buildContext()
// Umple PageContext-ul pentru display din modelul jocului.
// currentPage / batteryPercent / scroll-urile vin din controller (.ino).
// ============================================================
void buildContext(PageContext& ctx, uint8_t currentPage, uint8_t batteryPercent,
                  uint8_t page4Scroll, uint8_t page5Scroll);
