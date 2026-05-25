// ParentalTimer Sysmodule v5
// =============================================================
// Runs as Atmosphere sysmodule (background daemon).
// Communicates with companion NRO via file on SD card.
//
// Uses pctl IPC service to directly unlock/lock parental controls
// — no virtual pad, no UI navigation, no button simulation.
// Kid can keep playing while we toggle restrictions instantly.
//
// PIN: 8473 (hardcoded) | Default: 15 min
// Title ID: 0x4200000000000001
// Install: sd:/atmosphere/contents/4200000000000001/exefs.nsp
//          sd:/atmosphere/contents/4200000000000001/boot2.flag
//
// Communication via sd:/parental_timer.cmd:
//   "UNLOCK <minutes>"  -> unlock parental, start timer
//   "STATUS"            -> sysmodule writes back status
//   "STOP"              -> stop timer and re-lock
// =============================================================

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define PIN_CODE    "8473"
#define S_1MS       (1000000ULL)
#define S_1S        (1000000000ULL)
#define CMD_PATH    "sdmc:/parental_timer.cmd"
#define STATUS_PATH "sdmc:/parental_timer.status"

// Timer state
typedef enum {
    STATE_IDLE,       // Waiting for command
    STATE_COUNTDOWN,  // Timer running, parental off
} TimerState;

static TimerState g_state = STATE_IDLE;
static u32 g_minutes = 0;
static u64 g_deadline = 0;

// pctl:a service session (admin privilege)
static Service g_pctlSrv;
static bool g_pctlReady = false;

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

// ---- pctl service helpers ----
// These use raw IPC calls since libnx doesn't wrap the setter commands.

// Initialize pctl:a (admin) service and get IParentalControlService
static Result pctlAdminInit(void)
{
    Result rc;

    // Connect to pctl:a (admin session, allows write operations)
    rc = smGetService(&g_pctlSrv, "pctl:a");
    if (R_FAILED(rc)) {
        // Fallback: try pctl (general session)
        rc = smGetService(&g_pctlSrv, "pctl");
        if (R_FAILED(rc)) return rc;
    }

    // Call CreateService (Cmd 0) to get IParentalControlService
    // Input: u64 process_id (auto-filled by kernel)
    // Output: moved handle (IParentalControlService object)
    Service pctl_child;
    rc = serviceDispatch(&g_pctlSrv, 0,
        .out_num_objects = 1,
        .out_objects = &pctl_child
    );

    if (R_FAILED(rc)) {
        serviceClose(&g_pctlSrv);
        return rc;
    }

    // Replace g_pctlSrv with the child service (the actual IParentalControlService)
    serviceClose(&g_pctlSrv);
    g_pctlSrv = pctl_child;

    // Initialize the service (some FW versions require this)
    // Cmd 1 = Initialize (for older FW), Cmd 4000 = IsFreeCommunicationAvailable
    // We just try to use it and handle errors

    g_pctlReady = true;
    return 0;
}

static void pctlAdminExit(void)
{
    if (g_pctlReady) {
        serviceClose(&g_pctlSrv);
        g_pctlReady = false;
    }
}

// UnlockRestrictionTemporarily (IPC Cmd 1201)
// Input: PIN code as buffer
// This temporarily unlocks all parental control restrictions for the current session.
static Result pctlUnlockRestrictionTemporarily(void)
{
    if (!g_pctlReady) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    char pin[8] = {0};
    strncpy(pin, PIN_CODE, sizeof(pin) - 1);

    // Cmd 1201: UnlockRestrictionTemporarily
    // Takes a buffer containing the PIN code string
    Result rc = serviceDispatch(&g_pctlSrv, 1201,
        .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcPointer },
        .buffers = { { pin, strlen(pin) } }
    );

    return rc;
}

// RevertRestrictionTemporaryUnlocked (IPC Cmd 1007)
// Re-locks parental controls that were temporarily unlocked.
static Result pctlRevertRestrictionTemporarily(void)
{
    if (!g_pctlReady) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    // Cmd 1007: RevertRestrictionTemporaryUnlocked — no input
    return serviceDispatch(&g_pctlSrv, 1007);
}

// IsRestrictionEnabled (IPC Cmd 1031)
// Output: bool
static Result myPctlIsRestrictionEnabled(bool *enabled)
{
    if (!g_pctlReady) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    u8 tmp = 0;
    Result rc = serviceDispatchOut(&g_pctlSrv, 1031, tmp);
    *enabled = (tmp != 0);
    return rc;
}

// IsRestrictionTemporaryUnlocked (IPC Cmd 1006)
// Output: bool
static Result myPctlIsRestrictionTemporaryUnlocked(bool *unlocked)
{
    if (!g_pctlReady) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    u8 tmp = 0;
    Result rc = serviceDispatchOut(&g_pctlSrv, 1006, tmp);
    *unlocked = (tmp != 0);
    return rc;
}

// SetSafetyLevel (IPC Cmd 1033) — requires pctl:a
// SafetyLevel 0 = no restrictions
static Result pctlSetSafetyLevel(u32 level)
{
    if (!g_pctlReady) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    return serviceDispatchIn(&g_pctlSrv, 1033, level);
}

// DeleteSettings (IPC Cmd 1043) — requires pctl:a
// Deletes ALL parental control settings (nuclear option)
static Result pctlDeleteSettings(void)
{
    if (!g_pctlReady) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    return serviceDispatch(&g_pctlSrv, 1043);
}

// ---- Unlock / Lock parental controls ----

