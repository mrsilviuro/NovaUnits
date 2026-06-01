// ============================================================
// NOVA Units — sketch principal
// Faza 1 (interfata). Pasul 3: cele 6 pagini (statice).
// ============================================================
#include "config.h"
#include "display.h"
#include "buttons.h"
#include "game.h"
#include "rfid.h"

#include <esp_wifi.h>
#include <esp_bt.h>

// ============================================================
// Stare masina de stari
// ============================================================
GameState currentState      = STATE_BOOT;
bool      needsDisplayUpdate = true;

// Anti-drift display
uint32_t lastDisplayRefresh = 0;

// --- Meniu / UI ---
uint8_t  menuIndex        = 0;
uint32_t loadingStartTime = 0;
uint32_t readyScreenStart = 0;

// --- Pagini ---
uint8_t     currentPage      = 0;
uint8_t     page4ScrollIndex = 0;
uint8_t     page5ScrollIndex = 0;
PageContext ctx;

// --- Sistem ---
uint8_t batteryPercent = 100;

// --- Power off (latch) ---
uint32_t powerOffStart  = 0;
bool     powerOffPulsed = false;
uint32_t latchLowStart  = 0;
bool     latchPulsing   = false;

// --- Admin ---
uint8_t   adminMenuIndex    = 0;
uint8_t   adminScrollIndex  = 0;
uint8_t   adminSelectedPage = 0;
uint32_t  adminSavedTime    = 0;
GameState previousStateBeforeAdmin = STATE_MENU;

// Indecsi setari admin (UI)
uint8_t gsIndex = 0, gsWinCond = 0, gsTimeLimit = 0, gsBonus = 2, gsActionIdx = 2;
uint8_t bsIndex = 0, bsTimerIdx = 2, bsCooldownIdx = 3, bsExpPtsIdx = 6, bsDefPtsIdx = 3;
uint8_t rsIndex = 0, rsTimeIdx = 6, rsPenaltyIdx = 3;
uint8_t rsLimitIdx[4] = {0, 0, 0, 0};
uint8_t twIndex = 0, twOptionIdx = 0;

// --- RFID ---
uint8_t  tagStatusMsg  = 0;
uint32_t tagStatusTime = 0;
uint32_t tagWaitStart  = 0;
uint32_t lastRfidRead  = 0;

// --- Actiuni / gameplay ---
ActionType currentAction    = ACT_NONE;
uint8_t    actingTeam       = 0;
uint32_t   actionStartTime  = 0;
uint32_t   successStartTime = 0;
bool       isRelayActive    = false;
uint32_t   relayTurnOffTime = 0;

// ============================================================
// refreshLEDs()
// Aprinde LED-ul echipei proprietare + orice buton tinut fizic.
// ============================================================
void refreshLEDs() {
    for (uint8_t i = 0; i < 4; i++) digitalWrite(PIN_LEDS[i], LOW);

    // LED-ul echipei proprietare — mereu aprins (din randul propriu)
    Team owner = TEAM_NEUTRAL;
    if (selectedMode == 0 && myRow().status == SEC_CAPTURED) owner = myRow().team;
    else if (selectedMode == 1 && myRow().status == BOMB_ARMED) owner = myRow().team;
    else if (selectedMode == 2) owner = myRow().team;
    if (owner != TEAM_NEUTRAL) digitalWrite(PIN_LEDS[owner - 1], HIGH);

    // LED-urile butoanelor fizic apasate
    for (uint8_t i = 0; i < 4; i++)
        if (digitalRead(PIN_BTNS[i]) == LOW) digitalWrite(PIN_LEDS[i], HIGH);
}

// ============================================================
// updateBattery() — GPIO36, divizor 10k/3.3k, smoothing 80/20
// ============================================================
void updateBattery() {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < 20; i++) sum += analogRead(PIN_BATTERY);
    float adcAvg = sum / 20.0;
    float pinV = (adcAvg / 4095.0) * 3.56;
    float batV = pinV * ((10.0 + 3.3) / 3.3);

    uint8_t newPercent = 0;
    if (batV >= 12.6)      newPercent = 100;
    else if (batV <= 9.0)  newPercent = 0;
    else                   newPercent = (uint8_t)(((batV - 9.0) / (12.6 - 9.0)) * 100.0);

    if (batteryPercent == 100) batteryPercent = newPercent;
    else batteryPercent = (batteryPercent * 80 + newPercent * 20) / 100;
}

