# 待机省电

实现：

- CPU 使用 Mynewt RTC tickless/WFI，保留 System ON，仍可广播与连接。
- CR2450 电压和芯片温度在启动、快广播结束及未连接时每 5 分钟采样，复用已有定时器和空闲命令槽；采样完成关停 ADC/TEMP，广播数据原地更新。已有命令占用时跳过周期采样，不打断图像传输。`00 44` 额外触发按需采样。
- 屏幕在启动时复位并关电；刷新结束/活动传输取消时关电，BUSY 就绪后发送 `07 A5`，MOSI/DC 拉低，CS 保持高。关电超时返回失败，不盲发深睡。下次传输复位唤醒。
- 上电和断连后广播参数为 100–150 ms，30 秒后转 1 秒；发射功率仍为 0 dBm。慢广播可能延长发现时间。相较先前 1–1.2 秒 / 3–5 秒参数，这提高了发现和连接机会，也增加了广播耗电；实际续航需重测。
- 看门狗使用 Mynewt 原生机制：180 秒硬件超时，idle task 每 15 秒在任务 sanity 通过后喂狗。主任务与屏幕任务的进展期限均为 120 秒（主任务配置项为 60000 ms，上游内部乘 2）；空闲队列最长等待 60 秒，等待结束后再开始回调，成功发送通知也会签到。无需新任务、定时器或额外状态缓冲。任务可调度时约 120–135 秒无进展就 assertion 复位；中断/调度卡死由硬件看门狗恢复。原生 Cortex-M0 fatal/assert 自动复位路径保持启用，调试器连接时可能先停在断点。
- 连接建立或命令结束后空闲 120 秒主动断连；队列有工作时每秒延后检查，不中断正在执行的刷屏。未实现连接间隔动态切换，采用空闲断连策略。
- LFRC 启动校准后才能启动 BLE，校准完成后以 CTIV=8（2 秒）重新计时。POWER_CLOCK 中断处理 HFXO 就绪、校准完成和定时器到期。HFXO 使用 MCU 的共享引用计数，射频仍持有引用时校准不会停掉晶振。BLE_LL_SCA=500 ppm。
- `tools/patch_mynewt.py` 为本 BSP 的校准实现放开原有 LFRC 限制，不更改 MCU/NimBLE 时钟运行代码；补丁可重复应用，未知上游文本会报错。请通过 `tools/build.sh` 构建。

依据：Nordic nRF51 Reference Manual §13.1.3，校准前需 HFXO 就绪，CAL/CTSTART 任务必须相隔至少一个 LFCLK 周期；这里通过校准完成事件分隔。BSP 启动时先停止 LFCLK、等待停止，再清事件并重新启动 LFRC；实板发现 SWD/软件复位后仅信任 LFCLKSTAT=Running 会出现校准与 RTC 无进展。启动阶段的停止、启动、HFXO 和校准等待各限约 1 秒，超时自动复位；计时用 CPU 延迟，不依赖尚未运行的 RTC。

可选实板回归：烧入匹配的默认构建后运行 `python3 tools/test_boot_hardware.py --trials 3`。脚本通过 probe-rs 连续复位并读取系统 tick、RTC0/RTC1，要求三者均推进；会打断当前连接，不写 Flash。该检查覆盖软件复位，不代替断电冷启动和整板电流测量。
https://infocenter.nordicsemi.com/pdf/nRF51_RM_v3.0.pdf

验证范围：主机协议/解压测试、GPIO 模拟深睡命令和关电超时测试、ARM 固件构建及 RAM 容量检查。LFRC 的真实频率、温漂、BLE 长时间连接和整板电流尚未实测。

实板验证：

1. 从电池输入测电流，断开 SWD 调试器；记录上电前 30 秒与进入慢广播后的平均电流和波形。
2. 分别比较未刷屏、全刷后、局刷后、传输中途断连后的待机电流；检查深睡后全刷/局刷能正常唤醒。
3. 建立连接不发命令，确认约 120 秒后断连；持续传输和长刷屏不得被空闲计时误断开。未完成传输从 START 起达到 15 分钟时须取消并断连，即使期间持续发送命令。
4. 连续更新与重复重连，并在目标温度范围验证 LFRC 校准、接收窗口和连接稳定性；观察每约 2 秒的校准活动，确认活动结束后电流恢复。
5. 断开 SWD，分别注入主任务/屏幕任务卡死、通知完成丢失、关中断卡死和 assertion/fault，检查对应复位时限；同时确认空闲数分钟、MTU 23 完整配置读取和屏幕 BUSY 超时清理不会误重启。局刷改为 100 帧后需复测残影、时间与电荷。
6. 以实测平均电流估算续航，不用芯片最低电流代替整板功耗。当前未启用 MCU System OFF，也未假定存在外部电源开关或 DC/DC 电感。

