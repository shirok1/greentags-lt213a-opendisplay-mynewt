# 待机省电

实现：

- CPU 使用 Mynewt RTC tickless/WFI，保留 System ON，仍可广播与连接。
- 屏幕在启动时复位并关电；刷新结束/活动传输取消时关电，BUSY 就绪后发送 `07 A5`，MOSI/DC 拉低，CS 保持高。关电超时返回失败，不盲发深睡。下次传输复位唤醒。
- 上电和断连后广播参数为 1–1.2 秒，30 秒后转 3–5 秒；发射功率仍为 0 dBm。慢广播可能延长发现时间。
- 连接建立或命令结束后空闲 30 秒主动断连；队列有工作时每秒延后检查，不中断正在执行的刷屏。未实现连接间隔动态切换，采用空闲断连策略。
- LFRC 启动校准后才能启动 BLE，校准完成后以 CTIV=8（2 秒）重新计时。POWER_CLOCK 中断处理 HFXO 就绪、校准完成和定时器到期。HFXO 使用 MCU 的共享引用计数，射频仍持有引用时校准不会停掉晶振。BLE_LL_SCA=500 ppm。
- `tools/patch_mynewt.py` 为本 BSP 的校准实现放开原有 LFRC 限制，不更改 MCU/NimBLE 时钟运行代码；补丁可重复应用，未知上游文本会报错。请通过 `tools/build.sh` 构建。

依据：Nordic nRF51 Reference Manual §13.1.3，校准前需 HFXO 就绪，CAL/CTSTART 任务必须相隔至少一个 LFCLK 周期；这里通过校准完成事件分隔。初次晶振启动与校准等待沿用启动阶段等待硬件事件的方式；硬件时钟故障可能阻止启动。
https://infocenter.nordicsemi.com/pdf/nRF51_RM_v3.0.pdf

验证范围：主机协议/解压测试、GPIO 模拟深睡命令和关电超时测试、ARM 固件构建及 RAM 容量检查。LFRC 的真实频率、温漂、BLE 长时间连接和整板电流尚未实测。

实板验证：

1. 从电池输入测电流，断开 SWD 调试器；记录上电前 30 秒与进入慢广播后的平均电流和波形。
2. 分别比较未刷屏、全刷后、局刷后、传输中途断连后的待机电流；检查深睡后全刷/局刷能正常唤醒。
3. 建立连接不发命令，确认约 30 秒后断连；持续传输和长刷屏不得被误断开。
4. 连续更新与重复重连，并在目标温度范围验证 LFRC 校准、接收窗口和连接稳定性；观察每约 2 秒的校准活动，确认活动结束后电流恢复。
5. 以实测平均电流估算续航，不用芯片最低电流代替整板功耗。当前未启用 MCU System OFF，也未假定存在外部电源开关或 DC/DC 电感。

## 官方固件对照

对照固定版本 `7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb`：

- [NRF BLE 实现](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/ble_transport_nrf.cpp)：使用 nRF52840 Bluefruit/SoftDevice；常规广告 fast/slow 参数为 160/1000 ms，boost 为 20/30 ms。LT213A 保留 1–1.2 秒、随后 3–5 秒的 NimBLE 广播区间；两个 API 的参数语义不同，不能把 Bluefruit fast/slow 当成 NimBLE min/max 直接移植。
- [屏幕电源会话](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/docs/epd-panel-power-session.md)：OFF/WARM/ACTIVE 状态避免重复电源切换，并支持最多 30 秒保温以减少密集更新延迟。本板采用简化的休眠状态保护，重复 epd_off 不再发命令或等待；复位开始时清除状态，关电失败不标记休眠。不启用保温，刷新完成立即深睡。
- [电源与引脚处理](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/main.cpp)：官方切断屏幕电源前拉低控制线，防止反向供电。本板不能切断屏幕电源，因此保持 CS 高（取消选中）、RESET 高、SCK/MOSI/DC/BS 低，BUSY 输入无拉电阻；不照搬全部拉低。
- 官方启用 SoftDevice LOWPWR 和 DC/DC；本板由 Mynewt RTC/WFI 休眠，不调用 SoftDevice API。未确认 DC/DC 外围电感，保持不启用。
- 官方 NRF 路径请求 2M PHY、251 字节 DLE 提升吞吐以缩短活动期；nRF51822 不具备对应 BLE 2M PHY/DLE 能力，保持 1M/27 字节链路层包。
- 本板的 LFRC 校准与 30 秒空闲断连是本移植的策略，不宣称是官方 NRF 实现。

新增 GPIO 测试验证重复休眠无 SPI 命令且无等待，复位后正常重新初始化，超时不误标为已休眠。实际功耗依旧以实板测量为准。
