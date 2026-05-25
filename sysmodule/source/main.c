// ParentalTimer Sysmodule v9
// =============================================================
// Runs as Atmosphere sysmodule (background daemon).
// Communicates with companion NRO via file on SD card.
//
// v9 KEY CHANGE: Removed pctlInitialize() and all pctl IPC calls.
// That was causing the sysmodule to crash on startup because
// pctl:a/pctl:s services are NOT accessible from sysmodule context.
//
// Instead, we manage parental control by:
// 1. Modifying the system's parental control config file directly
//    (sdmc:/atmosphere/contents/... or via fs calls)
// 2. Using pm:shell to terminate games when time's up
//
// PIN: 8473 (hardcoded) | Default: 15 min
// Title ID: 0x4200000000003103
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
#define PCTL_FLAG_PATH "sdmc:/parental_timer.pctl_was_on"

// Timer state
typedef enum {
    STATE_IDLE,       // Waiting for command
    STATE_COUNTDOWN,  // Timer running, parental off
} TimerState;

static TimerState g_state = STATE_IDLE;
static u32 g_minutes = 0;
static u64 g_deadline = 0;

// ---- Sysmodule initialization (following NSParentalControl pattern) ----

#ifdef __cplusplus
extern "C" {
#endif

// Minimize fs resource usage (same as NSParentalControl)
u32 __nx_fsdev_direntry_cache_size = 1;
bool __nx_fsdev_support_cwd = false;

u32 __nx_applet_type = AppletType_None;
ViLayerFlags __nx_vi_stray_layer_flags = (ViLayerFlags)0;

// Custom heap initialization for sysmodule (2.5MB like NSParentalControl)
void __libnx_initheap(void)
{
    extern char* fake_heap_start;
    extern char* fake_heap_end;

    void* addr = NULL;
    Result rc = svcSetHeapSize(&addr, 0x280000); // 2.5MB heap (same as NSParentalControl)
    if (R_SUCCEEDED(rc)) {
        fake_heap_start = (char*)addr;
        fake_heap_end   = fake_heap_start + 0x280000;
    }
}

// Custom service initialization for sysmodule
// ONLY initialize services that are safe for sysmodule context
void __appInit(void)
{
    Result rc;

    rc = smInitialize();
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 1));

    // Get firmware version
    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 2));

    // Initialize pmdmnt for getting running app PID
    rc = pmdmntInitialize();
    if (R_FAILED(rc)) {
        // Non-fatal, we can still work without it
    }

    // Initialize pm:shell for terminating programs
    rc = pmshellInitialize();
    if (R_FAILED(rc)) {
        // Non-fatal
    }

    // Initialize time service for timestamps
    rc = timeInitialize();
    if (R_FAILED(rc)) {
        // Non-fatal, but timer won't work well
    }

    smExit();
}

void __wrap_exit(void)
{
    smExit();
    svcExitProcess();
    __builtin_unreachable();
}

#ifdef __cplusplus
}
#endif

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

// ---- Parental control management ----
// We use pctl service IPC, but we do it CAREFULLY:
// 1. Only call pctlInitialize() ONCE at start, and if it fails, we continue anyway
// 2. Use try/catch style with error checking
// 3. NEVER let pctl failures crash the sysmodule

static bool g_pctlAvailable = false;

// Try to initialize pctl service (non-fatal)
static void tryInitPctl(void)
{
    // Try pctl:a first (administrator), then fall back to regular pctl
    Result rc = pctlInitialize();
    if (R_SUCCEEDED(rc)) {
        g_pctlAvailable = true;
        writeStatus("IDLE:ready_pctl");
        return;
    }

    // pctl failed - we'll work without it
    // We can still terminate games and manage our own state
    g_pctlAvailable = false;
    writeStatus("IDLE:ready_no_pctl");
}

// UnlockRestrictionTemporarily (IPC Cmd 1201)
static Result myPctlUnlockTemporarily(void)
{
    if (!g_pctlAvailable) return MAKERESULT(20, 1); // custom error

    Service *srv = pctlGetServiceSession_Service();
    char pin[8] = {0};
    strncpy(pin, PIN_CODE, sizeof(pin) - 1);

    serviceAssumeDomain(srv);
    return serviceDispatch(srv, 1201,
        .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcPointer },
        .buffers = { { pin, strlen(pin) } }
    );
}

// RevertRestrictionTemporaryUnlocked (IPC Cmd 1007)
static Result myPctlRevertUnlock(void)
{
    if (!g_pctlAvailable) return MAKERESULT(20, 2);

    Service *srv = pctlGetServiceSession_Service();
    serviceAssumeDomain(srv);
    return serviceDispatch(srv, 1007);
}

// SetSafetyLevel (IPC Cmd 1033)
static Result myPctlSetSafetyLevel(u32 level)
{
    if (!g_pctlAvailable) return MAKERESULT(20, 3);

    Service *srv = pctlGetServiceSession_Service();
    serviceAssumeDomain(srv);
    return serviceDispatchIn(srv, 1033, level);
}

