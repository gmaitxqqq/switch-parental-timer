// ParentalTimer Sysmodule v4
// =============================================================
// Runs as Atmosphere sysmodule (background daemon).
// Communicates with companion NRO via shared memory file.
//
// PIN: 8473 (hardcoded) | Default: 15 min
// Title ID: 0x4200000000000001
// Install: sd:/atmosphere/contents/4200000000000001/exefs.nsp
//          sd:/atmosphere/contents/4200000000000001/boot2.flag
//
// Communication via sd:/parental_timer.cmd:
//   Line 1: "UNLOCK <minutes>"  -> unlock parental, start timer
//   Line 2: "STATUS"            -> sysmodule writes back status
//   Line 3: "STOP"              -> stop timer and re-lock
// =============================================================

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define PIN_CODE    "8473"
#define PIN_LEN     4
#define S_1MS       (1000000ULL)
#define S_1S        (1000000000ULL)
#define CMD_PATH    "sdmc:/parental_timer.cmd"
#define STATUS_PATH "sdmc:/parental_timer.status"

// Timer state
typedef enum {
    STATE_IDLE,       // Waiting for command
    STATE_UNLOCKING,  // Auto-navigating to unlock
    STATE_COUNTDOWN,  // Timer running, parental off
    STATE_LOCKING     // Time up, re-locking
} TimerState;

static TimerState g_state = STATE_IDLE;
static u32 g_minutes = 0;
static u64 g_deadline = 0;

// Virtual pad
static bool g_vpadReady = false;
static HiddbgAbstractedPadState g_vpadState;

// ---- File-based IPC ----

static void writeStatus(const char *msg)
{
    FILE *f = fopen(STATUS_PATH, "w");
    if (f) {
        fprintf(f, "%s", msg);
        fclose(f);
    }
}

// Read and delete command file
static bool readCmd(char *cmd, size_t cmdSize, u32 *param)
{
    FILE *f = fopen(CMD_PATH, "r");
    if (!f) return false;

    char line[128] = {0};
    if (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "UNLOCK", 6) == 0) {
            strncpy(cmd, "UNLOCK", cmdSize);
            *param = 15; // default
            int mins = 0;
            if (sscanf(line + 6, " %d", &mins) == 1 && mins > 0) {
                *param = (u32)mins;
            }
            fclose(f);
            remove(CMD_PATH);
            return true;
        }
        if (strncmp(line, "STOP", 4) == 0) {
            strncpy(cmd, "STOP", cmdSize);
            fclose(f);
            remove(CMD_PATH);
            return true;
        }
        if (strncmp(line, "STATUS", 6) == 0) {
            strncpy(cmd, "STATUS", cmdSize);
            fclose(f);
            remove(CMD_PATH);
            return true;
        }
    }
    fclose(f);
    remove(CMD_PATH);
    return false;
}

// ---- Time helpers ----
static u64 getUnixSeconds(void)
{
    u64 timestamp = 0;
    Result rc = timeGetCurrentTime(TimeType_UserSystemClock, &timestamp);
    if (R_FAILED(rc)) return 0;
    return timestamp;
}

// ---- Virtual pad (AbstractedPad AutoPilot, FW 5.0+) ----
static Result vpadInit(void)
{
    Result rc = hiddbgInitialize();
    if (R_FAILED(rc)) return rc;

    memset(&g_vpadState, 0, sizeof(g_vpadState));
    g_vpadState.type = 0x2; // Pro Controller
    g_vpadState.state.buttons = 0;
    g_vpadState.state.analog_stick_l.x = 0x0800;
    g_vpadState.state.analog_stick_l.y = 0x0800;
    g_vpadState.state.analog_stick_r.x = 0x0800;
    g_vpadState.state.analog_stick_r.y = 0x0800;

    rc = hiddbgSetAutoPilotVirtualPadState(0, &g_vpadState);
    if (R_FAILED(rc)) {
        hiddbgExit();
        return rc;
    }

    g_vpadReady = true;
    return 0;
}

static void vpadCleanup(void)
{
    if (g_vpadReady) {
        hiddbgUnsetAutoPilotVirtualPadState(0);
        hiddbgUnsetAllAutoPilotVirtualPadState();
    }
    hiddbgExit();
    g_vpadReady = false;
}

static void pressBtn(u32 buttons, u32 holdMs, u32 gapMs)
{
    if (!g_vpadReady) return;

    g_vpadState.state.buttons = buttons;
    hiddbgSetAutoPilotVirtualPadState(0, &g_vpadState);
    svcSleepThread((u64)holdMs * S_1MS);

    g_vpadState.state.buttons = 0;
    hiddbgSetAutoPilotVirtualPadState(0, &g_vpadState);
    svcSleepThread((u64)gapMs * S_1MS);
}

// ---- Auto-navigate to Parental Controls and disable ----
static void autoDisableParental(void)
{
    writeStatus("UNLOCKING:navigating");

    // Go to Home Menu
    pressBtn(HiddbgNpadButton_Home, 300, 1500);

    // Navigate to System Settings
    for (int i = 0; i < 5; i++)
        pressBtn(HidNpadButton_Down, 100, 200);
    for (int i = 0; i < 6; i++)
        pressBtn(HidNpadButton_Left, 100, 200);

    // Open System Settings
    pressBtn(HidNpadButton_A, 200, 1500);

    // Scroll to Parental Controls
    for (int i = 0; i < 15; i++)
        pressBtn(HidNpadButton_Down, 80, 150);

    // Open Parental Controls
    pressBtn(HidNpadButton_A, 200, 800);

    // Select "Parental Controls Settings"
    pressBtn(HidNpadButton_A, 200, 600);

    // Enter PIN: 8473
    for (int i = 0; i < PIN_LEN; i++) {
        int digit = PIN_CODE[i] - '0';
        for (int j = 0; j < digit; j++)
            pressBtn(HidNpadButton_Right, 80, 80);
        pressBtn(HidNpadButton_A, 150, 200);
    }
    // Confirm PIN entry
    pressBtn(HidNpadButton_A, 150, 800);

    // Toggle restriction OFF
    pressBtn(HidNpadButton_A, 200, 500);
    // Confirm
    pressBtn(HidNpadButton_A, 200, 500);

    // Return to home
    pressBtn(HiddbgNpadButton_Home, 300, 800);
}

