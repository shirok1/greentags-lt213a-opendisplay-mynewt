# LT213A 协议覆盖与边界

实现依据是 OpenDisplay canonical protocol 2.2 的命令集合。
固件版本 0.2.0；这是 nRF51822 固定硬件实现，不是所有 OpenDisplay 硬件功能的超集。

| Opcode | 功能 | LT213A 行为 |
|---|---|---|
| `000F` | 重启 | 清理传输后 MCU reset；不发 ACK |
| `0040` | 读配置 | 返回持久化配置或出厂配置；按 MTU 分片 |
| `0041` | 写配置 | 单包或带总长度的首块；CRC/记录/板型校验后原子提交 |
| `0042` | 配置续块 | 有活动事务才接受；溢出/断线/空闲超时会取消 |
| `0043` | 版本 | 明文 0.2.0，空 SHA |
| `0044` | MSD | 明文读取；采样芯片温度与内部 VDD ADC，外设超时则 NACK |
| `0045` | 清配置 | 原子提交出厂配置；重置认证会话 |
| `0050` | 认证 | CMAC challenge/proof、KDF、服务端证明、限流、超时 |
| `0051` | 进入 DFU | **NACK：当前没有 DFU bootloader，未实现 OTA** |
| `0052` | 硬电源关闭 | `FF 52 00 00`：没有电源锁存 |
| `0053` | 深度休眠 | `FF 53 00 00`：canonical nRF unsupported；没有外部唤醒输入 |
| `0070` | Direct START | 原始图像或 zlib 流；校验解压后长度 |
| `0071` | Direct DATA | 顺序流式解压/写屏、逐包 ACK |
| `0072` | Direct END | 完整性检查、刷新 ACK、结果通知；支持可选 ETag |
| `0073` | LED 激活 | NACK：没有 LED |
| `0075` | LED 停止 | NACK：没有 LED |
| `0076` | Partial START | 大端矩形/ETag、旧图＋新图，可压缩 |
| `0077` | 蜂鸣器 | NACK：没有蜂鸣器 |
| `0080` | PIPE START | 协商 1–2 包窗口、ACK 间隔、帧长；支持压缩和局部扩展 |
| `0081` | PIPE DATA | 序号、窗口 2 重排、重复包、SACK、回绕与 fatal NACK |
| `0082` | PIPE END | 尾部 SACK、完整性检查、刷新 ACK 与结果通知 |
| `0083` | NFC | 格式/子命令校验；读写失败或未 START；没有 NFC，不产生成功结果 |

`0074` 是刷新超时通知的 echo，不是主机请求命令。
未知命令返回 `FF <echo>`。缺少认证时受保护命令返回 `FE <echo>`。

## 传输

- 单 BLE 连接，最大 ATT MTU 247；应用不持有整幅 framebuffer。
- Direct DATA：最大 230 字节，MTU 23 时为 18 字节。只按顺序写入；无断点续传。
- zlib：使用固定版本的 OpenDisplay `lib/uzlib`（见 `libs/od-uzlib/UPSTREAM.md`），原始供应商源码不作修改。适配层由屏幕 worker 独占，支持 stored、fixed Huffman、dynamic Huffman、多个 block、Adler-32。只接受 CINFO ≤ 1，即最多 **512 字节窗口**，不接受 preset dictionary、尾随垃圾或超量输出。不是 32 KiB 窗口的通用 zlib 解码器。
- 能力字节为 `0x19`：streaming decompression、direct、PIPE；不声明大窗口 ZIP。
- PIPE：当前包直接处理，单个缓存槽保存提前到达的后续包。窗口和 `ack_every` 取客户端请求与本机上限 2 的较小值。重排窗口以外的前向序号为协议错误。处理过的近期重复包只重发 SACK，不重复写屏。客户端应按协商窗口限流，并处理重传；不能按 32 包窗口硬发。
- PIPE 收到 fatal NACK 后丢弃后续 DATA，直到新的 START。序号按 256 回绕，SACK mask 覆盖前 32 个序号。
- Direct 和 PIPE 两种会话不能混用 DATA/END。
- 刷新成功通知 `00 73`，刷新超时 `00 74`；END ACK 不等于刷新成功。

## 配置和 Flash

内部 Flash 最后 10 KiB 分成两个 5 KiB 槽：`0x1D800`、`0x1EC00`。
代码链接区限制为 118 KiB。槽头保存版本计数和长度，commit magic 最后写入；只有完整校验通过的槽才会被加载。提交失败或掉电保留上一份已提交配置。

