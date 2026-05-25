# Switch Parental Control Manager

A lightweight Nintendo Switch homebrew application for managing parental control settings directly from the console. Pure `.nro` app — no sysmodule required.

**Version**: 11.4 | **Firmware**: Compatible with 22.1.0 (Atmosphere CFW)

**[English](#features) | [中文文档](#中文文档)**

---

## Features

| # | Feature | Description |
|---|---------|-------------|
| 1 | **View Current Status** | Display safety level, PIN status, restriction state, play timer settings, remaining time, and per-day limits |
| 2 | **Set / Change PIN** | Launch the system PIN setup applet to create or modify the parental control PIN |
| 3 | **Unlock Temporarily** | Auto-read the current PIN and temporarily lift all parental control restrictions |
| 4 | **Set Weekly Play Time (per day)** | Configure individual play time limits for each day of the week (Sun–Sat) |
| 5 | **Set Uniform Daily Time** | Apply the same play time limit to all 7 days in one step |
| 6 | **Clear Play Time Limits** | Remove all daily play time limits and disable the timer |
| 7 | **Delete Parental Controls** | Permanently delete all parental control settings including PIN |
| 8 | **Delete Phone Pairing** | Unlink the Nintendo Switch Parental Controls smartphone app from this console |

---

## Installation

1. Download the latest `parental_control_manager.nro` from [GitHub Actions Artifacts](../../actions) or build from source
2. Copy `parental_control_manager.nro` to your SD card: `/switch/parental_control_manager.nro`
3. Launch from Homebrew Menu (hold R while opening any game/app, or use Album)

### Requirements

- Nintendo Switch with **Atmosphere CFW** (tested with AMS 1.11.1)
- Firmware **22.1.0** (other CFW versions may work but are untested)
- Homebrew Menu (hbmenu)

---

## Usage

### Controls

| Button | Action |
|--------|--------|
| **Up / Down** | Navigate menu items |
| **A** | Select / Confirm |
| **B** | Back / Cancel / Exit app |
| **X** | Quick-set all days to 15 min (in weekly timer screen) |

### Play Timer Editor Controls (Weekly / Uniform screens)

| Button | Action |
|--------|--------|
| **Up / Down** | ±1 minute |
| **Left / Right** | ±10 minutes |
| **L** | Set to 0 (blocked all day) |
| **R** | Set to no limit (65535) |
| **A** | Confirm value / Apply |
| **B** | Cancel / Back |

### Play Timer Value Reference

| Value | Meaning |
|-------|---------|
| `0` | Blocked all day — no play time allowed |
| `1–65534` | Play time limit in minutes |
| `65535` (0xFFFF) | No limit — unrestricted |

### Typical Workflow

**Set a 30-minute daily limit:**
1. Launch the app → Select "Set Uniform Daily Time"
2. Press Right ×3 (15→45), then Down ×15 (45→30), or just use Up/Down to reach 30
3. Press **A** to apply

**Temporarily unlock for the day:**
1. Launch the app → Select "Unlock Temporarily"
2. The app auto-reads your PIN and lifts restrictions — done!

**Configure different limits per day:**
1. Launch the app → Select "Set Weekly Play Time (per day)"
2. Navigate to a day, press **A** to edit
3. Adjust with Up/Down/L/R, press **A** to confirm
4. Repeat for other days, then select "Apply"

---

## Architecture

### Why Pure .nro (Not Sysmodule)

Early versions (v1–v10) attempted to implement parental control as a background **sysmodule**. This approach **fundamentally failed** because the `pctl` (Parental Control) service is not accessible from sysmodule context — `pctlInitialize()` crashes when called from a sysmodule process.

The solution: a regular `.nro` homebrew application runs as a normal user-mode process, where `pctlInitialize()` works correctly. This is a design constraint of the Switch OS — parental control IPC requires a standard application context.

### System Overview

```
┌──────────────────────────────────────────┐
│         Parental Control Manager         │
│              (.nro app)                  │
├──────────────────────────────────────────┤
│  Console UI (printf + consoleUpdate)     │
│  Pad Input (padConfigureInput API)       │
├──────────────────────────────────────────┤
│           pctl IPC Layer                 │
│  ┌────────────────────────────────────┐  │
│  │ pctlInitialize() / pctlExit()      │  │
│  │ pctlGetServiceSession_Service()    │  │
│  │ serviceDispatch / serviceDispatchIn│  │
│  │ serviceDispatchOut                 │  │
│  └────────────────────────────────────┘  │
├──────────────────────────────────────────┤
│         Switch OS (Horizon)              │
│  pctl:a / pctl:s / pctl:r / pctl        │
│  pctlauth (PIN setup applet)            │
└──────────────────────────────────────────┘
```

### File Structure

```
switch-parental-timer/
├── source/
│   └── main.c              # Complete application (single-file, ~700 lines)
├── .github/
│   └── workflows/
│       └── build.yml       # GitHub Actions CI — builds .nro artifact
├── Makefile                # devkitPro build system
└── README.md               # This file
```

---

## pctl IPC Call Reference

All IPC calls go through `pctlGetServiceSession_Service()` which returns a `Service*` handle after `pctlInitialize()`. The app uses libnx's `serviceDispatch*` family for type-safe IPC.

### Service Initialization

| Function | Notes |
|----------|-------|
| `pctlInitialize()` | Opens pctl:a → pctl:s → pctl:r → pctl in order; auto `serviceConvertToDomain` |
| `pctlExit()` | Closes the pctl session |
| `pctl_ops_reinit()` | `pctlExit()` + `pctlInitialize()` — required between certain calls to reset session state |

### Status Queries

| Cmd ID | Name | Signature | Description |
|--------|------|-----------|-------------|
| 1031 | IsRestrictionEnabled | `serviceDispatchOut(srv, 1031, bool)` | Check if parental controls are enabled |
| 1032 | GetSafetyLevel | `serviceDispatchOut(srv, 1032, u32)` | Get current safety level (0=None, 1=Custom, 2=YoungChild, 3=Child, 4=Teen) |
| 1206 | GetPinLength | `serviceDispatchOut(srv, 1206, u32)` | Get PIN length (0 = not set) |

### Play Timer Queries

| Cmd ID | Name | Signature | Description |
|--------|------|-----------|-------------|
| 1453 | IsPlayTimerEnabled | `serviceDispatchOut(srv, 1453, bool)` | Whether play timer is enabled |
| 1454 | GetPlayTimerRemainingTime | `serviceDispatchOut(srv, 1454, u64)` | Remaining time in nanoseconds |
| 1455 | IsPlayTimerRestricted | `serviceDispatchOut(srv, 1455, bool)` | Whether today's time has been exhausted |
| 145601 | GetPlayTimerSettings | `serviceDispatchOut(srv, 145601, u16[34])` | Read the full PlayTimerSettings array |

### Restriction Management

| Cmd ID | Name | Signature | Description |
|--------|------|-----------|-------------|
| 1201 | UnlockRestrictionTemporarily | `serviceDispatch(srv, 1201, .buffers={pin, len})` | Temporarily unlock with PIN buffer (HIPC pointer, In) |
| 1208 | GetPinCode | `serviceDispatchOut(srv, 1208, u32, .buffers={pin, 32})` | Read current PIN string (HIPC pointer, Out) |
| 1043 | DeleteSettings | `serviceDispatch(srv, 1043)` | Delete all parental control settings |
| 1941 | DeletePairing | `serviceDispatch(srv, 1941)` | Unlink smartphone app pairing |

### Play Timer Settings

| Cmd ID | Name | Signature | Description |
|--------|------|-----------|-------------|
| 195101 | SetPlayTimerSettingsForDebug | `serviceDispatchIn(srv, 195101, u16[34])` | Write the full PlayTimerSettings array |

### PlayTimerSettings Binary Layout (`u16[34]`)

```
Index   Field              Description
─────   ──────             ───────────
[0]     Header magic       0x0101 when any day is set, 0x0000 when clearing
[1]     Header flag        0x0001 when enabled
[2-6]   Reserved           Padding (zeros)

Per-day groups (n = 0..6 for Sun..Sat):
[7+4n+0]  Day flags        0x0600 = this day is configured
[7+4n+1]  Day enabled      0x0100 = day has a limit, 0x0000 = skip (no limit)
[7+4n+2]  Day minutes      Play time in minutes (0 = blocked, 0xFFFF = no limit)
[7+4n+3]  Day padding      Reserved (zero)

[35]    End marker         0x0000 (unused, array is 34 elements)
```

**Example**: Set Sunday = 30 min, Monday = 60 min, rest unrestricted:
```c
u16 c[34] = {0};
c[0] = 0x0101;  // header
c[1] = 0x0001;
// Sunday (n=0)
c[7]  = 0x0600; c[8]  = 0x0100; c[9]  = 30;  // 30 min
// Monday (n=1)
c[11] = 0x0600; c[12] = 0x0100; c[13] = 60;  // 60 min
// Tuesday–Saturday: leave as zeros (no limit)
```

### Key IPC Implementation Notes

1. **Session Reinitialization**: Many operations require `pctl_ops_reinit()` (close + reopen) before calling. The pctl service session state can become stale after certain commands.

2. **HIPC Pointer Buffers**: PIN-related commands use `SfBufferAttr_HipcPointer` (not map-alias). This is critical — using the wrong buffer attribute will cause IPC failures.

3. **PIN NUL Termination**: When passing the PIN to `UnlockRestrictionTemporarily` (cmd 1201), the buffer must include a NUL terminator byte after the PIN string.

4. **pctlauth Applet**: The `pctlauthRegisterPasscode()` system applet opens its own pctl session. The app must `pctlExit()` before calling it and `pctlInitialize()` after it returns, otherwise the session conflicts.

---

## Build from Source

### Prerequisites

| Dependency | Version | Notes |
|-----------|---------|-------|
| devkitA64 | Latest | ARM64 toolchain for Switch |
| libnx | Latest | Switch homebrew SDK |
| devkitPro switch-tools | Latest | nacptool, elf2nro, etc. |

### Local Build

```bash
# Set up devkitPro environment
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=/opt/devkitpro/devkitA64
export PATH=$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH

# Clone and build
git clone https://github.com/gmaitxqqq/switch-parental-timer.git
cd switch-parental-timer
make clean && make

# Output: parental_control_manager.nro
```

### GitHub Actions Build (Automatic)

Every push to `main`/`master` triggers the CI pipeline:

1. **Environment**: `devkitpro/devkita64:latest` Docker container
2. **Build**: `make clean && make`
3. **Artifact**: `parental_control_manager.nro` uploaded, retained for 30 days

You can also trigger a manual build via "Run workflow" on the Actions tab.

---

## Dependencies

### Runtime

- **Switch Horizon OS** — `pctl` service (parental control IPC)
- **Switch Horizon OS** — `pctlauth` applet (PIN setup UI)
- **Atmosphere CFW** — Required for `SetPlayTimerSettingsForDebug` (cmd 195101) which is a debug command not available on stock firmware

### Build-time

| Library | Purpose |
|---------|---------|
| **libnx** (`-lnx`) | Switch homebrew SDK — provides console, pad, pctl, service dispatch, applet APIs |
| **devkitA64 (gcc)** | ARM64 cross-compiler targeting Cortex-A57 |

No additional libraries or frameworks are required. The app uses only libnx's built-in console text UI — no Borealis, no SDL, no external UI toolkit.

---

## Limitations & Known Issues

| Issue | Explanation |
|-------|-------------|
| **No CJK text** | libnx console only supports Latin/ASCII characters. Chinese/Japanese/Korean text renders as garbled characters. UI is English-only. |
| **Console UI only** | Text-based interface using `printf` + `consoleUpdate`. No graphical UI. |
| **Manual timer enforcement** | The app sets play timer limits, but does NOT auto-close games or re-enable controls on expiry. The Switch OS itself enforces the timer by showing a notification and suspending the game. |
| **Debug command dependency** | `SetPlayTimerSettingsForDebug` (cmd 195101) requires CFW. This will NOT work on unmodified (stock) firmware. |
| **Session reinit required** | Some pctl calls require closing and reopening the service session between operations. The app handles this internally with `pctl_ops_reinit()`. |

---

## References

This project's pctl IPC implementation is based on:

- **[NX-Pctl-Manager](https://github.com/tailiang2008/NX-Pctl-Manager)** by tailiang2008 — Core pctl IPC calls, PlayTimerSettings layout, session reinit pattern
- **[Reset-Parental-Controls-NX_Mod](https://github.com/nangongjing1/Reset-Parental-Controls-NX_Mod)** by nangongjing1 — pctlInitialize/pctlExit usage, DeleteSettings (cmd 1043)

---

## Version History

| Version | Changes |
|---------|---------|
| **v11.4** | Reverted UI to English — CJK characters are garbled on Switch console (libnx limitation) |
| **v11.3** | Chinese UI, 15-min default, 1-min granularity |
| **v11.2** | Fixed black screen — added `consoleUpdate(NULL)` after all printf output |
| **v11.1** | Fixed deprecated libnx pad API — migrated to `padConfigureInput`/`padUpdate`/`padGetButtonsDown` |
| **v11.0** | Complete rewrite — abandoned sysmodule approach, pure `.nro` with pctl IPC |
| **v1–v10** | Sysmodule-based attempts — all failed because `pctlInitialize()` crashes in sysmodule context |

---

## License

This project is provided as-is for personal use. The pctl IPC calling conventions are derived from the referenced open-source projects.

---
---

# 中文文档

# Switch 家长控制管理器

一个轻量级 Nintendo Switch 自制软件，可直接在主机上管理家长控制设置。纯 `.nro` 应用，无需 sysmodule。

**版本**：11.4 | **固件**：兼容 22.1.0（Atmosphere 自制固件）

---

## 功能列表

| 编号 | 功能 | 说明 |
|------|------|------|
| 1 | **查看当前状态** | 显示安全等级、PIN 状态、限制状态、游玩计时器设置、剩余时间、每日限制 |
| 2 | **设置/修改 PIN** | 启动系统 PIN 设置小程序，创建或修改家长控制 PIN 码 |
| 3 | **临时解锁** | 自动读取当前 PIN 码，临时解除所有家长控制限制 |
| 4 | **设置每周游玩时间（按天）** | 为一周中每一天单独配置游玩时间限制（周日–周六） |
| 5 | **设置统一每日时间** | 一步将相同的游玩时间限制应用到全部 7 天 |
| 6 | **清除游玩时间限制** | 删除所有每日游玩时间限制并禁用计时器 |
| 7 | **删除家长控制** | 永久删除所有家长控制设置，包括 PIN 码 |
| 8 | **删除手机配对** | 解除 Nintendo Switch Parental Controls 手机应用与此主机的关联 |

---

## 安装

1. 从 [GitHub Actions 构建产物](../../actions) 下载最新的 `parental_control_manager.nro`，或自行编译
2. 将 `parental_control_manager.nro` 复制到 SD 卡：`/switch/parental_control_manager.nro`
3. 从自制软件菜单启动（按住 R 键打开任意游戏/应用，或通过相册进入）

### 运行要求

- Nintendo Switch，已安装 **Atmosphere 自制固件**（测试版本 AMS 1.11.1）
- 固件 **22.1.0**（其他 CFW 版本可能可用，但未经测试）
- Homebrew Menu（hbmenu）

---

## 使用说明

### 按键操作

| 按键 | 功能 |
|------|------|
| **上 / 下** | 切换菜单项 |
| **A** | 选择 / 确认 |
| **B** | 返回 / 取消 / 退出应用 |
| **X** | 快速设置所有天数为 15 分钟（在每周计时器界面中） |

### 游玩计时器编辑按键（每周/统一设置界面）

| 按键 | 功能 |
|------|------|
| **上 / 下** | ±1 分钟 |
| **左 / 右** | ±10 分钟 |
| **L** | 设为 0（当天禁止游玩） |
| **R** | 设为无限制（65535） |
| **A** | 确认数值 / 应用 |
| **B** | 取消 / 返回 |

### 游玩计时器数值说明

| 数值 | 含义 |
|------|------|
| `0` | 当天禁止游玩 — 不允许任何游戏时间 |
| `1–65534` | 游玩时间限制（分钟） |
| `65535`（0xFFFF） | 无限制 |

### 常用操作

**设置每天 30 分钟限制：**
1. 启动应用 → 选择 "Set Uniform Daily Time"
2. 按右键 ×3（15→45），再按上键 ×15（45→30），或直接用上下键调到 30
3. 按 **A** 键应用

**临时解锁当天：**
1. 启动应用 → 选择 "Unlock Temporarily"
2. 应用自动读取 PIN 码并解除限制 — 完成！

**每天设置不同限制：**
1. 启动应用 → 选择 "Set Weekly Play Time (per day)"
2. 导航到某一天，按 **A** 编辑
3. 用上下/L/R 调整，按 **A** 确认
4. 对其他天重复操作，然后选择 "Apply" 应用

---

## 架构设计

### 为什么用纯 .nro（而不是 sysmodule）

早期版本（v1–v10）尝试将家长控制实现为后台 **sysmodule**。这个方案**根本行不通**，因为 `pctl`（家长控制）服务无法从 sysmodule 上下文访问 — `pctlInitialize()` 在 sysmodule 进程中调用时会崩溃。

解决方案：普通的 `.nro` 自制应用以标准用户模式进程运行，`pctlInitialize()` 在此上下文中正常工作。这是 Switch 操作系统的设计约束 — 家长控制 IPC 需要标准应用上下文。

### 系统架构

```
┌──────────────────────────────────────────┐
│         家长控制管理器                      │
│            (.nro 应用)                    │
├──────────────────────────────────────────┤
│  控制台 UI (printf + consoleUpdate)       │
│  手柄输入 (padConfigureInput API)         │
├──────────────────────────────────────────┤
│           pctl IPC 层                     │
│  ┌────────────────────────────────────┐  │
│  │ pctlInitialize() / pctlExit()      │  │
│  │ pctlGetServiceSession_Service()    │  │
│  │ serviceDispatch / serviceDispatchIn│  │
│  │ serviceDispatchOut                 │  │
│  └────────────────────────────────────┘  │
├──────────────────────────────────────────┤
│       Switch 操作系统 (Horizon)           │
│  pctl:a / pctl:s / pctl:r / pctl        │
│  pctlauth (PIN 设置小程序)               │
└──────────────────────────────────────────┘
```

### 文件结构

```
switch-parental-timer/
├── source/
│   └── main.c              # 完整应用（单文件，约 700 行）
├── .github/
│   └── workflows/
│       └── build.yml       # GitHub Actions CI — 构建 .nro 产物
├── Makefile                # devkitPro 构建系统
└── README.md               # 本文件
```

---

## pctl IPC 调用参考

所有 IPC 调用通过 `pctlGetServiceSession_Service()` 进行，该函数在 `pctlInitialize()` 后返回 `Service*` 句柄。应用使用 libnx 的 `serviceDispatch*` 系列函数实现类型安全的 IPC。

### 服务初始化

| 函数 | 说明 |
|------|------|
| `pctlInitialize()` | 按顺序打开 pctl:a → pctl:s → pctl:r → pctl；自动执行 `serviceConvertToDomain` |
| `pctlExit()` | 关闭 pctl 会话 |
| `pctl_ops_reinit()` | `pctlExit()` + `pctlInitialize()` — 某些调用之间需要重置会话状态 |

### 状态查询

| 命令 ID | 名称 | 签名 | 说明 |
|---------|------|------|------|
| 1031 | IsRestrictionEnabled | `serviceDispatchOut(srv, 1031, bool)` | 检查家长控制是否启用 |
| 1032 | GetSafetyLevel | `serviceDispatchOut(srv, 1032, u32)` | 获取当前安全等级（0=无, 1=自定义, 2=幼童, 3=儿童, 4=青少年） |
| 1206 | GetPinLength | `serviceDispatchOut(srv, 1206, u32)` | 获取 PIN 长度（0 = 未设置） |

### 游玩计时器查询

| 命令 ID | 名称 | 签名 | 说明 |
|---------|------|------|------|
| 1453 | IsPlayTimerEnabled | `serviceDispatchOut(srv, 1453, bool)` | 游玩计时器是否启用 |
| 1454 | GetPlayTimerRemainingTime | `serviceDispatchOut(srv, 1454, u64)` | 剩余时间（纳秒） |
| 1455 | IsPlayTimerRestricted | `serviceDispatchOut(srv, 1455, bool)` | 当天时间是否已用尽 |
| 145601 | GetPlayTimerSettings | `serviceDispatchOut(srv, 145601, u16[34])` | 读取完整 PlayTimerSettings 数组 |

### 限制管理

| 命令 ID | 名称 | 签名 | 说明 |
|---------|------|------|------|
| 1201 | UnlockRestrictionTemporarily | `serviceDispatch(srv, 1201, .buffers={pin, len})` | 使用 PIN 缓冲区临时解锁（HIPC pointer, In） |
| 1208 | GetPinCode | `serviceDispatchOut(srv, 1208, u32, .buffers={pin, 32})` | 读取当前 PIN 字符串（HIPC pointer, Out） |
| 1043 | DeleteSettings | `serviceDispatch(srv, 1043)` | 删除所有家长控制设置 |
| 1941 | DeletePairing | `serviceDispatch(srv, 1941)` | 解除手机应用配对 |

### 游玩计时器设置

| 命令 ID | 名称 | 签名 | 说明 |
|---------|------|------|------|
| 195101 | SetPlayTimerSettingsForDebug | `serviceDispatchIn(srv, 195101, u16[34])` | 写入完整 PlayTimerSettings 数组 |

### PlayTimerSettings 二进制布局（`u16[34]`）

```
索引     字段              说明
─────    ──────            ───────────
[0]     头部魔数          有天数设置时为 0x0101，清除时为 0x0000
[1]     头部标志          启用时为 0x0001
[2-6]   保留              填充（零）

每日组（n = 0..6 对应 周日..周六）：
[7+4n+0]  日期标志        0x0600 = 该天已配置
[7+4n+1]  日期启用        0x0100 = 该天有限制, 0x0000 = 跳过（无限制）
[7+4n+2]  日期分钟数      游玩时间（分钟），0 = 禁止, 0xFFFF = 无限制
[7+4n+3]  日期填充        保留（零）

[35]    结束标记          0x0000（未使用，数组共 34 个元素）
```

**示例**：设置周日 = 30 分钟，周一 = 60 分钟，其余无限制：
```c
u16 c[34] = {0};
c[0] = 0x0101;  // 头部
c[1] = 0x0001;
// 周日 (n=0)
c[7]  = 0x0600; c[8]  = 0x0100; c[9]  = 30;  // 30 分钟
// 周一 (n=1)
c[11] = 0x0600; c[12] = 0x0100; c[13] = 60;  // 60 分钟
// 周二至周六：保持为零（无限制）
```

### IPC 实现要点

1. **会话重新初始化**：许多操作在调用前需要 `pctl_ops_reinit()`（关闭+重新打开）。pctl 服务会话状态在某些命令后可能过期。

2. **HIPC 指针缓冲区**：PIN 相关命令使用 `SfBufferAttr_HipcPointer`（不是 map-alias）。这很关键 — 使用错误的缓冲区属性会导致 IPC 失败。

3. **PIN NUL 终止**：向 `UnlockRestrictionTemporarily`（命令 1201）传递 PIN 时，缓冲区必须在 PIN 字符串后包含一个 NUL 终止字节。

4. **pctlauth 小程序**：`pctlauthRegisterPasscode()` 系统小程序会打开自己的 pctl 会话。应用必须在调用前 `pctlExit()`，返回后 `pctlInitialize()`，否则会话冲突。

---

## 从源码编译

### 前置条件

| 依赖 | 版本 | 说明 |
|------|------|------|
| devkitA64 | 最新 | Switch ARM64 工具链 |
| libnx | 最新 | Switch 自制软件 SDK |
| devkitPro switch-tools | 最新 | nacptool、elf2nro 等 |

### 本地编译

```bash
# 设置 devkitPro 环境
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=/opt/devkitpro/devkitA64
export PATH=$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH

# 克隆并编译
git clone https://github.com/gmaitxqqq/switch-parental-timer.git
cd switch-parental-timer
make clean && make

# 输出：parental_control_manager.nro
```

### GitHub Actions 自动构建

每次推送到 `main`/`master` 分支会触发 CI 流水线：

1. **环境**：`devkitpro/devkita64:latest` Docker 容器
2. **编译**：`make clean && make`
3. **产物**：`parental_control_manager.nro` 上传，保留 30 天

也可以在 Actions 页面点击 "Run workflow" 手动触发构建。

---

## 依赖说明

### 运行时依赖

- **Switch Horizon OS** — `pctl` 服务（家长控制 IPC）
- **Switch Horizon OS** — `pctlauth` 小程序（PIN 设置界面）
- **Atmosphere 自制固件** — `SetPlayTimerSettingsForDebug`（命令 195101）是调试命令，原厂固件不可用

### 编译时依赖

| 库 | 用途 |
|----|------|
| **libnx**（`-lnx`） | Switch 自制软件 SDK — 提供控制台、手柄、pctl、服务调度、小程序 API |
| **devkitA64 (gcc)** | 针对 Cortex-A57 的 ARM64 交叉编译器 |

不需要额外的库或框架。应用仅使用 libnx 内置的控制台文本 UI — 不使用 Borealis、SDL 或其他外部 UI 工具包。

---

## 限制与已知问题

| 问题 | 说明 |
|------|------|
| **不支持中文显示** | libnx 控制台仅支持拉丁/ASCII 字符。中文/日文/韩文会显示为乱码。UI 只能使用英文。 |
| **纯文本界面** | 基于 `printf` + `consoleUpdate` 的文本界面，无图形化 UI。 |
| **手动计时器执行** | 应用设置游玩时间限制，但不会自动关闭游戏或在到期后重新启用控制。Switch 系统本身通过显示通知和暂停游戏来执行计时器。 |
| **依赖调试命令** | `SetPlayTimerSettingsForDebug`（命令 195101）需要自制固件，在原厂固件上不可用。 |
| **需要会话重新初始化** | 某些 pctl 调用之间需要关闭并重新打开服务会话。应用内部通过 `pctl_ops_reinit()` 处理。 |

---

## 参考项目

本项目的 pctl IPC 实现基于以下开源项目：

- **[NX-Pctl-Manager](https://github.com/tailiang2008/NX-Pctl-Manager)** 作者 tailiang2008 — 核心 pctl IPC 调用、PlayTimerSettings 布局、会话重新初始化模式
- **[Reset-Parental-Controls-NX_Mod](https://github.com/nangongjing1/Reset-Parental-Controls-NX_Mod)** 作者 nangongjing1 — pctlInitialize/pctlExit 用法、DeleteSettings（命令 1043）

---

## 版本历史

| 版本 | 变更 |
|------|------|
| **v11.4** | UI 回退为英文 — CJK 字符在 Switch 控制台上显示为乱码（libnx 限制） |
| **v11.3** | 中文界面、15 分钟默认值、1 分钟粒度 |
| **v11.2** | 修复黑屏 — 在所有 printf 输出后添加 `consoleUpdate(NULL)` |
| **v11.1** | 修复已废弃的 libnx 手柄 API — 迁移到 `padConfigureInput`/`padUpdate`/`padGetButtonsDown` |
| **v11.0** | 完全重写 — 放弃 sysmodule 方案，采用纯 `.nro` + pctl IPC |
| **v1–v10** | 基于 sysmodule 的尝试 — 全部失败，因为 `pctlInitialize()` 在 sysmodule 上下文中崩溃 |

---

## 许可证

本项目按原样提供，仅供个人使用。pctl IPC 调用约定来自上述参考的开源项目。
