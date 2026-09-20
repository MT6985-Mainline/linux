# corot (Redmi K60 Ultra / MT6985) — mainline 7.2 display bring-up

## 仓库布局（corot-bringup/）

| 路径 | 内容 |
|---|---|
| `patch-chain/` | 复现当前内核状态所需的补丁链，按数字顺序执行（`python3 apply_rNNN.py`）。注意 `apply_r115.py` 需要环境变量 `R115_MAX=1`，且依赖厂商树 `linux-corot-t-oss` 的 `dispsys_config` 时钟表 |
| `build/` | `build_r131.sh`（当前链的完整构建+打包）、`build_r125..r130.sh`（中间轮次）、`build_channel.sh`（重编 initramfs）、`pack_corot_images.py`（打 boot.img / init_boot.img） |
| `run/` | 设备侧脚本：`r125run.sh <tag>`（一轮完整测试：刷机→观察→恢复 stock→双通道读日志）、`r125read.sh`（只读日志）、`custuuid.sh`（读 cust UUID）、`scanexpdb.sh`（全分区扫 ring）、`flash2.sh` / `readexpdb.sh`（旧流程） |
| `README.md` | 本文：状态、根因链、下一步 |

initramfs 侧（log-catcher + UUID 探测）在同组织的 `initramfs` 仓库分支 `corot-mt6985`。


This directory is the bring-up kit that goes with this branch: the patch chain,
the build/pack scripts, the device-side round scripts and the state of the port.


# COROT 显示 bring-up —— 2026-09-19 夜 交接（r130/r131 突破）

设备 corot = `UGEEOVYX4TZ9EE45`。**绝不操作另一台**（`fy6tnblfeemf7dkb` / `BUFEJF5X8TCQ9HZL` = pearl/yuechu）。

## 一、今晚最重要的两件事

### 1. 测量通道修好了（这是所有混乱的根源）

之前每一轮"看不到日志"都不是内核没打印，而是通道坏了。现在有两级通道，都验证过：

| 通道 | 机制 | 状态 |
|---|---|---|
| **cust boot-log.txt**（主） | initramfs 的 log-catcher 把 kmsg 流式写入 cust，每次 write 都 fsync | **已修复并验证**：`corot-log: cust found at … (uuid match)` |
| expdb ring（辅） | 内核把 pr_err 镜像进 `log_store@0x7ffbf000`，LK 下次启动 dump 进 expdb 尾部 | 只在 panic 时可靠；挂死型故障 LK 不 dump |

**关键修复**：log-catcher 原来写死 `/dev/sdc80`，但**测试内核的 UFS 枚举和 stock 不一致**，日志写到了别处（或根本没写）。改成**按 ext4 superblock UUID 探测** cust（`a6333b1b-a1a1-4cf7-90ca-8317634b7aec`，卷标 `debian-cust`）：
- 源码 `E:\corot\corot-initramfs\init-log.c` → `find_cust()` / `uuid_match()`
- 构建 `E:\corot\.zcode\build_channel.sh`（编译+cpio+lz4，落到 `corot-work/corot-initramfs/initramfs.cpio.lz4`）
- 读回 `E:\corot\.zcode\r125read.sh` / `r125run.sh`（含 expdb 双通道）

### 2. 真正的根因链（全部有日志证据）

**(a) 显示域被 genpd 断电 → SError**（r127 的 ring 证据）
```
[before-first-enable] ovl_en=0x6 dsc_con=0x00010089 dsi_start=0x1   ← LK 交接时显示是活的
[after-kms-init]      ovl_en=0x0 dsc_con=0x0        dsi_start=0x0   ← 在这段窗口里被断电
crtc_enable: is_dual_pipe=1 → 寄存器全 0 → 碰 DSI → Kernel panic: Asynchronous SError
```
原因：r107 给 `dispsys_config` 挂了 `power-domains`，驱动又 `pm_runtime_enable()`，**没有任何引用被持有**时内核 runtime-suspend → DIS0 域真断电。厂商注释写得很清楚："*Here we only decrease ref count, **the power will hold on***"。

**(b) 修复 = 取住引用且不释放**（r128/r129 验证 SError 消失）：`mtk_drm_top_clk_prepare_enable()` 里 `pm_runtime_get_sync(mmsys_dev)` 真的上电，且**不应用 r108**（r108 是那条在 kms_init 结尾释放电源的补丁）。→ `suspended=0`，寄存器全程保持活值。

**(c) 双 pipe 打开后暴露空指针**（r130 的 cust 日志里是完整 oops）
```
Internal error: Oops: 96000006 [#1] SMP
pc : mtk_crtc_dual_layer_config+0xb4/0x16c      x0 = 0
Call trace: mtk_crtc_dual_layer_config ← mtk_crtc_restore_plane_setting
            ← mtk_drm_crtc_enable ← … ← drm_fb_helper_set_par ← fbcon_init
```
根因：`dual_pipe_comp_mapping()` 只有 MT6983/MT6895/MT6885 分支，**MT6985 落到 default 返回 0** → `priv->ddp_comp[0]`（OVL0，本树无此节点）= NULL → oops。这也解释了"半屏之谜"：`is_dual_pipe=0` 时这个函数根本不会被调用，所以屏幕只有主 pipe（上半）且不崩。

