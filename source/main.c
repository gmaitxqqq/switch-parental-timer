// Switch 家长控制管理器 v11.3
// =============================================================
// 纯 .nro homebrew 应用 — 无需 sysmodule
//
// 基于 NX-Pctl-Manager (tailiang2008) 和
// Reset-Parental-Controls-NX_Mod (nangongjing1) 的 pctl IPC 调用
//
// 关键要点:
//   - pctlInitialize() 在普通 .nro 进程中正常工作
//   - sysmodule 无法访问 pctl 服务 (v1-v10 失败根因)
//   - SetPlayTimerSettingsForDebug (cmd 195101) 兼容 fw 22.1.0
//   - UnlockRestrictionTemporarily (cmd 1201) 读取PIN后回传
//   - PlayTimerSettings 布局: u16[34], 每日组在[7+4n], 分钟在[7+4n+2]
//
// 关键: printf 后必须调 consoleUpdate(NULL) 才能刷新到屏幕!
//
// 兼容: Atmosphere CFW + fw 22.1.0 (AMS 1.11.1)
// =============================================================

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ---- 常量 ----
#define PT_DAY_NOLIMIT 0xFFFFu

// 家长控制安全等级
enum {
    PctlSafetyLevel_None       = 0,
    PctlSafetyLevel_Custom     = 1,
    PctlSafetyLevel_YoungChild = 2,
    PctlSafetyLevel_Child      = 3,
    PctlSafetyLevel_Teen       = 4,
};

// ---- Pctl 状态 ----
typedef struct {
    bool safety_level_ok;
    u32  safety_level;
    bool pin_length_ok;
    u32  pin_length;
    bool restriction_enabled_ok;
    bool restriction_enabled;
} PctlStatus;

// ---- 游玩计时器状态 ----
typedef struct {
    bool valid;
    bool enabled;
    bool restricted;
    u64  remaining_ns;
    u16  day_min[7];  // 周日..周六
} PtState;

// ---- Pctl 服务操作 (源自 NX-Pctl-Manager pctl_ops.c) ----

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
    // pctlauth 小程序会打开自己的 pctl 会话，所以调用前后需要关闭/重开
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
    // 通过 GetPinCode (cmd 1208) 读取当前 PIN，
    // 然后传给 UnlockRestrictionTemporarily (cmd 1201)
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
        case PctlSafetyLevel_None:       return "无";
        case PctlSafetyLevel_Custom:     return "自定义";
        case PctlSafetyLevel_YoungChild: return "幼童";
        case PctlSafetyLevel_Child:      return "儿童";
        case PctlSafetyLevel_Teen:       return "青少年";
        default:                         return "未知";
    }
}

// ---- 游玩计时器操作 (源自 NX-Pctl-Manager pctl_ops.c) ----

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

// ---- 手柄输入 (新版 libnx API) ----
static PadState g_pad;

static void initPad(void)
{
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
}

static u64 padGetDown(void)
{
    padUpdate(&g_pad);
    return padGetButtonsDown(&g_pad);
}

// ---- UI 辅助 ----

