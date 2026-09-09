# 实板点亮与调试记录

记录 LT213A 首次刷机与联调的结论。调试环境：CMSIS-DAP 探头（Sipeed RV-Debugger）+ OpenOCD（Homebrew）+ SWD 1000 kHz，macOS 端 bleak 空口扫描做黑盒验证。

## 刷机流程

配置见 [tools/openocd-lt213a.cfg](../tools/openocd-lt213a.cfg)。首次刷机前先备份原厂固件（128 KiB 全片），备份保留在仓库根目录 `stock-lt213a-backup.bin`（不入库，机器特定）：

```sh
openocd -f tools/openocd-lt213a.cfg -c "init" -c "halt" \
  -c "dump_image stock-lt213a-backup.bin 0x00000000 0x20000" -c "shutdown"

openocd -f tools/openocd-lt213a.cfg -c "init" -c "reset halt" \
  -c "nrf51 mass_erase 0" \
  -c "program bin/targets/lt213a/app/apps/opendisplay/opendisplay.bin verify 0" \
  -c "reset run" -c "shutdown"
```

## 兼容芯片的调试限制

这批 LT213A 用的是 nRF51822 兼容片（clone），调试口表现与原厂芯片不同：

- FICR 调试读取被屏蔽，读到掩码值（`0x55AA55AA` 模式）；CPU 侧自读正常。
- PC 读数出现 +0x10 偏移，RTC 的 CC 寄存器经调试口读回恒为 0，NVIC ISER/ISPR 读数存疑。
- gdb attach 有时会触发 external reset，且附带一次 host 同步风暴（约 25 次/秒的 `on_sync`，attach 干扰消失后停止）。

结论：**调试器判读不可信，只能用「刷入 + 空口扫描」做黑盒验证**；所有基于调试读取的定位都要打折。

## 已修复：os_time 符号陷阱（真实 Mynewt 缺陷）

`repos/apache-mynewt-core` 的 `hal_os_tick.c` 中 `rtc1_timer_handler()` 存在 int/uint 符号陷阱，触发链：

1. `os_tick_idle()` 的 WFI 被 RTC0（LL 定时器）的 pending 唤醒（PRIMASK=1 期间中断不投递）；
2. 唤醒后无条件调用 `rtc1_timer_handler()`，而此时 RTC1 compare 并未到期；
3. `sub24()` 算出负 delta，`int delta / uint32_t` 的符号除法把 ticks 放大到 ≈2^24；
4. `os_time` 一次前跳约 37 小时，callout 风暴（`adv_retry` 提前触发、NimBLE 内部定时器错乱）；
5. 系统进入 PRIMASK 卡死的热空转，链路层定时事件停发，广播静默。

修复：负 delta 直接返回（compare 仍处于武装状态，什么都不该做）。补丁持久化在 [tools/patch_mynewt.py](../tools/patch_mynewt.py)，`tools/build.sh` 构建前自动应用，可重复执行。

实测效果（bleak 扫描，我们的标签 OD742B6D vs 对照标签 ODDA67FC）：修复前 6 分钟仅 2 次发现；修复后 5 分钟 9–11 次（对照 43–69 次）。快广播窗口（0–30 秒）和 BLE 同步恢复正常，但慢广播期间仍有间歇静默（见下）。

## 未解决：慢广播期间的间歇性静默

刷入最终固件（原厂 BSP + os_time 修复）后仍存在间歇性静默：广播突发与静默窗口交替，静默长度从 40 秒到 45 分钟以上不等，之后自行恢复。静默期间：

- 主机侧 `adv_active=1`（认为自己在广播），任务正常阻塞，系统其余部分健康；
- LL 定时器中断计数 `timer_isrs` 冻结——RTC0 compare 中断不触发是核心物理症状，RTC1 的 compare 也同样错过，而两个计数器都在以正确的 32.768 kHz 走。

已用对照实验排除的假设（实验代码均已回退）：

- 周期性 LFRC 校准（CTIV=2s）：禁用后静默依旧；
- 启动时的初始校准：跳过后静默依旧；
- HFXO 按事件启停的循环干扰 LFCLK：BSP 持有永久 HFXO 引用后静默依旧。