static Result unlockParental(void)
{
    Result rc;

    // Method 1: Try UnlockRestrictionTemporarily with PIN
    // This is the cleanest way — temporary, doesn't modify settings permanently
    rc = pctlUnlockRestrictionTemporarily();
    if (R_SUCCEEDED(rc)) {
        // Verify it worked
        bool unlocked = false;
        myPctlIsRestrictionTemporaryUnlocked(&unlocked);
        if (unlocked) return 0;
    }

    // Method 2: Try SetSafetyLevel(0) via pctl:a
    // This permanently changes the safety level to "no restriction"
    rc = pctlSetSafetyLevel(0);
    if (R_SUCCEEDED(rc)) return 0;

    // Method 3: Nuclear — DeleteSettings
    // Wipes ALL parental control config
    rc = pctlDeleteSettings();
    return rc;
}

static Result lockParental(void)
{
    Result rc;

    // Method 1: Revert temporary unlock
    rc = pctlRevertRestrictionTemporarily();
    if (R_SUCCEEDED(rc)) {
        // Verify restriction is back
        bool enabled = false;
        myPctlIsRestrictionEnabled(&enabled);
        if (enabled) return 0;
    }

    // Method 2: Set safety level back to something restrictive
    // Safety level 10 = most restrictive (standard for kids)
    rc = pctlSetSafetyLevel(10);
    return rc;
}

// ---- Close foreground game via pm:shell ----

static Result closeForegroundApp(void)
{
    Result rc;
    Service pmsrv;

    // Connect to pm:shell
    rc = smGetService(&pmsrv, "pm:shell");
    if (R_FAILED(rc)) return rc;

    // pm:shell Cmd 5: GetApplicationProcessId
    // Returns the PID of the currently running application
    u64 app_pid = 0;
    rc = serviceDispatchOut(&pmsrv, 5, app_pid);
    serviceClose(&pmsrv);

    if (R_FAILED(rc) || app_pid == 0) {
        // No app running, that's fine
        return 0;
    }

    // Connect to pm:dmnt (debug monitor) to terminate the process
    Service pmdmnt;
    rc = smGetService(&pmdmnt, "pm:dmnt");
    if (R_FAILED(rc)) return rc;

    // pm:dmnt Cmd 3: TerminateProcessByPid
    rc = serviceDispatchIn(&pmdmnt, 3, app_pid);
    serviceClose(&pmdmnt);

    return rc;
}

// ---- Main sysmodule loop ----
int main(int argc, char **argv)
{
    // Initialize services
    timeInitialize();
    fsInitialize();
    fsdevMountSdmc();

    // Initialize pctl:a admin service
    Result rc = pctlAdminInit();
    if (R_FAILED(rc)) {
        char err[64];
        snprintf(err, sizeof(err), "ERROR:pctl_init_failed:0x%lx", (unsigned long)rc);
        writeStatus(err);
    } else {
        writeStatus("IDLE:ready");
    }

    // Clean up any stale command file
    remove(CMD_PATH);

    // Main loop - runs forever as sysmodule
    while (true) {
        // Check for commands from companion NRO
        char cmd[16] = {0};
        u32 param = 0;

        if (readCmd(cmd, sizeof(cmd), &param)) {
            if (strcmp(cmd, "UNLOCK") == 0 && g_state == STATE_IDLE) {
                g_minutes = param;

                // Unlock parental control via IPC
                if (g_pctlReady) {
                    rc = unlockParental();
                    if (R_FAILED(rc)) {
                        char err[64];
                        snprintf(err, sizeof(err), "ERROR:unlock_failed:0x%lx", (unsigned long)rc);
                        writeStatus(err);
                        // Don't start timer if unlock failed
                        g_state = STATE_IDLE;
                    } else {
                        // Start countdown
                        g_deadline = getUnixSeconds() + (u64)g_minutes * 60ULL;
                        g_state = STATE_COUNTDOWN;

                        char status[64];
                        snprintf(status, sizeof(status), "COUNTDOWN:%lu:%llu",
                                 (unsigned long)g_minutes, (unsigned long long)g_deadline);
                        writeStatus(status);
                    }
                } else {
                    writeStatus("ERROR:pctl_not_ready");
                }

            } else if (strcmp(cmd, "STOP") == 0 && g_state == STATE_COUNTDOWN) {
                // Early stop — close game, then re-lock
                closeForegroundApp();
                svcSleepThread(500 * S_1MS); // Brief delay for process to terminate

                rc = lockParental();
                g_state = STATE_IDLE;

                if (R_FAILED(rc)) {
                    char err[64];
                    snprintf(err, sizeof(err), "IDLE:lock_failed:0x%lx", (unsigned long)rc);
                    writeStatus(err);
                } else {
                    writeStatus("IDLE:stopped_early");
                }

            } else if (strcmp(cmd, "STATUS") == 0) {
                // Report current status
                char status[128];
                switch (g_state) {
                    case STATE_IDLE:
                        snprintf(status, sizeof(status), "IDLE:ready");
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
                }
                writeStatus(status);
            }
        }

        // Check if countdown expired
        if (g_state == STATE_COUNTDOWN) {
            u64 now = getUnixSeconds();
            if (now >= g_deadline) {
                // Time's up — close game, then re-lock
                closeForegroundApp();
                svcSleepThread(500 * S_1MS);

                rc = lockParental();
                g_state = STATE_IDLE;

                if (R_FAILED(rc)) {
                    char err[64];
                    snprintf(err, sizeof(err), "IDLE:lock_failed:0x%lx", (unsigned long)rc);
                    writeStatus(err);
                } else {
                    writeStatus("IDLE:timer_expired");
                }
            }
        }

        // Sleep before next poll (check every 500ms)
        svcSleepThread(500 * S_1MS);
    }

    // Cleanup (never reached in sysmodule, but good practice)
    pctlAdminExit();
    fsdevUnmountAll();
    fsExit();
    timeExit();
    return 0;
}