**(d) 修复 = 补 MT6985 映射 + 空指针保护**（r131）：补上能对上的映射项，缺对手时**只配主 pipe 并打印明确日志**，绝不 oops。

## 二、r131 的实测结果（当前最好状态）

```
Kernel panic 0   SError 0   underflow 0
suspended=0 ×10                        ← 显示域全程上电
is_dual_pipe=1 ×3                      ← 双 pipe 已建立
COROT-STAGE crtc_enable DONE (all 15 steps)   ← 15 步全部走完（r130 死在第 4 步）
mutex mtx0 EN=0x00010001               ← MUTEX 已使能
dsc   CON=0x00014009 MODE=0x00000707   ← DSC 已配置
dsi   INTEN=0x00000004 (TE_RDY)  te=1850  ← TE 中断在到（~55/s）
稳定运行 123.8 秒（日志末尾仍在正常输出 UDC dump）
```
唯一缺的是 `frames=0`（没有真正的帧提交，initramfs 里没有 DRM 用户态；fbcon 那次提交已走完）。

**缺口清单（日志明说）**：`no dual-pipe counterpart for comp 31 (mapped 0)` ×7 —— comp 31 = `DDP_COMPONENT_RDMA0_VIRTUAL0`。右 pipe 组件（ovlsys 侧 OVL、第二 DSC、MUTEX1）本树没有：DTS 里 `ovl1_2l`=0、`dsc1`=0、`disp1_ovl`=0；我们的枚举里连 `OVL4_2L..OVL7_2L` 都不存在。

## 三、脚本与工具（今晚新增）

| 脚本 | 用途 |
|---|---|
| `build_channel.sh` | 重新编译/打包 initramfs（UUID 探测版）——**每次改 initramfs 都要先跑** |
| `r125run.sh [tag]` | 加固版单轮：刷机（按退出码判定成功，fastboot 输出走 stderr 的坑已修）+ 观察 + 恢复 stock + 双通道读日志 |
| `r125read.sh` | 只读日志（不刷机） |
| `build_r125..r131.sh` + `apply_r126..r131.py` | 本轮次的构建与补丁 |
| `custuuid.sh` | 读 cust UUID/superblock |

**注意**：`build_rNNN.sh` 会 `git checkout HEAD -- drivers/ arch/ include/ .config` 后重放补丁链，所以每轮都是干净起点 + 明确补丁集。

## 四、下一步（按优先级）

1. **先问屏幕现象**：r131 时面板显示什么？（全黑 / 上半有内容 / 有变化）——这决定是继续补右 pipe 还是先查"帧为什么没提交"。
2. **帧没提交（frames=0）**：initramfs 没有 DRM 用户态，fbcon 那一次提交已完成 15 步。需要确认 `mtk_drm_crtc_enable` 里是否真的把 DSI 启动（`dsi START=0x00000000`）以及 CMDQ 包是否真的执行（GCE audit 显示 GCE 活着、`gctl=0x00020002`）。
3. **补右 pipe 组件**（下半屏的最后一公里）：把厂商的 `disp_ovl1_2l@14403000`、`disp1_dsc_wrap`、`mutex1` 节点连同 `ovlsys_clk` 门控、IOMMU/larb 端口、OVLSYS 电源域一起移植；在驱动里注册成组件；再把 `dual_comp_map_mt6985()` 的其余条目补全（RDMA0_VIRTUAL0、OVL4_2L..OVL7_2L、OVLSYS_WDMA*）。每补一个，日志里的 "no dual-pipe counterpart" 就少一条。
4. 之后才是交接文档里列的 48 时钟 / ovlsys_dev / DSI 真时钟 / lane config 顺序。

## 五、测量纪律（今晚用血换来的，务必遵守）

1. **必须用 `adb exec-out`**（`adb shell` 会做 CRLF 转换毁掉二进制读回）；读 expdb 还要**拒绝全 0/空 dump**。
2. **PowerShell 的 `>` 会把二进制转成 UTF-16**！所有设备读取都放到 WSL bash 里做（`r125read.sh` 就是这么写的）。
3. `fastboot` 的 OKAY 走 **stderr**，判定成功要用**退出码**。
4. LK 的 `ab_retry` 是倒计数，扣完就停在 fastboot；判据是 LK 日志里的 `kernel_sz=0x…`（我们的镜像 ≈ `0x01218xxx`）。
5. USB 会抽风：fastboot 枚举失败、adb 掉线、刷写挂死。所有命令都要包 `timeout` + 重试（`r125run.sh` 已做）。
6. 挂死型故障 LK 不 dump ring —— 想知道挂在哪，就得让**故障发生在用户态之后**（r130 的 `-EPROBE_DEFER` 直到 `system_state >= SYSTEM_RUNNING` 就是干这个）。

> 仓库：https://github.com/MT6985-Mainline/linux （分支 `7.2-mt6985-xiaomi-corot`），initramfs 在同组织 `initramfs` 仓库分支 `corot-mt6985`。