// ============================================================
// buildAdminContext() — impacheteaza indicii admin pentru drawAdminPages
// ============================================================
AdminContext buildAdminContext() {
    AdminContext ac;
    ac.selectedPage = adminSelectedPage;
    ac.gsIndex = gsIndex; ac.gsWinCond = gsWinCond; ac.gsTimeLimit = gsTimeLimit;
    ac.gsBonus = gsBonus; ac.gsActionIdx = gsActionIdx;
    ac.bsIndex = bsIndex; ac.bsTimerIdx = bsTimerIdx; ac.bsCooldownIdx = bsCooldownIdx;
    ac.bsExpPtsIdx = bsExpPtsIdx; ac.bsDefPtsIdx = bsDefPtsIdx;
    ac.rsIndex = rsIndex; ac.rsTimeIdx = rsTimeIdx; ac.rsPenaltyIdx = rsPenaltyIdx;
    for (uint8_t i = 0; i < 4; i++) ac.rsLimitIdx[i] = rsLimitIdx[i];
    ac.twIndex = twIndex; ac.twOptionIdx = twOptionIdx;
    return ac;
}

// ============================================================
// syncAdminIndices() — recalculeaza indicii admin din valorile modelului
// ============================================================
void syncAdminIndices() {
    gsIndex = bsIndex = rsIndex = twIndex = 0;

    const uint32_t tl[] = {0, 900, 1800, 3600, 7200, 10800, 14400, 18000,
        21600, 25200, 28800, 32400, 36000, 39600, 43200, 86400};
        for (uint8_t i = 0; i < 16; i++)
            if (tl[i] == gameTimeLeftSeconds) { gsTimeLimit = i; break; }

            const uint16_t bn[] = {0, 15, 30, 60, 120, 180, 240};
        for (uint8_t i = 0; i < 7; i++)
            if (bn[i] == bonusIntervalMinutes) { gsBonus = i; break; }

            gsWinCond = (uint8_t)currentWinCondition;

        const uint32_t at[] = {5000, 10000, 15000, 20000};
        for (uint8_t i = 0; i < 4; i++)
            if (at[i] == actionTimeMs) { gsActionIdx = i; break; }

            const uint32_t tv[] = {5, 10, 15, 20, 30, 45, 60, 120};
        for (uint8_t i = 0; i < 8; i++)
            if (tv[i] * 60000UL == bombTimerMs) { bsTimerIdx = i; break; }
            for (uint8_t i = 0; i < 8; i++)
                if (tv[i] * 60000UL == cooldownMs) { bsCooldownIdx = i; break; }

                const uint32_t pv[] = {50, 100, 200, 300, 400, 500, 600, 700,
                    800, 900, 1000, 1500, 2000, 2500, 3000};
                    for (uint8_t i = 0; i < 15; i++)
                        if (pv[i] == bombPointsExplode) { bsExpPtsIdx = i; break; }
                        for (uint8_t i = 0; i < 15; i++)
                            if (pv[i] == bombPointsDefuse) { bsDefPtsIdx = i; break; }

                            const uint32_t ts[] = {10, 30, 60, 120, 180, 240, 300, 600, 900, 1200, 1500, 1800};
                        for (uint8_t i = 0; i < 12; i++)
                            if (ts[i] * 1000UL == respawnTimeMs) { rsTimeIdx = i; break; }

                            const uint16_t pp[] = {0, 5, 10, 25, 50, 75, 100};
                        for (uint8_t i = 0; i < 7; i++)
                            if (pp[i] == respawnPenaltyPoints) { rsPenaltyIdx = i; break; }

                            const uint16_t lm[] = {0, 10, 25, 50, 75, 100, 200, 300, 400, 500, 1000};
                        for (uint8_t t = 0; t < 4; t++)
                            for (uint8_t i = 0; i < 11; i++)
                                if (lm[i] == teamMaxRespawns[t]) { rsLimitIdx[t] = i; break; }
}

