# corot display bring-up — status

Xiaomi Redmi K60 Ultra (`corot`), MediaTek MT6985 (Dimensity 9200+), mainline
Linux 7.2, vendor `mediatek_v2` DRM driver, GCE/CMDQ disabled so the whole
display path is programmed from the CPU.

All statements below are backed by register dumps or logs captured on the
device; the log files themselves are not in this repository.

---

## 1. Hardware

| | |
|---|---|
| Panel | CSOT **M12A** (`panel-m12-min`), 1220x2712 |
| Interface | 4-lane MIPI DSI, 1152 Mbps/lane, D-PHY |
| Mode | **command mode, panel has its own GRAM**, TE line back to the SoC |
| Compression | **DSC v1.7**, slice 610x12 (2 slices per line), 8 bpp, bpc 10, line buffer depth 11, `dsc_cfg` 0x28 |
| Backlight | works |
| Known-good reference | the Xiaomi bootloader (LK) drives this panel fine and shows a full-screen Mi logo every boot |

The panel is the strongest evidence available that the hardware, the MIPI
link and the panel's own DSC decoder are all fine.

---

## 2. What works

| Item | Evidence |
|---|---|
| Kernel boots, log catcher runs, device returns to stock Android | every round |
| mmsys clock gates | all four display blocks read `sta = 0` (open) |
| DSI PHY / PLL | locks at 1152 Mbps/lane; `phy_power_on done` |
| Panel power-on + init DCS sequence | panel lights up and accepts it |
| Panel TE | `DSI_INTSTA` bit 2 (`TE_RDY`) set — the panel is running and talking back |
| OVL setup | ROI `0x0a9804c4` (2712x1220), layer `SRC_SIZE` same, pitch 4880, address correct |
| DSC setup | every register (`PIC_W`/`PIC_H`/`SLICE_W`/`SLICE_H`/`CHUNK`/`BUF`/`MODE`/`CFG`/`ENC_W`/PPS0..PPS19) matches the vendor formula and LK's known-good values |
| **Pixels actually arrive and decode correctly** | the top ~1300 lines of every frame are pixel-perfect on the panel: colour bands, 1-line white ruler markers and the magenta band all land on the exact expected scanlines |
| Frame trigger runs | ~50 frames/s pushed; the image visibly updates |
| SoC survives | the display IRQ storm that used to reset the SoC after ~10.5 s is masked immediately after the first enable; test windows now run 90–200 s |

Because the panel reproduces our test pattern line-exactly in the region it
does receive, everything *up to the truncation* is correct: PS_WC, SIZE_CON,
VACT_NL, the DSC PPS table, the slice geometry and the MIPI timing.

---

## 3. What does not work

Every frame delivers only **~1300–1400 of 2712 lines**. Below that the panel
shows decoder noise whose colour follows the intended content (i.e. the panel
keeps decoding a stream that stopped).

Per frame, without exception:

| Block | Flag | Meaning |
|---|---|---|
| OVL (`disp_ovl0_2l@14402000`, DDP name `OVL0_2L`) | `INTSTA` bit 2 `FME_UND` | frame underflow |
| DSC (`0x1400c000`) | `INTSTA` bit 3 `DSC_ABN_EOF` | abnormal end of frame |
| DSI (`0x1400d000`) | `INTSTA` bit 12 `BUFFER_UNDERRUN`, bit 14 `INP_UNFINISH`, bit 4 `FRAME_DONE` | input did not finish |

### The two measurements that constrain the cause

1. **Frame period does not matter.** Pushing frames every 16 ms, 62 ms,
   125 ms, 250 ms or 500 ms changes nothing: the OVL still ends at the same
   line, and the flags are still one per push.
   *(r42 sweep: `pushed`/`ur`/`inp`/`frm` all scale with the push rate, the
   delivered line count does not.)*
2. **Content does not matter.** With the OVL layer disabled entirely — only
   the background colour, i.e. **zero DRAM reads** — the cut is at the same
   place. A flat colour behaves exactly like the full pattern.
   *(r43: the "frame underflow" IRQ count is 250 per 5 s in both the layer-on
   and layer-off phases, i.e. exactly one per frame either way.)*

So it is **not** DRAM bandwidth, not the memory/QoS path, not the mm clock,
not a DSC rate limit. Something ends the frame on a *count*, not on time.

### Where the OVL stops

The vendor's OVL dump decodes `DISP_REG_OVL_ADDCON_DBG` (0x244) as the OVL's
current position: `ROI_X` = bits[12:0], `ROI_Y` = bits[28:16].

* r44 traced it at 200 µs resolution: it sits frozen at Y = **1398** with the
  flow-control FSM in `wait_SOF`, `frame_done` clear, `frame_underrun` clear.
* r45 sampled it at every push: `ovlY_atpush = 1398` **every single frame**,
  and `Y` returns to 0 immediately after the push with the FSM in `eng_act`.
* So the OVL scans 0 -> ~1398 and stops on its own, in well under 16 ms.

That line number matches the boundary measured off photographs of the panel
(the flat-colour photographs put the cut at 1307 and 1325 lines; the magenta
band at 1356..1423 is only partly shown).

---

## 4. Root causes already found and fixed

| What | Detail |
|---|---|
| MUTEX never latched | MT6985 `MUTEX_EN` (0x20 + 0x20*n) only latches when the SOF source field is non-zero. `mtk_disp_mutex_src_set(..., true)` writes SINGLE_MODE (0) and the crtc path used it, so EN stayed 0 and nothing scanned out. Fixed by programming SOF = DSI0\|EOF (`0x41`) and enabling the mutex by CPU. |
| ovlsys SOF table | MT6985 has a dedicated ovlsys SOF table; the inherited DSI0<->DSI1 swap points the ovlsys mutex at a DSI that does not exist. |
| Three missing MT6985 DSI specialisations | `mtk_dsi_clk_hs_mode` must write `DSI_PHY_LCPAT`; `mtk_dsi_poweron` must use the *mt6983* mipi_tx lane config; the per-frame trigger must skip the `mmsys+0xF0` write on MT6985. |
| Display IRQ storm reset the SoC | the DSI underrun path fed `mtk_drm_crtc_dump()` + `mtk_smi_dbg_hang_detect()` from the IRQ handler. Masking `DSI_INTEN` and the MUTEX INTEN **immediately** after `mtk_drm_first_enable()` (nothing slow before it) fixed it. |
| Top of the frame was garbage | the DSC must use the vendor's PPS table: `CONFIG_MI_DISP_DSC2712=y` plus `dsc_config_panel_name = 0x6d126102` in the panel's `dsc_params`. `PS_WC` 1224 -> 1220 and `SIZE_CON` 408 -> 407 match LK. |
| `DSI_TX_BUF_RW_TIMES` and the valid threshold | use the vendor's literal `/9` and `/18`; `buffer_unit` is 32 on MT6985 and gives 450 instead of 800. |
| OVL pointed at the wrong buffer | fbcon stays on the simplefb buffer, so the OVL's layer 0 address is repointed to it. |

