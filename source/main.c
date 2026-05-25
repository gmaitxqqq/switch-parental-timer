// Switch Parental Control Manager v11.2
// =============================================================
// Pure .nro homebrew app - NO sysmodule needed.
//
// Based on proven pctl IPC calls from NX-Pctl-Manager (tailiang2008)
// and Reset-Parental-Controls-NX_Mod (nangongjing1).
//
// Key insights:
//   - pctlInitialize() works fine from a normal .nro process
//   - sysmodule CANNOT access pctl service (root cause of v1-v10 failures)
//   - SetPlayTimerSettingsForDebug (cmd 195101) sets play time on fw 22.1.0
//   - UnlockRestrictionTemporarily (cmd 1201) reads PIN via GetPinCode (cmd 1208)
//     and passes it back - works even if user forgot the PIN
//   - PlayTimerSettings layout: u16[34], per-day groups at [7+4n], minutes at [7+4n+2]
//
// CRITICAL: consoleUpdate(NULL) must be called after printf to flush to screen!
//
// Compatible: Atmosphere CFW + fw 22.1.0 (AMS 1.11.1)
// =============================================================

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ---- Constants ----
#define PT_DAY_NOLIMIT 0xFFFFu

// PctlSafetyLevel
enum {
    PctlSafetyLevel_None       = 0,
    PctlSafetyLevel_Custom     = 1,
    PctlSafetyLevel_YoungChild = 2,
    PctlSafetyLevel_Child      = 3,
    PctlSafetyLevel_Teen       = 4,
};

// ---- Pctl Status ----
typedef struct {
    bool safety_level_ok;
    u32  safety_level;
    bool pin_length_ok;
    u32  pin_length;
    bool restriction_enabled_ok;
    bool restriction_enabled;
} PctlStatus;

// ---- Play Timer State ----
typedef struct {
    bool valid;
    bool enabled;
    bool restricted;
    u64  remaining_ns;
    u16  day_min[7];  // Sun..Sat
} PtState;

// ---- Pctl Service Operations (from NX-Pctl-Manager pctl_ops.c) ----

static Result pctl_ops_reinit(void)
{
    pctlExit();
    return pctlInitialize();
}

static void pctl_status_fetch(PctlStatus *out)
{
    memset(out, 0, sizeof(*out));
    Service *srv = pctlGetServiceSession_Service();

    u32 level = 0;
    if (R_SUCCEEDED(serviceDispatchOut(srv, 1032, level))) {
        out->safety_level = level;
        out->safety_level_ok = true;
    }

    u32 pin_len = 0;
    if (R_SUCCEEDED(serviceDispatchOut(srv, 1206, pin_len))) {
        out->pin_length = pin_len;
        out->pin_length_ok = true;
    }

    bool enabled = false;
    if (R_SUCCEEDED(serviceDispatchOut(srv, 1031, enabled))) {
        out->restriction_enabled = enabled;
        out->restriction_enabled_ok = true;
    }
}

static Result pctl_set_pin(void)
{
    // pctlauth applet opens its own pctl session, so drop ours around the call
    pctlExit();
    Result rc = pctlauthRegisterPasscode();
    pctlInitialize();
    return rc;
}

static Result pctl_delete_parental_controls(void)
{
    return serviceDispatch(pctlGetServiceSession_Service(), 1043);
}

static Result pctl_delete_pairing(void)
{
    return serviceDispatch(pctlGetServiceSession_Service(), 1941);
}

static Result pctl_unlock_restriction_temporarily(void)
{
    // Read current PIN via GetPinCode (cmd 1208) and pass it back to
    // UnlockRestrictionTemporarily (cmd 1201).
    pctl_ops_reinit();
    Service *srv = pctlGetServiceSession_Service();

    char pin[32];
    memset(pin, 0, sizeof(pin));
    u32 pin_len = 0;
    Result rc = serviceDispatchOut(srv, 1208, pin_len,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_Out },
        .buffers      = { { pin, sizeof(pin) } });
    if (R_FAILED(rc)) return rc;

    size_t n = (pin_len > 0 && pin_len < (u32)sizeof(pin)) ? ((size_t)pin_len + 1) : sizeof(pin);
    return serviceDispatch(srv, 1201,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
        .buffers      = { { pin, n } });
}

static const char *safety_level_name(u32 level)
{
    switch (level) {
        case PctlSafetyLevel_None:       return "None";
        case PctlSafetyLevel_Custom:     return "Custom";
        case PctlSafetyLevel_YoungChild: return "Young Child";
        case PctlSafetyLevel_Child:      return "Child";
        case PctlSafetyLevel_Teen:       return "Teen";
        default:                         return "Unknown";
    }
}