## 官方固件对照

对照固定版本 `7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb`：

- [NRF BLE 实现](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/ble_transport_nrf.cpp)：使用 nRF52840 Bluefruit/SoftDevice；常规广告 fast/slow 参数为 160/1000 ms，boost 为 20/30 ms。LT213A 使用 100–150 ms、随后固定 1 秒的 NimBLE 广播参数；两个 API 的参数语义不同，不能把 Bluefruit fast/slow 当成 NimBLE min/max 直接移植。
- [屏幕电源会话](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/docs/epd-panel-power-session.md)：OFF/WARM/ACTIVE 状态避免重复电源切换，并支持最多 30 秒保温以减少密集更新延迟。本板采用简化的休眠状态保护，重复 epd_off 不再发命令或等待；复位开始时清除状态，关电失败不标记休眠。不启用保温，刷新完成立即深睡。
- [电源与引脚处理](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/main.cpp)：官方切断屏幕电源前拉低控制线，防止反向供电。本板不能切断屏幕电源，因此保持 CS 高（取消选中）、RESET 高、SCK/MOSI/DC/BS 低，BUSY 输入无拉电阻；不照搬全部拉低。
- 官方启用 SoftDevice LOWPWR 和 DC/DC；本板由 Mynewt RTC/WFI 休眠，不调用 SoftDevice API。未确认 DC/DC 外围电感，保持不启用。
- 官方 NRF 路径请求 2M PHY、251 字节 DLE 提升吞吐以缩短活动期；nRF51822 不具备对应 BLE 2M PHY/DLE 能力，保持 1M/27 字节链路层包。
- 本板保留 LFRC 校准策略；BLE 空闲断连现为 120 秒，与官方 NRF 默认值一致。传输另有从 START 起 15 分钟的绝对期限，复用同一 callout，不新增定时器。

新增 GPIO 测试验证重复休眠无 SPI 命令且无等待，复位后正常重新初始化，超时不误标为已休眠。实际功耗依旧以实板测量为准。

## 2026-09-12 刷新延迟优化

旧图填充不再每 128 字节强制休眠一个 tick；显示任务优先级低于 BLE host/controller，可被抢占。BUSY 就绪后的固定等待从 200 ms 改为 10 ms（`READY_SETTLE_MS`，经 128 Hz tick 向上量化），保留上电/关电后 10 ms、刷新命令后 100 ms 和 30 秒 BUSY 超时。依据用户提供的 GDEW0213T5 手册更新流程：BUSY 高后进入下一步；10 ms 是本板保守余量，尚非全温区光学验证结果。

旧图与每次 `epd_write` 的连续数据保持 CS 有效；命令和局刷的两平面边界仍重新选择。1 us 半周期、BLE 中断抢占、100 帧 LUT 和深睡命令保持原值。压缩解码当前逐字节输出，因此 SPI 批量收益主要在旧图初始化和原始数据块。没有增加 framebuffer、heap 或硬件 SPI 驱动。

主机 GPIO 解码测试覆盖连续 CS 下的所有字节、平面切换、字节边界、片选释放和旧图片选次数，并保留 BUSY 超时测试。实板时间与验证限制见兄弟 Zephyr 仓库 `docs/performance-optimization-2026-09-12.md`。

## 编译参数实验结论

2026-09-12 后续系统裁剪已改为默认全局 `-O2`：禁用本应用不使用的 GATT indication 与客户端过程池（`BLE_GATT_INDICATE=0`、`BLE_GATT_MAX_PROCS=0`），保留 notification、MTU 247 和原有 BLE 数据池。静态 GATT 服务不使用运行期间 Service Changed indication；未来若增加动态服务、绑定缓存或 GATT 客户端，需要恢复对应过程池。

`tools/patch_mynewt.py` 为下载的 Cortex-M0 编译器追加 `lt213a_o2` profile，`targets/lt213a/target.yml` 选择它；原有 upstream optimized/speed/debug profile 保持原值。使用 `tools/build.sh` 构建，以保证依赖补丁已应用。GCC 15.3.1 的默认地址产物为 Flash 110,460 B、静态 RAM 13,936 B、IRQ 栈 384 B、heap 2,064 B，未放宽原有 2 KiB heap 检查。

全局 O2 测试地址候选通过三次复位、10 次原始/压缩全刷和局刷。原始 START ACK 中位数从 0.463 s 降至 0.344 s；全刷总时长主要受面板波形影响。局部 LTO、Oz 等早期实验仍见兄弟 Zephyr 仓库 `docs/compiler-optimization-2026-09-12.md`；最新裁剪与验证见 `docs/system-trimming-2026-09-12.md`。默认 BIN SHA-256：`44cc651379eb30db09167739eeb2de71daadfeaeb9aa0557e5a9cab599b28740`。