// ============================================================
// handleActionProgress() — hold-to-progress + aplicare actiune
// ============================================================
void handleActionProgress() {
    uint32_t now = millis();
    uint32_t elapsed = now - actionStartTime;

    // 1. Buton eliberat -> ANULARE
    if (digitalRead(PIN_BTNS[actingTeam]) == HIGH) {
        currentState = STATE_PAGES;
        currentPage = 0;
        currentAction = ACT_NONE;
        needsDisplayUpdate = true;
        tone(PIN_BUZZER, 300, 300);
        refreshLEDs();
        Serial.println("[ACTION] Anulat - buton eliberat.");
        return;
    }

    // 2. Timp atins -> SUCCESS
    if (elapsed >= actionTimeMs) {
        if (currentAction == ACT_CAPTURE) {
            myRow().status     = SEC_CAPTURED;
            myRow().team       = (Team)(actingTeam + 1);
            myRow().actionTime = now;
            liveCapturePoints  = 0;
            lastPointTick      = now;
            Serial.print("[SECTOR] Cucerit de: ");
            Serial.println(TEAM_NAMES[actingTeam]);
        } else if (currentAction == ACT_NEUTRALIZE) {
            // comitem punctele live la totalul echipei care detinea sectorul
            if (myRow().team != TEAM_NEUTRAL)
                myRow().savedPoints[myRow().team - 1] += (int32_t)liveCapturePoints;
            myRow().status     = SEC_NEUTRAL;
            myRow().team       = TEAM_NEUTRAL;
            myRow().actionTime = now;
            liveCapturePoints  = 0;
            lastPointTick      = 0;
            Serial.print("[SECTOR] Neutralizat de: ");
            Serial.println(TEAM_NAMES[actingTeam]);
        }

        // Releu scurt (sirena)
        digitalWrite(PIN_RELAY, LOW);
        isRelayActive = true;
        relayTurnOffTime = now + 5000;

        currentState = STATE_SUCCESS;
        successStartTime = now;
        currentAction = ACT_NONE;
        drawSuccessScreen();
        refreshLEDs();
        tone(PIN_BUZZER, 1800, 600);
        return;
    }

    // 3. In desfasurare -> bara de progres
    drawActionScreen(currentAction, actingTeam, elapsed, actionTimeMs);
}

// ============================================================
// setup()
// ============================================================
void setup() {
    // Oprim WiFi + Bluetooth (economie + stabilitate)
    esp_wifi_stop();
    esp_bt_controller_disable();

    Serial.begin(115200);

    // Power latch — tinem alimentarea pornita
    pinMode(PIN_LATCH, OUTPUT);
    digitalWrite(PIN_LATCH, HIGH);

    // LED-uri
    for (uint8_t i = 0; i < 4; i++) {
        pinMode(PIN_LEDS[i], OUTPUT);
        digitalWrite(PIN_LEDS[i], LOW);
    }

    // Releu (open-drain, HIGH = oprit) + Buzzer
    pinMode(PIN_RELAY, OUTPUT_OPEN_DRAIN);
    digitalWrite(PIN_RELAY, HIGH);
    pinMode(PIN_BUZZER, OUTPUT);

    buttonsInit();
    rfidInit();
    // LoRa se adauga la Faza 2.
    displayInit();

    Serial.println("[BOOT] Setup complet.");
}

