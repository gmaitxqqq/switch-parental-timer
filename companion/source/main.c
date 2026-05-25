// ParentalTimer Companion App v9
// =============================================================
// NRO app launched from Homebrew Menu (Album).
// UI: select duration -> sends command to sysmodule -> done.
//
// Communication: writes "UNLOCK <minutes>" to
//   sdmc:/parental_timer.cmd
// The sysmodule reads this file, unlocks parental control
// via IPC (instant, no button simulation), starts timer,
// and auto re-locks when time's up.
// =============================================================

#include <switch.h>
#include <stdio.h>
#include <string.h>

#define DEF_MIN    15
#define CMD_PATH   "sdmc:/parental_timer.cmd"
#define STATUS_PATH "sdmc:/parental_timer.status"

static void writeCmd(const char *cmd)
{
    FILE *f = fopen(CMD_PATH, "w");
    if (f) {
        fprintf(f, "%s", cmd);
        fclose(f);
    }
}

static void readStatus(char *buf, size_t size)
{
    FILE *f = fopen(STATUS_PATH, "r");
    if (f) {
        if (fgets(buf, size, f)) {
            // Remove trailing newline
            char *nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
        }
        fclose(f);
    } else {
        snprintf(buf, size, "UNKNOWN:sysmodule_not_running");
    }
}

int main(int argc, char **argv)
{
    consoleInit(NULL);
    fsInitialize();
    fsdevMountSdmc();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    u32 min = DEF_MIN;
    bool started = false;
    bool done = false;

    while (appletMainLoop() && !done) {
        padUpdate(&pad);
        u64 k = padGetButtonsDown(&pad);

        consoleClear();
        printf("\n\n");
        printf("  ===============================\n");
        printf("   Parental Timer Companion v9\n");
        printf("  ===============================\n\n");

        if (!started) {
            // Duration selection screen
            printf("   PIN: 8473 (auto)\n\n");
            printf("   Play time: [ %d min ]\n\n", min);
            printf("   Up/Down:    +/- 1 min\n");
            printf("   Left/Right: +/- 5 min\n");
            printf("   A / + :     START TIMER\n");
            printf("   B :         EXIT\n\n");

            if (k & HidNpadButton_Up)    { if (min < 180) min++; }
            if (k & HidNpadButton_Down)  { if (min > 1)   min--; }
            if (k & HidNpadButton_Right) { if (min <= 175) min += 5; else min = 180; }
            if (k & HidNpadButton_Left)  { if (min >= 6)   min -= 5; else min = 1;   }

            if (k & (HidNpadButton_A | HidNpadButton_Plus)) {
                // Send UNLOCK command to sysmodule
                char cmd[32];
                snprintf(cmd, sizeof(cmd), "UNLOCK %u", min);
                writeCmd(cmd);
                started = true;
            }
            if (k & HidNpadButton_B) {
                done = true;
            }
        } else {
            // Running screen - show status from sysmodule
            char status[128] = {0};
            readStatus(status, sizeof(status));

            printf("   Timer started: %d min\n\n", min);
            printf("   Sysmodule status:\n");
            printf("   %s\n\n", status);

            if (strncmp(status, "COUNTDOWN:", 10) == 0) {
                unsigned long long remaining = 0;
                if (sscanf(status + 10, "%llu", &remaining) == 1) {
                    unsigned long mm = (unsigned long)(remaining / 60);
                    unsigned long ss = (unsigned long)(remaining % 60);
                    printf("   Remaining: %02lu:%02lu\n\n", mm, ss);
                }
                printf("   Go play games now!\n");
                printf("   This app can be closed.\n\n");
                printf("   X : STOP TIMER EARLY\n");
                printf("   B : EXIT\n");

                if (k & HidNpadButton_X) {
                    writeCmd("STOP");
                }
            } else if (strncmp(status, "IDLE:timer_expired", 18) == 0) {
                printf("   ** Time's up! **\n");
                printf("   Parental control re-enabled.\n\n");
                printf("   A : Start new timer\n");
                printf("   B : EXIT\n");
                if (k & HidNpadButton_A) {
                    started = false;
                    min = DEF_MIN;
                }
            } else if (strncmp(status, "IDLE:stopped_early", 18) == 0) {
                printf("   ** Timer stopped **\n");
                printf("   Parental control re-enabled.\n\n");
                printf("   A : Start new timer\n");
                printf("   B : EXIT\n");
                if (k & HidNpadButton_A) {
                    started = false;
                    min = DEF_MIN;
                }
            } else if (strncmp(status, "ERROR", 5) == 0) {
                printf("   ERROR: %s\n\n", status);
                printf("   A : Retry\n");
                printf("   B : EXIT\n");
                if (k & HidNpadButton_A) {
                    started = false;
                    min = DEF_MIN;
                }
            } else if (strncmp(status, "UNKNOWN", 7) == 0) {
                printf("   ** Sysmodule not detected! **\n\n");
                printf("   Make sure parental_timer sysmodule\n");
                printf("   is installed in:\n");
                printf("   sd:/atmosphere/contents/\n");
                printf("   4200000000003103/\n\n");
                printf("   B : EXIT\n");
            } else {
                printf("   Waiting for sysmodule...\n\n");
                printf("   B : EXIT\n");
            }

            if (k & HidNpadButton_B) {
                done = true;
            }
        }

        printf("  ===============================\n");
        consoleUpdate(NULL);
        svcSleepThread(500000000ULL); // 500ms refresh
    }

    fsdevUnmountAll();
    fsExit();
    consoleExit(NULL);
    return 0;
}
