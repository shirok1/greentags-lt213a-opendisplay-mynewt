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

## 现场状态（收尾时）

- 最终固件 = 原厂 BSP + os_time 修复，已于 2026-09-08 17:35 刷入并 `reset run`；
- 收尾时实测：标签存活，广播可被扫描发现（RSSI −54 ～ −86），伴随间歇静默；
- 原厂固件备份在 `stock-lt213a-backup.bin`，需要时可整片回刷。