// ---- Play Timer Operations (from NX-Pctl-Manager pctl_ops.c) ----

static void pctl_play_timer_query(PtState *out)
{
    memset(out, 0, sizeof(*out));
    for (int n = 0; n < 7; n++) out->day_min[n] = PT_DAY_NOLIMIT;
    pctl_ops_reinit();
    Service *srv = pctlGetServiceSession_Service();

    bool b = false;
    if (R_SUCCEEDED(serviceDispatchOut(srv, 1453, b))) out->enabled = b;
    b = false;
    if (R_SUCCEEDED(serviceDispatchOut(srv, 1455, b))) out->restricted = b;
    u64 rem = 0;
    if (R_SUCCEEDED(serviceDispatchOut(srv, 1454, rem))) out->remaining_ns = rem;

    u16 c[34]; memset(c, 0, sizeof(c));
    if (R_SUCCEEDED(serviceDispatchOut(srv, 145601, c))) {
        out->valid = true;
        for (int n = 0; n < 7; n++)
            out->day_min[n] = c[7 + 4 * n + 1] ? c[7 + 4 * n + 2] : PT_DAY_NOLIMIT;
    }
}

static Result pctl_play_timer_set_days(const u16 days_min[7])
{
    pctl_ops_reinit();

    bool any = false;
    for (int n = 0; n < 7; n++) if (days_min[n] != PT_DAY_NOLIMIT) any = true;

    u16 c[34] = {0};
    if (any) {
        c[0] = 0x0101;   // header magic
        c[1] = 0x0001;
        for (int n = 0; n < 7; n++) {
            if (days_min[n] == PT_DAY_NOLIMIT) continue;
            c[7 + 4 * n + 0] = 0x0600;
            c[7 + 4 * n + 1] = 0x0100;
            c[7 + 4 * n + 2] = days_min[n];
        }
    }

    return serviceDispatchIn(pctlGetServiceSession_Service(), 195101, c);
}

static Result pctl_play_timer_set_uniform(u16 minutes)
{
    u16 d[7];
    for (int i = 0; i < 7; i++) d[i] = minutes;
    return pctl_play_timer_set_days(d);
}

static Result pctl_play_timer_clear(void)
{
    u16 d[7];
    for (int i = 0; i < 7; i++) d[i] = PT_DAY_NOLIMIT;
    return pctl_play_timer_set_days(d);
}

// ---- Global Pad State (new libnx API) ----
static PadState g_pad;

static void initPad(void)
{
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
}

// Read buttons pressed since last update
static u64 padGetDown(void)
{
    padUpdate(&g_pad);
    return padGetButtonsDown(&g_pad);
}

// ---- UI Helpers ----