static const char *day_names[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

static void printSeparator(void)
{
    printf("  ========================================\n");
}

static void consoleFlush(void)
{
    consoleUpdate(NULL);
}

static void waitForKey(void)
{
    printf("\n   按任意键继续...\n");
    consoleFlush();
    while (appletMainLoop()) {
        u64 k = padGetDown();
        if (k) break;
        consoleFlush();
        svcSleepThread(10000000ULL);
    }
}

// ---- 菜单界面 ----

static void showStatus(void)
{
    consoleClear();
    printf("\n");
    printSeparator();
    printf("   当前状态\n");
    printSeparator();
    printf("\n");
    consoleFlush();

    PctlStatus st;
    pctl_status_fetch(&st);

    if (st.safety_level_ok)
        printf("   安全等级:    %s (%u)\n", safety_level_name(st.safety_level), st.safety_level);
    else
        printf("   安全等级:    (不可用)\n");

    if (st.pin_length_ok)
        printf("   PIN长度:     %u %s\n", st.pin_length, st.pin_length > 0 ? "(已设置)" : "(未设置)");
    else
        printf("   PIN长度:     (不可用)\n");

    if (st.restriction_enabled_ok)
        printf("   限制状态:    %s\n", st.restriction_enabled ? "已启用" : "未启用");
    else
        printf("   限制状态:    (不可用)\n");

    printf("\n");
    consoleFlush();

    PtState pt;
    pctl_play_timer_query(&pt);

    printf("   游玩计时器:\n");
    printf("   已启用:      %s\n", pt.enabled ? "是" : "否");
    printf("   今日受限:    %s\n", pt.restricted ? "是 (今日时间已到)" : "否");

    if (pt.remaining_ns > 0) {
        u64 rem_min = pt.remaining_ns / 60000000000ULL;
        printf("   剩余时间:    %llu 分钟\n", (unsigned long long)rem_min);
    }

    printf("\n   每日时长限制 (分钟):\n");
    for (int i = 0; i < 7; i++) {
        if (pt.day_min[i] == PT_DAY_NOLIMIT)
            printf("     %s: 无限制\n", day_names[i]);
        else
            printf("     %s: %u 分钟 (%u小时%u分钟)\n", day_names[i],
                   pt.day_min[i], pt.day_min[i] / 60, pt.day_min[i] % 60);
    }

    if (!pt.valid)
        printf("\n   (无法读取计时器设置)\n");

    waitForKey();
}

static void menuSetPin(void)
{
    consoleClear();
    printf("\n");
    printSeparator();
    printf("   设置/修改 PIN\n");
    printSeparator();
    printf("\n");
    printf("   将打开系统 PIN 设置界面。\n");
    printf("   可以设置新 PIN 或修改已有的 PIN。\n\n");
    consoleFlush();

    Result rc = pctl_set_pin();
    consoleClear();
    printf("\n");
    if (R_SUCCEEDED(rc))
        printf("   PIN 设置成功!\n");
    else
        printf("   设置失败: 0x%08X\n", (unsigned)rc);

    waitForKey();
}

static void menuUnlockTemporarily(void)
{
    consoleClear();
    printf("\n");
    printSeparator();
    printf("   临时解锁\n");
    printSeparator();
    printf("\n");
    printf("   自动读取当前 PIN 并临时解除\n");
    printf("   家长控制限制。\n\n");
    consoleFlush();

    Result rc = pctl_unlock_restriction_temporarily();
    if (R_SUCCEEDED(rc))
        printf("   解锁成功!\n");
    else
        printf("   解锁失败: 0x%08X\n", (unsigned)rc);

    waitForKey();
}

static void menuSetPlayTimer(void)
{
    u16 days[7];
    // 读取当前值
    PtState pt;
    pctl_play_timer_query(&pt);
    for (int i = 0; i < 7; i++) days[i] = pt.day_min[i];

    int cursor = 0;  // 0..6 = 每日, 7 = 应用, 8 = 取消
    bool editing_value = false;
    u16 edit_val = 0;
    bool done = false;

    while (appletMainLoop() && !done) {
        u64 k = padGetDown();

        consoleClear();
        printf("\n");
        printSeparator();
        printf("   设置每周游玩时长\n");
        printSeparator();
        printf("\n");
        printf("   每日游玩时长限制 (分钟)\n");
        printf("   0=全天禁止  65535=无限制\n\n");

        for (int i = 0; i < 7; i++) {
            bool sel = (!editing_value && cursor == i);
            if (editing_value && cursor == i) {
                printf("   %s %s [%u 分钟]  <- 编辑中\n",
                       sel ? ">" : " ",
                       day_names[i], edit_val);
                printf("     上/下: +/-1  L/R: +/-10\n");
                printf("     A=确认  B=取消\n");
            } else {
                if (days[i] == PT_DAY_NOLIMIT)
                    printf("   %s %s  无限制\n",
                           sel ? ">" : " ", day_names[i]);
                else
                    printf("   %s %s  %u 分钟 (%u小时%u分钟)\n",
                           sel ? ">" : " ", day_names[i],
                           days[i], days[i] / 60, days[i] % 60);
            }
        }

        printf("\n");
        printf("   %s [ 应用 ]\n", (!editing_value && cursor == 7) ? ">" : " ");
        printf("   %s [ 取消 ]\n", (!editing_value && cursor == 8) ? ">" : " ");
        printf("\n");
        printf("   A=选择/编辑  B=取消  X=全部设为15分钟\n");
        consoleFlush();

        if (editing_value) {
            if (k & HidNpadButton_Up)    { if (edit_val < 65535) edit_val += 1; }
            if (k & HidNpadButton_Down)  { if (edit_val > 0) edit_val -= 1; }
            if (k & HidNpadButton_Right) { if (edit_val <= 65525) edit_val += 10; }
            if (k & HidNpadButton_Left)  { if (edit_val >= 10) edit_val -= 10; else edit_val = 0; }
            if (k & HidNpadButton_R)     { edit_val = PT_DAY_NOLIMIT; }  // R = 无限制
            if (k & HidNpadButton_L)     { edit_val = 0; }               // L = 全天禁止
            if (k & HidNpadButton_A)     { days[cursor] = edit_val; editing_value = false; }
            if (k & HidNpadButton_B)     { editing_value = false; }
        } else {
            if (k & HidNpadButton_Up)   { if (cursor > 0) cursor--; }
            if (k & HidNpadButton_Down) { if (cursor < 8) cursor++; }
            if (k & HidNpadButton_A) {
                if (cursor <= 6) {
                    editing_value = true;
                    edit_val = days[cursor];
                } else if (cursor == 7) {
                    // 应用
                    Result rc = pctl_play_timer_set_days(days);
                    consoleClear();
                    printf("\n");
                    if (R_SUCCEEDED(rc))
                        printf("   游玩时长设置成功!\n");
                    else
                        printf("   设置失败: 0x%08X\n", (unsigned)rc);
                    waitForKey();
                    done = true;
                } else {
                    done = true;
                }
            }
            if (k & HidNpadButton_B) done = true;
            if (k & HidNpadButton_X) {
                // 快速设置: 全部15分钟
                for (int i = 0; i < 7; i++) days[i] = 15;
            }
        }

        svcSleepThread(50000000ULL);
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
        printf("   统一设置每日时长\n");
        printSeparator();
        printf("\n");
        printf("   为每天的游玩时长设置相同的限制。\n\n");
        if (minutes == PT_DAY_NOLIMIT)
            printf("   游玩时长: [ 无限制 ]\n\n");
        else if (minutes == 0)
            printf("   游玩时长: [ 0 分钟 (全天禁止) ]\n\n");
        else
            printf("   游玩时长: [ %u 分钟 ] (%u小时%u分钟)\n\n",
                   minutes, minutes / 60, minutes % 60);
        printf("   上/下:    +/- 1 分钟\n");
        printf("   左/右:    +/- 10 分钟\n");
        printf("   L: 全天禁止 (0分钟)\n");
        printf("   R: 无限制\n\n");
        printf("   A : 应用\n");
        printf("   B : 取消\n");
        consoleFlush();

        if (k & HidNpadButton_Up)    { if (minutes < 65535) minutes += 1; if (minutes == PT_DAY_NOLIMIT) minutes = 65534; }
        if (k & HidNpadButton_Down)  { if (minutes > 0) minutes -= 1; }
        if (k & HidNpadButton_Right) { if (minutes <= 65525) minutes += 10; }
        if (k & HidNpadButton_Left)  { if (minutes >= 10) minutes -= 10; else minutes = 0; }
        if (k & HidNpadButton_L)     { minutes = 0; }
        if (k & HidNpadButton_R)     { minutes = PT_DAY_NOLIMIT; }

        if (k & HidNpadButton_A) {
            Result rc = pctl_play_timer_set_uniform(minutes);
            consoleClear();
            printf("\n");
            if (R_SUCCEEDED(rc)) {
                if (minutes == PT_DAY_NOLIMIT)
                    printf("   已清除时长限制 (无限制)!\n");
                else if (minutes == 0)
                    printf("   已设置全天禁止!\n");
                else
                    printf("   已设置每天 %u 分钟!\n", minutes);
            } else {
                printf("   设置失败: 0x%08X\n", (unsigned)rc);
            }
            waitForKey();
            done = true;
        }
        if (k & HidNpadButton_B) done = true;

        svcSleepThread(50000000ULL);
    }
}

static void menuDeleteParentalControls(void)
{
    consoleClear();
    printf("\n");
    printSeparator();
    printf("   !!! 删除家长控制 !!!\n");
    printSeparator();
    printf("\n");
    printf("   警告: 此操作不可恢复!\n");
    printf("   将删除 PIN 和所有限制设置。\n\n");
    printf("   按 A 确认, B 取消。\n");
    consoleFlush();

    while (appletMainLoop()) {
        u64 k = padGetDown();
        if (k & HidNpadButton_A) {
            Result rc = pctl_delete_parental_controls();
            consoleClear();
            printf("\n");
            if (R_SUCCEEDED(rc))
                printf("   家长控制已删除!\n");
            else
                printf("   删除失败: 0x%08X\n", (unsigned)rc);
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
    printf("   删除手机配对\n");
    printSeparator();
    printf("\n");
    printf("   解除 Nintendo Switch 家长控制\n");
    printf("   手机 App 与此主机的绑定。\n\n");
    printf("   按 A 确认, B 取消。\n");
    consoleFlush();

    while (appletMainLoop()) {
        u64 k = padGetDown();
        if (k & HidNpadButton_A) {
            Result rc = pctl_delete_pairing();
            consoleClear();
            printf("\n");
            if (R_SUCCEEDED(rc))
                printf("   手机配对已删除!\n");
            else
                printf("   删除失败: 0x%08X\n", (unsigned)rc);
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
    printf("   清除游玩时长限制\n");
    printSeparator();
    printf("\n");
    printf("   移除每日游玩时长限制。\n");
    printf("   计时器将被关闭。\n\n");
    printf("   按 A 确认, B 取消。\n");
    consoleFlush();

    while (appletMainLoop()) {
        u64 k = padGetDown();
        if (k & HidNpadButton_A) {
            Result rc = pctl_play_timer_clear();
            consoleClear();
            printf("\n");
            if (R_SUCCEEDED(rc))
                printf("   游玩时长限制已清除!\n");
            else
                printf("   清除失败: 0x%08X\n", (unsigned)rc);
            waitForKey();
            break;
        }
        if (k & HidNpadButton_B) break;
        consoleFlush();
        svcSleepThread(10000000ULL);
    }
}

// ---- 主菜单 ----

int main(int argc, char **argv)
{
    consoleInit(NULL);

    // 立即显示启动画面
    consoleClear();
    printf("\n");
    printSeparator();
    printf("   Switch 家长控制管理器\n");
    printf("   v11.3 - 兼容 fw 22.1.0\n");
    printSeparator();
    printf("\n");
    printf("   正在初始化...\n");
    consoleFlush();

    // 初始化手柄
    initPad();

    // 初始化 pctl 服务
    Result pctl_rc = pctlInitialize();

    if (R_FAILED(pctl_rc)) {
        printf("\n   !! pctl 初始化失败: 0x%08X\n", (unsigned)pctl_rc);
        printf("   !! 请确保运行在 CFW (Atmosphere) 环境\n");
        printf("   !! 部分功能可能无法使用。\n\n");
        consoleFlush();
    } else {
        printf("   pctl 服务初始化成功。\n\n");
        consoleFlush();
    }

    int cursor = 0;
    const int menu_count = 8;
    const char *menu_items[] = {
        "查看当前状态",
        "设置/修改 PIN",
        "临时解锁",
        "设置每周游玩时长 (按天)",
        "统一设置每日时长",
        "清除游玩时长限制",
        "删除家长控制",
        "删除手机配对",
    };

    // 主循环
    while (appletMainLoop()) {
        u64 k = padGetDown();

        consoleClear();
        printf("\n");
        printSeparator();
        printf("   Switch 家长控制管理器\n");
        printf("   v11.3 - 兼容 fw 22.1.0\n");
        printSeparator();
        printf("\n");

        if (R_FAILED(pctl_rc)) {
            printf("   !! pctl 初始化失败: 0x%08X\n", (unsigned)pctl_rc);
            printf("   !! 请确保运行在 CFW 环境\n\n");
        }

        for (int i = 0; i < menu_count; i++) {
            printf("   %s %s\n", (cursor == i) ? ">" : " ", menu_items[i]);
        }

        printf("\n");
        printf("   上/下: 导航   A: 选择   B: 退出\n");

        consoleFlush();

        if (k & HidNpadButton_Up)   { if (cursor > 0) cursor--; }
        if (k & HidNpadButton_Down) { if (cursor < menu_count - 1) cursor++; }
        if (k & HidNpadButton_B) break;

        if (k & HidNpadButton_A) {
            if (R_FAILED(pctl_rc) && cursor != 0) {
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

        svcSleepThread(50000000ULL);
    }

    if (R_SUCCEEDED(pctl_rc)) pctlExit();
    consoleExit(NULL);
    return 0;
}