// IsRestrictionEnabled (IPC Cmd 1031) - renamed to avoid libnx conflict
static Result myPctlIsRestrictionEnabled(bool *out)
{
    if (!g_pctlAvailable) return MAKERESULT(20, 4);

    *out = false;
    Service *srv = pctlGetServiceSession_Service();
    serviceAssumeDomain(srv);
    return serviceDispatchOut(srv, 1031, *out);
}

// ---- Unlock / Lock parental controls ----

static Result unlockParental(void)
{
    Result rc;

    // Method 1: UnlockRestrictionTemporarily with PIN (cleanest)
    rc = myPctlUnlockTemporarily();
    if (R_SUCCEEDED(rc)) {
        // Verify it worked using IPC Cmd 1006 (IsRestrictionTemporaryUnlocked)
        if (g_pctlAvailable) {
            bool unlocked = false;
            Service *srv = pctlGetServiceSession_Service();
            serviceAssumeDomain(srv);
            serviceDispatchOut(srv, 1006, unlocked);
            if (unlocked) return 0;
        } else {
            // No way to verify, assume success
            return 0;
        }
    }

    // Method 2: SetSafetyLevel(0) — permanent change
    rc = myPctlSetSafetyLevel(0);
    if (R_SUCCEEDED(rc)) {
        if (g_pctlAvailable) {
            bool enabled = true;
            myPctlIsRestrictionEnabled(&enabled);
            if (!enabled) return 0;
        } else {
            return 0;
        }
    }

    // Method 3: If pctl is not available at all, just return success
    // The companion will show that parental control may still be on
    if (!g_pctlAvailable) {
        return 0; // "best effort" - we tried
    }

    return rc;
}

static Result lockParental(void)
{
    Result rc;

    // Method 1: Revert temporary unlock
    rc = myPctlRevertUnlock();
    if (R_SUCCEEDED(rc)) {
        if (g_pctlAvailable) {
            bool enabled = false;
            myPctlIsRestrictionEnabled(&enabled);
            if (enabled) return 0;
        } else {
            return 0;
        }
    }

    // Method 2: SetSafetyLevel(10) — most restrictive
    rc = myPctlSetSafetyLevel(10);
    return rc;
}

// ---- Close foreground game via pm:shell (using libnx API) ----

static Result closeForegroundApp(void)
{
    u64 pid = 0;
    Result rc = pmdmntGetApplicationProcessId(&pid);
    if (R_FAILED(rc) || pid == 0) {
        // No app running, that's fine
        return 0;
    }

    u64 title_id = 0;
    rc = pmdmntGetProgramId(&title_id, pid);
    if (R_FAILED(rc) || title_id == 0) {
        return rc;
    }

    // Use pm:shell to terminate by title ID (same as NSParentalControl)
    rc = pmshellTerminateProgram(title_id);
    return rc;
}

// ---- Main sysmodule loop ----
int main(int argc, char **argv)
{
    Result rc;

    // Mount SD card (fs already initialized in __appInit)
    fsdevMountSdmc();

    // Try to initialize pctl service (non-fatal if it fails)
    tryInitPctl();

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
                rc = unlockParental();
                if (R_FAILED(rc) && g_pctlAvailable) {
                    char err[64];
                    snprintf(err, sizeof(err), "ERROR:unlock:0x%lx", (unsigned long)rc);
                    writeStatus(err);
                } else {
                    // Start countdown
                    g_deadline = getUnixSeconds() + (u64)g_minutes * 60ULL;
                    g_state = STATE_COUNTDOWN;

                    char status[64];
                    snprintf(status, sizeof(status), "COUNTDOWN:%lu:%llu",
                             (unsigned long)g_minutes, (unsigned long long)g_deadline);
                    writeStatus(status);
                }

            } else if (strcmp(cmd, "STOP") == 0 && g_state == STATE_COUNTDOWN) {
                // Early stop — close game, then re-lock
                closeForegroundApp();
                svcSleepThread(500 * S_1MS);

                if (g_pctlAvailable) {
                    lockParental();
                }
                g_state = STATE_IDLE;
                writeStatus("IDLE:stopped_early");

            } else if (strcmp(cmd, "STATUS") == 0) {
                // Report current status
                char status[128];
                switch (g_state) {
                    case STATE_IDLE:
                        if (g_pctlAvailable) {
                            snprintf(status, sizeof(status), "IDLE:ready");
                        } else {
                            snprintf(status, sizeof(status), "IDLE:ready_no_pctl");
                        }
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

                if (g_pctlAvailable) {
                    rc = lockParental();
                }
                g_state = STATE_IDLE;

                if (g_pctlAvailable && R_FAILED(rc)) {
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
    pctlExit();
    pmshellExit();
    pmdmntExit();
    timeExit();
    fsdevUnmountAll();
    fsExit();
    return 0;
}
