# 元驭 Hall Lab

霍尔传感器，从直线到旋转。

两种 PCB，一套采样与通信。板卡固定，磁铁移动：直线版读位移，旋转版读单圈角度。从接线、读数到校准，可以边做边看懂位置反馈。

**项目主页：** <https://ywtai3d.cn/sensor/index.html>

主页上可以拖动磁体看三路信号，打开直线和旋转的三维模型，并进入原理、打板、实验和协议说明。教学 Wiki 从 [学习路线](https://ywtai3d.cn/sensor/wiki/Home.html) 开始。

GD32E230 · 三路线性霍尔 · RS485

## 两种构型

| | HallPos · 直线 | Encoder · 旋转 |
| --- | --- | --- |
| 机械 | 板卡固定，带磁铁的滑块沿打印夹具移动 | 板卡固定，圆形磁铁座绕中心旋转 |
| 磁铁 | Φ10 × 3 mm，轴向充磁 | Φ6 × 3 mm，径向充磁 |
| 设备类型 | `0x02` | `0x01` |
| 读数单位 | 0.01 mm，0 在中间那颗霍尔，正方向指向 U4 | 0.01°，单圈周期角 |

同一套固件用命令 `0x07` 切换：载荷 `1` 为角度，`2` 为直线。切换会清掉两种模式的零点和校准，并写入 Flash。出厂默认是角度模式；直线板要先切到直线，再做基线校准。

0.01 mm / 0.01° 是协议编码单位，实际精度以测量为准。主页上的曲线是理想示意，范围不是已验证的传感量程。

## 仓库里有什么

```text
src/  inc/          应用：三路采样、直线/角度解算、RS485 协议
Firmware/          GD32E23x CMSIS 与标准外设库
gcc_startup/       启动文件
ldscripts/         链接脚本，GD32E230G6U6TR，31 KB Flash / 6 KB RAM
cmake/  CMakeLists.txt  CMakePresets.json
scripts/           Windows 下的编译与 OpenOCD 烧录
host/              Python 图形上位机
hardware/          HallPos、Encoder 原理图与 PCB PDF，以及 EDA 工程
mechanical/        HALLPOS、ENCODER 测试结构 STEP
hall.csv           采样记录
```

更完整的课程、动画和复刻步骤在项目主页，不在这个固件仓库里重复展开：

- [学习路线](https://ywtai3d.cn/sensor/wiki/Learning-Path.html)
- [八个实验](https://ywtai3d.cn/sensor/wiki/Labs.html)
- [硬件接线](https://ywtai3d.cn/sensor/wiki/Hardware-Guide.html)
- [PCB 与机械](https://ywtai3d.cn/sensor/wiki/PCB-and-Mechanics.html)
- [校准与测量](https://ywtai3d.cn/sensor/wiki/Calibration-and-Measurement.html)
- [协议参考](https://ywtai3d.cn/sensor/wiki/Protocol-Reference.html)
- [上位机](https://ywtai3d.cn/sensor/wiki/Host-Tools.html)

原理图和 PCB PDF 用来读电路和安装。嘉立创打板还需要 Gerber、钻孔等制造文件，步骤见 [打板说明](https://ywtai3d.cn/sensor/wiki/PCB-Ordering.html)。测试夹具可按 `mechanical/` 里的 STEP 打印。另备一块 USB 转 RS485 模块；USB-TTL 的 TX/RX 不能接到总线 A/B 上。

## 电路要点

MCU 为 `GD32E230G6U6TR`。三颗霍尔 `SC4703SO-TR` 输出模拟电压，经 ADC 采样：

| 器件 | 网络 | MCU 引脚 | ADC 通道 |
| --- | --- | --- | --- |
| U1 | ADC1 | PA0 | 0 |
| U2 | ADC2 | PA1 | 1 |
| U4 | ADC3 | PA4 | 4 |

`ADC3` 是原理图网络名，进 MCU 后是硬件通道 4。DMA 数组顺序为 U1、U2、U4。每路 64 倍过采样后再右移 6 位，结果仍是 12 位原码。

直线布局中相邻霍尔间距 10 mm：U1 在 −10 mm，U2 在 0，U4 在 +10 mm。解算用相邻两路下拉量的对数比值，默认敏感宽度 3.4 mm，内部结果限在约 ±15 mm。它估计的是单个磁铁在这三点附近的位置，没有长行程磁栅计数。旋转版输出与磁路周期对应的单圈角度，没有多圈绝对位置。

供电从接口 5 V 进入，经 `XC6210B332MR` 得到 3.3 V。RS485 收发器为 `SIT3088`：PA9 发送、PA10 接收，PB5 高电平发送、低电平接收。通信口为 5 V、GND、A、B。SWD 调试口为 3.3 V、GND、SWDIO（PA13）、SWCLK（PA14）。

## 编译与烧录

在 Windows PowerShell 中，于仓库根目录执行。需要 CMake 3.22+、Ninja、`arm-none-eabi-gcc`，烧录还需要带 GD32E23x 支持的 OpenOCD 和 CMSIS-DAP。脚本会查找本机 GD32 Embedded Builder 自带的 ARM GCC。

```powershell
.\scripts\build.ps1
.\scripts\build.ps1 -Preset debug
.\scripts\build.ps1 -Flash
```

默认预设是 `release`（`-Os -g`）。`debug` 为 `-O0 -g3`。产物在 `build/release/` 或 `build/debug/`：

- `Hall_Senser.elf`：烧录和调试用这个
- `Hall_Senser.bin`：起始地址 `0x08000000`
- `Hall_Senser.hex`
- `Hall_Senser.map`

工具已在 PATH 中时也可以：

```powershell
cmake --preset release
cmake --build --preset release
```

SWD 只负责下载。读数走 RS485，和烧录是两条线。第一次先只接一块板。烧录细节见 [编译与烧录](https://ywtai3d.cn/sensor/wiki/Build-and-Flash.html)；本仓库的脚本就在根目录的 `scripts/`，不必再进入 `software/firmware`。

## 上位机

图形上位机用 Python 3.10+，依赖 pyserial、PySide6、pyqtgraph、NumPy。在仓库根目录：

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r host\requirements.txt
.\.venv\Scripts\python.exe -m host
```

连接参数：115200、8N1、设备号 1。先确认供电和 A/B 接线，再读信息和采样。改设备号、波特率、模式、零点和校准会写 Flash，应答可能要几十毫秒，不要放进每个控制周期。

## 协议摘要

帧以 `0xA5 0x5A` 开头，多字节小端。CRC16 Modbus 覆盖目标地址到载荷末尾。默认设备号 1，主机地址 `0x00`，广播地址 `0xFF`。广播只执行设置，不回答。

常用命令：

| 命令 | 作用 |
| --- | --- |
| `0x01` | 读信息 |
| `0x07` | 切换测量模式：1 角度，2 直线 |
| `0x10` | 读一帧采样 |
| `0x21` / `0x22` | 设置 / 清除机械零点 |
| `0x23` / `0x24` / `0x25` | 开始、结束、清除校准 |
| `0x06` | 恢复出厂：设备号 1，115200，轮询，角度模式 |

没有确认磁铁时，位置整数固定为 `0x80000000`，标志 bit4 为 0。此时幅度和三路原码仍可观察，不要当成位置。完整帧格式、状态码和标志位在 `inc/rs485_proto.h`，讲解见 [协议参考](https://ywtai3d.cn/sensor/wiki/Protocol-Reference.html)。

## 许可

`Firmware/` 中的 CMSIS 与 GD32 标准外设库版权归 GigaDevice，条款见 `Firmware/CMSIS/LICENSE.TXT`。应用代码、图纸和机械模型的开源许可以仓库后续补充的声明为准。
