# Huawei MateBook 13 (2020) OpenCore EFI — macOS Sequoia 15

[English summary below](#english)

适用于 **华为 MateBook 13 2020（i5-10210U / UHD 620）** 的 OpenCore EFI，在 macOS 15.8 上日常使用。

## 硬件

| 部件 | 型号 | 状态 |
|---|---|---|
| CPU | Intel Core i5-10210U（Comet Lake） | ✅ 原生电源管理，CPUFriend 调节能效（EPP 0x80） |
| 核显 | Intel UHD 620 | ✅ 硬件加速，HDMI 输出，亮度调节；HEVC 硬件编码由自制补丁修复 |
| 独显 | NVIDIA MX250 | ⛔ 已用 ACPI 关闭（macOS 不支持） |
| 声卡 | Realtek ALC256（layout-id 21） | ✅ 喇叭、耳机；耳机麦克风未完整测试 |
| 无线网卡 | Intel Wireless-AC 9462（CNVi） | ✅ AirportItlwm（原生 Wi‑Fi 菜单）；无隔空投送/接力 |
| 蓝牙 | Intel | ✅ IntelBluetoothFirmware |
| 硬盘 | WD SN730 NVMe | ✅ NVMeFix |
| 键盘 | PS/2 | ✅ 亮度、音量快捷键；F7（麦克风键）→ F16，可映射为听写 |
| 触控板 | I2C HID | ✅ VoodooI2C，多指手势 |
| 摄像头 | 13d3:56c6（UVC 1.5） | ✅ 自制补丁 MatebookUVCFix |
| USB | 两个 Type-C | ✅ 无代码 USBMap（按 ACPI 表核对过） |
| 睡眠/唤醒 | | ✅ |
| 指纹 | | ⛔ 无驱动 |

## 内容

- OpenCore 1.0.8
- Kexts：Lilu 1.7.2、VirtualSMC 1.3.8、WhateverGreen 1.7.1、AppleALC 1.9.8、VoodooPS2 2.3.7、VoodooI2C 2.9.1、AirportItlwm 2.4.0-alpha（laobamac 分支）、IntelBluetoothFirmware 2.4.0、BlueToolFixup 2.7.2、NVMeFix 1.1.3、CPUFriend 1.3.0、FeatureUnlock 1.1.8、HibernationFixup 1.5.4、NullEthernet 1.0.6
- 自制 kext（源码在 `src/`）：
  - **MatebookGVAFix** — 修复 UHD 620 在 macOS 15 上 HEVC 硬件编码不可用（自动定位 VideoToolbox 中的代码位置，系统更新后通常不用重做）。启动参数 `-mbgvaoff` 可停用。
  - **MatebookUVCFix** — 修复内置摄像头（UVC 1.5）识别问题。
- `USBMap.kext`：无代码 USB 端口映射。

## 使用前必须做的

1. **生成你自己的机型信息。** `config.plist → PlatformInfo → Generic` 里的 `SystemSerialNumber`、`MLB`、`SystemUUID` 已清空，`ROM` 为全零。
   - 用 [GenSMBIOS](https://github.com/corpnewt/GenSMBIOS) 或 OpenCore 自带的 `macserial` 生成 **MacBookPro16,3** 的序列号、MLB；`uuidgen` 生成 UUID。
   - `ROM` 填你自己网卡的 MAC 地址（6 字节）。
   - 不要用别人的序列号，否则 iCloud / iMessage 会出问题。
2. BIOS：关闭 Secure Boot。
3. 把 `EFI` 文件夹放到 EFI 分区。

## 说明

- 只针对 macOS 15（Sequoia）测试。macOS 26 需要改机型（MacBookPro16,2）、补声卡（AppleHDA 已被移除）和换 Wi‑Fi 驱动，本仓库未包含。
- 启动参数：`darkwake=2 lilucpu=10 -nvmefaspm`。
- CPUFriend 默认启用 Save80（更凉、更省电）；想要更高性能可改用 Default 或 Save60。
- 左边 Type-C 口在接集线器+移动硬盘时 USB3 可能掉线，大流量设备建议插右边。
- 耳机孔在 CPU 负载变化时有轻微电流声，属于主板硬件，驱动无法解决。

## 致谢

[Acidanthera](https://github.com/acidanthera)（OpenCore、Lilu 及插件）、[OpenIntelWireless](https://github.com/OpenIntelWireless)、[laobamac](https://github.com/laobamac/itlwm)、[VoodooI2C](https://github.com/VoodooI2C)、[Dortania](https://dortania.github.io/)。各 kext 版权归原作者所有。

---

## English

OpenCore 1.0.8 EFI for the **Huawei MateBook 13 (2020)**, i5-10210U / UHD 620, tested on **macOS Sequoia 15.8**.

Working: iGPU acceleration and HDMI, speakers and headphones (ALC256, layout 21), Intel AC 9462 Wi‑Fi and Bluetooth, the I2C trackpad, the PS/2 keyboard with Fn keys, the camera, both Type-C ports, and sleep/wake.

Not working: the MX250 dGPU (disabled), fingerprint, and AirDrop/Continuity (no AWDL on Intel Wi‑Fi).

**Before use:** the serial, MLB, UUID and ROM in `PlatformInfo → Generic` are blank. Generate your own **MacBookPro16,3** values with GenSMBIOS or macserial, and set ROM to your NIC's MAC address.

Custom kexts (source in `src/`):
- `MatebookGVAFix` restores HEVC hardware encoding on macOS 15.
- `MatebookUVCFix` fixes detection of the built-in UVC 1.5 camera.
