#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

#include "config.h"

// ============================================================
// display.h — Afisaj OLED (NOVA Units)
// ============================================================

// Obiectul display — definit in display.cpp, folosit peste tot
extern Adafruit_SSD1306 display;

// ============================================================
// PageContext — toate datele de care display-ul are nevoie.
// Populat de buildContext() (game.cpp), pasat la drawPages().
// ============================================================
struct PageContext {
    // General
    uint8_t batteryPercent;
    uint8_t currentPage;
    int8_t  selectedMode;
    bool    isTimeOut;
    Team    conquestWinner;
    WinCondition winCondition;
    bool    isGameTimerRunning;
    uint32_t gameTimeLeftSeconds;
    int32_t appliedPenalties[4];
    bool    isGamePaused;
    uint32_t pauseStartTime;
    // Sector
    Team    sectorOwner;
    uint32_t captureStartTime;
    uint32_t currentCapturePoints;
    uint32_t bonusIntervalMinutes;
    // Bomb
    bool    isBombArmed;
    bool    isCooldownActive;
    uint32_t bombPlantTime;
    uint32_t bombTimerMs;
    uint32_t cooldownStartTime;
    uint32_t cooldownMs;
    // Respawn
    Team    respawnTeam;
    uint8_t queueCount;
    uint8_t queueHead;
    uint32_t respawnQueue[1];
    uint16_t respawnPenaltyPoints;
    uint16_t teamMaxRespawns[4];
    uint16_t teamKills[4];
    // Scoruri
    int32_t  liveScore[4];
    uint16_t globalKills[12][4];
    // Retea
    uint8_t  globalUnitMode[12];
    Team     globalUnitStatus[12];
    uint32_t lastSeenTime[12];
    uint32_t globalEventTime[12];
    uint8_t  globalBattery[12];
    // Scroll
    uint8_t page4ScrollIndex;
    uint8_t page5ScrollIndex;
};

// ------------------------------------------------------------
// Initializare
// ------------------------------------------------------------
void displayInit();

// Anti-drift imagine SSD1309 — apelata periodic din loop() (la 3s)
void displayRefreshRegisters();

// ------------------------------------------------------------
// Ecran BOOT — returneaza TRUE cand cele 3 secunde s-au scurs.
// ------------------------------------------------------------
bool handleBoot();

// ------------------------------------------------------------
// Meniu + selectare mod + Loading/Ready
// ------------------------------------------------------------
void drawMenu(uint8_t menuIndex);
void drawRespawnSetup();
void drawLoadingScreen(uint32_t elapsed, uint32_t totalMs);
void drawReadyScreen(int8_t selectedMode);

// ------------------------------------------------------------
// Cele 6 pagini
// ------------------------------------------------------------
void drawPageHeader(uint8_t currentPage, uint8_t batteryPercent);
void drawScrollbar(uint8_t totalItems, uint8_t visibleItems, uint8_t scrollIndex, uint8_t yStart, uint8_t barHeight);
void drawPages(const PageContext& ctx);

// ============================================================
// ADMIN — meniu + sub-pagini de setari
// ============================================================
void drawAdminMenu(uint8_t menuIndex, uint8_t scrollIndex, int8_t selectedMode);

// AdminContext — datele celor 4 sub-pagini (0=Game,1=Bomb,2=Respawn,3=TagWriter)
struct AdminContext {
    uint8_t selectedPage;
    uint8_t gsIndex, gsWinCond, gsTimeLimit, gsBonus, gsActionIdx;
    uint8_t bsIndex, bsTimerIdx, bsCooldownIdx, bsExpPtsIdx, bsDefPtsIdx;
    uint8_t rsIndex, rsTimeIdx, rsPenaltyIdx, rsLimitIdx[4];
    uint8_t twIndex, twOptionIdx;
};
void drawAdminPages(const AdminContext& ac);

void drawAdminSaved();      // ecran "SAVED"
void drawPowerOffScreen();  // ecran "Turning Off ..."