// ============================================================
// loop()
// ============================================================
void loop() {
    uint32_t now = millis();

    // Refresh registre display — previne "drift"-ul de imagine
    if (now - lastDisplayRefresh >= 3000) {
        lastDisplayRefresh = now;
        displayRefreshRegisters();
    }

    // Citire baterie la fiecare 10 secunde
    static uint32_t lastBatteryCheck = 0;
    if (now - lastBatteryCheck >= 10000 || lastBatteryCheck == 0) {
        lastBatteryCheck = now;
        updateBattery();
    }

    // Power latch pulse non-blocking (power-off)
    if (latchPulsing && millis() - latchLowStart >= 100) {
        latchPulsing = false;
        digitalWrite(PIN_LATCH, HIGH);
    }

    // Citire RFID — card Admin (cardurile de puncte in joc vin la Faza 2)
    if ((currentState == STATE_PAGES || currentState == STATE_MENU || currentState == STATE_RESPAWN_SETUP)
        && (millis() - lastRfidRead >= 100)) {
        RfidReadData rfid = rfidReadTag();
    if (rfid.result == RFID_READ_ADMIN) {
        tone(PIN_BUZZER, 2000, 200);
        previousStateBeforeAdmin = currentState;
        adminMenuIndex = 0;
        adminScrollIndex = 0;
        currentState = STATE_ADMIN_MENU;
        needsDisplayUpdate = true;
        Serial.println("[RFID] Admin tag detectat!");
    }
    lastRfidRead = millis();
        }

        // Sector — puncte LIVE (3 + bonus la fiecare 10s cat timp e cucerit)
        if (selectedMode == 0 && myRow().status == SEC_CAPTURED && lastPointTick == 0)
            lastPointTick = now;
    if (!isTimeOut && !isGamePaused && selectedMode == 0 && myRow().status == SEC_CAPTURED) {
        if (now - lastPointTick >= 10000) {
            uint32_t minutesHeld = (now - myRow().actionTime) / 60000;
            uint32_t bonus = (bonusIntervalMinutes > 0) ? (minutesHeld / bonusIntervalMinutes) : 0;
            if (bonus > 3) bonus = 3;
            liveCapturePoints += (3 + bonus);
            lastPointTick += 10000;
        }
    }

    // Releu — oprire non-blocking
    if (isRelayActive && millis() >= relayTurnOffTime) {
        digitalWrite(PIN_RELAY, HIGH);
        isRelayActive = false;
    }

    switch (currentState) {
        case STATE_BOOT:
            if (handleBoot()) {
                currentState = STATE_MENU;
                needsDisplayUpdate = true;
            }
            break;

        case STATE_MENU:
            if (needsDisplayUpdate) {
                drawMenu(menuIndex);
                needsDisplayUpdate = false;
            }
            handleButtons();
            break;

        case STATE_RESPAWN_SETUP:
            if (needsDisplayUpdate) {
                drawRespawnSetup();
                needsDisplayUpdate = false;
            }
            handleButtons();
            break;

        case STATE_LOADING: {
            uint32_t elapsed = millis() - loadingStartTime;
            drawLoadingScreen(elapsed, 2000);
            if (elapsed >= 2000) {
                readyScreenStart = millis();
                currentState = STATE_READY;
                needsDisplayUpdate = true;
                for (uint8_t i = 0; i < 4; i++) digitalWrite(PIN_LEDS[i], HIGH);
                tone(PIN_BUZZER, 1500, 500);
            }
            break;
        }

        case STATE_READY: {
            if (needsDisplayUpdate) {
                drawReadyScreen(selectedMode);
                needsDisplayUpdate = false;
            }
            if (millis() - readyScreenStart >= 2000) {
                currentState = STATE_PAGES;
                needsDisplayUpdate = true;
                refreshLEDs();
            }
            break;
        }

        case STATE_PAGES: {
            static uint32_t lastRefresh = 0;
            uint32_t refreshInterval = 200;
            // pe pagina 1, mod Bomb amorsata, vom avea nevoie de refresh rapid (Pasul Bomb)
            if (selectedMode == 1 && myRow().status == BOMB_ARMED && currentPage == 0) refreshInterval = 50;
            if (needsDisplayUpdate || (millis() - lastRefresh >= refreshInterval)) {
                buildContext(ctx, currentPage, batteryPercent, page4ScrollIndex, page5ScrollIndex);
                drawPages(ctx);
                needsDisplayUpdate = false;
                lastRefresh = millis();
            }
            handleButtons();
            break;
        }

        case STATE_ACTION:
            handleActionProgress();
            break;

        case STATE_SUCCESS:
            if (millis() - successStartTime >= 2000) {
                currentState = STATE_PAGES;
                currentPage = 0;
                needsDisplayUpdate = true;
                refreshLEDs();
            }
            break;

        case STATE_ADMIN_MENU:
            if (needsDisplayUpdate) {
                drawAdminMenu(adminMenuIndex, adminScrollIndex, selectedMode);
                needsDisplayUpdate = false;
            }
            handleButtons();
            break;

        case STATE_ADMIN_PAGES:
            if (needsDisplayUpdate) {
                AdminContext ac = buildAdminContext();
                drawAdminPages(ac);
                needsDisplayUpdate = false;
            }
            handleButtons();
            break;

        case STATE_ADMIN_SAVED:
            drawAdminSaved();
            if (millis() - adminSavedTime >= 2000) {
                currentState = STATE_ADMIN_MENU;
                needsDisplayUpdate = true;
            }
            break;

        case STATE_POWER_OFF:
            if (needsDisplayUpdate) {
                drawPowerOffScreen();
                needsDisplayUpdate = false;
            }
            if (!powerOffPulsed && millis() - powerOffStart >= 2000) {
                powerOffPulsed = true;
                digitalWrite(PIN_LATCH, LOW);
                latchLowStart = millis();
                latchPulsing = true;
            }
            if (powerOffPulsed && !latchPulsing) {
                powerOffPulsed = false;
                currentState = STATE_ADMIN_MENU;
                needsDisplayUpdate = true;
            }
            break;

        case STATE_ADMIN_TAG_WRITE: {
            // Timeout 3 secunde fara card
            if (tagStatusMsg == 0 && millis() - tagWaitStart >= 3000) {
                tagStatusMsg = 6;
                tagStatusTime = millis();
                tone(PIN_BUZZER, 200, 300);
            }

            drawTagWriter(tagStatusMsg);

            // Dupa 2 secunde pe ecranul de rezultat, revenim la Tag Writer
            if (tagStatusMsg != 0 && millis() - tagStatusTime >= 2000) {
                currentState = STATE_ADMIN_PAGES;
                tagStatusMsg = 0;
                needsDisplayUpdate = true;
                break;
            }

            // Cat timp asteptam card, incercam sa scriem
            if (tagStatusMsg == 0) {
                const uint16_t ptsOpt[] = {50, 100, 150, 200, 250, 500, 750, 1000, 1500, 2000, 0};
                uint8_t type = (twOptionIdx == 10) ? 2 : 1;
                uint16_t points = ptsOpt[twOptionIdx];

                RfidWriteResult r = rfidWriteTag(type, points);
                if (r == RFID_TIMEOUT) break;  // niciun card inca

                switch (r) {
                    case RFID_OK_NEW:       tagStatusMsg = 1; tone(PIN_BUZZER, 1500, 500); break;
                    case RFID_OK_OVERWRITE: tagStatusMsg = 2; tone(PIN_BUZZER, 1500, 500); break;
                    case RFID_OK_ADMIN:     tagStatusMsg = 3; tone(PIN_BUZZER, 1500, 500); break;
                    case RFID_OK_REVOKED:   tagStatusMsg = 4; tone(PIN_BUZZER, 1500, 500); break;
                    case RFID_DENIED:       tagStatusMsg = 5; tone(PIN_BUZZER, 200, 400);  break;
                    default:                tagStatusMsg = 7; tone(PIN_BUZZER, 200, 300);  break;
                }
                tagStatusTime = millis();
            }
            handleButtons();
            break;
        }

                    default:
                        break;
        }
}

