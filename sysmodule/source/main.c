// ParentalTimer Sysmodule v8
// =============================================================
// Runs as Atmosphere sysmodule (background daemon).
// Communicates with companion NRO via file on SD card.
//
// Uses pctl IPC service to directly unlock/lock parental controls
// — instant, no UI navigation, no button simulation.
// Kid can keep playing while we toggle restrictions.
//
// PIN: 8473 (hardcoded) | Default: 15 min
// Title ID: 0x4200000000000001
// Install: sd:/atmosphere/contents/4200000000000001/exefs.nsp
//          sd:/atmosphere/contents/4200000000000001/flags/boot2.flag
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
static bool g_pctlReady = false;

// ---- Sysmodule initialization (following NSParentalControl pattern) ----

#ifdef __cplusplus
extern "C" {
#endif

// Minimize fs resource usage (same as NSParentalControl)
u32 __nx_fsdev_direntry_cache_size = 1;
bool __nx_fsdev_support_cwd = false;

u32 __nx_applet_type = AppletType_None;
ViLayerFlags __nx_vi_stray_layer_flags = (ViLayerFlags)0;

// Custom heap initialization for sysmodule
void __libnx_initheap(void)
{
    extern char* fake_heap_start;
    extern char* fake_heap_end;

    void* addr = NULL;
    Result rc = svcSetHeapSize(&addr, 0x200000); // 2MB heap
    if (R_SUCCEEDED(rc)) {
        fake_heap_start = (char*)addr;
        fake_heap_end   = fake_heap_start + 0x200000;
    }
}

// Custom service initialization for sysmodule
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

    rc = timeInitialize();
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 3));

    rc = pmdmntInitialize();
    if (R_FAILED(rc)) {
        // Non-fatal, we can still work without it
    }

    rc = pmshellInitialize();
    if (R_FAILED(rc)) {
        // Non-fatal
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

// ---- pctl raw IPC helpers ----
// libnx's pctlInitialize() already connects to pctl:a (with fallback)
// and creates IParentalControlService session. We use
// pctlGetServiceSession_Service() to get the Service* for raw IPC calls.

// UnlockRestrictionTemporarily (IPC Cmd 1201)
// Input: PIN code as Type-5 buffer (HipcPointer)
static Result myPctlUnlockRestrictionTemporarily(void)
{
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
static Result myPctlRevertRestrictionTemporarily(void)
{
    Service *srv = pctlGetServiceSession_Service();
    serviceAssumeDomain(srv);
    return serviceDispatch(srv, 1007);
}

// SetSafetyLevel (IPC Cmd 1033) — NOT in libnx, raw IPC
static Result myPctlSetSafetyLevel(u32 level)
{
    Service *srv = pctlGetServiceSession_Service();
    serviceAssumeDomain(srv);
    return serviceDispatchIn(srv, 1033, level);
}

// DeleteSettings (IPC Cmd 1043) — NOT in libnx, raw IPC
static Result myPctlDeleteSettings(void)
{
    Service *srv = pctlGetServiceSession_Service();
    serviceAssumeDomain(srv);
    return serviceDispatch(srv, 1043);
}

// ---- Unlock / Lock parental controls ----

static Result unlockParental(void)
{
    Result rc;

    // Method 1: UnlockRestrictionTemporarily with PIN (cleanest)
    rc = myPctlUnlockRestrictionTemporarily();
    if (R_SUCCEEDED(rc)) {
        bool unlocked = false;
        pctlIsRestrictionTemporaryUnlocked(&unlocked);
        if (unlocked) return 0;
    }

    // Method 2: SetSafetyLevel(0) — permanent change
    rc = myPctlSetSafetyLevel(0);
    if (R_SUCCEEDED(rc)) {
        bool enabled = true;
        pctlIsRestrictionEnabled(&enabled);
        if (!enabled) return 0;
    }

    // Method 3: DeleteSettings — nuclear, wipes all pctl config
    rc = myPctlDeleteSettings();
    return rc;
}

static Result lockParental(void)
{
    Result rc;

    // Method 1: Revert temporary unlock
    rc = myPctlRevertRestrictionTemporarily();
    if (R_SUCCEEDED(rc)) {
        bool enabled = false;
        pctlIsRestrictionEnabled(&enabled);
        if (enabled) return 0;
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

    // Initialize pctl service
    // libnx's pctlInitialize tries pctl:a → pctl:s → pctl:r → pctl
    rc = pctlInitialize();
    if (R_FAILED(rc)) {
        char err[64];
        snprintf(err, sizeof(err), "ERROR:pctl_init:0x%lx", (unsigned long)rc);
        writeStatus(err);
        // Don't crash — keep running so companion can see the error
    } else {
        g_pctlReady = true;
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
                if (!g_pctlReady) {
                    writeStatus("ERROR:pctl_not_ready");
                } else {
                    g_minutes = param;

                    // Unlock parental control via IPC
                    rc = unlockParental();
                    if (R_FAILED(rc)) {
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
                }

            } else if (strcmp(cmd, "STOP") == 0 && g_state == STATE_COUNTDOWN) {
                // Early stop — close game, then re-lock
                closeForegroundApp();
                svcSleepThread(500 * S_1MS);

                if (g_pctlReady) {
                    rc = lockParental();
                }
                g_state = STATE_IDLE;

                if (g_pctlReady && R_FAILED(rc)) {
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
                        if (!g_pctlReady) {
                            snprintf(status, sizeof(status), "ERROR:pctl_not_ready");
                        } else {
                            snprintf(status, sizeof(status), "IDLE:ready");
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

                if (g_pctlReady) {
                    rc = lockParental();
                }
                g_state = STATE_IDLE;

                if (g_pctlReady && R_FAILED(rc)) {
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
