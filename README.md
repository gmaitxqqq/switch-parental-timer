# Switch 家长控制管理器

Nintendo Switch 自制软件，直接在主机上管理家长控制设置。纯 `.nro` 应用，无需 sysmodule。

**版本**：11.5 | **固件**：兼容 22.1.0（Atmosphere 自制固件）

---

## 安装

1. 从 [GitHub Actions 构建产物](../../actions) 下载最新的 `parental_control_manager.nro`
2. 复制到 SD 卡：`/switch/parental_control_manager.nro`
3. 从自制软件菜单启动（按住 R 键打开任意游戏/应用，或通过相册进入）

### 运行要求

- Nintendo Switch + **Atmosphere 自制固件**
- 固件 22.1.0（其他版本可能可用，未测试）
- Homebrew Menu（hbmenu）

---

## 首次打开 — PIN 验证

打开 app 后**必须输入 PIN 才能进入主菜单**，防止小孩随意修改设置。

- 如果系统已设置家长控制 PIN → 输入系统 PIN
- 如果系统未设置 PIN → 默认密码 `8473`

**PIN 输入操作**：

| 按键 | 功能 |
|------|------|
| 上 / 下 | 切换数字（0-9） |
| 左 / 右 | 移动光标 |
| A | 确认提交 |
| B | 退出 app |

最多 5 次尝试，输错超过 5 次自动退出。

---

## 主菜单

| 编号 | 功能 | 说明 |
|------|------|------|
| 1 | **View Current Status** | 查看安全等级、PIN 状态、限制开关、游玩计时器、每日时间限制 |
| 2 | **Set / Change PIN** | 启动系统 PIN 设置界面，创建或修改 PIN |
| 3 | **Unlock Temporarily** | 自动读取 PIN 并临时解除家长控制限制 |
| 4 | **Set Weekly Play Time** | 为一周每一天单独设置游玩时间 |
| 5 | **Set Uniform Daily Time** | 统一设置所有天的游玩时间 |
| 6 | **Clear Play Time Limits** | 清除所有每日时间限制，禁用计时器 |
| 7 | **Delete Parental Controls** | 删除所有家长控制设置（包括 PIN），不可恢复 |
| 8 | **Delete Phone Pairing** | 解除手机家长控制 App 配对 |

### 主菜单按键

| 按键 | 功能 |
|------|------|
| 上 / 下 | 切换菜单项 |
| A | 选择 |
| B | 退出 app |

---

## 游玩计时器设置

### 每周设置（按天）

进入后显示周日到周六，每天可单独设置分钟数。

| 按键 | 功能 |
|------|------|
| 上 / 下 | 切换天数 / 编辑时 ±1 分钟 |
| 左 / 右 | 编辑时 ±10 分钟 |
| A | 选中天数进入编辑 / 确认数值 |
| B | 取消编辑 / 返回 |
| X | 快速设置所有天为 15 分钟 |
| L | 设为 0（当天禁止游玩） |
| R | 设为无限制 |

### 统一每日设置

一次设置所有天的相同时间。

| 按键 | 功能 |
|------|------|
| 上 / 下 | ±1 分钟 |
| 左 / 右 | ±10 分钟 |
| L | 设为 0（禁止） |
| R | 设为无限制 |
| A | 应用 |
| B | 取消 |

### 时间值说明

| 数值 | 含义 |
|------|------|
| 0 | 当天禁止游玩 |
| 1–65534 | 游玩时间（分钟） |
| 65535 | 无限制 |

---

## 常用操作示例

**设置每天 30 分钟限制：**
1. 主菜单 → 选 "Set Uniform Daily Time"
2. 用上下/左右调到 30 分钟
3. 按 A 应用

**临时解锁当天：**
1. 主菜单 → 选 "Unlock Temporarily"
2. 自动读取 PIN 并解除限制，完成

**每天不同时间：**
1. 主菜单 → 选 "Set Weekly Play Time"
2. 选中某天按 A 进入编辑
3. 上下/L/R 调整，按 A 确认
4. 全部设好后选 "Apply"

---

## 已知限制

| 问题 | 说明 |
|------|------|
| 不支持中文 | Switch 控制台只有英文字体，中文会乱码，UI 只能用英文 |
| 纯文本界面 | 没有图形化 UI |
| 计时器由系统执行 | App 设置时间限制，到时由 Switch 系统自动暂停游戏并通知 |
| 需要自制固件 | SetPlayTimerSettingsForDebug 命令需要 CFW，原厂固件不可用 |

---

## 从源码编译

需要 devkitA64 + libnx。每次推送到 GitHub 会自动通过 Actions 构建。

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=/opt/devkitpro/devkitA64
export PATH=$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH

git clone https://github.com/gmaitxqqq/switch-parental-timer.git
cd switch-parental-timer
make clean && make
```

---

## 版本历史

| 版本 | 变更 |
|------|------|
| **v11.5** | 新增 PIN 验证门控 — 打开 app 必须输入系统 PIN，默认密码 8473 |
| **v11.4** | UI 回退英文 — CJK 字体在 Switch 上乱码 |
| **v11.3** | 中文界面、15 分钟默认、1 分钟粒度 |
| **v11.2** | 修复黑屏 — consoleUpdate(NULL) |
| **v11.1** | 修复已废弃手柄 API |
| **v11.0** | 完全重写 — 放弃 sysmodule，纯 .nro + pctl IPC |
| **v1–v10** | sysmodule 方案 — 全部失败（pctl 在 sysmodule 中不可用） |
