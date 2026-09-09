# GreenTags LT213A · OpenDisplay / Mynewt / NimBLE

nRF51822 xxAB：**128 KiB Flash / 16 KiB RAM**。
Mynewt 1.15.0 + NimBLE 1.10.0，BLE Host/Controller 同机运行，固件从地址 0 启动，无 SoftDevice。

固件 0.2.0 已实现配置持久化、Direct/PIPE 原始和压缩传输、局部更新、ETag、认证加密、重启和芯片遥测。
已交叉编译并通过主机测试，**未烧录或实机验证**。
DFU/OTA 未实现；无 LED、蜂鸣器、NFC、电源锁存或唤醒按键，对应不可用命令返回协议错误。

完整命令表、行为和限制见 [协议覆盖说明](docs/protocol-coverage.md)。

## 硬件

GDEW0213T5，104 × 212 黑白屏，每帧 2,756 字节。

| 信号 | nRF51 GPIO | 行为 |
|---|---|---|
| MOSI / SDA | P0.30 | MSB first，只输出 |
| SCK | P0.0 | 软件 SPI，空闲低 |
| CS | P0.1 | 低有效 |
| DC | P0.2 | 低命令、高数据 |
| RESET | P0.3 | 低有效 |
| BUSY | P0.4 | **低忙、高就绪** |
| BS | P0.5 | 输出低，四线 SPI |

驱动参考 Good Display 的 `GDEW0213T5_Arduino_20191016` 全刷示例及 `GDEW0213T5_Arduino_P20201021` 局刷示例/LUT。
图像直接流向屏幕 RAM，不分配 framebuffer。屏幕操作位于独立任务，BUSY 等待会让出 CPU，每阶段最多 15 秒。

使用 **LFRC**，启动时校准，之后硬件定时器每约 2 秒触发校准；BLE SCA 配置为 500 ppm。
BSP 通过 HFXO 引用计数与 NimBLE 共享晶振。`tools/patch_mynewt.py` 仅为本板提供的校准实现放开 Mynewt 的 LFRC 配置限制，构建和依赖下载脚本自动应用。
省电行为及实测指南见 [待机省电](docs/power-saving.md)。

## 构建和测试

需要 Go（此 Newt 版本使用 Go 1.24.10 工具链）、ARM GCC、Python 3.10+、主机 C 编译器、Git、curl 和 tar。本机已验证 ARM GCC 15.3.1。

Newt 按源码安装。下面固定到当前已验证的源码提交，其 `newt version` 显示 1.15.0：

```sh
git clone https://github.com/apache/mynewt-newt
cd mynewt-newt
git checkout ae817d0ccbfbae1e1e020d086a6d3c50a8687206
go install ./newt
export PATH="$(go env GOPATH)/bin:$PATH"
newt version
cd ..
```

然后在本固件仓库根目录执行：

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements-test.txt
./tools/bootstrap.sh
./tools/build.sh
./tools/test.sh
```

`bootstrap.sh` 下载固定标签依赖并应用本板的 LFRC 配置补丁，不要求下载完整依赖 Git 历史；已有 `repos/` 子目录会保留。也可用 `newt upgrade` 获取依赖，再通过 `tools/build.sh` 应用补丁。只运行主机测试不需要 Newt 或 ARM GCC，但仍需先下载依赖。

构建和测试脚本从 Git HEAD 生成版本头（12 位 commit SHA，不包含未提交改动标记；不纳入 Git）。`00 43` 返回版本、SHA 和 patch，18 字节响应可用于 ATT MTU 23。

构建脚本生成配置记录、编译、检查 ELF 尺寸并导出：

```text
bin/targets/lt213a/app/apps/opendisplay/opendisplay.elf
bin/targets/lt213a/app/apps/opendisplay/opendisplay.hex
bin/targets/lt213a/app/apps/opendisplay/opendisplay.bin
```

BIN 烧录地址为 **0**，HEX 已含绝对地址。不要用 `newt create-image` 添加 MCUboot 镜像头。

应用可用 Flash 为 **118 KiB**，最后 **10 KiB** 是两个独立配置槽。
链接脚本还要求至少 **2 KiB 堆区**。实际数值由 `tools/check_size.py` 从 ELF 计算；堆区还会被 NimBLE GATT 初始化及加密临时分配消耗，不是启动后的空闲量。

当前使用 OpenDisplay 的流式 uzlib（固定版本见 `libs/od-uzlib/UPSTREAM.md`），
静态 512 B 历史窗口，不使用解压堆分配。适配层每次耗尽输入后才归还命令缓冲，输出使用 16 B 栈缓冲。
PIPE 保持窗口 2，当前包直接处理，只缓存一个提前到达的后续包。

本次构建：应用 Flash **92,100 B**，静态 RAM **13,928 B**，中断栈 **384 B**，初始堆区 **2,072 B**。

Cortex-M0 的任务栈参数按 32 位字计：主任务 384 字、屏幕任务 384 字、LL 任务 256 字。
构建生成 `.su` 栈使用报告；认证函数单独保留栈帧，避免把 AES 认证临时内存叠加到普通加密响应路径。实机栈高水位仍需测量。

## BLE 与上传

设备名为 `OD` 加地址末三字节，例如 `OD12AB34`。服务与特征 UUID 都是：

```text
00002446-0000-1000-8000-00805f9b34fb
```

单连接，支持 Write、Write Without Response、Notify；须先订阅通知。
BLE 空闲 120 秒后主动断连，命令完成后重新计时；未完成图像传输从 START 起另有 15 分钟绝对期限。安全配置中的 session_timeout 按认证后的总时长计算，命令不续期（0 表示禁用认证到期）。

Direct 模式逐命令等待 ACK。PIPE 模式遵守协商后的 1–2 包窗口和 SACK，不能按 32 包硬发。
最大 ATT MTU 247，也支持 MTU 23；认证和配置写入需要更大的 MTU。

图像按行排列，每行 13 字节，MSB 对应左像素，`1=白 / 0=黑`。
压缩使用 zlib **512 字节窗口**；不支持通用 32 KiB 窗口。局部流为旧区域位图接新区域位图，需要匹配 ETag。

未启用加密时，内置脚本可用于首次上板验证：

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements-upload.txt
python tools/upload.py --device OD12AB34 --pattern checkerboard
python tools/upload.py --device OD12AB34 --pattern white --compress
python tools/upload.py --device OD12AB34 --image image-104x212.png --compress
python tools/upload.py --device OD12AB34 --raw frame.bin
```

