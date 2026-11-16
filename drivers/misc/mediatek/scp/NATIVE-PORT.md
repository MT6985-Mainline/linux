# Native corot RV SCP provider

This provider supplies the real reserved-memory and mailbox/IPI dependencies
used by the complete corot DRM v2/AOD source port. It does not establish that
SCP firmware or display output has worked on hardware; runtime verification
remains with the root task.

## Provenance and implemented path

Reference: MiCode `corot-t-oss`, commit
`cb2ab59a621b17aafa4a3f349c84d9f18adf7aaf`.

- `rv/scp_helper.c` derives the reserved-memory partitioning, firmware release,
  ready notification and PM/shutdown protocol from the reference SCP helper.
  The reserved-memory operations table uses Linux 7.2's real `node_init` API.
  The stock `scp_mem_key` binding maps this region directly in probe and does
  not assign a DMA pool through `memory-region`/`device_init`.
- `rv/scp_reservedmem_define.h` is the authentic reference ID table. Sizes and
  alignments come from the real DT, with the secure-dump size overriding ID 0.
- `rv/scp_ipi.c` derives the physical 64-bit mailbox slot/IRQ layout from
  `scp_wrapper_ipi.c`, and the always-on awake lock/unlock and SPM interrupt
  clear protocol from `scp_awake.c` and `scp_reg.h`.
- `rv/scp_ipi_table.h` contains the genuine named SCP mailbox/IPI objects with
  those awake/clear callbacks. Probe parses and validates the actual send and
  receive tables, creates all IPI endpoints and registers handlers before IRQs.
- `rv/scp_calibration.c` retains the reference VLP two-phase ULPOSC search,
  DT configuration/targets and firmware calibration synchronization. The real
  MT6985 frequency-meter transaction comes from `clk-fmeter-mt6985.c`:
  OSC_2 selector 22, 1024 load cycles, DIV4, and the fixed 40 us capture window.
  Counts convert to the 512-cycle units used by the reference SCP search.
  Native CCF has no downstream `mt_get_fmeter_freq()` API, so the same physical
  register transaction runs through the DT's native syscon regmap.

The four original missing exports are `scp_get_reserve_mem_phys`,
`scp_get_reserve_mem_virt`, `scp_get_reserve_mem_size`, and `scp_ipidev`.
The provider also implements genuine `is_scp_ready`, awake lock/unlock and
ready-notifier interfaces. It never sets ready solely because probe succeeds.

## Exact corot ABI evidence

The generated stock-derived DT contains:

- SCP share carveout: physical `0x8f000000`, size `0x830000`, `no-map`.
- Secure dump ID 0: `secure_dump_size = 0x480000`.
- Total ID table consumption: `0x82a900`; spare: `0x5700`.
- AOD ID 15: physical `0x8f81a900`, size `0x10000`.
- Outbound AOD IPI 37: mailbox 3, IRQ bit 2, word offset 4, one word.
- Inbound AOD IPI 38: mailbox 3, IRQ bit 15, word offset 30, one word.
- Mailboxes 0 through 4 consume 25, 14, 25, 26 and 17 of 32 IRQ slots.
- Awake SET/CLEAR registers: SCP-infra `0x10001000 + 0xb14/0xb18`.
- SCP-to-SPM clear: SCP CFG `0x1c724000 + 0x94`.
- Firmware expected TCM size in READY: `0x300000`.
- Secure firmware RESET_RELEASE: SiP function `0xc2000528`, operation 3.
- SCP 26 MHz resource request: SiP function `0xc2000232`, operation 1,
  argument 1; argument 0 releases it.
- Secure ULPOSC on/off uses that DVFS function's operations 2/3.
- DT ULPOSC targets are 300, 500, 620 and 700 MHz; tolerance is the reference
  40 per thousand. Each calibration message waits for a real mailbox ACK.

Zero-sized memory IDs still consume their proper positions in the firmware
ID table. Missing/invalid memory or mailbox resources cause probe errors.
A failed physical awake, calibration, or secure resource handshake leaves
readiness false and propagates real errors.