配置传输上限 4096 字节，首个 chunked WRITE 必须含总长度及完整的 200 字节首块，随后每块最多 200 字节。当前合法记录集合是 system、manufacturer、power、display，以及可选 security，通常总长为 133 或 199 字节，因此通常直接单包写入。

不接受重复记录、坏 CRC、未知记录，或与这块固定板不符的屏幕尺寸/引脚/颜色/能力。不存在的外设记录不会被存储后假装启用。固定引脚由 syscfg 和生成器提供。

CLEAR 是逻辑恢复出厂配置，不承诺将另一槽的历史 key 字节物理擦净。没有重置按键；忘记密钥时需通过 SWD 恢复配置区。正常应用 HEX 不包含配置槽。

## 安全

SecurityConfig 采用标准 64 字节记录；全零 master key 视为未启用。支持 session_timeout 和 rewrite_allowed；本板没有 reset GPIO，show-key-on-screen 是未提供的功能，相关 flags 会被拒绝。

- Challenge：硬件随机数；30 秒有效，一次 proof 后即失效。
- KDF、session ID 和服务端 MAC 与官方 Python 客户端格式对齐。
- AES-128-CCM，AAD 为两字节命令/响应头，13 字节 CCM nonce、12 字节 tag、长度前缀。
- 设备发送 counter 使用最高位为 1 的空间，客户端 counter 必须最高位为 0，避免同一个 key 下请求与响应复用 nonce；官方 Python response decoder 兼容此编码。
- 客户端 counter 严格递增；重放、坏 tag 或不一致的长度会结束安全会话并取消图像传输。
- 60 秒窗口最多 10 次 challenge，断开连接不会重置限流计数。
- CONFIG 修改成功的响应使用旧安全会话发送，之后清除会话；新 key 需要重新认证。
- 认证 proof 长 34 字节，要求 ATT MTU 至少 37。通常应使用 247，尤其是配置写入和带矩形头的命令。
- Version、Authenticate、Read MSD 明文可用；其余命令按 security 配置处理。

## 屏幕与 ETag

全刷来自 `GDEW0213T5_Arduino_20191016` 示例。
局刷来自同一下载目录中的 `GDEW0213T5_Arduino_P20201021` 示例，包含局刷 LUT、`0x91/0x90` 矩形窗口和对应 RAM 极性；修正了示例只按正方形计算字节数的做法。

局部流是 `old_plane || new_plane`，长度必须是 `width / 8 * height * 2`；x 和 width 要字节对齐，矩形必须在 104×212 范围内。
局部更新必须匹配当前有效 ETag。刷新成功才保存新 ETag；刷新失败、未完成传输被取消或 MCU 重启会使其失效，客户端须回退全刷。ETag 不写 Flash。

全帧请求的 FAST hint 当前回退到可靠的全刷；局部会话使用供应商区域波形，不根据 END 的 FULL/FAST hint 切换波形。尚未验证实屏残影和长期局刷次数，需要按供应商建议周期性全刷。

## 验证范围

- 跨语言 C/Python 测试：明文与加密、KDF/服务端证明、重放、坏 tag、超时、密钥变更、Direct、PIPE 重排/回绕/SACK、局刷和 ETag。
- Flash 模型：每个擦写/提交阶段注入失败，检查重启后仍能加载旧配置。
- ASan/UBSan：62,720 组 opcode/长度变异。
- 标准 zlib 对照：96 组成功流，另含截断、校验和损坏、错误窗口和尾随数据。
- GPIO 替身：解码实际软件 SPI 字节，检查全刷、矩形、LUT 长度、局刷极性、BUSY 超时和 tick 回绕。

没有烧录或实机 BLE/显示/功耗测试。逻辑分析仪、Radio 时序、Flash 擦除期间的连接稳定性、ADC 校准与栈高水位仍需验证。

## 待机行为

屏幕关电成功后发送 `07 A5` 深睡；上电时也复位并关电/深睡，下次传输前复位唤醒。
上电/断连后快广播 30 秒，随后使用 3–5 秒慢广播参数。连接建立后或命令完成后开始计时，空闲 30 秒主动断连；仍有命令在处理时延后检查。
因此不能依赖空闲连接永久保留认证或 ETag 会话状态；长时间暂停的客户端应处理断连并重新认证/开始传输。
