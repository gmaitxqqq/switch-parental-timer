# Switch Parental Control Manager

A lightweight Nintendo Switch homebrew application for managing parental control settings directly from the console. Pure `.nro` app — no sysmodule required.

**Version**: 11.4 | **Firmware**: Compatible with 22.1.0 (Atmosphere CFW)

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