## Framework corrections

`drivers/soc/mediatek/mtk-mbox.c` now checks resources and full raw transfer
bounds, uses managed IRQ registration for probe rollback, and acknowledges
unregistered receive messages while reporting their loss. Consumer-buffer
updates synchronize with ISR access instead of panicking on a missing buffer.

`mtk_tinysys_ipi.c` and its header retain the real rpmsg transport and add an
explicit channel count, index checks, complete partial-registration rollback,
and an unregister operation requiring quiesced IRQs/users. WAIT and POLLING
senders share a pin ownership flag. Invalid atomic WAIT calls return an error.
ACK waits clear stale completions and require an actual receive event; signal
interruptions and absent buffers are errors. The ISR reads the live ACK holder
rather than copying the atomic value.

`drivers/rpmsg/mtk_rpmsg_mbox.c` no longer frees the device twice when current
rpmsg core registration fails: that core already calls `put_device()` and the
registered release callback.

Firmware-ready work and sends become possible only after all resources and
IRQs exist and firmware release succeeds. Cleanup cancels work before freeing
IRQs/endpoints, revokes exported pin tables before their memory is freed, and
revokes shared-memory getters before their WC mapping is removed.

## Build integration

Parent `drivers/misc/mediatek/Kconfig` must source
`drivers/misc/mediatek/scp/Kconfig`; its Makefile must contain
`obj-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += scp/`.

Required built-in configuration:

```
CONFIG_MTK_MBOX=y
CONFIG_RPMSG_MTK_MBOX=y
CONFIG_MTK_IPI=y
CONFIG_MTK_TINYSYS_SCP_SUPPORT=y
CONFIG_OF_RESERVED_MEM=y
CONFIG_COMMON_CLK=y
CONFIG_MFD_SYSCON=y
```

The SCP Kconfig selects `MFD_SYSCON` and requires the real mailbox/IPI framework
built in. The existing upstream `CONFIG_MTK_SCP=m` is a separate remoteproc
interface with a different binding and does not provide these vendor symbols.

The root task owns sequential builds and source `.config` preservation.
Focused verification command:

```
python3 scripts/build-corot.py --source linux-native --name r351 --target scp --jobs 8
```

The root has applied parent wiring and required configuration selections.
Independent DT memory and mailbox ABI calculations passed. The initial focused
compile exposed the old reserved-memory declaration API, a missing OF include,
and a short register-name buffer; all were corrected.

The root's fresh focused SCP target at `2026-11-15 13:16:30` passed in
`out/r351/scp-build.log` (last build section): zero errors and zero warnings in
new SCP source. Six pre-existing missing-prototype warnings remain in the
mailbox core. All three SCP objects are newer than their final source files.
`llvm-nm --defined-only out/r351/drivers/misc/mediatek/scp/rv/built-in.a`
confirmed actual text definitions and export records for the three reserved
memory getters, and a data definition plus export record for `scp_ipidev`.
The IPI provider's ordinary kernel/clock/regmap and native mailbox dependencies
remain real; no new vendor sensor/audio/AEE provider is required for this path.
This is scoped object/ABI verification, not a complete-kernel or runtime claim.

## Runtime boundaries

This is the focused genuine display dependency provider authorized by the root
agent. It does not import unrelated audio/sensor feature DVFS controls, vendor
AEE/logging, thermal work, or automatic crash recovery. A SCP watchdog is
reported, cleared through the real monitor/register operation, and makes the
service unavailable; no recovery success is fabricated.

The bootloader must load the matching corot SCP firmware and its secure
monitor must implement the reference SiP calls. Firmware READY, frequency
measurement, calibration ACKs, awake handshakes and AOD activity still require
actual device evidence. On failure, the provider reports the condition and
keeps readiness false; configured SCP PM notifications also cannot succeed
without a ready service.

The stock-derived tree currently has no `AOD_SCP_ON` feature node; the full AOD
consumer retains its real DT activation behavior. This provider does not add
or remove that feature switch. No device, USB, flashing or slot operations were
performed by this subtask.