// ---- Auto re-enable Parental Controls ----
static void autoEnableParental(void)
{
    writeStatus("LOCKING:closing_game");

    // HOME to see running game
    pressBtn(HiddbgNpadButton_Home, 300, 1000);

    // Close game with X
    pressBtn(HidNpadButton_X, 200, 500);
    pressBtn(HidNpadButton_A, 200, 1000);
    svcSleepThread(1 * S_1S);

    writeStatus("LOCKING:navigating");

    // Navigate to System Settings
    for (int i = 0; i < 5; i++)
        pressBtn(HidNpadButton_Down, 100, 200);
    for (int i = 0; i < 6; i++)
        pressBtn(HidNpadButton_Left, 100, 200);
    pressBtn(HidNpadButton_A, 200, 1500);

    // Scroll to Parental Controls
    for (int i = 0; i < 15; i++)
        pressBtn(HidNpadButton_Down, 80, 150);
    pressBtn(HidNpadButton_A, 200, 800);

    // Enter settings
    pressBtn(HidNpadButton_A, 200, 600);

    // Enter PIN
    for (int i = 0; i < PIN_LEN; i++) {
        int digit = PIN_CODE[i] - '0';
        for (int j = 0; j < digit; j++)
            pressBtn(HidNpadButton_Right, 80, 80);
        pressBtn(HidNpadButton_A, 150, 200);
    }
    pressBtn(HidNpadButton_A, 150, 800);

    // Toggle restriction ON
    pressBtn(HidNpadButton_A, 200, 500);
    // Confirm
    pressBtn(HidNpadButton_A, 200, 500);

    // Home
    pressBtn(HiddbgNpadButton_Home, 300, 800);
}

// ---- Main sysmodule loop ----
int main(int argc, char **argv)
{
    // Initialize services
    timeInitialize();
    fsInitialize();
    fsdevMountSdmc();

    // Try to initialize virtual pad
    Result rc = vpadInit();
    if (R_FAILED(rc)) {
        writeStatus("ERROR:vpad_init_failed");
    }

    // Clean up any stale command/status files
    remove(CMD_PATH);
    writeStatus("IDLE:ready");

    // Main loop - runs forever as sysmodule
    while (true) {
        // Check for commands from companion NRO
        char cmd[16] = {0};
        u32 param = 0;

        if (readCmd(cmd, sizeof(cmd), &param)) {
            if (strcmp(cmd, "UNLOCK") == 0 && g_state == STATE_IDLE) {
                g_minutes = param;
                g_state = STATE_UNLOCKING;

                // Auto-unlock parental control
                if (g_vpadReady) {
                    autoDisableParental();
                }

                // Start countdown
                g_deadline = getUnixSeconds() + (u64)g_minutes * 60ULL;
                g_state = STATE_COUNTDOWN;

                char status[64];
                snprintf(status, sizeof(status), "COUNTDOWN:%lu:%llu",
                         (unsigned long)g_minutes, (unsigned long long)g_deadline);
                writeStatus(status);

            } else if (strcmp(cmd, "STOP") == 0 && g_state == STATE_COUNTDOWN) {
                // Early stop - re-lock immediately
                g_state = STATE_LOCKING;
                if (g_vpadReady) {
                    autoEnableParental();
                }
                g_state = STATE_IDLE;
                writeStatus("IDLE:stopped_early");

            } else if (strcmp(cmd, "STATUS") == 0) {
                // Report current status
                char status[128];
                switch (g_state) {
                    case STATE_IDLE:
                        snprintf(status, sizeof(status), "IDLE:ready");
                        break;
                    case STATE_UNLOCKING:
                        snprintf(status, sizeof(status), "UNLOCKING:in_progress");
                        break;
                    case STATE_COUNTDOWN: {
                        u64 now = getUnixSeconds();
                        if (now < g_deadline) {
                            u64 rem = g_deadline - now;
                            snprintf(status, sizeof(status), "COUNTDOWN:%llu", (unsigned long long)rem);
                        } else {
                            snprintf(status, sizeof(status), "COUNTDOWN:expired");
                        }
                        break;
                    }
                    case STATE_LOCKING:
                        snprintf(status, sizeof(status), "LOCKING:in_progress");
                        break;
                }
                writeStatus(status);
            }
        }

        // Check if countdown expired
        if (g_state == STATE_COUNTDOWN) {
            u64 now = getUnixSeconds();
            if (now >= g_deadline) {
                g_state = STATE_LOCKING;
                if (g_vpadReady) {
                    autoEnableParental();
                }
                g_state = STATE_IDLE;
                writeStatus("IDLE:timer_expired");
            }
        }

        // Sleep before next poll (check every 500ms)
        svcSleepThread(500 * S_1MS);
    }

    // Cleanup (never reached in sysmodule, but good practice)
    vpadCleanup();
    fsdevUnmountAll();
    fsExit();
    timeExit();
    return 0;
}