// ============================================================
// CALLBACKS BUTOANE
// ============================================================

void onShortPress(uint8_t btnIndex) {
    tone(PIN_BUZZER, 800, 30);

    if (currentState == STATE_MENU) {
        if (btnIndex == 2) {            // VERDE — scroll
            menuIndex++;
            if (menuIndex > 2) menuIndex = 0;
            needsDisplayUpdate = true;
        } else if (btnIndex == 3) {     // GALBEN — confirm
            selectedMode = menuIndex;
            myRow().mode = (selectedMode == 0) ? 1 : (selectedMode == 1) ? 2 : 3;
            if (selectedMode == 2) {
                currentState = STATE_RESPAWN_SETUP;
            } else {
                currentState = STATE_LOADING;
                loadingStartTime = millis();
            }
            needsDisplayUpdate = true;
            Serial.print("[MENU] Mod selectat: ");
            Serial.println(selectedMode);
            // (alerta LoRa EVT_MODE_* se adauga in Faza 2)
        }

    } else if (currentState == STATE_RESPAWN_SETUP) {
        myRow().mode = 3;
        myRow().team = (Team)(btnIndex + 1);
        currentState = STATE_LOADING;
        loadingStartTime = millis();
        refreshLEDs();
        needsDisplayUpdate = true;
        Serial.print("[RESPAWN] Echipa selectata: ");
        Serial.println(TEAM_NAMES[btnIndex]);

    } else if (currentState == STATE_PAGES) {
        if (btnIndex == 0) {            // ROSU — pagina anterioara
            currentPage = (currentPage == 0) ? 5 : currentPage - 1;
            needsDisplayUpdate = true;
        } else if (btnIndex == 1) {     // ALBASTRU — pagina urmatoare
            currentPage = (currentPage >= 5) ? 0 : currentPage + 1;
            needsDisplayUpdate = true;
        } else if (btnIndex == 2) {     // VERDE — scroll pe paginile 4 si 5
            if (currentPage == 3) {
                page4ScrollIndex++;
                needsDisplayUpdate = true;
            } else if (currentPage == 4) {
                page5ScrollIndex++;
                needsDisplayUpdate = true;
            }
            // pag 3 (kill reset) / pag 6 (start/pause/time reset) -> pasii urmatori
        }
        // GALBEN (start/pause/respawn kill) -> pasii urmatori
    }

    else if (currentState == STATE_ADMIN_MENU) {
        if (btnIndex == 2) {            // VERDE — scroll jos
            adminMenuIndex++;
            if (adminMenuIndex == 5 && selectedMode == -1) adminMenuIndex++;
            if (adminMenuIndex >= 8) {
                adminMenuIndex = 0;
                adminScrollIndex = 0;
            } else {
                uint8_t vis = 0;
                for (uint8_t i = adminScrollIndex; i <= adminMenuIndex; i++) {
                    if (i == 5 && selectedMode == -1) continue;
                    vis++;
                }
                while (vis > 5) {
                    adminScrollIndex++;
                    if (adminScrollIndex == 4 && selectedMode == -1) adminScrollIndex++;
                    vis--;
                }
            }
            needsDisplayUpdate = true;

        } else if (btnIndex == 3) {     // GALBEN — confirmare
            if (adminMenuIndex == 3) {
                // SYNC UNITS — comunicarea LoRa vine in Faza 2
                tone(PIN_BUZZER, 1000, 50);
                Serial.println("[SYNC] Sync Units — vine in Faza 2 (LoRa).");
            } else if (adminMenuIndex == 5) {
                // CHANGE MODE
                selectedMode = -1;
                myRow() = UnitRow{};       // reset complet randul propriu
                liveCapturePoints = 0;
                lastPointTick = 0;
                queueCount = queueHead = queueTail = 0;
                currentState = STATE_MENU;
                menuIndex = 0;
                adminMenuIndex = 0;
                adminScrollIndex = 0;
                refreshLEDs();
                needsDisplayUpdate = true;
                tone(PIN_BUZZER, 1000, 300);
            } else if (adminMenuIndex == 6) {
                // SYSTEM RESTART
                display.clearDisplay();
                display.setTextSize(2);
                uint8_t x = (SCREEN_WIDTH - (strlen("REBOOTING") * 12)) / 2;
                display.setCursor(x, 24);
                display.print("REBOOTING");
                display.display();
                tone(PIN_BUZZER, 2000, 800);
                uint32_t t = millis();
                while (millis() - t < 800) {}  // exceptie acceptata — inainte de restart
                ESP.restart();
            } else if (adminMenuIndex == 7) {
                // POWER OFF
                currentState = STATE_POWER_OFF;
                powerOffStart = millis();
                powerOffPulsed = false;
                needsDisplayUpdate = true;
                tone(PIN_BUZZER, 1500, 300);
            } else {
                if      (adminMenuIndex == 0) adminSelectedPage = 0;
                else if (adminMenuIndex == 1) adminSelectedPage = 1;
                else if (adminMenuIndex == 2) adminSelectedPage = 2;
                else if (adminMenuIndex == 4) adminSelectedPage = 3;
                syncAdminIndices();
                currentState = STATE_ADMIN_PAGES;
                needsDisplayUpdate = true;
            }

        } else if (btnIndex == 0) {     // ROSU — inapoi la joc
            currentState = previousStateBeforeAdmin;
            needsDisplayUpdate = true;
        }

    } else if (currentState == STATE_ADMIN_PAGES) {
        if (btnIndex == 0) {            // ROSU — inapoi la meniu admin
            currentState = STATE_ADMIN_MENU;
            needsDisplayUpdate = true;
            return;
        }

        if (adminSelectedPage == 0) {           // GAME SETTINGS
            if (btnIndex == 2) {
                gsIndex = (gsIndex + 1) % 5;
                needsDisplayUpdate = true;
            } else if (btnIndex == 3) {
                if      (gsIndex == 0) gsWinCond   = (gsWinCond + 1) % 3;
                else if (gsIndex == 1) gsTimeLimit = (gsTimeLimit + 1) % 16;
                else if (gsIndex == 2) gsBonus     = (gsBonus + 1) % 7;
                else if (gsIndex == 3) gsActionIdx = (gsActionIdx + 1) % 4;
                else if (gsIndex == 4) {          // CONFIRM
                    const uint32_t tl[] = {0, 900, 1800, 3600, 7200, 10800, 14400, 18000, 21600, 25200, 28800, 32400, 36000, 39600, 43200, 86400};
                    const uint16_t bn[] = {0, 15, 30, 60, 120, 180, 240};
                    const uint32_t at[] = {5000, 10000, 15000, 20000};
                    gameTimeLeftSeconds  = tl[gsTimeLimit];
                    bonusIntervalMinutes = bn[gsBonus];
                    currentWinCondition  = (WinCondition)gsWinCond;
                    actionTimeMs         = at[gsActionIdx];
                    adminSavedTime = millis();
                    tone(PIN_BUZZER, 1500, 300);
                    currentState = STATE_ADMIN_SAVED;
                }
                needsDisplayUpdate = true;
            }

        } else if (adminSelectedPage == 1) {    // BOMB PARAMETERS
            if (btnIndex == 2) {
                bsIndex = (bsIndex + 1) % 5;
                needsDisplayUpdate = true;
            } else if (btnIndex == 3) {
                if      (bsIndex == 0) bsTimerIdx    = (bsTimerIdx + 1) % 8;
                else if (bsIndex == 1) bsCooldownIdx = (bsCooldownIdx + 1) % 8;
                else if (bsIndex == 2) bsExpPtsIdx   = (bsExpPtsIdx + 1) % 15;
                else if (bsIndex == 3) bsDefPtsIdx   = (bsDefPtsIdx + 1) % 15;
                else if (bsIndex == 4) {          // CONFIRM
                    const uint32_t tv[] = {5, 10, 15, 20, 30, 45, 60, 120};
                    const uint32_t pv[] = {50, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1500, 2000, 2500, 3000};
                    bombTimerMs       = tv[bsTimerIdx] * 60000UL;
                    cooldownMs        = tv[bsCooldownIdx] * 60000UL;
                    bombPointsExplode = pv[bsExpPtsIdx];
                    bombPointsDefuse  = pv[bsDefPtsIdx];
                    adminSavedTime = millis();
                    tone(PIN_BUZZER, 1500, 300);
                    currentState = STATE_ADMIN_SAVED;
                }
                needsDisplayUpdate = true;
            }

        } else if (adminSelectedPage == 2) {    // RESPAWN RULES
            if (btnIndex == 2) {
                rsIndex = (rsIndex + 1) % 7;
                needsDisplayUpdate = true;
            } else if (btnIndex == 3) {
                if      (rsIndex == 0) rsTimeIdx    = (rsTimeIdx + 1) % 12;
                else if (rsIndex == 1) rsPenaltyIdx = (rsPenaltyIdx + 1) % 7;
                else if (rsIndex >= 2 && rsIndex <= 5)
                    rsLimitIdx[rsIndex - 2] = (rsLimitIdx[rsIndex - 2] + 1) % 11;
                else if (rsIndex == 6) {          // CONFIRM
                    const uint32_t ts[] = {10, 30, 60, 120, 180, 240, 300, 600, 900, 1200, 1500, 1800};
                    const uint16_t pp[] = {0, 5, 10, 25, 50, 75, 100};
                    const uint16_t lm[] = {0, 10, 25, 50, 75, 100, 200, 300, 400, 500, 1000};
                    respawnTimeMs        = ts[rsTimeIdx] * 1000UL;
                    respawnPenaltyPoints = pp[rsPenaltyIdx];
                    for (uint8_t i = 0; i < 4; i++) teamMaxRespawns[i] = lm[rsLimitIdx[i]];
                    adminSavedTime = millis();
                    tone(PIN_BUZZER, 1500, 300);
                    currentState = STATE_ADMIN_SAVED;
                }
                needsDisplayUpdate = true;
            }

        } else if (adminSelectedPage == 3) {    // TAG WRITER
            if (btnIndex == 2) {
                twIndex = (twIndex + 1) % 2;
                needsDisplayUpdate = true;
            } else if (btnIndex == 3) {
                if (twIndex == 0) {
                    twOptionIdx = (twOptionIdx + 1) % 11;
                    needsDisplayUpdate = true;
                } else {
                    // WRITE — pornim scrierea pe card
                    tagStatusMsg = 0;
                    tagWaitStart = millis();
                    currentState = STATE_ADMIN_TAG_WRITE;
                    tone(PIN_BUZZER, 1000, 50);
                    needsDisplayUpdate = true;
                }
            }
        }
    }
}

