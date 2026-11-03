# 固件工程

> 三块板的固件都在这里。每个工程能独立编译、烧录、跑。

## 一、硬件参数（实测，2026-11-02）

| 项 | 值 |
|---|---|
| 芯片 | ESP32-S3 (QFN56) rev v0.2，双核 + LP 核 |
| 主频 | **240 MHz**（已配） |
| Flash | 16 MB（Quad，DIO 80MHz） |
| PSRAM | **8 MB Octal，80 MHz**（4 MB 读写测试通过） |
| 晶振 | 40 MHz |
| MAC | `84:c7:bb:75:82:a0` |
| 串口 | **COM17**（原生 USB-Serial/JTAG，不是 CH340） |

## 二、工程列表

| 目录 | 内容 | 状态 |
|---|---|---|
| `00_s3_bringup/` | 上电自检：芯片/Flash/PSRAM/TWAI/GPIO | ✅ 通过 |
| `01_s3_query/` | 只读查询：芯片/固件/Flash/PSRAM/内存/分区表/MAC/温度/复位原因 | ✅ COM17 实测通过（复位原因已补 USB/JTAG/eFuse 等） |

## 三、怎么编译和烧录

### 方式 1：用脚本（推荐，环境已配好）

```bash
# 只编译
python D:\GitHub_Project\_build_bringup.py "D:\GitHub_Project\CAR_Project\firmware\00_s3_bringup" build

# 编译 + 烧录
python D:\GitHub_Project\_build_bringup.py "D:\GitHub_Project\CAR_Project\firmware\00_s3_bringup" flash

# 硬件查询工程（01_s3_query）
python D:\GitHub_Project\_build_bringup.py "D:\GitHub_Project\CAR_Project\firmware\01_s3_query" build
python D:\GitHub_Project\_build_bringup.py "D:\GitHub_Project\CAR_Project\firmware\01_s3_query" flash

# 看日志
C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe D:\GitHub_Project\_serial_read.py COM17 10
```

### 方式 2：用 VSCode ESP-IDF 插件

打开工程目录 → 底部状态栏选目标 `esp32s3`、端口 `COM17` → 点 🔨 编译、⚡ 烧录、📺 监视。

⚠️ **不要用 Git Bash** 跑 `idf.py`（v6.x 明确不支持 MSYS/Mingw），要用 PowerShell 或 VSCode 终端。

## 四、踩过的坑（重要，别再踩）

### 坑 1：GCC 15.2 在 `-Og` 下会段错误

```
esp_lcd_panel_rgb.c:788:1: internal compiler error: Segmentation fault
```

ESP-IDF v6.1 默认的 Debug 优化级别（`-Og`）编 `esp_lcd` 组件时**编译器自己崩了**。

**解法**：`sdkconfig.defaults` 里改用 `-O2`：

```ini
CONFIG_COMPILER_OPTIMIZATION_PERF=y
```

### 坑 2：组件校验警告被当成 error

```
Private include directory ... belongs to component wpa_supplicant but is being used by component esp_wifi
```

ESP-IDF 自己内部的组件声明不规范，v6.1 把这**警告**当**错误**处理，编译直接失败。

**解法**：

```ini
CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS=y
```

### 坑 3：GPIO43 / GPIO44 被串口占用

启动日志里会打：

```
GPIO 44 and 43 are used as console UART I/O pins
```

**这两个脚不能做别的功能**（除非把日志改到 USB CDC）。

画底板排引脚时要避开。

## 五、ESP32-S3 关键引脚（画板用）

| 用途 | 引脚 | 说明 |
|---|---|---|
| CAN TX | **GPIO4** | TWAI 默认 |
| CAN RX | **GPIO5** | TWAI 默认 |
| 串口日志 TX | GPIO43 | 被占用 |
| 串口日志 RX | GPIO44 | 被占用 |
| WS2812B | GPIO48 | 板载 RGB 灯 |
| USB D- | GPIO19 | 原生 USB |
| USB D+ | GPIO20 | 原生 USB |
| 启动模式 | GPIO0 | 低电平进下载模式 |
| 复位 | CHIP_PU | |

## 六、下一步

| 优先级 | 工程 | 依赖 |
|---|---|---|
| 1 | `01_s3_can_echo`：TWAI 自发自收 + 与 F407 对发 | CAN 收发器到货 |
| 2 | `02_s3_wifi_ap`：AP + WebSocket 手机控制 | 无 |
| 3 | `03_s3_camera`：OV2640 MJPEG 推流 | 摄像头模组 |
| 4 | `10_f407_motor`：1 kHz 速度环 | 核心板 + 面包板 |
| 5 | `20_f103_power`：INA226 + CAN 0x300 | 核心板 + 面包板 |