static const char *day_names[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static void printSeparator(void)
{
    printf("  ========================================\n");
}

// Convenience: print + flush to screen immediately
static void consoleFlush(void)
{
    consoleUpdate(NULL);
}

static void waitForKey(void)
{
    printf("\n   Press any key to continue...\n");
    consoleFlush();
    while (appletMainLoop()) {
        u64 k = padGetDown();
        if (k) break;
        consoleFlush();
        svcSleepThread(10000000ULL); // 10ms
    }
}

// ---- Menu Screens ----

static void showStatus(void)
{
    consoleClear();
    printf("\n");
    printSeparator();
    printf("   Current Status\n");
    printSeparator();
    printf("\n");
    consoleFlush();

    PctlStatus st;
    pctl_status_fetch(&st);

    if (st.safety_level_ok)
        printf("   Safety Level:    %s (%u)\n", safety_level_name(st.safety_level), st.safety_level);
    else
        printf("   Safety Level:    (unavailable)\n");

    if (st.pin_length_ok)
        printf("   PIN Length:      %u %s\n", st.pin_length, st.pin_length > 0 ? "(PIN set)" : "(no PIN)");
    else
        printf("   PIN Length:      (unavailable)\n");

    if (st.restriction_enabled_ok)
        printf("   Restriction:     %s\n", st.restriction_enabled ? "ENABLED" : "Disabled");
    else
        printf("   Restriction:     (unavailable)\n");

    printf("\n");
    consoleFlush();

    PtState pt;
    pctl_play_timer_query(&pt);

    printf("   Play Timer:\n");
    printf("   Enabled:         %s\n", pt.enabled ? "YES" : "No");
    printf("   Restricted:      %s\n", pt.restricted ? "YES (time's up today)" : "No");

    if (pt.remaining_ns > 0) {
        u64 rem_min = pt.remaining_ns / 60000000000ULL;
        printf("   Remaining:       %llu min\n", (unsigned long long)rem_min);
    }

    printf("\n   Per-day limits (minutes):\n");
    for (int i = 0; i < 7; i++) {
        if (pt.day_min[i] == PT_DAY_NOLIMIT)
            printf("     %s: No limit\n", day_names[i]);
        else
            printf("     %s: %u min (%u hr %u min)\n", day_names[i],
                   pt.day_min[i], pt.day_min[i] / 60, pt.day_min[i] % 60);
    }

    if (!pt.valid)
        printf("\n   (Could not read PlayTimerSettings)\n");

    waitForKey();
}

static void menuSetPin(void)
{
    consoleClear();
    printf("\n");
    printSeparator();
    printf("   Set / Change PIN\n");
    printSeparator();
    printf("\n");
    printf("   This will open the system PIN screen.\n");
    printf("   You can set a new PIN or change the existing one.\n\n");
    consoleFlush();

    Result rc = pctl_set_pin();
    consoleClear();
    printf("\n");
    if (R_SUCCEEDED(rc))
        printf("   PIN set successfully!\n");
    else
        printf("   Failed: 0x%08X\n", (unsigned)rc);

    waitForKey();
}

static void menuUnlockTemporarily(void)
{
    consoleClear();
    printf("\n");
    printSeparator();
    printf("   Unlock Temporarily\n");
    printSeparator();
    printf("\n");
    printf("   This temporarily lifts the parental control\n");
    printf("   restriction. It reads the current PIN and\n");
    printf("   passes it back automatically.\n\n");
    consoleFlush();

    Result rc = pctl_unlock_restriction_temporarily();
    if (R_SUCCEEDED(rc))
        printf("   Unlocked successfully!\n");
    else
        printf("   Failed: 0x%08X\n", (unsigned)rc);

    waitForKey();
}

static void menuSetPlayTimer(void)
{
    u16 days[7];
    // Initialize with current values
    PtState pt;
    pctl_play_timer_query(&pt);
    for (int i = 0; i < 7; i++) days[i] = pt.day_min[i];

    int cursor = 0;  // 0..6 = days, 7 = apply, 8 = cancel
    bool editing_value = false;
    u16 edit_val = 0;
    bool done = false;

    while (appletMainLoop() && !done) {
        u64 k = padGetDown();

        consoleClear();
        printf("\n");
        printSeparator();
        printf("   Set Play Timer (Weekly)\n");
        printSeparator();
        printf("\n");
        printf("   Per-day play time limit (minutes).\n");
        printf("   0 = blocked all day, 65535 = no limit\n\n");

        for (int i = 0; i < 7; i++) {
            bool sel = (!editing_value && cursor == i);
            if (editing_value && cursor == i) {
                printf("   %s %s [%u min]  <- editing (Up/Down +/-5, L/R +/-30)%s\n",
                       sel ? ">" : " ",
                       day_names[i], edit_val,
                       sel ? " A=confirm" : "");
            } else {
                if (days[i] == PT_DAY_NOLIMIT)
                    printf("   %s %s  No limit\n",
                           sel ? ">" : " ", day_names[i]);
                else
                    printf("   %s %s  %u min (%u hr %u min)\n",
                           sel ? ">" : " ", day_names[i],
                           days[i], days[i] / 60, days[i] % 60);
            }
        }

        printf("\n");
        printf("   %s [ Apply ]\n", (!editing_value && cursor == 7) ? ">" : " ");
        printf("   %s [ Cancel ]\n", (!editing_value && cursor == 8) ? ">" : " ");
        printf("\n");
        printf("   A = Select/Edit   B = Cancel   X = Set All Days\n");
        consoleFlush();

        if (editing_value) {
            if (k & HidNpadButton_Up)   { if (edit_val <= 65530) edit_val += 5; }
            if (k & HidNpadButton_Down) { if (edit_val >= 5) edit_val -= 5; else edit_val = 0; }
            if (k & HidNpadButton_Right){ if (edit_val <= 65495) edit_val += 30; }
            if (k & HidNpadButton_Left) { if (edit_val >= 30) edit_val -= 30; else edit_val = 0; }
            if (k & HidNpadButton_R)    { edit_val = 65535; }  // R = no limit
            if (k & HidNpadButton_L)    { edit_val = 0; }      // L = blocked
            if (k & HidNpadButton_A)    { days[cursor] = edit_val; editing_value = false; }
            if (k & HidNpadButton_B)    { editing_value = false; }
        } else {
            if (k & HidNpadButton_Up)   { if (cursor > 0) cursor--; }
            if (k & HidNpadButton_Down) { if (cursor < 8) cursor++; }
            if (k & HidNpadButton_A) {
                if (cursor <= 6) {
                    // Start editing this day
                    editing_value = true;
                    edit_val = days[cursor];
                } else if (cursor == 7) {
                    // Apply
                    Result rc = pctl_play_timer_set_days(days);
                    consoleClear();
                    printf("\n");
                    if (R_SUCCEEDED(rc))
                        printf("   Play timer set successfully!\n");
                    else
                        printf("   Failed: 0x%08X\n", (unsigned)rc);
                    waitForKey();
                    done = true;
                } else {
                    // Cancel
                    done = true;
                }
            }
            if (k & HidNpadButton_B) done = true;
            if (k & HidNpadButton_X) {
                // Quick: set all days to same value
                u16 val = 15; // default 15 min
                for (int i = 0; i < 7; i++) days[i] = val;
            }
        }

        svcSleepThread(50000000ULL); // 50ms
    }
}

static void menuSetUniformTimer(void)
{
    u16 minutes = 15;
    bool done = false;

    while (appletMainLoop() && !done) {
        u64 k = padGetDown();

        consoleClear();
        printf("\n");
        printSeparator();
        printf("   Set Uniform Daily Timer\n");
        printSeparator();
        printf("\n");
        printf("   Set the same play time limit for\n");
        printf("   every day of the week.\n\n");
        printf("   Play time: [ %u min ] (%u hr %u min)\n\n",
               minutes, minutes / 60, minutes % 60);
        printf("   Up/Down:    +/- 5 min\n");
        printf("   Left/Right: +/- 30 min\n");
        printf("   L: Set to 0 (block all day)\n");
        printf("   R: Set to unlimited\n\n");
        printf("   A : Apply\n");
        printf("   B : Cancel\n");
        consoleFlush();

        if (k & HidNpadButton_Up)    { if (minutes <= 65530) minutes += 5; }
        if (k & HidNpadButton_Down)  { if (minutes >= 5) minutes -= 5; else minutes = 0; }
        if (k & HidNpadButton_Right) { if (minutes <= 65495) minutes += 30; }
        if (k & HidNpadButton_Left)  { if (minutes >= 30) minutes -= 30; else minutes = 0; }
        if (k & HidNpadButton_L)     { minutes = 0; }
        if (k & HidNpadButton_R)     { minutes = PT_DAY_NOLIMIT; }

        if (k & HidNpadButton_A) {
            Result rc = pctl_play_timer_set_uniform(minutes);
            consoleClear();
            printf("\n");
            if (R_SUCCEEDED(rc)) {
                if (minutes == PT_DAY_NOLIMIT)
                    printf("   Play timer cleared (unlimited)!\n");
                else if (minutes == 0)
                    printf("   All days blocked (0 minutes)!\n");
                else
                    printf("   Play timer set to %u min/day!\n", minutes);
            } else {
                printf("   Failed: 0x%08X\n", (unsigned)rc);
            }
            waitForKey();
            done = true;
        }
        if (k & HidNpadButton_B) done = true;

        svcSleepThread(50000000ULL); // 50ms
    }
}

static void menuDeleteParentalControls(void)
{
    consoleClear();
    printf("\n");
    printSeparator();
    printf("   !!! DELETE Parental Controls !!!\n");
    printSeparator();
    printf("\n");
    printf("   WARNING: This is IRREVERSIBLE!\n");
    printf("   It will delete the PIN and ALL restrictions.\n\n");
    printf("   Press A to confirm, B to cancel.\n");
    consoleFlush();

    while (appletMainLoop()) {
        u64 k = padGetDown();
        if (k & HidNpadButton_A) {
            Result rc = pctl_delete_parental_controls();
            consoleClear();
            printf("\n");
            if (R_SUCCEEDED(rc))
                printf("   Parental controls deleted!\n");
            else
                printf("   Failed: 0x%08X\n", (unsigned)rc);
            waitForKey();
            break;
        }
        if (k & HidNpadButton_B) break;
        consoleFlush();
        svcSleepThread(10000000ULL);
    }
}

static void menuDeletePairing(void)
{
    consoleClear();
    printf("\n");
    printSeparator();
    printf("   Delete App Pairing\n");
    printSeparator();
    printf("\n");
    printf("   This unlinks the Nintendo Switch Parental\n");
    printf("   Controls mobile app from this console.\n\n");
    printf("   Press A to confirm, B to cancel.\n");
    consoleFlush();

    while (appletMainLoop()) {
        u64 k = padGetDown();
        if (k & HidNpadButton_A) {
            Result rc = pctl_delete_pairing();
            consoleClear();
            printf("\n");
            if (R_SUCCEEDED(rc))
                printf("   App pairing deleted!\n");
            else
                printf("   Failed: 0x%08X\n", (unsigned)rc);
            waitForKey();
            break;
        }
        if (k & HidNpadButton_B) break;
        consoleFlush();
        svcSleepThread(10000000ULL);
    }
}

static void menuClearPlayTimer(void)
{
    consoleClear();
    printf("\n");
    printSeparator();
    printf("   Clear Play Timer\n");
    printSeparator();
    printf("\n");
    printf("   This removes the daily play time limit.\n");
    printf("   The play timer will be turned off.\n\n");
    printf("   Press A to confirm, B to cancel.\n");
    consoleFlush();

    while (appletMainLoop()) {
        u64 k = padGetDown();
        if (k & HidNpadButton_A) {
            Result rc = pctl_play_timer_clear();
            consoleClear();
            printf("\n");
            if (R_SUCCEEDED(rc))
                printf("   Play timer cleared!\n");
            else
                printf("   Failed: 0x%08X\n", (unsigned)rc);
            waitForKey();
            break;
        }
        if (k & HidNpadButton_B) break;
        consoleFlush();
        svcSleepThread(10000000ULL);
    }
}

// ---- Main Menu ----

int main(int argc, char **argv)
{
    consoleInit(NULL);

    // Show splash immediately so user knows app started
    consoleClear();
    printf("\n");
    printSeparator();
    printf("   Switch Parental Control Manager\n");
    printf("   v11.2 - Compatible with fw 22.1.0\n");
    printSeparator();
    printf("\n");
    printf("   Initializing...\n");
    consoleFlush();

    // Initialize pad input
    initPad();

    // Initialize pctl service
    Result pctl_rc = pctlInitialize();

    if (R_FAILED(pctl_rc)) {
        printf("\n   !! pctl init failed: 0x%08X\n", (unsigned)pctl_rc);
        printf("   !! Make sure you're running CFW (Atmosphere)\n");
        printf("   !! Some features may not work.\n\n");
        consoleFlush();
    } else {
        printf("   pctl service initialized OK.\n\n");
        consoleFlush();
    }

    int cursor = 0;
    const int menu_count = 8;
    const char *menu_items[] = {
        "View Current Status",
        "Set / Change PIN",
        "Unlock Temporarily",
        "Set Weekly Play Timer (per-day)",
        "Set Uniform Daily Timer",
        "Clear Play Timer",
        "Delete Parental Controls",
        "Delete App Pairing",
    };

    // Main loop
    while (appletMainLoop()) {
        u64 k = padGetDown();

        consoleClear();
        printf("\n");
        printSeparator();
        printf("   Switch Parental Control Manager\n");
        printf("   v11.2 - Compatible with fw 22.1.0\n");
        printSeparator();
        printf("\n");

        if (R_FAILED(pctl_rc)) {
            printf("   !! pctl init failed: 0x%08X\n", (unsigned)pctl_rc);
            printf("   !! Make sure you're running CFW (Atmosphere)\n\n");
        }

        for (int i = 0; i < menu_count; i++) {
            printf("   %s %s\n", (cursor == i) ? ">" : " ", menu_items[i]);
        }

        printf("\n");
        printf("   Up/Down: Navigate   A: Select   B: Exit\n");

        // CRITICAL: flush console buffer to screen
        consoleFlush();

        if (k & HidNpadButton_Up)   { if (cursor > 0) cursor--; }
        if (k & HidNpadButton_Down) { if (cursor < menu_count - 1) cursor++; }
        if (k & HidNpadButton_B) break;

        if (k & HidNpadButton_A) {
            if (R_FAILED(pctl_rc) && cursor != 0) {
                // Only allow viewing status even if pctl init failed
                continue;
            }

            switch (cursor) {
                case 0: showStatus(); break;
                case 1: menuSetPin(); break;
                case 2: menuUnlockTemporarily(); break;
                case 3: menuSetPlayTimer(); break;
                case 4: menuSetUniformTimer(); break;
                case 5: menuClearPlayTimer(); break;
                case 6: menuDeleteParentalControls(); break;
                case 7: menuDeletePairing(); break;
            }
        }

        svcSleepThread(50000000ULL); // 50ms
    }

    if (R_SUCCEEDED(pctl_rc)) pctlExit();
    consoleExit(NULL);
    return 0;
}