### Corrections to earlier assumptions (things that were believed and are not true)

* `DSI_INTSTA` bit 31 is **not** a mirror of `DSI_START` — it is a genuine busy
  flag that asserts after the START edge and clears when the transfer ends.
* `DSI_CON_CTRL` has **no** `DSI_EN` bit on MT6985; its writable bits are
  {0,2,4,16,20,21,24,25,27}.
* `DSI_SHADOW_DEBUG` (0x190) is read-only on this part.
* The "DSI is busy" reading in early rounds came from polling bit 31 while the
  engine was legitimately running, not from a hang.
* `DSC_CON`: forcing LK's `0x00010089` breaks the picture, because bit 3
  (`IN_SRC_SEL`) is the dual-pipe input select and this is a single-pipe path.
  Our `0x00014001` matches the vendor Linux driver.

---

## 5. Current best hypothesis

The vendor's per-frame trigger sequence (`mtk_dsi_trigger`,
`MTK_TRIG_FLAG_TRIGGER`) writes more than `DSI_START`:

| Register | Value | Do we write it per frame? |
|---|---|---|
| `DSI_CMD_TYPE1_HS` (0x6c) | `\|= BIT(16)` (stay in HS across the horizontal blanking) | **no** |
| `DSI_CMDQ0` (0xd00) | `0x002c3909` | **no** |
| `DSI_CMDQ_SIZE` (0x60) | `0x8001` | **no** |
| `DSI_CON_CTRL` (0x10) | bit 0, 1 then 0 | **no** |
| `DSI_START` (0x00) | 0 then 1 | yes |

Our 60 Hz CPU timer only pokes `DSI_START`. The bring-up frame
(`mtk_crtc_comp_trigger`) does run the whole sequence — which is exactly the
frame that looks best. Hypothesis: every subsequent frame is malformed, the
DSI ends it early (`INP_UNFINISH`), the back-pressure stops the DSC and the
OVL, and the OVL reports `FME_UND`. That would explain all three flags, the
constant line count and the independence from period and content.

### The next experiment (r47)

An A/B sweep, one candidate per time window, with the **background colour
encoding the phase** so the result is readable off a photograph: the OVL layer
is disabled, so if a candidate fixes the truncation the whole screen becomes a
flat colour instead of only the top half.

| Phase | Colour | Change under test |
|---|---|---|
| 0 | red | baseline |
| 1 | green | full vendor trigger sequence per frame (the four missing writes) |
| 2 | blue | full sequence minus `CMDQ0`/`CMDQ_SIZE`/`CMD_TYPE1_HS` (isolate which part matters) |
| 3 | yellow | full sequence without the MUTEX EN pulse |
| 4 | magenta | full sequence with SOF = `0x01` (DSI0 only, no EOF bit) |
| 5 | cyan | baseline without the MUTEX EN pulse |

Alternatives if that fails, in order of likelihood:

1. Re-examine the MUTEX per-frame re-arm against the vendor's GCE flow
   (`DISP_REG_MUTEX_CFG`, `EN` 0->1, and which register blocks get written).
2. Test whether the delivered amount is a byte count rather than a line count
   by changing the DSC target bpp (the DSC rate control targets a constant bpp,
   so the two are hard to tell apart — this decouples them).
3. Port the remaining MT6985 differences of `mtk_disp_ovl.c`
   (`MT6985_OVL_LAYER_OFFEST`, `is_right_ovl_comp_MT6985`, the MT6985 INTEN
   set) and the full MT6985 crossbar cases in `mtk_drm_ddp.c`
   (`mtk_ddp_mout_en` / `sout_sel` / `sel_in` currently have no
   `case MMSYS_MT6985:`).
4. Enable GCE/CMDQ properly and use the vendor's real per-frame flow.

---

## 7. r47 .. r63: every remaining hypothesis, and what it cost

Everything below is measured.  The r47 sweep described in section 5 ran, and
then each surviving candidate was tested on its own.

### 7.1 Eliminated by experiment

| Round | Candidate | Result |
|---|---|---|
| r47 | the full vendor per-frame DSI trigger sequence (the four writes of section 5) | **no change** - same cut, same flags |
| r47 | `SOF = 0x01` (DSI0 without the EOF bit) | **wedges the pipeline permanently** - the OVL stops scanning (`in_frame` frozen at 1398) and does not recover.  Ruled out for good.  It also poisoned phases 3-5 of that round, so "no MUTEX EN pulse" was never actually observed. |
| r49 | `DSI_CON_CTRL` pacing bits BIT(24) (`CM_MODE_WAIT_DATA_EVERY_LINE`) and BIT(27) (`CM_WAIT_FIFO_FULL`) | **no change**; BIT(24) alone makes it worse (DSI `busy`/timeouts grow) |
| r50 | `DSI_TX_BUF_RW_TIMES` - the value is exactly one full frame in 9-byte units (367627) while only ~51.5 % is delivered | x2 and max: **no change** |
| r51 | switching the panel's own refresh rate per phase (120/60/144/90 Hz) | mid-stream switching **freezes the whole pipeline instantly** (the panel stops accepting, back-pressure stops the OVL) - unusable as a test |
| r55 | `DSI_LFR_CON` (0x30), `DSI_LFR_STA` (0x34), `DSI_VFP_EARLY_STOP` (0x3c), `DSI_TARGET_NL` (0x300) | **all read 0** - the DSI has no frame-length bound programmed.  The vendor's `VFP_EARLY_STOP` feature (a "minimum lines then end the frame early" register) would have explained a fixed line count exactly, but it is disabled. |
| r61 | display power domains being switched off by genpd (`pd_ignore_unused`) | no effect on the truncation |

### 7.2 The two facts that survived everything

1. **The cut is a time, not a count, as far as the hardware is concerned.**
   Per-frame measurement (r48, verified again in r49/r55/r56):
   `dsi_us avg = 7051..7120`, `ovl_us` identical, `ovlY in_frame = 0..1398`,
   `ur = inp = frm = aeof = pushed`.  The window is stable to a few tens of
   microseconds on every single frame.
2. **The stop line is `min(OVL ROI height, ~1398)`** (r50: ROI 1000 -> stops at
   1000; ROI 2000 and 2712 -> both stop at 1398).  So the ROI can pull the cut
   *earlier* but nothing can push it later.

Rates, for the record: the OVL produces ~198 lines/ms; 60 Hz needs only 163;
the MIPI link is ~42 % busy while this happens.  Nothing is saturated.  The
delivered data is 1398 x 1220 = 1.71 MB, i.e. 51.5 % of a frame.

