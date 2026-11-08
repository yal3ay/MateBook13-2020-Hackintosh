# MatebookGVAFix 0.3.0 —— 黑苹果 UHD 620 的 H.265（HEVC）硬件编码修复

日期：2026-11-07　适用：macOS 15.8.2（24H232）　机器：Huawei MateBook 13，i5-10210U / UHD 620

## 结论
- macOS 15 上 HEVC 硬件编码已可用：60 帧 1280×720 约 2.1 秒（软件编码约 10 秒）；连续多次可用。
- H.264 硬件编码仍正常；没有新的编码器崩溃报告；编码器文件签名完好（`codesign -v` 通过，sha256 c8cb4abc…）。

## 根本原因（看反汇编和日志确认）
1. VideoToolbox 创建编码器后，总会调用编码器的 SetProperty("MetalRegistryID", 当前 GPU)。
2. AppleGVAHEVCEncoder 收到后置“已指定 GPU”标志（对象 +0x4cc8）。
3. 指定 GPU 的路径里，只要 GPU 是 Intel 核显就 `assert(0)`：“[AppleGVAHEVC] Error: metalId for IG”。
   真 Mac 的纯 Intel 机型带 T2，走 T2 编码，所以苹果没处理这条路。
4. 另外，核显上带 `AAPL,slot-name` 时编码器不选核显（日志“Encoder failed for all vendors / vendorId=1”），已从 config 删除该属性（只影响“系统信息”里的插槽名）。

## 修复方式
- 内核扩展 MatebookGVAFix 0.3.0（Lilu 插件），用 `routeMultipleLong` 挂 `_cs_validate_page`（和 RestrictEvents / FeatureUnlock 相同做法）。
- 只修改 dyld 共享缓存主文件 `/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h`
  中 VideoToolbox 的 1 个字节：文件偏移 0x12cec9f7，`74`（je）→ `EB`（jmp），即不再把 MetalRegistryID 转给编码器。
  解码器（VTDecoderMetalRegistryID）是另一个函数，未改动。
- 58 字节特征码含相对跳转偏移，只匹配这一个系统版本；在页面里必须唯一出现，且文件路径必须完全一致，否则不改（安全失败）。
- 只在 macOS 15（Darwin 24）加载；macOS 13 不加载，留作救援系统。启动参数 `-mbgvaoff` 可关闭。

## 失败的旧版本（记录在案，别再用）
- 0.1.0：用短跳转挂钩，和已挂同一函数的 WhateverGreen/FeatureUnlock 冲突 → macOS 15 开机卡死。
- 0.2.0：直接改编码器文件的缓存页，能编码一次，但之后读文件拿到改过的字节、签名失败，VideoToolbox 隐藏硬件编码器（-12908），直到重启。

## 升级注意
- 升级到 macOS 15 的其他小版本或 macOS 26 后，特征码不再匹配，补丁自动不生效（编码退回软件，不会崩溃）。
  需要按同样方法从新系统的共享缓存里重新找这一处并更新特征码。
- 备份：`EFI/OC/config-before-gva030-20261107.plist`、`config-before-noslot-20261107.plist`、`config-before-gvafix-20261107.plist`。

## 文件
- 源码：`kern_start.cpp`、`gva_patch.hpp`、`gva_patch_tests.cpp`（含对真实共享缓存的逐页扫描测试）、`build.sh`
- 分析用共享缓存副本：`../vt-dsc/`（约 870MB，可删）

## 0.4.1 自动定位版（2026-11-08，当前使用）

- 不再依赖写死的 15.8.2 字节：开机进到登录界面后，kext 自己读 dyld 共享缓存，按结构找到 VideoToolbox 里「把 MetalRegistryID 转发给编码器」的那一处（`dsc_locate.hpp`），只认唯一一处，找不到就什么都不改（编码退回软件）。15.8.2 的静态特征码保留作后备。
- 状态在 IORegistry：`ioreg -rd1 -c IOResources | grep MatebookGVAFix`。15.8.2 实测：`located: file '' offset 0x12cec9f7 kind 1 (shape candidates 2)`，补丁由自动定位路径打上（日志 `located, page 0x12cec000`）；hwreq/default/hw 全部 usingHW=1，60 帧约 2.1–2.2 秒；编码器签名正常。
- 0.4.0 教训：开机极早期（系统盘未挂载）就 `vnode_lookup`，15 卡在 `console`。0.4.1 改为等 IOResources 出现 IOConsoleUsers 后才碰文件。
- 仍限 Darwin 24（macOS 15）。升 26 前要先用 26 的缓存在 Mac 上跑 `dsc_locate_tests` 验证，再放宽 MaxKernel。
- 备份：EFI-Backups/MatebookGVAFix-0.3.0、-0.4.1；config-before-gva041-20261108.plist。救急：选 13 进系统，把 0.3.0 拷回 EFI/OC/Kexts。