剩余怀疑指向兼容片的中断投递/RTC compare 外设行为异常，但该芯片调试读取不可信（见上），无法进一步定位。对照标签 ODDA67FC 广播稳定（芯片批次或固件版本未知，不能作为同型对照）。如需继续排查，可行的下一步：在固件内做片上自诊断（如静默期间以 GPIO/看门狗方式记录 `timer_isrs` 冻结的现场，绕开调试口），或换正品 nRF51822 验证是否为兼容片特有。

## 串口

`/dev/cu.usbmodem*` 无输出：控制台在本固件里是 stub，属预期行为，不是故障。

## py-opendisplay 互通（已验证，带前提）

2026-09-08 19:27 实测（py-opendisplay 7.16.0 + bleak 3.0.2，macOS）：连接、`interrogate()` 配置读取（含 MTU 分片）、zlib 压缩直写（205 分块）到收到 `00 73` 刷新完成通知，端到端 11 秒，面板参数正确协商为 104x212 MONO。结论：**协议链路与绘制的无线互通已打通**。

两个前提：

1. **版本响应差异**：py-opendisplay 7.16 要求 `00 43` 版本响应携带 SHA 哈希，本固件按 canonical protocol 2.2 返回空 SHA（`shaLength=0`），显式调用 `read_firmware_version()` 或 CLI 的 interrogate 路径会抛 `InvalidResponseError`。跳过版本读取后一切正常（`OpenDisplayDevice` 自动握手读的是 `00 40` 配置，不读版本）。当前源码已在 `00 43` 响应中加入 12 位构建 commit SHA，并通过主机回归测试；此处记录的是旧固件的实板行为，更新后的 interrogate 路径尚未上板复验。
2. **必须处于健康广播窗口**：间歇静默期间标签数分钟无广播包，连接无法建立（实测静默期 4 次连接尝试全部超时；健康窗口期 20 次尝试 17 次成功，耗时 1.3–34 秒）。连接建立后的传输稳定，205 分块 + 刷新通知一次通过。另复现：刷机/复位后的启动会卡 3–5 分钟才开始广播。

## 现场状态（收尾时）

- 最终固件 = 原厂 BSP + os_time 修复，已于 2026-09-08 19:09 重新刷入并验证 py-opendisplay 绘制；
- 标签存活，健康窗口内广播与连接正常（RSSI −54 ～ −86），伴随间歇静默；
- 原厂固件备份在 `stock-lt213a-backup.bin`，需要时可整片回刷。

## 2026-09-09 probe-rs 刷写与电池广播检查

包含版本 SHA、认证边界修复和 CR2450 电池报告的固件已通过 RV CMSIS-DAP / SWD 1000 kHz 用 probe-rs 下载；读回校验通过，并执行 reset。未执行全片擦除，配置槽保留。应用 Flash 为 92,368 字节。

随后复现「官方页面找不到标签」：直接 bleak 扫描也连续出现 25 秒及 40 秒无 OpenDisplay 广播（可发现数百个其他 BLE 设备），因此不是仅浏览器过滤问题。后续一次 40 秒扫描在第 34.2 秒收到 OD742B6D，RSSI −49 dBm，scan response 包含服务 UUID `00002446-0000-1000-8000-00805f9b34fb`。MSD 电压为 3.40 V，确认新电池广播字段已发布；测量点是 VDD，不能据此校准 CR2450 电池端子电压。间歇广播/启动延迟仍未解决，不能把 Flash verify 成功视为无线可发现性验证。

## 2026-09-09 广播间隔对照

针对「长时间才能发现且无法连接」，仅修改广播间隔：快广播从 1–1.2 秒缩短为 100–150 ms，慢广播从 3–5 秒缩短为 1–1.2 秒。快广播时长 30 秒、已连接空闲断连 30 秒、LFRC 校准和 500 ppm SCA 均保持不变。构建、主机检查和 probe-rs 下载校验通过，配置槽未擦除，应用 Flash 为 92,364 字节，RAM 未变。

基线两次扫描分别在 40 秒和 30 秒内未发现目标。修改后两次连接成功：发现耗时 25.75 / 20.25 秒，连接建立累计耗时 26.61 / 23.38 秒；两次均读到包含 SHA 的版本和 MSD（3.40 V）。这是实测改善，不是完整修复证明：后续 70 秒扫描仍出现约 31 秒回调空白；macOS 默认会合并重复广播，启用重复回调的另一次 30 秒扫描也只有 3 次报告，最大回调间隔约 14 秒。回调间隔不能直接等同于射频包间隔，长期无线稳定性仍未确认。