**7051..7120 us is very close to one 144 Hz panel frame (6.94 ms)** and far from
60 Hz (16.67 ms) or 120 Hz (8.33 ms), which is why the panel's own refresh rate
remains the leading suspect and would also explain why the cut ignores our frame
period, our content and every SoC-side register.

### 7.3 The one experiment r50 also produced, and what it proves

In r50's ROI=1000 phase the frame **finished** (the OVL reached its ROI), and
the bottom of the panel turned into **clean, uniform stale content instead of
decoder noise**.  That is the direct proof of the mechanism: the noise is what
the panel's DSC decoder emits when it is handed a *truncated* stream.  It is not
a wrong pixel, not a wrong register - that part of the frame simply never
arrives, and the decoder keeps chewing the bits that follow.

### 7.4 Why the panel refresh-rate test kept failing to run

* r51 proved the rate cannot be changed while frames are flowing.
* r54/r56 put the change at bring-up (one DCS sequence), but r54's boot died
  before the display came up, so the call never executed, and r56 - which did
  boot - produced **no `COROT-FPS` line at all**.  `corot_m12_apply_fps()`
  returns silently when its dsi pointer is unset, so "did not run" and "ran and
  failed" were indistinguishable.
* The measurement that was supposed to settle it independently does not exist:
  **`DSI_INTSTA` bit 2 (`TE_RDY`) is a level, not a per-frame pulse** (r58
  counted 500 edges in 500 polls), and `DSI_STATE_DBG7` (0x164, the register the
  vendor's msync code reads as the VFP period) reads `1`, not a period (r59).

So the panel rate is still **untested**, not disproven.

### 7.5 The blocker: the display bring-up now resets the SoC

From r52 onwards almost every test build fails to bring the display up at all.
The device resets ~10.5-11 s into the kernel, i.e. exactly as `mtk_drm_probe`
starts, and then loops.  What was established about it:

* It is **not** the console: removing `console=tty0` did not help (r60), and
  neither did restoring it (r63).
* It is **not** the power domains (r61) and **not** the deferred-probe queue
  (r53's retry net made things worse; r58's single late kick changed nothing).
* **It is not the kernel source**: r62 was built from r56's exact sources and
  failed, and r63 from r56's exact sources *and* device tree and failed too -
  while r55 and r56 themselves booted twice each.  The failure is therefore
  environmental or time-varying (device state left by the previous boot), not
  something a source diff can explain.
* The reset is a hard hang, not a panic (no Oops, no `panic=15` trace).

**Do not trust the last line of a captured log as the death point.**  The log
catcher's final, unflushed chunk is lost with the reset: r63's log ends at
9.44 s while the reset is at ~11 s, and an early conclusion ("dies inside
`devm_kzalloc`") was drawn from exactly that stale tail and is wrong.

### 7.6 Practical notes

* `shutil.copy2()` preserves the backup's mtime, so `make` will happily skip the
  dtb after a restore.  `touch` the dts (or `rm` the dtb) or the flash silently
  tests the previous configuration.
* Build staleness: the kernel banner carries `dirty #NNN`; confirm it advanced.
* The DTB's bootargs are visible in the built dtb, so verify a bootargs change
  actually landed there before spending a flash cycle on it.

### 7.7 Where to go next

1. **Cold-boot the device** (full power-off, not a reboot) before the next test:
   the bring-up failure is state-dependent and LK's display init path differs
   between a cold boot and a warm reboot.
2. Then **observe the "no MUTEX EN pulse" phase**: it is still the last untested
   dynamic candidate (r47's attempt never ran, r59 never got a boot).
3. Deliver the 60 Hz change with a marker that cannot be lost (repeat the print,
   or write a byte to the command buffer) and see whether the cut moves.  If it
   does not, the panel rate is out and the next step is per-frame byte counters
   on the OVL and DSI output paths.
4. Only after that: the MT6985 OVL/crossbar code that was never ported
   (`mtk_ovl_mmsys_mapping_MT6985`, `is_right_ovl_comp_MT6985`, the MT6985
   INTEN set, and the missing `case MMSYS_MT6985:` in `mtk_ddp_mout_en` /
   `sout_sel` / `sel_in`).

### 7.8 r64 .. r69: the boot hang, the bootloader's display path, and the mechanism

**The boot hang was self-inflicted: our own USB instrumentation.**  Every
failing boot stopped at 9.44 s, and the last line was always

    COROT-USBREG[1] pm=... soft_conn=1 ...

`corot_usb_dump_work()` in `drivers/usb/mtu3/mtu3_plat.c` reads the MTU3
controller's registers (`U3D_POWER_MANAGEMENT`, `U3D_DEVICE_CONTROL`,
`SSUSB_U2_CTRL(0)`, the IP power-control registers) and reschedules itself every
4000 ms, first firing 5000 ms after probe - exactly the 5.34 s / 9.44 s cadence
in every log.  A USB controller idle for a few seconds is runtime-suspended, and
reading its registers with power/clock gated stalls the interconnect: a hard
hang with no Oops, then a watchdog reset.  Whether it stalls depends on the
runtime-PM state at that instant, which is the whole "the same build boots one
time and dies the next" story.  It also explains why r62 and r63 - r56's *exact*
kernel sources and device tree - failed where r56 itself had booted: the dump
lives in a file none of those rounds touched.  Removed in r64; `COROT-USBREG`
now appears zero times in the log.

**A round marker is required to tie a log to an image.**  The `dirty #NNN`
banner cannot be used for this: `scripts/mkcompile_h` only bumps that number
when the version string changes, so successive builds of a patch that does not
touch the version carry the same number.  r64's log showed a `COROT-USBREG`
line although a binary grep of both the built `Image` and the *decoded packed
kernel* found zero occurrences.  Since r65 every round prints

    COROT-MARKER r65-...: DSI probe reached (image check)

at the DSI probe (~0.37 s, before anything can fail), so a captured log can be
tied to its image.  The pack pipeline was verified separately: unpacking
`corot-boot.img` reproduces the built `Image` byte for byte.

**The log tail is trustworthy** - the catcher fsyncs after every chunk ("push
every chunk: a WDT reset must not lose the tail"), so the last line really is
where the kernel stopped.  An earlier conclusion drawn from a "lost tail" was
wrong; the tail is not lost.

**Skipping the DRM bring-up proves where the resets came from.**  r67/r68 made
`mtk_drm_probe()` return `-ENODEV` immediately.  The kernel then ran for
**210 seconds without a single reset** (with a work item re-drawing a test
pattern every 5 s the whole time), against ~11 s for every round that ran the
bring-up.  So the resets come from our DRM bring-up, not from the hardware or
the environment.

**The bootloader's display path, as read from the hardware.**  With our DRM
skipped nothing had touched the OVL since LK set it up, so its registers were
the live truth (r68):

    COROT-LKOVL r68 en=0x00000006 dp=0x07000001
                    con=0x00000000 size=0x00000000 pitch=0x00000000
                    off=0x00000000 addr=0xfd91f000 roi=... src=... bg=...

Layer 0 is **not configured** (`CON`/`SIZE`/`PITCH` all zero) - LK switched its
overlay off after drawing its logo.  **The logo on screen is not a live image:
the panel is self-refreshing from its own GRAM.**  Two consequences:

* nothing written to *any* framebuffer can appear on screen while LK's pipeline
  is off - which is why the r67/r68 pattern never showed;
* the address in the L0 `ADDR` register (0xfd91f000) is a leftover from an
  unconfigured layer, not proof of what was being scanned, so it should not be
  used to "fix" the simplefb address.  Our DTS deliberately places simplefb at
  0xfda1f000 and documents 0xfd91f000 as the videolfb reserved-region start.
  (An earlier conclusion in this session - that a wrong console framebuffer
  address was hanging the bus - does not hold: that address is inside a reserved
  DRAM region, and writing plain DRAM does not hang an interconnect.)

**The mechanism of the truncation, quantified.**  The frame is cut after a
window of ~7.05 ms (measured every frame, stable to tens of microseconds).  The
OVL produces ~198 lines/ms.  A full frame needs 2712 lines / 7.05 ms =
**385 lines/ms** - the pipeline is roughly **twice** too slow to finish inside
the window.  This is consistent with everything measured before: r50's ROI=1000
phase *completed* (it needs only 1000/198 = 5.05 ms) and produced clean stale
content instead of decoder noise, while 1398+ lines never finish.  It also
explains why the cut ignores our frame period, our content and every SoC-side
register: it is a race between production and a fixed deadline.

Two ways to fix it, both testable:

1. **Lengthen the window** - run the panel at 60 Hz instead of whatever LK left
   it at.  At 60 Hz the window is 16.7 ms and 198 lines/ms delivers 3300 lines,
   comfortably more than 2712.  This is the experiment r54/r56 tried and never
   actually ran (r54 died before the display came up; r56 booted but printed no
   `COROT-FPS` line at all, and the function returns silently when its dsi
   pointer is unset - so "never called" and "called and failed" were
   indistinguishable).
2. **Speed the pipeline up** - the display clocks in this DTS are stubs
   (`mmsys0_clk` is a syscon), so the real rates are whatever LK left behind,
   and LK only ever had to show a static logo.  Raising the mm clock to roughly
   twice its current rate has the same effect as (1).

### 7.9 State of the tree at this point

* r64 removed the USB register dump; r65 added the round marker; r66 added a
  repeated deferred-probe kick (without the USB traffic nothing triggers the
  queue any more, and then `mtk_drm_probe` never runs at all); r67/r68 skipped
  the DRM and drove the pattern from a late_initcall.
* r69 restores the DRM bring-up together with the one-shot 60 Hz request and
  the loud fps prints, i.e. the experiment described in 7.8 (1).  Its first
  flash did not boot: the log still contained the r68 pattern messages
  (`COROT-LKOVL`, `COROT-TESTPAT`) and none of `COROT-FPS`, `COROT-BC probe`,
  `COROT-DSI[output_enable]`, so that kernel died before the initramfs ran and
  the device fell back to fastboot.  Same code, so worth retrying: r55/r56
  booted on some attempts and not on others even with identical sources.

### 7.10 r70/r71: the probe chain, split into two concrete faults

The second r69 flash finally produced a genuine r69 log, and it showed the
bring-up dying **before our probe code runs at all**:

    [2366120us] COROT-DEFPROBE r66: kick 1
    [2367583us] probe of 14000000.dispsys-config returned -517 after 7 usecs
    [4382105us] COROT-DEFPROBE r66: kick 2   -> -517
    [6398114us] COROT-DEFPROBE r66: kick 3   -> -517
    <log ends at ~6.4 s>

`mtk_drm_probe()`'s own first print ("COROT-BC probe:1") never appears, and the
master returns `-EPROBE_DEFER` in ~8 microseconds.  Both point at the driver
core, not at our code: `really_probe()` runs `pinctrl_bind_pins()` *before* the
driver's probe, and the `dispsys_config` node carried

    pinctrl-names = "default";
    pinctrl-0 = <&dsi0_te_pins>;

Applying that pinmux touches the pinctrl registers; if that block's clock or
power is gated at that moment the access stalls the interconnect - a hard hang
with no Oops, then a watchdog reset.  The TE pinmux is the bootloader's job
anyway, so **r70 removed the `pinctrl-0` reference**, and the kernel immediately
became stable: **160 s alive with no reset**, against 6.4 s before.

With the hang gone, what was left is clean and fully readable from the log:

* every display component probed successfully - `disp_mutex`, `disp-dsc-wrap0`,
  `disp-rdma`, `disp-ovl0`, the DSI, all the mmsys/ovlsys blocks;
* the panel driver probed: `m12-min: probed on /dsi@1400d000/panel@0`;
* the DSI deferred once and then bound (`probe of 1400d000.dsi returned 0`);
* the **only** device still returning `-EPROBE_DEFER`, forever, is
  `14000000.dispsys-config` - the DRM master;
* **the GCE never probes at all**: zero `probe of 1e980000` lines.

The master's node carries

    mboxes = <&gce 0 0 CMDQ_THR_PRIO_4>, <&gce 1 0 ...>, ... ;

and a mailbox phandle creates a device link to that supplier.  With the GCE
unbound the master defers forever and the display never starts - even though
every component it actually needs is ready.  This build disables CMDQ on
purpose, so it does not need the GCE at all: **r71 removed the `mboxes`
property** (only `mboxes`; the `gce-client-names` / `gce-subsys` /
`gce-event-names` / `gce-events` properties are plain data and were left
alone).

A tooling note learned the hard way: successive rounds can produce *identical*
logs (r70 and r71 both show the deferred-probe kicks and no DRM), so a log
cannot be attributed by content alone.  **Bump the round marker every round** -
`COROT-MARKER rNN` at the DSI probe makes "this kernel booted" versus "this is
last round's log" unambiguous.

---

## 6. Test instrumentation in the tree

The tree deliberately carries a lot of `COROT-*` debug code. It is how all of
the above was measured, and it is still the tool in use:

* register dumps of OVL / RDMA / DSC / DSI / MUTEX / DSI buffer at each stage
  of enable (`corot_dump_disp`, `corot_dump_dsi_buf`)
* `corot_test_pattern()` — writes a ruler into the framebuffer: 24 bands of
  113 lines, 1-line white markers, a ladder for the boundary, and colour
  bands reserved for edge detection
* `corot_ftrig_tick()` — the 60 Hz CPU frame trigger, which also samples the
  DSI status, the DSC status and the OVL scan position and prints a summary
  every 5 s
* `corot_mutex_enable_all()`, `corot_mt6985_xbar()`, `corot_mask_display_irqs*()`

**All of it should be stripped before this is proposed upstream.**

Log staleness check: the kernel banner contains `dirty #NNN` with the build
counter — always confirm it advanced before trusting a captured log.

---

## 8. r72–r87: the CMDQ path, and what the GCE error actually is

### 8.1 Where r72 left the tree

r71 removed `mboxes` from `dispsys-config`, so the DRM master could bind without
a GCE supplier.  That produced the boot-stable tree this work started from:
CPU-direct display path, `-DDRM_CMDQ_DISABLE`, kernel alive for 160 s.

### 8.2 Option B — build with the vendor's real CMDQ stack (r80/r81)

Remove `ccflags-y += -DDRM_CMDQ_DISABLE` and `mediatek-drm-y += mtk_cmdq_dummy.o`,
and build with `CONFIG_MTK_CMDQ_MBOX_EXT=y` while `CONFIG_MTK_CMDQ` and
`CONFIG_MTK_CMDQ_MBOX` are off.  The result is real progress on the *binding*
problem:

* `mtk_cmdq_mbox 1e980000.gce: register mailbox successfully`
* `probe of 1e980000.gce returned 0`
* `bound 14402000.disp-ovl0 / 1400c000.disp-dsc-wrap0 / 14010000.disp-rdma / 1400d000.dsi`
* `[drm] Initialized mediatek 0.0.0`, and the `-EPROBE_DEFER` count drops 8 → 1

**Hard constraint:** `mtk_cmdq_dummy.c` and `mtk-cmdq-helper-ext.c` define the
same `cmdq_*` symbols.  `-DDRM_CMDQ_DISABLE` and `CONFIG_MTK_CMDQ_MBOX_EXT=y` are
mutually exclusive — mixing them is a *link* error (`duplicate symbol:
cmdq_pkt_read / cmdq_mbox_create / gce_shift_bit`), not a runtime failure.  Any
revert of one half must revert the other.

But no frames appear at all, so option B trades a working picture for a working
probe chain.

### 8.3 The GCE error, decoded from r86's log

The failure is **not** "the GCE cannot be reached" and **not** "OVL bandwidth".
It is a GCE **error interrupt on the very first instruction of the packet**:

    XAGA-GCE: thr0 EN=0x1 STATUS=0x00000002 PC=0x68801000 IRQ=0x00000010
    [cmdq][err] pc:0x0000000068801000 end:0x00000000688011a8 err:-22 thread:0
    [CMDQ]<0>(0)[cmdq][err] begin of error irq 0
    [CMDQ]<0>(0)[cmdq] error irq buffer 0: pa:0x00000000688001000 iova:0x0000000000000000
    [CMDQ]<0>(0)[cmdq] >>0x68801000 0xa080000000001440 [Logic] Reg Index 0x0000 = 0x00001440
    [cmdq][err] cannot get dev domain @cmdq_thread_irq_handler,1438
    [cmdq] failed to get mediatek,smi
    [cmdq] failed to find smi node
    [cmdq][err] smi hang:0

Read them together:

* `IRQ=0x00000010` with `STATUS=0x2` is the thread's error state, and the reported
  PC is the packet's *first* instruction — the GCE never got as far as executing
  the display writes.  The driver's `err:-22` is its classification of that
  status, not an independent fault code.
* `iova:0x0000000000000000` for a command buffer whose physical address is
  `0x68801000`, and `iommu_get_domain_for_dev(cmdq->mbox.dev)` returning NULL:
  **the GCE has no IOMMU domain.**
* `failed to get mediatek,smi` / `failed to find smi node`: the GCE has no SMI
  supplier either.
* **`smi hang:0` is not evidence of a healthy SMI.**  With no SMI nodes in the
  tree at all, the hang check has nothing to interrogate and returns 0 by
  default.  Earlier rounds read this line as "the SMI is fine"; it means nothing.

The GCE is a DMA engine that fetches its command buffer from DRAM.  In the
vendor tree the node carries

    mediatek,smi = <&smi_mdp_2x1_subcommon>;
    iommus      = <&mdp_iommu M4U_PORT_L39_GCE_DM>;
    dma-mask-bit = <34>;

and this device tree has no SMI or IOMMU nodes whatsoever.  That is the concrete
blocker for the CMDQ path.

### 8.4 Why option B alone cannot fix the truncation

The GCE *writes registers*.  It does not produce pixels.  Section 7.8's race is
about pixel production: the stop line is `198 lines/ms × 7.05 ms ≈ 1396`, against
a measured 1398.  Making the GCE work changes which agent writes the MUTEX/OVL
registers; it does not change how fast the OVL fills a line.  So the CMDQ path and
the truncation are two separate problems, and fixing the first cannot fix the
second.

### 8.5 The two levers, with the vendor's own numbers

`mtk_disp_bdg.c` documents `bdg_mm_clk` = 270 / 405 / 546 MHz and
`disp_pipe_line_time = width × 1000 / bdg_mm_clk`.  For width 1220:

| OPP | µs per line | lines/ms | 2712 lines need |
|-----|-------------|----------|-----------------|
| 270 MHz | 4.52 | **221** | 12.3 ms |
| 405 MHz | 3.01 | 332 | 8.2 ms |
| 546 MHz | 2.23 | **448** | **6.05 ms** |

The measured 198 lines/ms sits in the 270 MHz band, so the display is running at
the lowest operating point — which is what the bootloader left behind, since it
only ever had to show a static logo.

* **Lengthen the window.**  At 60 Hz the window is 16.7 ms and 198 lines/ms
  delivers 3300 lines > 2712.  This is the experiment r54/r56 tried and never ran.
* **Speed the pipeline up.**  Requires the 546 MHz OPP, i.e. MM DVFS.

### 8.6 r87 — the 60 Hz experiment, made observable

r87 goes back to the CPU-direct path that did put a picture on the panel, and
inside it:

1. restores `Makefile.pre-B` (the two no-CMDQ lines) and the matching pre-B CMDQ
   config (`MBOX_EXT` off, `CMDQ_MBOX=y`, `CMDQ=m`) — both halves together, per
   8.2;
2. makes `corot_m12_apply_fps_tag()` print, on every call, which call site it came
   from (tag), the per-command success/failure tally with the first failure, and
   **a DCS read-back of FCON (`0x2f`)**;
3. calls it from three tagged sites — `prepare`, `enable` (from
   `drm_panel_enable()`, i.e. after `mtk_dsi_clk_hs_mode(dsi, 1)` and before any
   frame is pushed) and `ftrig` (inside `trigger_without_cmdq()`).

The read-back is the point.  Section 7.4's ambiguity — "never called", "called
before the DSI was up", "called and rejected" all looking identical — is what kept
this lead untested for thirty rounds.  A read-back of `0x2f` returning `0x08` is
the difference between "we asked" and "the panel is at 60 Hz".

### 8.7 The SMI/LARB/IOMMU port, scoped (for whenever it is wanted)

Better news than the vendor-tree size suggests: **the drivers are already here.**

* `drivers/iommu/mtk_iommu_mt6985.c` exists and is already built —
  `CONFIG_MTK_IOMMU_MT6985=y`, compatibles `mediatek,mt6985-disp-iommu` and
  `mediatek,mt6985-mdp-iommu`.  It parses `mediatek,larbs`, and walks
  larb → `mediatek,smi` → sub-common chasing `compatible` strings for
  `"sub-common"`.
* `drivers/clk/mediatek/clk-mt6985-mmsys.c` already binds
  `mediatek,mt6985-mminfra_config` and provides `CLK_MMINFRA_GCE_D`,
  `CLK_MMINFRA_GCE_M`, `CLK_MMINFRA_SMI`, `CLK_MMINFRA_GCE_26M`, plus the
  `mmsys0/1`, `ovlsys`, `ovlsys1` gates.

What is missing is the device tree, plus one driver-side compatible:

* 3 top-level commons — `smi-disp-comm@1e801000`, `smi-mdp-comm@1e80f000`,
  `smi-sysram-comm@1e80b000`;
* ~14 sub-commons (`smi-mdp-2x1-subcommon@1e819000` is the GCE's);
* 25 larbs, `smi-larb0` … `smi-larb37` (ids 0–23, 25–35, 37 — there is no 24,
  36, 38 or 39, even though the GCE's `iommus` names port `L39`);
* `disp_iommu@1e802000` and `mdp_iommu@1e810000`, each with four
  `mediatek,iommu_banks` child nodes (one IRQ each) and a `mediatek,larbs` list
  of 19–21 phandles;
* `include/dt-bindings/memory/mt6985-larb-port.h`, which exists only in the
  vendor tree and is where `M4U_PORT_L39_GCE_DM` is defined;
* `mediatek,mt6985-smi-larb` / `-smi-common` entries in
  `drivers/memory/mtk-smi.c`, whose `mtk_smi_larb_of_ids[]` stops at mt8195.

The GCE's own node also needs `mediatek,smi` added; note that the CMDQ driver uses
that phandle **only** to create a PM-runtime device link, so on its own it is not
what unblocks the fetch — the larb and IOMMU nodes are.

### 8.8 Two measurement mistakes worth not repeating

* `COROT-PHASE_MS` is 8000, so the per-frame trigger re-pulses the MUTEX every
  8 s, not every frame.  The 7.05 ms cut therefore cannot be our own re-trigger;
  combined with the frame-period sweep in 7.2 it is a fixed deadline, not a race
  we control.
* `COROT-GCETHR ... cur=0x15100200 / end=0x15100235` was read for several rounds
  as "the GCE is executing and its PC is moving".  The driver's own
  `cmdq_thread_get_pc()` reports `PC=0x68801000` — the packet.  The `cur`/`end`
  pair comes from a different register and does not track execution.  Do not use
  it as evidence that the GCE ran.

### 8.9 The instrument that was missing: `expdb`

`cust:/boot-log.txt` is written by the initramfs, so it only exists if the kernel
reached userspace.  Every failure before that looked like "no log at all" — which
is exactly how r88 was first read, twice.

The kernel already had the answer.  `CONFIG_COROT_MARKER_WRITER=y` mirrors every
`printk()` into the LK log_store ring at physical **0x7ffbf000**
(`drivers/misc/corot-marker-writer.c`):

    0x0000  magic      re-asserted on every write, because MTK aee/mrdump_mini
                       rewrites the region header
    0x0004  cursor     next ring write offset
    0x0008  total      bytes ever written
    0x1000  stage      corot_marker_stage()
    0x2000  ring body  COROT_RING_SZ = 0xE000 = 57344 bytes

and every write is followed by `dcache_clean_poc()` *precisely* so that a WDT hard
reset cannot lose it.  LK then recovers the region into the **expdb** partition on
the next boot.  Read it from Android once Android is back:

    adb shell su -c "dd if=/dev/block/by-name/expdb bs=1M" > expdb.bin

expdb is 128 MiB (`/dev/block/sdc3`) and the ring is **not** at offset 0, so search
the whole partition — a 2 MiB head read looks empty and is misleading.
`.zcode/readexpdb.sh` and `.zcode/dumpexpdb.sh` do this and print every `COROT-*`
line.

Two caveats.  The ring cursor lives in DRAM and survives a reset, so the ring
**accumulates across boots** and can hold several runs interleaved — split it at
the `COROT-MARKER rNN-...` lines to attribute content to a boot.  And the ring is
only 56 KB while a single `corot_dump_disp()` is ~4.5 KB, so a run with five dump
stages can rotate its own early lines out; absence of a line is not proof the code
did not run.

### 8.10 r87 / r88 / r88b: three unrelated faults, all now identified

r87 went back to the CPU-direct build (`Makefile.pre-B` plus the pre-B CMDQ
config) and tagged every FCON call site.  All three runs failed to reach
userspace.  The expdb log shows three separate causes stacked on top of each
other — none of them the one being tested.

1. **`mboxes` left in the device tree.**  Turning `CONFIG_MTK_CMDQ_MBOX_EXT` off
   means nothing binds `mediatek,mt6985-gce` any more, but the dispsys node still
   named it as the master's mailbox supplier, so the master deferred forever:

       probe of 14000000.dispsys-config returned -517 after 7 usecs     (forever)
       probe of 1400d000.dsi           returned 0 after 6850 usecs

   `-517` is `-EPROBE_DEFER`, and every component the master actually needs had
   already probed.  r71 knew this; the option-B round put `mboxes` back,
   so the revert had to take it out again.  Fixed in r88.

2. **The userspace telemetry reader.**  `corot-initramfs/init-log.c`'s `tele()`
   reads DSI, MUTEX, OVL and GCE registers through `/dev/mem` every two seconds.
   While the display power domain is on that is harmless — which is why r86 ran a
   full 60 s with it.  The moment the master fails to bind the domain stays off,
   every read returns `0x00000000`, and the access stalls the interconnect:

       tele 0 0x00000000 ... (2.37 s)   tele 1 ... (4.38 s)
       tele 2 ... (6.40 s)   tele 3 ... (8.42 s)
       <log ends at 10.43 s, exactly the fifth call>

   This is the userspace twin of the in-kernel `corot_tele_thread()` that r81
   disabled — only the kernel side had been fixed.  Fixed in r88.

   **The ramdisk build path matters.**  The running ramdisk comes from
   `corot-work/corot-initramfs-log/`, built by `build_log_initramfs.sh` /
   `rebuild_init_log.sh` with `-Wl,-e,_start`, and the result is copied into
   `corot-work/corot-initramfs/`.  The `root/init` sitting in `corot-initramfs/`
   is a 4328-byte leftover from an older `init.c` flow and is *not* what runs.
   Rebuilding the wrong directory produces a ramdisk that does not run.
   Control that proves the path: rebuild the unpatched source that way and `init`
   comes out at 68072 bytes and the cpio at 69120 — byte-identical sizes to the
   known-good build.

3. **The 60 Hz FCON write panicked the kernel** — see 8.11.  This is the one that
   actually mattered.

### 8.11 The SError, and why the FCON write had to move

Reproduced identically **5 times out of 5** (expdb):

    COROT-MARKER r87-fps60: frame trigger reached
    COROT-FPSR r87 enter[ftrig]: fps=60 ... n=7 want=0x08
    COROT-DSI[transfer] type=0x29 tx_len=6        <- 0xf0 page select, OK
    COROT-STAGE dsi_start: START 0->1 (was CON=0x00000000 MODE=0x00000000)
    COROT-STAGE dsi_start: DSI_EN was lost, re-enabling
    COROT-STAGE dsi_start done: START=0x00000001 INTSTA=0x80000000
    COROT-DSI[transfer] type=0x15 tx_len=2        <- 0x6f 0x44
    0Kernel panic - not syncing: Asynchronous SError Interrupt

`COROT-FPSR r87 result[...]` and `readback[...]` never appear, so it dies on
command **2 of 7**.  The trigger runs with the DSI reading `DSI_CON_CTRL =
0x00000000` — disabled.  The first command is a generic long write that happens to
get through; the second, a DCS short write, raises an asynchronous bus error.

During a healthy enable the same register reads `0x00000021` with
`DSI_START = 0x00000010`.  So r89 sends the FCON sequence from
`mtk_output_dsi_enable()`, immediately after `mtk_dsi_clk_hs_mode(dsi, 1)`, with
the DSI registers printed either side of the call, and adds
`corot_m12_dsi_ready()` — a gate the panel driver checks before any DCS write, set
in that same window and cleared in `mtk_dsi_stop()`.  The worst case is now a loud
`COROT-FPSR r89 SKIP[tag]` instead of a panic, and the frame trigger no longer
touches the panel at all.

A finding for 7.4 while we are here: the panel's `prepare` and `enable` callbacks
**never run** in this bring-up — `enter[prepare]` and `enter[enable]` count 0
across every boot in the ring — so of the three tagged call sites r87 added, the
frame trigger was the only live one.  `COROT-DSC[after_panel_enable]` does print,
which means `drm_panel_enable()` is reached; the panel's own callbacks are what do
not fire.

### 8.12 The one-line difference from the state that did scan out a frame

`git diff 680d554ab70a caae3b9b4f95 -- <the dts>` is a single hunk:

    -		pinctrl-names = "default";
    -		pinctrl-0 = <&dsi0_te_pins>;
    +		/* COROT r70: no pinctrl-0 here on purpose. ... */

So at `680d554ab70a` — the commit that put a frame on the panel — the dispsys
node carried **both** `pinctrl-0 = <&dsi0_te_pins>` and `mboxes`.  r70 removed the
pinmux and r71 removed `mboxes`, each for a real immediate symptom, and the
combination that demonstrably worked was never restored as a unit.  For a
command-mode panel the TE pin is the pacing signal, so it is worth re-testing
`pinctrl-0` on a node that does not run `pinctrl_bind_pins()` before its driver
probe — the r70 hang was in the *bind* path, not in the pinmux itself.

---

## 9. r89–r98: every lever tried, and what each one proved

### 9.1 The measurement, unchanged since r43

The CPU-direct build carries the frame telemetry, and on every single run it
reports the same thing (r94, r95, r97, r98 all agree):

    ovlY at_push=1398 in_frame=0..1398
    dsi_us n=... avg=7043..7084 max=7100 to=0
    ovl_us       avg=... (identical to dsi_us, to the microsecond)
    pushed == ur == inp == frm == aeof

Three things in there are worth staring at:

* the cut is at **1398 of 2712 lines = 51.5%** — very nearly half;
* `max=7100` is **bit-identical in every build**, including builds that changed
  the frame period, three display clocks, the PLL and the SMI tree.  A hard
  7.1 ms ceiling that nothing moves;
* `dsi_us == ovl_us` — OVL production and DSI transfer are locked together, so
  the OVL is being consumed at the DSI's pace, not the other way round.

### 9.2 Ruled out, each by a direct experiment

| variable | what was changed | result |
|---|---|---|
| our frame period | 16 ms → 500 ms (7.2) | stop line unchanged |
| `disp`/`disp1` | 624 → 728 MHz (`mainpll_d3`), `set_parent` ret=0 | unchanged |
| `ovl` | 624 → 728 MHz, ret=0 | unchanged |
| `mm_infra` | 624 → 832 MHz (`univpll_d3`), ret=0 | unchanged |
| SMI/LARB tree | 36 larbs + 10 commons + 2 IOMMUs ported (r95/r96) | **kernel crashes** in `crtc_enable` |
| display-master `iommus` | added to OVL/RDMA (r95) | **kernel crashes** (see 9.3) |
| MIPI TX PLL unit | `data_rate_adpt` Hz → MHz (r97) | unchanged |
| MIPI TX PLL band | forced the mt6985 PLL config to run at all (r98) | **ran, correct band, unchanged** |

The clock tree itself was printed for the record (`COROT-CLK r94`), read from
`mediatek,mt6985-scpsys` — **not** from `dispsys-config`, which is where an
`of_clk_get_by_name` attempt returns -EINVAL for all eight names.  The eight
muxes and the full parent list with rates are in
`corot-work/r94-clk-bootlog.txt`.

### 9.3 Two ways to crash this kernel, both now understood

* **Display-master `iommus` (r95).**  This build is CPU-direct, so
  `mtk_cmdq_dummy` hands the OVL the *physical* framebuffer address; an `iommus`
  property at the same time puts that master behind `disp_iommu`, so the OVL
  reads a physical address as an IOVA, misses the page table, and the fault path
  takes the kernel down.  Mapping a master onto the IOMMU and feeding it physical
  addresses are mutually exclusive — pick one.
* **The SMI/LARB tree on top of a live display (r96).**  The port configures
  larbs with the generic gen2 settings and commons with `bus_sel = 0`, overwriting
  whatever the bootloader had set for a display path that is *working*.  In the
  option-B build (r92) the same tree is harmless precisely because nothing is
  being scanned out there.  Doing this safely needs the vendor's real per-larb
  OSTD/bandwidth tables and the correct `bus_sel`/init values — the gen data that
  was deliberately skipped when the tree was generated.

### 9.4 The A-plan port worked, and it does not paint

r92 (r92-smiiommu) is a genuine success and worth keeping:

    probe of 1e802000.iommu returned 0        (smi_common 1e801000 resolved)
    probe of 1e810000.iommu returned 0        (sub-common chased to 1e80f000)
    mtk_iommu_device_group create group, dev:1e980000.gce, domid:0
    XAGA-DOWN IOMMU attach dev=1e980000.gce ... ids[0]=0x4e1 larb=39 port=1
    mtk_iommu_domain_finalise ... table:0x42c00000
    XAGA-DOWN IOMMU config dev=1e980000.gce enable=1
    mtk-smi-common 1e801000.smi-common: mtk-smi-common probed, commid=0
    mtk_cmdq_mbox 1e980000.gce: register mailbox successfully
    [drm] Initialized mediatek 0.0.0

`0x4e1` = 1249 = `MTK_M4U_PORT_ID(MM_TAB, NORMAL_DOM, 39, 1)`, so the port maths
is right, and **every cmdq error is gone** (`iova:` 0, `cannot get dev` 0,
`err:-` 0, `smi hang` 0, `Kernel panic` 0).  The kernel is stable for the full
observation window with the vendor's real CMDQ stack.

But **option B does not put anything on the panel**: with the CPU-direct trigger
compiled out, the panel keeps the bootloader's logo and the SoC eventually resets
— which is what "stuck at Redmi then reboot" looks like.  The DRM binds, the GCE
runs, and nothing paints.  So the SMI/IOMMU work is banked but not yet usable for
the display.

### 9.5 The panel-rate route (the original lever 1) is blocked, precisely

Sections 7.8 and 8.11 stand.  r90 nailed the reason the FCON sequence cannot be
delivered from this kernel:

    COROT-FPSR r90 cmd[dsi_en] 1/7 cmd=0xf0 len=5
    COROT-DSI[transfer] type=0x29 tx_len=6          <- generic long write, OK
    COROT-STAGE dsi_start: DSI_EN was lost, re-enabling
    COROT-FPSR r90 cmd[dsi_en] 1/7 cmd=0xf0 ret=0   <- SUCCESS
    COROT-FPSR r90 cmd[dsi_en] 2/7 cmd=0x6f len=1
    COROT-DSI[transfer] type=0x23 tx_len=2          <- dies

Forcing every command through `mipi_dsi_generic_write()` did not help: the
discriminator is not DCS-vs-generic.  **This kernel's DSI command path completes
exactly one transfer and the second one stalls**, whether it is a DCS short write
or a generic one.  The vendor sends the whole panel init sequence through the
GCE-driven DSI CMDQ (`mipi_dsi_dcs_write_gce2`), which is why the panel's
`prepare`/`enable` callbacks have never once run here.

### 9.6 The PLL: two real bugs found, and neither is the cause

Both were found by reading the code after the experiment failed, and both are
worth keeping:

1. **`data_rate_adpt` unit mismatch.**  All 15 readers in `mtk_mipi_tx.c` are
   written `rate = data_rate_adpt ? data_rate_adpt : data_rate / 1000000`, so the
   field is in **MHz**; but `mtk_dsi_set_data_rate()` — our own bring-up
   workaround — stored `data_rate * 1000000`.  With 1152000000 in the field the
   PLL took the `>= 6000` band instead of `>= 1000`.  Fixed in r97 (the other
   four call sites in `mtk_dsi.c` already passed MHz).
2. **The PLL was never programmed at all.**  `mtk_mipi_tx_pll_prepare_mt6985()`
   begins with `if (mtk_is_mipi_tx_enable(hw)) return 0;` "if mipitx is on, skip
   it".  The bootloader left the PHY on to draw its logo, so the check is always
   true and nothing this kernel asks for ever reached the PLL.  r98 confirms it
   (`mipitx_on=1`) and forces the configuration through
   (`phy=0x0` → the D-PHY path).

After both fixes the log says the right thing

    COROT-PLL r97: adpt=1152 data_rate=0 -> rate=1152 MHz txdiv=2 div3=3 div3_en=1

i.e. the `>= 1000 MHz` chain that 1152 Mbps/lane needs — **and the throughput does
not change by a single line**.  `PLL_CON0` reads `0x84ec4ec4` before and after, so
the link is where it always was.  The serial link is therefore not the limiter
either.

Note `mipi_tx->data_rate` is **0** while `data_rate_adpt` is 1152: every path that
reads the first field falls back to `data_rate / 1000000` = 0, and the band
selection would land in the `else` branch ("data rate is too low") — which is
another reason the `data_rate_adpt` workaround existed.  Worth tidying.

### 9.7 What is left, honestly

Everything that can be varied from the pixel-clock and link side has now been
varied, and the 1398-line / 7100 µs ceiling does not move.  What remains is the
frame-boundary mechanism itself — the thing that *ends* the transfer at 7.1 ms:

* the panel's TE window (if the panel is at ~141–144 Hz the window is ~7 ms, and
  198 lines/ms only fills 1398 of 2712 lines in it).  stock Android agrees the
  panel is a 144 Hz-class part: `mSupportedRefreshRates = [60, 144, 120, 90]`,
  `defaultModeId` = 144 Hz.  The fix is lever 1, which needs the DSI command path
  (9.5);
* or whatever in the MUTEX/DSI path treats 7.1 ms as the end of a frame.  The
  next concrete step would be to instrument that boundary — `mtk_dsi_wait_idle`,
  `mtk_crtc_comp_trigger`, `corot_mutex_frame_trigger` and the MUTEX's own
  SOF/EOF registers — rather than the data path, which has now been cleared.

Two numbers to keep in mind while doing it: the OVL produces ~198 lines/ms and a
full frame needs ~385, so **any** fix must roughly double the effective rate or
roughly halve the required one (i.e. 60 Hz instead of 144 Hz).

### 9.8 Measurement hygiene learned the hard way

* **A boot loop looks exactly like a passing test** if liveness is inferred from
  "not visible in fastboot".  r92 and r95 both looked like "still running" while
  the device was crash-looping.  The reliable liveness signals are the
  initramfs's `tele N alive` counter growing, `ro.boot.bootreason`, and the
  absence of MTK's exception dumps (`dump_gpu_dfd_register`, `[RGU]
  mtk_wdt_pre_init`, `[PMIF]`, `MTK_DRM_DEBUG_CTL`) in the ring.
* **The expdb ring is only 56 KB and accumulates across boots**, and one
  `corot_dump_disp()` is ~4.5 KB.  Split it at the `COROT-MARKER rNN-...` lines
  before reading anything into it — content from four rounds can be interleaved,
  and an absent line is not proof that code did not run.
* **`cust:/boot-log.txt` being byte-identical to the previous round means the
  kernel never reached userspace.**  That is the fastest "this build did not
  boot" test there is.
* The initramfs hands the device back to fastboot at `hb/2 >= 90` (~3 min).  A
  longer window is available (`apply_hold_initrd.py` set it to an hour) but then
  the device has to be recovered with a long-press instead of automatically, and
  every cycle script that waits for fastboot will time out.