设备参数也支持 BLE 地址或 macOS UUID。脚本不缩放图片，必须为 104×212。
脚本会等到 `00 73` 刷新完成通知才报告成功；`00 72` ACK 本身不表示屏幕更新完成。

PIPE、加密和自动局部更新可通过官方 `py-opendisplay` 客户端验证，例如：

```python
import asyncio
import os
from PIL import Image
from opendisplay import OpenDisplayDevice

async def main():
    key = bytes.fromhex(os.environ['OD_KEY']) if 'OD_KEY' in os.environ else None
    async with OpenDisplayDevice(device_name='OD12AB34', encryption_key=key) as device:
        await device.upload_image(Image.open('image-104x212.png'), compress=True)

asyncio.run(main())
```

固件已有与 Python `cryptography` 对照的加密测试；py-opendisplay 明文无线互通与绘制已上板验证（py-opendisplay 7.16.0，版本 SHA 差异与连接窗口限制见[实板点亮与调试记录](docs/hardware-bringup.md)），加密连接互通仍需上板确认。
配置默认不开启加密，通过标准 SecurityConfig 记录设置密钥后启用。没有密钥恢复按键，遗失密钥需 SWD 恢复配置区。

## SWD

实板已验证：启动、BLE 同步与广播正常（快广播窗口），串口无输出（控制台为 stub，预期行为）；py-opendisplay 无线互通与绘制已打通（含压缩直写与刷新完成通知，前提与限制见调试记录）。详细刷机流程、兼容芯片调试限制与已知问题见[实板点亮与调试记录](docs/hardware-bringup.md)。

连接 SWDIO、SWCLK、GND 和目标电压参考后，可用 J-Link Commander，设备 `nRF51822_xxAB`、SWD、1000 kHz：

```text
r
h
loadfile bin/targets/lt213a/app/apps/opendisplay/opendisplay.hex
r
g
q
```

也可用任意 CMSIS-DAP 探头配 OpenOCD（配置见 `tools/openocd-lt213a.cfg`，首次刷机前先整片备份原厂固件）。

普通应用 HEX 不包含配置槽。没有 bootloader，`0051` 不能进入不存在的升级服务；若继续做 OTA，需要单独设计、实现并验证 bootloader/镜像更新布局。

上板待验证：PIPE 重传、局刷残影、加密连接、Flash 擦除期间的无线稳定性、温度/VDD 采样和任务栈余量。已知问题：慢广播期间的间歇性静默（连接只能在健康广播窗口建立，见调试记录）。

## 依据

- [OpenDisplay protocol](https://github.com/OpenDisplay/opendisplay-protocol/blob/main/src/opendisplay_protocol.h)
- [配置结构](https://github.com/OpenDisplay/opendisplay-protocol/blob/main/src/opendisplay_structs.h)
- [官方 Python 客户端](https://github.com/OpenDisplay/py-opendisplay)
- [RFC 1950 — zlib](https://www.rfc-editor.org/rfc/rfc1950)、[RFC 1951 — DEFLATE](https://www.rfc-editor.org/rfc/rfc1951)

## 仓库结构

- `apps/opendisplay/`：协议、BLE 服务、屏幕驱动、配置和安全实现。
- `hw/bsp/lt213a/`：引脚、时钟校准、链接布局。
- `libs/od-uzlib/`：固定版本的第三方流式解压器。
- `targets/lt213a/`：唯一固件构建目标。
- `tests/`、`tools/`：主机测试、依赖下载、构建和上传工具。
- `docs/`：协议覆盖、省电策略、实板点亮调试记录与实板验证步骤。

`repos/`、`bin/`、虚拟环境和调试产物不纳入 Git。CI 在 Ubuntu 上执行主机测试、交叉编译和资源检查，并上传 ELF/HEX/BIN artifact；不自动烧录或发布 Release。CI 的 ARM GCC 来自 Ubuntu，尺寸可能与本机 GCC 15.3.1 不同，最终以该次构建输出为准。

贡献指南见 [CONTRIBUTING.md](CONTRIBUTING.md)，许可证及来源见 [LICENSE](LICENSE)、[NOTICE](NOTICE) 和 [THIRD_PARTY.md](THIRD_PARTY.md)。
