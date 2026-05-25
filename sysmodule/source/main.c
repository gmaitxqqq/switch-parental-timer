// ParentalTimer Sysmodule v10
// =============================================================
// Runs as Atmosphere sysmodule (background daemon).
// Communicates with companion NRO via file on SD card.
//
// KEY DESIGN DECISIONS (learned from NSParentalControl):
// 1. NEVER call pctlInitialize() - pctl service is NOT accessible
//    from sysmodule context. NSParentalControl proves this.
// 2. Initialize services ONLY in __appInit (before smExit)
// 3. For services needed later (pm:shell, time), init/exit on-demand
// 4. Use pm:shell to terminate games when time's up
// 5. Since we can't use pctl IPC, we simply:
//    - Terminate the game when time expires
//    - The system's parental control remains active (if enabled)
//    - The child's game session is forcibly ended
//
// PIN: 8473 (hardcoded, used for documentation) | Default: 15 min
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

// Timer state
typedef enum {
    STATE_IDLE,       // Waiting for command
    STATE_COUNTDOWN,  // Timer running
} TimerState;

static TimerState g_state = STATE_IDLE;
static u32 g_minutes = 0;
static u64 g_deadline = 0;

// ---- Sysmodule initialization (following NSParentalControl pattern) ----
// This is CRITICAL: we must ONLY initialize services here, before smExit().
// Services that need sm must be init'd here or on-demand later.

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
    Result rc = svcSetHeapSize(&addr, 0x280000); // 2.5MB heap
    if (R_SUCCEEDED(rc)) {
        fake_heap_start = (char*)addr;
        fake_heap_end   = fake_heap_start + 0x280000;
    }
}

// Custom service initialization for sysmodule
// ONLY initialize services that are needed persistently.
// pm:shell and time are init'd on-demand (like NSParentalControl does).
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
    // This is safe and needed persistently
    rc = pmdmntInitialize();
    if (R_FAILED(rc)) {
        // Non-fatal, we can still work without it
    }

    // Do NOT initialize pm:shell here - init on demand like NSParentalControl
    // Do NOT initialize time here - init on demand like NSParentalControl
    // Do NOT call pctlInitialize() - it WILL crash in sysmodule context

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

// ---- Time helper (on-demand init like NSParentalControl) ----

static u64 getUnixSeconds(void)
{
    Result rc = timeInitialize();
    if (R_FAILED(rc)) return 0;

    u64 timestamp = 0;
    rc = timeGetCurrentTime(TimeType_UserSystemClock, &timestamp);
    timeExit(); // Always exit after use (like NSParentalControl's today())
    return timestamp;
}

// ---- Close foreground game via pm:shell (on-demand, like NSParentalControl) ----

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

    // Initialize pm:shell on demand (exactly like NSParentalControl does)
    rc = pmshellInitialize();
    if (R_FAILED(rc)) {
        return rc;
    }

    rc = pmshellTerminateProgram(title_id);

    // Exit pm:shell immediately after use (like NSParentalControl)
    pmshellExit();

    return rc;
}

// ---- Main sysmodule loop ----
int main(int argc, char **argv)
{
    // Mount SD card (fs already initialized in __appInit)
    fsdevMountSdmc();

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

                // Start countdown - we don't need pctl since
                // the game is already running and we just need to
                // track time and kill it when time's up
                g_deadline = getUnixSeconds() + (u64)g_minutes * 60ULL;
                g_state = STATE_COUNTDOWN;

                char status[64];
                snprintf(status, sizeof(status), "COUNTDOWN:%lu:%llu",
                         (unsigned long)g_minutes, (unsigned long long)g_deadline);
                writeStatus(status);

            } else if (strcmp(cmd, "STOP") == 0 && g_state == STATE_COUNTDOWN) {
                // Early stop — close game
                closeForegroundApp();
                svcSleepThread(500 * S_1MS);

                g_state = STATE_IDLE;
                writeStatus("IDLE:stopped_early");

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
                // Time's up — close game
                Result rc = closeForegroundApp();
                svcSleepThread(500 * S_1MS);

                g_state = STATE_IDLE;

                if (R_FAILED(rc)) {
                    char err[64];
                    snprintf(err, sizeof(err), "IDLE:terminate_failed:0x%lx", (unsigned long)rc);
                    writeStatus(err);
                } else {
                    writeStatus("IDLE:timer_expired");
                }
            }
        }

        // Sleep before next poll (check every 500ms)
        svcSleepThread(500 * S_1MS);
    }

    // Never reached in sysmodule
    fsdevUnmountAll();
    fsExit();
    return 0;
}