void onLongPress(uint8_t btnIndex) {
    // Actiunile sunt permise doar pe pagina 1, in STATE_PAGES
    if (currentState != STATE_PAGES || isTimeOut) return;
    if (isGamePaused) { tone(PIN_BUZZER, 200, 200); return; }
    if (gameTimeLeftSeconds > 0 && !isGameTimerRunning) { tone(PIN_BUZZER, 200, 200); return; }

    Team btnTeam = (Team)(btnIndex + 1);

    if (selectedMode == 0) {  // SECTOR
        if (myRow().status == SEC_NEUTRAL) {
            currentAction = ACT_CAPTURE;
        } else if (btnTeam == myRow().team) {
            tone(PIN_BUZZER, 200, 200);   // echipa proprie nu-si neutralizeaza sectorul
            return;
        } else {
            currentAction = ACT_NEUTRALIZE;
        }
        actingTeam = btnIndex;
        actionStartTime = millis();
        currentState = STATE_ACTION;
        needsDisplayUpdate = true;
        tone(PIN_BUZZER, 1000, 100);
        Serial.println(currentAction == ACT_CAPTURE ? "[SECTOR] Start CAPTURE" : "[SECTOR] Start NEUTRALIZE");
    }
    // BOMB (arm/defuse) -> pasul urmator
}

void onAdminCombo() {
    tone(PIN_BUZZER, 2000, 500);
    previousStateBeforeAdmin = currentState;
    adminMenuIndex = 0;
    adminScrollIndex = 0;
    currentState = STATE_ADMIN_MENU;
    needsDisplayUpdate = true;
    Serial.println("[ADMIN] Intram in Admin Mode.");
}
