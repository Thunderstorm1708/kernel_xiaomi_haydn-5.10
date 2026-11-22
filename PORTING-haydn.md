# haydn (Mi 11X Pro / Redmi K40 Pro+, SM8350 "lahaina") on Linux 5.10

Branch `haydn-5.10` (main) and `haydn-5.10-diag` (main + on-screen boot log and
debug options, see [Debugging boot](#debugging-boot)). Work in progress: the kernel
builds and installs; boot bring-up is ongoing.

This file is the porting log for this tree. It records what was taken from where,
every change made to get lahaina running on 5.10 and why, how to build, flash
and debug, and what is still open. It is written so that a person or an AI picking
this up later (or porting haydn/lahaina to another kernel version) can follow the
same path without rediscovering it. Update it together with each change.

## Contents

1. [Base](#base)
2. [Approach](#approach)
3. [Build](#build)
4. [Install: AnyKernel3 zip](#install-anykernel3-zip)
5. [Status](#status)
6. [Changes against msm-5.10](#changes-against-msm-510)
7. [Debugging boot](#debugging-boot)
8. [Boot test history](#boot-test-history)
9. [Known issues and TODO](#known-issues-and-todo)
10. [Porting to another kernel: checklist](#porting-to-another-kernel-checklist)
11. [Device tree side](#device-tree-side)
12. [Credits](#credits)

## Base

| Part | Source |
| --- | --- |
| Core kernel + Qualcomm 5.10 drivers | msm-5.10 from LineageOS `android_kernel_qcom_sm8450` (`lineage-20`, ed07fc6e2500): Qualcomm `android13-5.10-waipio`, merged up to Google ACK `ASB-2026-11-02_12-5.10` (5.10.272). It already contains the 5.10 lahaina drivers: GCC/disp/gpu/cam/video clocks, pinctrl, interconnect, LLCC, RPMh clocks, UFS PHY, KGSL with Adreno 660. |
| haydn/lahaina device trees | 5.4 tree (`kichukic/android_kernel_xiaomi_sm8350` b6eeaf3b8), `arch/arm64/boot/dts/vendor` |
| Xiaomi drivers | 5.4 tree: Focaltech SPI touch + `xiaomi_touch`, FPC fingerprint, Xiaomi pmic-glink charger (BQ2597X), hwid, `mi_thermal_interface` + `thermal_message`, IR SPI |
| techpacks | 5.4 tree (`techpack/`: display, audio, camera, video, dataipa, datarmnet, datarmnet-ext), ported to 5.10 one at a time |
| Reference | `xiaomi-lahaina-devs/5.10-sm8350-H` (an ACK 5.10 haydn port that boots), used to cross-check fixes and DT. See [Credits](#credits). |

KernelSU and SUSFS are not included. They are added once 5.10 boots.

The first attempt (ACK 5.10 with the whole 5.4 vendor tree grafted on top) is kept on
branch `wip-5.4-graft` for reference only.

## Approach

* **The ROM's userspace stays untouched.** The shipping haydn ROMs (vendor, HALs,
  firmware) are built for the msm-5.4 kernel. The kernel therefore keeps the
  msm-5.4 interfaces that vendor userspace talks to: the 5.4 device tree, the 5.4
  techpacks, msm-5.4 uapi values, legacy ION, the PIL/subsystem_restart framework
  (`/dev/subsys_*`, `/sys/kernel/boot_*`), and so on.
* **Use msm-5.10 for SoC drivers, but use lahaina data from 5.4.** The msm-5.10 lahaina drivers
  are newer than the 5.4 ones, but Qualcomm stopped tuning lahaina in them
  (waipio was the target). Where a 5.10 driver has per-SoC tables or sequences
  (pinctrl groups, NoC QoS, LLCC slices, clock PM ops, GPU BCL), the lahaina data
  is synced from msm-5.4 while the 5.10 driver code is kept.
* **Where a 5.10 driver expects 5.10 infrastructure that lahaina's 5.4 DT does not
  provide** (remoteproc handles, new DT properties), the 5.4 driver is used
  instead, or the DT gets the missing property. Examples: cdsp-loader, sensors_ssc,
  qseecom.
* **Hardware that lahaina does not have is disabled** in `haydn_GKI.config` (SCMI
  DCVS, eUSB2, MHI device/EP, esoc, guest VM, q6v5 PAS, and others) so
  that no wrong driver binds or loads.
* **No device or vendor tree changes are needed to try it.** The kernel ships as an
  AnyKernel3 zip that replaces the kernel, the DTB, the DTBO and the vendor_boot modules
  on an installed ROM.

## Build

Local:

```
make ARCH=arm64 LLVM=1 LLVM_IAS=1 O=out \
     gki_defconfig vendor/lahaina_GKI.config vendor/haydn_GKI.config
make ARCH=arm64 LLVM=1 LLVM_IAS=1 O=out -j$(nproc) Image modules dtbs
```

Diagnostic build (branch `haydn-5.10-diag`): add `vendor/haydn_diag.config` as
the last fragment.

* `vendor/lahaina_GKI.config`: msm-5.10 `waipio_GKI.config` with the SoC options
  switched to lahaina.
* `vendor/haydn_GKI.config`: Xiaomi drivers. Everything the 5.4 haydn kernel had
  built in stays built in (generated from the 5.4 `.config`). It also disables
  the drivers for hardware that lahaina/haydn does not have. No LTO/CFI during
  bring-up.
* `vendor/haydn_diag.config` (diag branch only): simplefb + fbcon console,
  `panic_timeout=0`, `ignore_loglevel`, `CONFIG_HAYDN_BOOTLOG`.

Build with `TARGET_PRODUCT=haydn` when building inside a ROM tree (LineageOS passes it
through `TARGET_KERNEL_ADDITIONAL_FLAGS`): the audio Kbuild picks the cs35l41
amps and the camera Makefile the haydn sensor config from it.

Outputs:

* `arch/arm64/boot/Image`
* `arch/arm64/boot/dts/vendor/qcom/lahaina{,-v2,-v2.1}.dtb`, concatenated into the
  vendor_boot `dtb`
* `arch/arm64/boot/dts/vendor/qcom/haydn-sm8350-overlay.dtbo`, packed with
  `mkdtboimg.py` into `dtbo.img`
* about 96 vendor modules (`.ko`)

### CI (GitHub Actions)

`.github/workflows/build.yml` runs on every push to `haydn-5.10` /
`haydn-5.10-diag` and on manual dispatch:

* ubuntu-24.04, clang/lld 18.
* ccache, kept between runs with `actions/cache` (restored before the build,
  saved even when the build fails). A warm build takes about 10 min; a cold one much longer.
* `set -o pipefail`, so a failing `make | tee build.log` fails the job.
* Artifacts:
  * `haydn-5.10-<run>`: Image, dtb, dtbo.img, `.config`, errors.txt, vendor_modules.
  * `haydn-5.10-anykernel3-<run>`: the flashable zip.
* Downloading artifacts requires being signed in to GitHub.

## Install: AnyKernel3 zip

AnyKernel3 is cloned at build time (osm0sis/AnyKernel3, pinned to `41f04bd`) and
is not committed to this repo. The zip contains:

| Content | Goes to |
| --- | --- |
| `Image` | `boot` (kernel replaced, ramdisk kept) |
| `dtbo.img` | `dtbo` |
| `dtb` | `vendor_boot` dtb |
| `vendor_modules/` (all `.ko` + `modules.load`, `modules.load.recovery`, `modules.alias`, `modules.softdep`) | `vendor_boot` vendor ramdisk `lib/modules` (replaced) |

Notes:

* Device check: `haydn`, `haydnin`, `haydnpro`.
* Current haydn ROMs (Zorix-based A16, Infinity-X A17) use a **monolithic** 5.4
  kernel and ship no `lib/modules` in the vendor ramdisk. The zip creates the
  directory. msm-5.4 modules elsewhere (vendor_dlkm) cannot load into 5.10, so all
  5.10 modules are loaded from first stage.
* `modules.load.recovery` = the haydn `BOOT_KERNEL_MODULES` set (11 modules:
  adsp_loader, apr, focaltech_touch, hwid, mmhardware_sysfs, msm_drm,
  q6_notifier, q6_pdr, qti_battery_charger_main, snd_event, xiaomi_touch). The
  build fails if one is missing.
* haydn has **no recovery partition**: recovery boots with the kernel in `boot`.
  A kernel that does not boot also breaks recovery. Keep fastboot and the stock
  images ready:
  ```
  fastboot flash boot_a boot.img        && fastboot flash boot_b boot.img
  fastboot flash vendor_boot_a vendor_boot.img && fastboot flash vendor_boot_b vendor_boot.img
  fastboot flash dtbo_a dtbo.img        && fastboot flash dtbo_b dtbo.img
  ```
  Stock Infinity-X images are in release `stock-5.4-boot`. Use the images of the
  ROM that is actually installed: other ROMs' images may not boot it.
* The repack was verified in CI against real ROM images (magiskboot under
  qemu-arm-static, `rom-inspect` branch `aktest.yml`).

## Status

| Area | State |
| --- | --- |
| Core kernel, lahaina SoC drivers (msm-5.10) | builds |
| DT: lahaina + haydn overlay | builds, overlay applies on all three base dtbs |
| Display (`techpack/display`, msm_drm + Xiaomi mi_disp) | builds; splash/DSI fixes for 5.10 (see below) |
| GPU (KGSL a660) | msm-5.10 driver, a660v2 BCL as on 5.4 |
| Audio (`techpack/audio`, APR/ASoC, cs35l41 amps, MIUS ultrasound) | builds; msm-5.4 DPCM/hostless behaviour restored |
| Camera (`techpack/camera`), video (`techpack/video`) | builds |
| IPA / rmnet (`techpack/dataipa`, `datarmnet`, `datarmnet-ext`) | builds |
| WLAN (`drivers/staging/qcacld-3.0`, QCA6490) + cnss2 | builds (`wlan.ko`) |
| FBE (HWKM v1, wrappedkey_v0) | builds |
| ION (msm heaps for gralloc/libion) | ported from msm-5.4 |
| CDSP / SLPI (sensors) loaders | 5.4 PIL versions |
| Boot on device | boots Zorix A16 to a usable system (diag #52: Settings, Wi-Fi, mobile data); post-boot fixes in progress, see [Boot test history](#boot-test-history) |

## Changes against msm-5.10

### lahaina data synced from msm-5.4

* `pinctrl-lahaina`: matches the msm-5.4 haydn tree.
* interconnect lahaina: NoC QoS is programmed at probe, as on msm-5.4.
* LLCC: lahaina slice table from msm-5.4 `llcc-lahaina.c`.
* clk lahaina: late system-sleep PM ops and debugcc lists as on msm-5.4.
* KGSL: a660v2 enables BCL as on msm-5.4.

### msm-5.4 interfaces kept for the ROM

* DRM core keeps the android11-5.4 behaviour the lahaina display HAL relies on:
  `drm_display_mode.private/private_flags/vrefresh`, the ACK 5.4 mode flags
  (`SUPPORTS_RGB/YUV`, `VID/CMD_MODE_PANEL`, `SEAMLESS`) and the QCOM DX/TIGHT/TILE
  modifiers.
* power_supply: the android11-5.4 GKI props the Xiaomi charger uses
  (`quick_charge_type`, `tx_adapter`, `signal_strength`, `reverse_chg_mode`).
* thermal: `/sys/class/thermal/thermal_message` for mi_thermald (thermal_message.c).
* uapi kept at msm-5.4 values where the lahaina HALs depend on them: IPA
  (`msm_ipa.h` and friends), rmnet flag bits, the video bitrate mode numbering
  (`V4L2_MPEG_VIDEO_BITRATE_MODE_CQ_MSM`), DRM mode flags and QCOM modifiers.
* **ION**: lahaina gralloc uses libion, not DMA-BUF heaps. The msm ION heaps
  (system, carveout, CMA, secure, ...) are ported from msm-5.4
  (`CONFIG_ION_MSM_HEAPS=y`, `include/linux/msm_ion_ids.h`,
  `trace/events/ion.h`). Adaptations: `dma-map-ops.h`, `vmap` for carveout
  mapping, a `dev_is_dma_coherent_hint_cached` helper, and haven→gunyah defines.
  `QCOM_LAZY_MAPPING` now depends on `QCOM_DMABUF_HEAPS || ION_MSM_HEAPS`, so it can be
  built in.

### GLINK

* msm-5.10 glink splits `qcom_glink_smem_register()` from
  `qcom_glink_smem_start()` (IRQ + version handshake); remoteproc calls start from
  its subdev. The 5.4 `glink_probe` helper (driven by PIL SSR notifications) must
  call start right after register, or no SMEM edge (ADSP, CDSP, SLPI, modem) ever
  opens a channel: fastrpc/sensors/APR/QMI over glink all fail, the modem stalls
  ("DOG detects stalled initialization") and the DSPs assert in
  `glink_channel_migration`.

### qseecom / keymaster

* lahaina qseecomd and keymaster allocate from the msm-5.4 ION heaps; msm-5.10
  qseecom assumed mem-buf buffers (`mem_buf_dma_buf_copy_vmperm` → -EINVAL), so
  no listener registered and keymaster never came up (vold → keystore2 →
  keymaster wait, init blocked in `vdc cryptfs mountFstab`).
  `mem_buf_dma_buf_exclusive_owner()` now answers from the ION flags for
  non-mem-buf buffers, and qseecom builds SHM bridges for ION buffers the 5.4
  way.
* logdump: haydn has **two** `logdump` partitions (sda24 and sde59); the
  bootlog alternates between them. `/dev/block/by-name/logdump` points to only
  one, so read both and take the higher `seq=`.

### DMA attributes

* msm-5.10 `qcom-dma-mapping.h` defines `DMA_ATTR_FORCE_COHERENT` as bit 15, the
  same bit as `DMA_ATTR_SYS_CACHE_ONLY_NWA` (`DMA_ATTR_IOMMU_USE_LLC_NWA`), which
  msm_drm sets on every import. The ported msm-5.4 ION map path must not read
  `FORCE_COHERENT` from the caller's attrs, or every display buffer is refused
  ("Cannot DMA map uncached buffer as IO-coherent").

### PIL / subsystem restart (not remoteproc)

* lahaina keeps the msm-5.4 PIL/SSR framework (the DT uses `qcom,pil-tz-generic`;
  `MSM_PIL`, `MSM_SUBSYSTEM_RESTART`, `MSM_PIL_SSR_GENERIC`). `subsys-pil-tz`
  allocates the PAS metadata buffer itself; cnss2's SSR ramdump path builds.
  `QCOM_Q6V5_PAS` is off.
* `cdsp-loader.c` and `sensors_ssc.c` are the **msm-5.4** versions. The 5.10 ones
  look up a `qcom,rproc-handle` that the 5.4 DT does not have ("Missing remotproc
  handle", cdsp-loader probe failure), so CDSP and SLPI were never started. The 5.4 ones
  use `subsystem_get()` and provide `/sys/kernel/boot_cdsp`, `boot_slpi`.
* `qcom_sysmon_get_reason()` stub (`!QCOM_SYSMON`) is `static inline` (duplicate
  symbol otherwise).

### Boot fixes

* `dwc3-msm-ops.c`: the kretprobe on `usb_ep_set_maxpacket_limit` assumed every
  endpoint is a dwc3 endpoint. With `dummy_hcd` built in, it dereferenced a
  non-dwc3 `usb_ep` and BRK-panicked at about 5.9 s (this also broke recovery). The entry
  handler now records only endpoints with `dep->dwc->eps[dep->number] == dep`,
  and the exit handler ignores the rest. `CONFIG_USB_DUMMY_HCD` (a test-only UDC) is off.
* qseecom probe returned -EINVAL: the lahaina qseecom node gets `qseecom_mem`,
  `qseecom_ta_mem` and `user_contig_mem` (the shmbridge regions the 5.10 driver needs).
* Focaltech SPI: with both `CONFIG_FB` (diag simplefb) and `CONFIG_DRM`, the
  driver used the FB notifier path (`FB_EARLY_EVENT_BLANK`, which is gone). It now prefers the
  DRM panel notifier (`CONFIG_FB && !CONFIG_DRM`).

### Display (`techpack/display`)

* sde_crtc: per-CRTC vblank enable/disable hooks (5.10 DRM core calls them through
  `drm_crtc_funcs`).
* Splash / DFPS carve-outs are `no-map`. Do not free them to the page allocator
  (`pfn_valid` guard; `memblock_is_map_memory` is not exported).
* `_sde_kms_drm_obj_init`: re-run `fill_modes` for DSI connectors after
  `possible_crtcs` is set, because 5.10 probes modes earlier than 5.4.
* `msm_smmu_one_to_one_map`: the splash region may already be mapped by the
  bootloader handoff. Return 0 if `iommu_iova_to_phys` already resolves it, and
  treat -EEXIST as success.

* TE: requesting the panel TE GPIO (82, PDC 140) as an IRQ re-muxes it away
  from `mdp_vsync` and hangs the SoC in `devm_request_irq()` during
  `dsi_display_bind`. `dsi_display_register_te_irq()` no longer requests it;
  ESD falls back to non-TE checking (found by xiaomi-lahaina-devs).

* GEM free: on 5.10 the DRM core drops the last GEM reference without
  `struct_mutex` (msm registers `.gem_free_object_unlocked`), but the 5.4
  techpack's `msm_gem_free_object()` expected the lock (it does
  `list_del(&msm_obj->mm_list)` from `priv->inactive_list`) and WARNed on every
  framebuffer destroy. Some callers drop references while already holding
  `struct_mutex`, so the free path cannot take it directly. Objects are now
  pushed onto `priv->free_list` (llist) and freed by `priv->free_work` on
  `priv->wq` under `struct_mutex`, the way upstream msm did it. Without this,
  the WARN storm froze the UI after boot (build #50).

### GPU

* msm-5.10 kgsl loads `a660_zap` itself (`qcom_mdt_load` into the `zap-shader`
  child's `memory-region`, then `qcom_scm_pas_auth_and_reset(13)`); the 5.4 DT
  only had the `qcom,kgsl-hyp` PIL node. `&msm_gpu { zap-shader { memory-region =
  <&pil_gpu_mem>; }; }` added. Without it the GPU never starts and SurfaceFlinger
  aborts in a loop.

### Slow screen wake (diag only)

* Diag builds had `console=tty0 fbcon=...` for a panic screen. Every resume
  then spent ~6.2 s in `pm_restore_console` (VT switch + fbcon redraw),
  measured by the diag PM timing. Removed from the diag cmdline (#94+);
  panics are still caught by bootlog. The normal build never had it.

* Even without fbcon, `pm_restore_console` still took up to ~1.1 s
  (#96): with no driver registered, `pm_vt_switch()` defaults to switching
  VTs around suspend. With `CONFIG_ANDROID` it now returns false (#102+).

### Bluetooth (QCA6490 "hastings" over HS-UART se18 @ 0x890000)

* #96: BT toggles on and straight back off. The vendor HAL powers the chip
  through btpower (`bt_ioctl: BT_CMD_PWR_CTRL`, BT_EN gpio), reads the SoC
  version over the UART at the default baud (works: `400c0110`), then
  switches the UART to high speed and the chip never answers:
  `InitTimeOut: SoC Initialization stuck`, secondary reason
  `SetBaudRateStuck`, `Captured UART CTS: 1`, then com.android.bluetooth
  aborts. The SE clock table (wrap2 s4) and the UART DT node match 5.4,
  and the driver logs no clock error. The UART driver's own log goes to
  ipc_logging, which this ROM has no debugfs for, so diag #102+ mirrors the
  control/power (not rx/tx data) messages to dmesg with
  `msm_geni_serial.haydn_uart_dbg=1` (`haydn-uart:` lines).
* #102 UART log: the HAL sends EDL set-baudrate (`01 48 fc 01 11`) at
  115200, then sets the host to 3.2 Mbaud: `set_termios src_clk
  freq_diff:1200000 baud:3200000 clk_rate:100000000 clk_div:2`. SE18's
  clock (wrap2 s4) uses `ftbl_gcc_qupv3_wrap0_s0_clk_src`, which stops at
  100 MHz, so the port ran at 3.125 Mbaud (-2.3%) and nothing was received
  after the switch. wrap2 s4 now uses the wrap1_s0 table (has 102.4 MHz =
  51.2 MHz x 2). If the bootloader left DFS on for this RCG the HW table wins;
  the `clk_rate` in the next log tells (#104+).
* #106: the table change works (102.4 MHz chosen, no freq_diff) but
  `SE clock set_rate error:-16` (+WARN): wrap2 s4's vdd table caps
  VDD_LOW at 100 MHz. wrap1_s0 allows 120 MHz at VDD_LOW, so wrap2 s4 now
  does too (#110+). Also seen: right after the set-baud command the host
  receives one garbage byte (`fe`/`fc`) at 115200, i.e. the controller has
  already switched and answers at 3.2 Mbaud before the host follows.
* The UART debug mirror is limited to the BT UART (`890000` in the device
  name), never the console UART.

### Missing text / blinking images: ION cache maintenance test

* #102 (KGSL range fix + GPU I/O coherency off): text and images (e.g. a
  shopping app's product grid) still drop out, so it is not KGSL's own
  memory. The 5.4 vendor stack allocates graphics buffers from `/dev/ion`
  (msm-5.4 ION ported in `drivers/staging/android/ion`). Its
  begin/end_cpu_access only sync through attachments that are mapped at
  that moment (`a->dma_mapped`), so CPU writes done while nothing is mapped
  are never cleaned. The `CONFIG_ION_FORCE_DMA_SYNC` path (whole buffer via
  the heap device, DMA addresses prepared at allocation) had no Kconfig;
  it is now the boot parameter `ion_force_dma_sync=1` (`core_param`), set in
  diag #104+. If that fixes it, the per-attachment path needs fixing.

* #106 (ion_force_dma_sync=1) made YouTube show green frames now and
  then (whole-buffer clean through the heap device on CPU access writes
  stale lines over video decoder output), so it is off again from diag
  #110 (the parameter stays, default off). #106 was also still blinking, and a video shows a
  diagonal black wedge across a GPU-rendered page (a quad half drawn), so
  this looks like a format/compression mismatch or early read, not CPU
  cache maintenance. Candidate: UBWC highest bank bit. KGSL uses 16 for
  a660 (15 on LPDDR4), SDE picks from `qcom,sde-highest-bank-bit =
  <0x8 0x3>, <0x7 0x2>` by `of_fdt_get_ddrtype()` and falls back to 2 (15)
  if the DDR type is unknown. `of_fdt_get_ddrtype()` on 5.10 reads
  `ddr_device_type` from `/memory` in the live tree. #108+ logs
  `haydn-ubwc: sde/gpu ddr_type .. highest_bank_bit ..` to compare.
  Quick discriminator: a screenshot (GPU composition) of a screen with
  missing text; if the screenshot is complete, the display side decodes
  the buffers wrongly.
* Result: screenshots on haydn show the same missing letters and black
  banners, so the rendered frames are incomplete (GPU side, not SDE/UBWC).
  Whole characters vanish consistently (e.g. every 'o'/'t' on a page),
  i.e. glyphs added to HWUI's glyph atlas texture later never land; images
  (uploads) blank the same way. Diag #114+ adds `msm_kgsl.gpu_ifpc` and
  `msm_kgsl.gpu_preempt` (default off in diag, on otherwise) to test GPU
  inter-frame power collapse and ringbuffer preemption; the boot log shows
  `haydn-gpu: ifpc .., preemption ..`.
* #114 (IFPC and preemption off): text still drops out; it is fine right
  after boot and goes wrong after a while. The phone also froze at a
  screen timeout and reset itself. Both switches default to on again in
  #116+ (still settable with `msm_kgsl.gpu_ifpc=0` / `gpu_preempt=0`).
* Live root tests on #125 (KernelSU), each one changed alone:
  `drop_caches` + `compact_memory` while text was fine did not make it
  vanish (not page migration/reclaim under the GPU);
  `force_rail_on/clk_on/bus_on/no_nap=1` did not help (not GPU power
  collapse); `debug.hwui.renderer=skiagl` + `stop; start` did not help
  (not the Vulkan path, it is below both GL and Vulkan). Scrolling also
  shows each glyph/icon doubled with an offset, seen by eye too.
* Suspect: stale GPU TLB. `_iopgtbl_unmap()` skips the TLB flush when the
  GPU is in slumber, assuming the GPU SMMU lost its TLB with the GPU power.
  If the SMMU stays powered on the 5.10 port, freed and reused pages stay
  mapped in the TLB and the GPU reads someone else's data: glyphs blank,
  more over time, independent of the API. Diag #126+ has
  `msm_kgsl.tlb_flush_slumber` (0644, default 0, diag cmdline 1) to flush
  always; toggle live through `/sys/module/msm_kgsl/parameters/`.
* #132 (tlb_flush_slumber=1): text still vanishes and parts still blink,
  so it is not the slumber TLB skip.
* Next: the GPU SMMU (`kgsl_smmu`, msm-arm-smmu-lahaina.dtsi) carries
  `dma-coherent`, so arm-smmu sets `ARM_SMMU_FEAT_COHERENT_WALK` ("coherent
  table walk" at boot) and KGSL's per-process io-pgtables, which copy the
  TTBR1 cfg, skip cleaning PTE writes from the CPU cache. If the walks are
  not really coherent, the GPU uses stale PTEs and reads the wrong pages.
  xiaomi-lahaina-devs/5.10-sm8350-H hit GPU faults from this and removed
  `dma-coherent` from `kgsl_smmu` (their stage 2Z-6). Diag #134+ does the
  same through `/delete-property/` in `haydn-sm8350.dtsi`; the boot log
  should then say "non-coherent table walk" for 3da0000.kgsl-smmu.
* **Dropped GPU submissions.** The #132 log (seq 38) has 3032
  `Adreno-GSL: ioctl ... (IOCTL_KGSL_GPU_COMMAND) failed: errno 14 Bad
  address` lines from many apps in one boot; every one is a draw batch that
  never reaches the GPU, which fits glyphs and parts of the UI missing or
  blinking. msm-5.4 KGSL copied the cmdlist, objlist and synclist elements
  with `kgsl_copy_from_user()` (min of kernel and user size, extra user bytes
  ignored); msm-5.10 uses `copy_struct_from_user()`, which fails (-E2BIG,
  turned into -EFAULT) when the user element is larger and its extra bytes
  are not zero. The vendor UMD is built for 5.4 KGSL. Diag #138+ copies these
  three lists the 5.4 way (`kgsl_list_copy()`) and logs any remaining
  `gpu_command -EFAULT at <stage>` with the sizes, rate limited.
* **Fixed in #138** (user confirmed: no missing text, no blinking). #138
  has both the 5.4-style list copy and the non-coherent `kgsl_smmu` walks;
  both are on the main branch since then. Which one did it is not yet
  split out (test a build without the `kgsl_smmu` change).
* `Skipped access check` (KGSL import of an ION buffer without mem_buf
  vmperm, ~1100/boot) is now dev_dbg.

### Root: KernelSU-Next (diag only)

* KernelSU-Next v3.4.2 (tag commit 4c706b59, 3355 commits) is vendored as
  plain files in `drivers/kernelsu` (from its `kernel/` directory plus the
  top-level `uapi/` headers, copied to `include/uapi`, which upstream links
  with a `../../uapi` symlink that would dangle here; the
  upstream `setup.sh` would clone it and add a symlink instead). Its Kbuild
  takes the version from git; in a vendored copy that fails, so the
  fallback is pinned to `KSU_VERSION 33355` (30000 + commit count) and tag
  `v3.4.2` so the manager accepts it. `drivers/Makefile` has
  `obj-$(CONFIG_KSU) += kernelsu/`, `drivers/Kconfig` sources its Kconfig.
  Needs KPROBES and EXT4_FS (both in GKI). Only `haydn_diag.config` sets
  `CONFIG_KSU=y` (#120+). SUSFS is not integrated (the `susfs/` file in
  the copy is not built). Manager: KernelSU-Next v3.4.2 release APK.
  Credit: KernelSU (tiann) and KernelSU-Next (rifsxd and contributors).

### Spontaneous resets during suspend (#102)

* One reset right after the first boot and one during a full suspend
  (log ends after `printk: Suspending console(s)`; next boot PMIC: PS_HOLD
  warm reset). bootlog cannot flush once suspend has started. ramoops is
  active (`ramoops: using 0x800000@0xb0000000`): after an unexpected
  reset, boot straight to OrangeFox and copy `/sys/fs/pstore/*`.

* #114: the freeze at screen timeout ended exactly like the #102 resets:
  the log stops right after `cs35l41 ... cs35l41_main_amp_event: event = 8`
  inside a suspend (UFS is already suspended, so bootlog cannot write).
  OrangeFox does not expose the ramoops records (`/sys/fs/pstore` empty).
  Diag #118+: `CONFIG_DPM_WATCHDOG` (timeout 5 s, needs EXPERT and
  PM_DEBUG) panics if a device suspend/resume callback hangs, ramoops keeps
  the panic dmesg, and bootlog copies the tail of
  `/sys/fs/pstore/{dmesg-ramoops-0,console-ramoops-0}` into the next boot's
  log at tick 20 (`==== bootlog: previous boot ... ====`).
* UBWC check (#114): `sde ddr_type 8 highest_bank_bit 3` (=16) and
  `gpu ddr_type 8 highest_bank_bit 16`, so they match; not the cause.
* BT at exactly 3.2 Mbaud (#114, no freq_diff, no set_rate error): still
  `SetBaudRateStuck`. The controller answers the set-baud command ~7 ms
  later at the new rate (one garbage byte at 115200), the host switches
  ~23 ms after the command, and afterwards nothing is received and the HAL
  sends nothing. The clock rate was not the (only) problem.

### GPU cache maintenance (missing UI text)

* `kgsl_cache_range_op()` (`drivers/gpu/msm/kgsl_sharedmem.c`) did
  `size += offset & PAGE_MASK; offset &= ~PAGE_MASK;` and then used the
  in-page remainder as a page index (sgt path) or as the start of a byte
  window (page-list path). The msm-5.10 version walks `memdesc->pages`
  for normal allocations, so for an unaligned range it skipped the
  range's first page (and could miss the last). msm-5.4 always built an
  sgt and mostly hid this. Symptom: letters missing in UI text (icons and
  images fine) until a theme change rebuilt the glyph atlas, with GPU I/O
  coherency on (#58+) or off (#61). Now the op covers
  `[offset >> PAGE_SHIFT, PAGE_ALIGN(offset + size))` on both paths (#92+).
  Any port of msm-5.10 KGSL should take this.
* #96 (range fix, I/O coherency on): text still drops out at random, so the
  coherent path itself is suspect. Diag #98+ turns GPU I/O coherency off
  again (`msm_kgsl.gpu_iocoherent=0`, set it to 1 to re-enable) on top of the
  range fix. The combination was never tested: #61 had coherency off, but
  still had the range bug. If text is fine there, the SMMU/interconnect
  coherent (snoop) path needs work before coherency can go back on.

### Audio (`techpack/audio`, ASoC core)

* `CONFIG_AUDIO_QGKI` carries the msm-5.4 core extensions. ASoC falls back to
  msm-5.4 style component callbacks (`.ops`, `.compr_ops`, `.pcm_new`), so the
  APR techpack runs mostly unchanged. The msm-5.4 async DPCM ops are not ported
  (BE ops run synchronously).
* `soc-pcm.c` (under `AUDIO_QGKI`):
  * DPCM links (`dynamic || no_pcm`) take their playback/capture directions from
    `dpcm_playback`/`dpcm_capture`, as on 5.4.
  * Hostless (`no_host_mode`) PCM restored from 5.4: the `no_host_hardware` table,
    hwparams in open, BE fixup and page allocation (`of_dma_configure` +
    `snd_pcm_lib_malloc_pages`) in hw_params, freeing in hw_free, and
    `hw_no_buffer` in `soc_new_pcm`.
* `soc-core.c`: missing-component messages at info level, with the node's full
  name, so a card that is stuck waiting for a component shows which one in the boot log.
* `lahaina.c`: dev PM QoS uses `PM_QOS_RESUME_LATENCY_NO_CONSTRAINT`
  (`PM_QOS_DEFAULT_VALUE` is gone from the dev PM QoS API).
* `msm-pcm-routing-v2.c`: `.read`/`.write` stubs are back on the routing
  component. It has no registers, but its DAPM muxes and widgets use
  reg/shift as virtual registers; without callbacks 5.10 ASoC returns -EIO
  (1132 read + 234 update_bits errors in one session, `Failed to add route
  ... VOC_EXT_EC MUX` at registration). msm-5.4 had them; only their comment
  was left.
* `iio/us_prox.c` (ultrasonic proximity): the IIO poll function gets real
  handlers (`iio_pollfunc_store_time` + a notify-done handler). 5.10's IIO
  core attaches it to the trigger when the triggered buffer is enabled and
  `request_threaded_irq()` refuses NULL handlers, so `buffer/enable` failed
  with -EINVAL and the in-call proximity never started. The routes for
  `RX1_CDC_DMA_DL_US`/`PRI_TDM_RX_1_DL_US` failing is expected: haydn uses
  `TERT_TDM_RX_1_DL_US`.
* `soc-pcm.c`: `snd_soc_set_runtime_hwparams()` returns 0 when
  `substream->runtime` is NULL (msm-5.4 behaviour). `soc_new_pcm()` applies
  `no_host_hardware` to hostless (`no_host_mode`) PCMs right after creating
  them, before any stream is open (#73 panic).
* `sound/core/pcm_native.c`: `snd_pcm_pre_start()` skips the "playback
  buffer empty" -EPIPE check for `hw_no_buffer` substreams again, as
  msm-5.4 did. ACK 5.10 keeps the `hw_no_buffer` field (and our `soc_new_pcm`
  sets it) but core ALSA never reads it, so starting the hostless voice FE
  (`VoiceMMode1`) failed: audio HAL `voice_start_usecase: cannot start
  channel: Broken pipe`, no call audio in either direction (#88+).
  Any QGKI audio port to a GKI 5.10 core needs this.
* `iio/us_prox.c` logs `us_prox: buffer enabled/disabled` so a test shows
  whether the ultrasound HAL starts the proximity stream; ADSP events show
  as `us_afe_callback: data = N` (0 = far, non-zero = near).
* `lahaina.c`: the BT/FM `SLIMBUS_7/8` dai links (`qcom,wcn-bt` and
  `qcom,wcn-btfm`; haydn's overlay uses `qcom,wcn-bt`, so the first fix in #57
  missed it) are left out unless an upstream `qcom,slim-ngd-v1.5.0`/`v2.1.0` node is available. Their
  codec `btfmslim_slave` sits on the msm `qcom,slim-ngd` controller, which 5.10
  does not drive, so it never registers and the whole card stayed deferred
  (build #52: `codec component btfmslim_slave not found for link SLIMBUS_7_RX`,
  no sound at all, and no ultrasonic proximity either, since that runs over the
  same card). BT call/A2DP-offload audio comes back when the slimbus NGD
  is ported.

### RPMh

* msm-5.10 `rpmh-rsc` has five TCS types (`FAST_PATH_TCS` = 4) and requires
  `qcom,tcs-config` to list all five, else probe fails with `-EINVAL` (silently,
  at arch_initcall, before any console). The 5.4 lahaina DT lists four, so the
  RSCs never probed and everything behind RPMh (regulators, rpmh clocks, BCM
  voters → GCC, NoC, SMMU, UFS, USB, display) deferred forever. Both RSC nodes now
  end with `<FAST_PATH_TCS 0>`.

### IOMMU

* msm-5.10 `arm_smmu_device_probe()` defers until `qcom-dma-iommu-generic`
  (`compatible = "qcom,iommu-dma"`, `memory-region` = the `linux,cma-default`
  pool, not dma-coherent) has probed. lahaina 5.4 DT had no such node; it is
  added under `&soc` and the CMA pool got the label `system_cma`.
* msm-5.4 arm-smmu handed off every SMR the bootloader left programmed;
  msm-5.10 hands off only those in `qcom,handoff-smrs`. Without it the splash
  display stream (MDSS, SID 0x820) is cut the moment apps-smmu probes. apps_smmu
  now lists `<0x820 0x0>, <0x800 0x0>`.

### QUPv3 (GENI SPI/I2C/UART)

* msm-5.10 `msm-geni-se` looks up its paths with `of_icc_get("qup-core")` and
  `("qup-ddr")`; the 5.4 DT only had msm-bus vectors, so all GENI controllers
  failed `geni_se_resources_init` with `-ENOENT` (touch SPI, cs35l41 I2C, ...).
  The three wrappers now carry `interconnects`/`interconnect-names` (core on
  `clk_virt`, QUP1 on `aggre1_noc`, QUP0/2 on `aggre2_noc`, DDR on `mc_virt`).

### First stage modules

* Android first stage init treats **any** module in `modules.load` that fails to
  insmod as fatal (`Failed to load kernel modules` → `InitFatalReboot` → reboot to
  bootloader). A module whose init returns `-ENODEV` because its DT node is missing
  is enough.
* msm-5.10 `cnss2` and `cnss_plat_ipc_qmi_svc` require a node with the
  `qcom,wlan` property; the lahaina cnss node now has it.
* logcat while adb is down: diag builds include `logcap` (from
  xiaomi-lahaina-devs), which copies errors/crashes and key tags into the kernel
  log so bootlog stores them in logdump.
* Debugging: boot with `printk.devkmsg=on`, otherwise `/dev/kmsg` rate limiting
  drops init's own error lines. Read them from logdump:
  `dd if=/dev/block/by-name/logdump bs=1M count=64 | tr -d '\000' | grep -a init:`.

### WLAN (cnss2 / qcacld)

* The WLAN PCIe link is never put to sleep on haydn for now
  (`cnss2.wlan_pcie_pm=0`):
  * runtime suspend: `cnss_pci_runtime_suspend()` returns `-EAGAIN`;
  * system suspend: the first `cnss_pci_suspend()` votes `MSM_PCIE_DISABLE_PC`
    (`pci_priv->disable_pc`), so only WoW is armed, the link stays in D0 and
    pci-msm skips its own suspend; the DRV check is skipped.
* Why: putting the link to sleep hung the whole SoC with no oops, panic or
  watchdog bark. Build #52: WLAN runtime suspend (qcacld
  `pmo_core_enable_wow_in_fw`, then the PCIe DRV handoff to the ADSP over the
  `pcie_drv` glink channel, lahaina DT `qcom,drv-supported`). Build #61 (runtime
  suspend already off): the first system suspend with Wi-Fi on, right after
  `__wlan_hdd_bus_suspend: starting bus suspend`; the phone reset on the next
  power key press.
* Cost: more idle and standby power. Next step: trace
  `msm_pcie_drv_suspend`/resume, the ADSP DRV handshake and the pci-msm link
  suspend, then set `wlan_pcie_pm` back to 1.
* **Still hangs with the link kept up and L1ss off (#120+/#122).** With
  `wlan_pcie_pm=0` (link in D0) and `qcom,no-l1ss-supported` on PCIe0 (diag
  since #82), two resets in one afternoon, both at screen-off system suspend.
  Both logs end on `pmo_core_enable_wow_in_fw: system suspend wow` with no
  `bus suspend succeeded` (normally 28 ms later). One was 60 s after boot, so
  a phone left with the screen off can reset over and over (it got warm). No
  WLAN firmware timeouts before either hang, no oops, nothing in pstore (no
  `previous boot` record: ramoops did not survive the reset). So the SoC hangs
  while the WOW_ENABLE command is sent or its ack is awaited.
* Diag #124+:
  * `haydn-wow:` markers before and after each step of
    `__wlan_hdd_bus_suspend` and `pmo_core_enable_wow_in_fw` (send, ack wait,
    hif early/bus suspend). Each marker forces a logdump write through
    `haydn_bootlog_sync()`, so the last `haydn-wow:` line in logdump names the
    step that hung. The write is bounded (100 ms for the lock, 250 ms for the
    bio, then it skips) because UFS can be suspending in parallel.
  * `pcie_aspm.policy=powersave`: `CONFIG_PCIEASPM_POWER_SUPERSAVE=y` (kept
    from the 5.4 config) lets the PCI core enable L1 substates on the
    QCA6490 side by itself, whatever pci-msm does. powersave keeps L1 and
    drops L1ss, which also takes the core's L1.2 LTR math out of the picture.
* #125 result: three more resets with Wi-Fi on, the last one with
  `/sys/power/pm_async=0` (devices suspend one at a time, so it is not an
  async ordering race). Every time the markers get through the whole WLAN
  part: `WOW_ENABLE sent (0)`, `ack wait done (0)`, `hif bus suspend done
  (0)`. So WoW itself works; the SoC hangs in a later suspend step, within
  the ~50 ms before the bootlog thread's next write (earlier logs ended on
  `system suspend wow` only because of that write interval).
* #132 (pm trace on, seq 38): the WLAN part completed (`bus suspend
  succeeded`, `done ... cnss_pci_suspend 0`), then cnss2 platform, three
  memshare clients and two coresight replicators suspended, and the last
  line on storage is `call type input input1 input_dev_suspend`
  (gpio-keys), 4 ms after WLAN finished. input_dev_suspend cannot hang the
  SoC, so the hang looks asynchronous: something fires a few ms after
  WLAN arms WoW, while the CPU does unrelated work. The hang also happens
  with Wi-Fi off and mobile data on (that log was lost), and not with both
  off.
* #138, `pm_async=0` (seq 44): the trace runs in order up to `call bus sd
  0:0:0:4 scsi_bus_suspend`, the UFS LUN that holds sde59; nothing can be
  written after that, so all earlier "last lines" were just storage going
  to sleep. The phone also hangs a few seconds after screen off with Wi-Fi
  and data both off.
* `mem_sleep=s2idle` still hangs. `/sys/power/pm_test` bisection (root,
  `CONFIG_PM_DEBUG`): `core`, `platform` and `devices` all hang, `freezer`
  does not. So a device in the suspend phase (or its resume, or the 5 s
  wait with devices suspended) hangs, and it comes after the UFS disks.
* Diag #140+: `haydn_pm_keep_ufs` (core_param 0644, default 0) skips every
  PM callback of `sd` and `ufshcd*` devices, so storage stays up and the
  trace can follow later devices. If the hang goes away with it set, UFS
  suspend itself is the suspect.
* #142 results: `haydn_pm_keep_ufs=1` + `pm_test=devices` passed three
  times; real suspend with it hung at or after the noirq phase (last line
  `call noirq ... cnss_pci_suspend_noirq`; all suspend and late callbacks,
  IPA, rmnet_ipa, clock controllers, rpmh, returned 0). UFS `spm_lvl` 1
  and 2 (instead of 3) both still hang in real suspend.
* What fits every result: bootlog itself. Its thread is not freezable and
  flushes to logdump every 5-50 ms through suspend, and the hang needs UFS
  to be suspended (freezer test fine, devices test with UFS kept up fine).
  bootlog is in haydn_GKI.config, so main has it too. Diag #144+ pauses
  all logdump writes from PM_SUSPEND_PREPARE to PM_POST_SUSPEND
  (`bootlog.pause_suspend`, default 1, 0644). If the hangs stop, that is
  the fix (and the per-device trace is no longer written past
  PM_SUSPEND_PREPARE).
* #144 (bootlog paused through suspend): still resets with screen off. So
  bootlog is not the cause.
* #146: the breadcrumb did not survive: sdam@8600 reads
  `17 01 00 00 04 00 18 00 24 00 1e 00 18 00 24 00` at 0x40, someone else's
  data, and our writes at 0x48 did not stick. #148+ moves it to SDAM 2
  (sdam@7100, which holds the restart reason at 0x48 and is writable from
  HLOS), bytes 0x4a-0x4f (zero before), and zeroes them after reading.
  Also: the extra SPMI writes slow suspend/resume enough to trigger a
  "Process system isn't responding" dialog on wake (debug only).
* **#148 breadcrumb result: the hang is on resume, not suspend.**
  `haydn-pmtrace: previous boot ended at after resume noirq callback (it
  returned), device cnss_pci 0000:01:00.0`. The WLAN endpoint is the last
  device with a noirq resume callback, and dpm_resume_noirq() calls
  resume_device_irqs() right after the noirq callbacks, before any early
  resume callback has turned clocks back on. So most likely a pending
  interrupt whose handler touches unclocked hardware (WLAN CE/MHI or IPA/GSI,
  which serves both Wi-Fi offload and mobile data) hangs the SoC.
* Diag #150+: `resume_irqs()` (kernel/irq/pm.c) writes each suspended IRQ
  to the breadcrumb (code 0x40, IRQ number) before re-enabling it and waits
  50 us so a pending interrupt runs; stage 0x32 is written after
  resume_device_irqs(). The next boot prints `previous boot ended while
  re-enabling device IRQ <n> on resume: <name> (chip, hwirq)`.
* **#150 result: IRQ 38, `spss` (GICv3 hwirq 384 = SPI 352).** That is the
  generic SCSR IRQ of the SPSS PIL node `qcom,spss@1880000`
  (`qcom,pil-tz-generic`, `qcom,pil-generic-irq-handler`), handled by
  `subsys_generic_handler()` in drivers/soc/qcom/subsys-pil-tz.c, a hard IRQ
  handler that reads sp2soc_irq_status / rmb_err_spare2 at 0x188xxxx. It is
  a wake IRQ; when resume_device_irqs() re-enables it with an interrupt
  pending, the handler reads that block before the resume callbacks have
  restored its votes, and the read hangs the SoC.
* #152 (SPSS IRQ parked) still resets, same breadcrumb (IRQ 38). Second
  look: the breadcrumb only covered IRQS_SUSPENDED IRQs. Wake IRQs stay
  armed through suspend (not IRQS_SUSPENDED) and resume_irq() replays a
  pending one in the same loop, unrecorded. IRQ 38 was a disabled non-wake
  IRQ (its handler cannot run), so it was just the last recorded IRQ before
  the culprit. Diag #154+ records wake-armed IRQs too (bit 31 = wake; the
  report says "WAKE IRQ").
* #154 (wake IRQs recorded too): still `device IRQ 38 spss`, no wake IRQ
  and no stage 0x32 after it, so the hang is right at IRQ 38 even with the
  IRQ parked. Diag #156+: SPSS not loaded at all (`status = "disabled"` on
  `/soc/qcom,spss@1880000` through the haydn overlay; SPSS backs optional
  StrongBox only), and the breadcrumb also records "IRQ n re-enabled + 50 us
  passed" (bit 30) for suspended IRQs and IRQ 38.
* #156 (SPSS disabled): still resets, breadcrumb again "IRQ 38", now
  `sps` (BAM, hwirq 304) because the numbering shifted without SPSS, and
  no "re-enabled + 50 us" record. Two different devices at the same loop
  position, and pm_test=devices (which never runs resume_device_irqs())
  also hangs: the hang is asynchronous, about 2 ms into resume_irqs(), not
  IRQ 38 itself. Suspect: a threaded IRQ handler, which runs in its kthread
  shortly after its IRQ is re-enabled while the loop has moved on. Diag
  #158+ records each threaded handler (`irq_thread_fn()`, bit 29) between
  "before dpm_resume_noirq" and "resume completed"; SPSS is enabled again
  (the subsys-pil-tz park stays).
* #158 (threaded handlers recorded): no threaded handler ran; again
  `device IRQ 38 spss` with no "re-enabled" record. So the hang is in the
  hard handler of the first pending IRQ, whichever device that is (spss,
  sps): handlers run right after resume_device_irqs(), before the early
  resume callbacks (dispcc/videocc/camcc pm_runtime_force_resume, IPA,
  i2c-geni, ...) restore what the hardware needs. Diag #160+:
  `haydn_late_irqs=1` (core_param 0644) moves resume_device_irqs() from
  dpm_resume_noirq() to the end of dpm_resume_early().
* #160 (`haydn_late_irqs=1`): still resets, breadcrumb again "device IRQ 38
  spss" although device IRQs are now re-enabled only after early resume.
  The same position under three different timings suggests the breadcrumb
  itself stops landing there rather than IRQ 38 being the culprit. It now
  resets on the first screen-off suspend (~30 s). With the breadcrumb and
  trace switched off live (`haydn_pm_sdam=0`, `haydn_pm_trace=0`) and
  `haydn_late_irqs=Y` it still resets. Open for next session: verify that
  SDAM writes land late in resume (e.g. a write in dpm_resume_end and in a
  PM_POST_SUSPEND notifier), check whether the PMIC arbiter / SPMI is
  usable at that point, and compare with a build without any of the
  haydn diag suspend hooks (pm trace, SDAM, IRQ breadcrumb, bootlog pause,
  SPSS park, late IRQs) to rule out the instrumentation.
* Fix candidate (diag #152+): subsys-pil-tz registers a PM notifier per
  subsystem with a generic IRQ; at PM_SUSPEND_PREPARE it drops the wake flag
  and disables the IRQ, at PM_POST_SUSPEND it re-enables both, so anything
  the SP signalled while asleep is handled once resume is complete. SPSS
  events can no longer wake the phone by themselves.
* Diag #146+: PMIC SDAM breadcrumb (`haydn_pm_sdam=1`, drivers/base/power/
  main.c). pmk8350 SDAM 23 (`sdam@8600`, not used on lahaina; yupik uses
  only 0xbf) keeps its contents across any reset short of removing the
  battery. Before and after every PM callback, suspend and resume, all
  phases, 6 bytes go to offset 0x48: 0xa5, a step code (bit 3 done, bit 2
  resume, bits 0-1 main/late/noirq), crc32 of the device name. suspend_enter()
  also writes stage codes 0x20 (noirq done), 0x22 (before CPU offline),
  0x23 (before irqs off/syscore/firmware), 0x24 (s2idle), 0x30/0x31 (back),
  0x3f (resume completed). 20 s after the next boot the kernel prints
  `haydn-pmtrace: previous boot ended ...` with the device name, then clears
  the magic. Never written with interrupts off (SPMI regmap may sleep).
* Diag #128+: `haydn_pm_trace=1` (`core_param`, 0644, live in
  `/sys/module/kernel/parameters/haydn_pm_trace`) logs every
  suspend-direction device callback in `dpm_run_callback()` before
  (`haydn-pm: call <phase> <driver> <device> <callback>`) and after
  (`haydn-pm: done ... <error>`), each followed by
  `haydn_bootlog_sync_fast()` (no lock wait, 50 ms bio wait, skipped while
  an earlier write is pending). The last `call` without a `done` is the
  device that hangs. If the last lines are UFS, storage went to sleep first
  and the hang is later (late/noirq phase, syscore or PSCI).
* Workaround until fixed: turn Wi-Fi off before leaving the phone with the
  screen off (#52 ran 20 min with Wi-Fi off and no hang).

### pstore / ramoops

* The ramoops carve-out (`haydn-sm8350.dtsi`, 8 MiB at 0xb0000000, no-map)
  registers fine (`ramoops: using 0x800000@0xb0000000`), and Android mounts
  `/sys/fs/pstore`, but it was empty even after a normal restart.
* Cause: every restart was a cold reset, which powers DDR down. Like the
  5.4 driver, `qcom-dload-mode` only sets `reboot_mode = REBOOT_WARM` when a
  download/dump mode is armed; with dump off (`DOWNLOAD_MODE_DEFAULT` unset)
  PSCI uses SYSTEM_RESET (cold), not SYSTEM_RESET2 (warm). The 5.4 lahaina
  DT has no `qcom,pshold` node either, so `msm-poweroff` (and its
  `qcom,force-warm-reboot`) is not used on either kernel.
* #132 with `reboot=warm`: the next suspend hang did not reset the phone
  at all (black, no response until a long power press), where every
  earlier hang reset by itself. Removed again in #136+; the warm reset
  (PSCI SYSTEM_RESET2) apparently does not complete on this firmware with
  download mode off. Earlier text, kept for reference: diag #132+ boots with `reboot=warm`: restarts and panics use SYSTEM_RESET2,
  DDR keeps its contents and pstore should show `console-ramoops-0` (and
  `dmesg-ramoops-0` after a panic). Check with a normal restart, then
  `echo c > /proc/sysrq-trigger` as root. A whole-SoC hang reset by the
  watchdog goes through TZ instead and may still be cold.

### fastrpc (adsprpc) and sensors

* msm-5.10 adsprpc refuses `FASTRPC_INIT_ATTACH_SENSORS` with `-ENOTCONN` until
  the PD restart (PDR) service reports the sensors PD up, and it only looks
  a PD up when the DT names it. The 5.4 lahaina DT had only
  `qcom,fastrpc-adsp-sensors-pdr`; lahaina runs sensors on the SLPI, so
  `qcom,fastrpc-slpi-sensors-pdr` is added (`msm/slpi/sensor_pd`, `tms/servreg`).
  Without it sscrpcd loops and the sensor HAL lists 2 sensors (stock 5.4: 41).

### Camera (`techpack/camera`)

* `cam_soc_util_regulator_enable()` warns and carries on when
  `regulator_set_voltage()` fails. Releasing the vote on power down was not
  enough: the rear sensor's power sequence enables cam_v_custom2 (2.8 V) and
  cam_vana (2.904 V) together on pm8008i_l7, so the second request always
  conflicted (#73 log). The rail stays at the first consumer's level.
* `cam_soc_util_regulator_disable()` releases the voltage vote with
  `(0, INT_MAX)` and also when `regulator_disable()` fails. A `(0, max)` vote
  still caps the rail for other consumers; the rear sensor's cam_vana
  (2.904 V) and cam_v_custom2 (2.8 V) share pm8008i_l7.

### devfreq (CPU-L3 / LLCC / DDR scaling)

* The lahaina DT's `qcom,devfreq-icc*` nodes ask for the msm-5.4 governors
  `mem_latency` (15 nodes), `compute` (3) and `bw_hwmon` (3). None were built,
  so every one failed with `Unable to find governor` and L3/LLCC/DDR votes
  never followed CPU load (stutter, and no power saving).
* Diag build #73 enables `ARM_MEMLAT_MON` + `DEVFREQ_GOV_MEMLAT`
  (mem_latency, compute):
  * `arm-memlat-mon.c`: msm-5.4's idle notifier chain (`IDLE_START`/`IDLE_END`,
    `idle_notifier_register`) is gone; it now hooks
    `android_vh_cpu_idle_enter/exit`.
  * `governor_memlat.c`: devfreq has no `min_freq`/`max_freq` on 5.10 (start
    just calls `update_devfreq()`); the memlat trace events are stubbed.
* `bw_hwmon` (`QCOM_BIMC_BWMON`, `DEVFREQ_GOV_QCOM_BW_HWMON`) is ported from
  diag #84: `governor_bw_hwmon.c` uses `scaling_max_freq` instead of
  `max_freq`, drops the `min_freq` writes and stubs `trace_bw_hwmon_*`.
* Diag #84+ prints every cpufreq policy (governor, cur/min/max, hw max) and the
  online/active CPU masks every 15 bootlog ticks (`show_cpufreq()`).
* Heat note for testers: the diag cmdline has `clk_ignore_unused
  pd_ignore_unused`, which keeps every unused clock and power domain on.
  The normal build does not.

### USB (UCSI / dwc3 role switch)

* msm-5.10's dwc3-msm only starts the controller when the Type-C role switch
  sets a role. `ucsi_find_fwnode()` takes the Nth child of the ucsi device as
  connector N and finds the role switch through the graph under it. The 5.4
  DT had the `port` directly under `qcom,ucsi`, so the lookup found nothing
  (`OF: graph: no port node found in /soc/qcom,pmic_glink/qcom,ucsi/port`), no
  role was set and the gadget never came up (`udc-core: couldn't find an
  available UDC`, adbd waiting for FUNCTIONFS_BIND). The port now sits inside
  a `connector` node.
* #82 froze ~1 min after a USB cable to another phone was unplugged: oops
  (NULL deref at 0x98) right after `DWC3 exited from low power mode`, from a
  host-role start with no xhci. The dwc3 child now has `usb-role-switch;
  role-switch-default-mode = "peripheral";` and `dwc3_otg_start_host()`
  returns -ENODEV without `dwc3_drd_sw` (#84+).
* #84 then panicked on a USB reconnect when the role went to host:
  `arm-smmu: Unhandled context fault from a600000.ssusb`, FAR =
  0x1344c7000 (a physical address), then `kernel BUG at arm-smmu.c:1017`.
  `xhci-plat` picks the first device with a DT node as its DMA `sysdev`,
  which is the `dwc3` child; that node has no `iommus` (they are on the
  `ssusb` glue parent, and the child is marked `linux,sysdev_is_parent`).
  So xhci used dma-direct, the controller DMA'd to raw physical addresses and
  the SMMU (SID 0x0) faulted. `xhci-plat.c` now follows
  `linux,sysdev_is_parent` like dwc3 core does for gadget mode; the PHY and
  TPL lookups still use the child node (#86+). Any msm dwc3 port to 5.10
  with xhci-plat needs this.
* #90: host mode works (a Nothing Phone 2 enumerated, no SMMU fault). Open:
  after the host session ended the gadget never came back (`udc-core:
  couldn't find an available UDC` every second from the USB gadget HAL, no
  MTP/adb); a USB DAC failed full-speed enumeration (`device descriptor
  read/64, error -71`) and the controller kept cycling low power. #94+
  logs every role change (`haydn-usb: set_role`, `haydn-usb: core role`).
* #96 role log: exactly one `set_role NONE -> HOST` (Nothing Phone 2 as the
  partner), then `UCSI: ucsi_qti_glink_write: timed out for sync_write_ack`
  / `UCSI_GET_PDOS failed (-110)`, and no role change ever again (not on
  unplug, not for later cables). 5.10's UCSI core asks a USB-PD partner for
  its source PDOs; msm-5.4's never did, and the 5.4-era ADSP charger
  firmware does not answer, which wedges the glink PPM. The USB-C DAC and
  earphones were then plugged into a port still in host mode (no proper
  VBUS/role) and failed full-speed enumeration (`device descriptor
  read/64, error -71`). `ucsi.c` now skips GET_PDOS unless
  `ucsi.get_pdos=1` (#102+).

### Power key (qpnp-power-on)

* pmk8350's `pon_hlos` asks for `qcom,kpdpwr-sw-debounce` but has no
  `qcom,pon-dbc-delay`, and the debounce read back is too short. The diag
  `haydn-key:` log showed the key bouncing on release (release, press,
  release within ~200 us), which Android reads as a double press (camera)
  or a long press. The software debounce is now at least 15625 us.
* #84: every press is one clean down/up pair. The remaining "double tick" on
  screen off also happens with double-tap-to-sleep (no key involved), so it is
  the screen-off feedback (lock sound or haptics HAL) playing twice, not the
  key driver. To check: turn off the screen locking sound and see whether the
  double tick stays (haptics) or goes away (audio).

### Suspend / resume latency (thermal core)

* `thermal_pm_notify()` read every thermal zone synchronously in
  `PM_POST_SUSPEND`, including ~26 modem QMI sensors (`set_trips` sends a QMI
  request with a 100 ms timeout; the mmW sensors do not exist on haydn). Every
  aborted or finished suspend took 4-7 s to reach `PM: suspend exit`, and power
  key or double-tap presses in between did nothing: the 5-7 s screen wake.
  Zones are now re-evaluated from their poll work (upstream made the same
  change later).

### Open: stability and speed (#76 log)

* Hard hang during YouTube, no oops; seconds before it the WLAN firmware
  stopped answering link-layer stats requests. Diag #82+ turns off L1
  substates on PCIe0 (`qcom,no-l1ss-supported`) to test the link.
* UI slow (3 s app starts, 1-7 s binder calls) while the kernel's own 2 s
  bootlog ticks stay on time. Compare with the normal build to separate diag
  overhead.
* **CPUs 1-7 stuck offline after the first full suspend (#90).** haydn
  uses `deep` suspend (PSCI SYSTEM_SUSPEND), which hot-unplugs CPUs 1-7.
  On resume `arm-memlat-mon`'s CPUHP_AP_ONLINE_DYN callback failed to
  recreate its PMU counters (`event 8 not set for cpu N ret -13`) and
  returned the error, so the hotplug core killed each CPU again (`Error
  taking CPU1 up: -13`). From then on only CPU0 ran: heavy lag/stutter,
  2-30 s binder calls (content providers, sensors, audio policy). The
  memlat hotplug callbacks now always return 0 (#94+).
* `walt_suspend()` used `ktime_get()` after `timekeeping_suspend()` (walt's
  syscore ops run after timekeeping's), a WARN on every suspend; it now
  reads `ktime_get_mono_fast_ns()`.
* cpufreq governor: all three policies run `schedutil` (GKI default).
  WALT is built in (`CONFIG_SCHED_WALT=y`) and registers its own `walt`
  cpufreq governor (`kernel/sched/walt/cpufreq_walt.c`), which msm-5.10
  vendor init normally selects; the haydn 5.4 ROM init sets/tunes
  schedutil instead. Options: switch to `walt` (tunables under
  `policyN/walt/`), or give schedutil the 5.4 `up_rate_limit_us` /
  `down_rate_limit_us` nodes the ROM writes.
* libperfmgr writes 5.4-only nodes (`schedutil/up_rate_limit_us`,
  `/proc/sys/kernel/sched_busy_hysteresis_enable_cpus`; WALT on 5.10 has
  `/proc/sys/walt/...`), so the ROM's boost hints partly do nothing.
* Thermal HAL cannot read CPU/GPU/NSP zones (`Temperature buf stoi error ...
  TZ:39-58`) and the thermal-cpufreq cooling devices fail to bind (-22).
* Camera: both sensors acquire and release again within 1-2 s and streaming
  never starts; no kernel error. The app opens (privacy dot on) with a black
  preview. In #84 the HAL loops every ~1.2 s: sensor ACQUIRE (0x1ad2 @0x20,
  0x355 @0x34), ICP `cam_hfi_resume`, then RELEASE, with no ISP acquire or
  CAM_START_DEV in between. With `debug_mdl` (#90): ICP (IPE/BPS)
  acquire, config_io and abort all succeed, the ISP context is acquired
  with the v2 flow (`num_resources` = 0xFEFEFEFE), then CamX releases
  everything about 1 s later without ever sending ACQUIRE_HW, and the
  kernel logs no error. The failing step is in CamX userspace; it needs
  CamX logs (`/vendor/etc/camera/camxoverridesettings.txt` log masks).
* #102 logcat (via adb, no root): the session gets as far as capture
  requests 70-75 (no results), then the provider aborts itself:
  `CamX::FDHwNode::PostPipelineCreate` -> `Node::NodeTriggerSystemEvent` ->
  `ExtensionModule::SignalRecoveryCondition` -> `raise(SIGABRT)` in
  com.qti.chi.override.so, i.e. CamX's face-detection HW node reports a
  fatal system event right after the pipeline is created. Lahaina has no
  FD block in DT (fd600 is kona) and neither 5.4 nor 5.10 builds
  `CONFIG_SPECTRA_FD`, so this node presumably runs on another engine
  (CDSP/ICP); the provider does open a CDSP FastRPC session just before.
  Next step needs CamX's own logs, which need root (KernelSU) to set the
  vendor log properties / override file.
  Diag #88+ adds `camera.debug_mdl=0x11a`
  (CORE|ISP|CRM|ICP) to the cmdline so the failing step shows up as
  `CAM_DBG:` lines.

### Other

* PDC: `qcom,lahaina-pdc` is matched.
* TSENS: the msm `qcom,tsens24xx` driver is built (lahaina DT).
* FBE: lahaina has no TME, so `drivers/soc/qcom/hwkm_v1` brings the msm-5.4 HWKM v1
  backend (kernel programmed HWKM master, `qcom,hwkm` node) behind the msm-5.10
  crypto-qti API (`CONFIG_QTI_HW_KEY_MANAGER_V1`). HWKM v1 only maps its ICE
  slave on the first key programming; msm-5.10 UFS evicts all keyslots at
  probe, so `crypto_qti_invalidate_key()` returns 0 until HWKM init is done.
* ramoops: `haydn-sm8350.dtsi` adds the shipping 5.4 carve-out (8 MiB at
  `0xb0000000`, record/console/ftrace/pmsg 2 MiB each, `max-reason=4`), which matches the
  ROM's dtb.

## Debugging boot

So far pstore/ramoops has come back empty after a panic on haydn, so these are used
instead:

1. **On-screen kernel log (diag branch).** A `simple-framebuffer` node at
   `0xdfc00000` (the XBL splash buffer, 1080x2400 a8r8g8b8 at stride 4320, `splash_memory`
   set to `no-map`) plus fbcon with `TER16x32` shows the kernel log on the panel
   before msm_drm takes over. `panic_timeout=0` keeps a panic on screen. Record
   a video of the boot.
   * Since build #52 the diag console uses `loglevel=4` (errors and warnings on
     screen only). With `loglevel=7` fbcon drew every info line, which slowed
     the boot animation and turned any WARN into a multi-second stall. bootlog
     reads the log buffer through `kmsg_dump`, so logdump still gets every line.
   * Slot choice: the log goes to the logdump partition with the lower
     sequence number. sde59 (UFS LUN 4) can appear after sda24; a boot
     that chose with only sda24 in sight wrote over the newest log (the
     crash being chased) and both slots ended up with the same number, so
     the next boot overwrote the other crash too. Since #130 bootlog waits
     until both are present (up to 30 s after boot, rescans twice a
     second; lines are kept in RAM meanwhile).
   * The bootlog buffer is 16 MiB. Since build #60 it never stops: once full,
     the first quarter (the boot) is kept and the rest wraps as a ring
     (`bootlog: ===== ring wrapped =====`). In a dump, the newest lines are
     the ones just before the point where timestamps jump back.
   * DT nodes are added under `&{/}` in the overlay (`/ {` in an overlay is not
     applied to the base).
2. **logdump bootlog (diag branch).** `drivers/misc/haydn_bringup/bootlog.c`
   (`CONFIG_HAYDN_BOOTLOG`) writes the kernel log to the `logdump` partition. On panic,
   `kernel/panic.c` calls `printk_safe_flush_on_panic(); haydn_bootlog_panic_wait();`
   so the last lines get written. Read back from recovery or a working ROM:
   ```
   dd if=/dev/block/by-name/logdump of=/sdcard/logdump.bin bs=1M count=16
   ```
   Check the partition first: `ls -l /dev/block/by-name/ | grep -iE "logdump|sda15"`.
   The Zorix 5.4 kernel's mtdoops may also write to it.
3. **PON reason** (the PON log, e.g. `pon.txt` from OrangeFox). `WARM_RESET` with
   trigger `PS_HOLD` means a software reset or panic, not a watchdog or power key.
4. **CI emulation.** `rom-inspect` branch, `aktest.yml`: runs the AnyKernel3 repack
   on the real ROM images to catch packaging failures before flashing.

## Boot test history

| Build | ROM | Result | Cause / fix |
| --- | --- | --- | --- |
| first zip | Zorix A16 (5.4 monolithic) | bootloop, recovery too | zip would have aborted on missing `lib/modules` → created by the zip |
| test-1 | Zorix A16 | bootloop; fastboot only | diag build showed BRK in `exit_usb_ep_set_maxpacket_limit` at ~5.9 s → dwc3 kretprobe guard + dummy_hcd off |
| diag #14 | Zorix A16 | to be tested | qseecom DT, audio, display fixes |
| diag #17 | Zorix A16 | **no panic**: kernel up, `/init` runs at 8.4 s, fbcon log for 100+ s, then reboots to fastboot | no UFS disk (`sd*`) and no USB UDC, so first stage init cannot open `system_b` and falls back to the recovery ramdisk; SPMI reads to `pmr735b` fail (0x3) |
| diag #20 | Zorix A16 | twice: screen goes black at ~9.1 s, right after first stage init loads `sys_pm_vx` / `hwid` from vendor_boot, Mi logo ~15 s later, then fastboot. #17 videos show the same black at the same point, so #17's 100 s run with no UFS was the **second** boot, in recovery mode (11 recovery modules) | silent hard reset while vendor_boot modules load (likely a deferred built-in device, e.g. UFS, probing once a module supplier appears) |
| diag #23 | Zorix A16 | recovery-mode boot shows 128 devices deferred, chains end at `100000.qcom,gcc`, `rpmh-regulator-*`, `qcom,rpmhclk` | dump only roots next |
| diag #24 | Zorix A16 | reaches `/init` hold, runs ~1:45, then fastboot. `key:` lines: cmd-db bound, **`18200000.rsc` and `af20000.rsc` exist but no driver**; all rpmh regulators/rpmhclk/bcm-voter never created; gcc, SMMU, NoC, UFS unbound (119 deferred, 29 roots) | **root cause**: msm-5.10 `rpmh-rsc` needs 5 `qcom,tcs-config` pairs (`FAST_PATH_TCS` added), 5.4 DT has 4 → `-EINVAL` before the console is up |
| diag #26 | Zorix A16 | **RSC fix works**: rsc, all rpmh regulators, rpmhclk, gcc, NoC, bcm-voters bound (47 deferred, 4 roots). Still no UFS; first stage init gives up and `reboot: Restarting system with command 'bootloader'` at ~30 s → fastboot | roots `15000000.apps-smmu`, `3da0000.kgsl-smmu` (no reason): msm-5.10 arm-smmu waits for `qcom,iommu-dma`, which the 5.4 DT lacks |
| diag #28 | Zorix A16 | SMMUs now probe (`arm-smmu 3da0000.kgsl-smmu: stream matching with 60 register groups` at ~5 s); right after, the screen goes **black** (splash scanout cut), Mi logo after ~100 s, loops | msm-5.10 arm-smmu only hands off SMRs listed in `qcom,handoff-smrs`; 5.4 DT has none, so the MDSS stream lost its mapping |
| diag #29 | Zorix A16 | screen stays on past the SMMU probe; **UFS now probes** and panics at ~7.6 s: `qti_hwkm_run_transaction` ← `crypto_qti_invalidate_key` ← `ufshcd_crypto_qti_keyslot_evict` ← `ufshcd_init_crypto` ← `ufs_qcom_probe` | `qcom,handoff-smrs = <0x820 0x0>, <0x800 0x0>` on apps_smmu; diag also boots with `arm-smmu.disable_bypass=0` |
| diag #31 | Zorix A16 | no panic; all built-in devices bind (`defer: 0`), UFS LUNs sda–sdf found, bootlog writes to logdump (sda24). First stage init loads 14 vendor_boot modules, then `reboot bootloader` at 33.9 s | init's own messages were rate limited away |
| diag #35 | Zorix A16 | logdump: `cnss_plat_ipc_qmi_svc: module init failed: -19` → `init: Failed to load kernel modules` → `InitFatalReboot` | boots with `printk.devkmsg=on`; kernel logs failing module inits |
| diag #38 | Zorix A16 | all vendor_boot modules up to msm_drm load; DSI ctrl0/1 probe; **hard freeze at 20.35 s** while dsi-display-primary probes (no further output, logger stopped too) | TE GPIO request hang (below) |
| diag #40 | Zorix A16 | **Android second stage init runs** (system/vendor mounted, logd, servicemanager, keystore2...). Stuck from ~61 s: init blocked in `vdc cryptfs mountFstab`; vold waits for keystore2, keymaster never registers. qseecomd listener registration fails (`mem_buf_dma_buf_copy_vmperm -22`) | qseecom / mem-buf did not understand msm-5.4 ION buffers |
| diag #43 | Zorix A16 | **data unlocks**; zygote, system_server, audio/camera/wifi services start. SurfaceFlinger aborts at start (17×) → zygote loop; after ~180 s SLPI crashes (glink assertion) and SSR level SYSTEM resets the SoC | kgsl `zap-shader node not found` |
| diag #45 | Zorix A16 | **SurfaceFlinger runs** (GPU up), commits every frame but each fails: `smmu_sde_unsec_cb Cannot DMA map uncached buffer as IO-coherent attrs:0x28020` → `sde_plane_prepare_fb` -22. SLPI crash at ~148 s resets the SoC | DMA_ATTR bit 15 clash (below) |
| diag #48 | Zorix A16 | **ROM boot animation on screen** (display + GPU + SurfaceFlinger work); animation freezes after ~1 min. glink edges register but no channel ever opens; modem DOG stall at ~145 s | glink edges never started |
| diag #50 | Zorix A16 | **boots to the lock screen** (unlock works, modem registers on the network). Boot 1: opening Settings → hard reset at ~138 s. Boot 2: lock screen frozen (no touch, power key dead), then `WLAN Panic ... Scheduler Watchdog Bite` → `Kernel panic - Fatal exception in interrupt` at 175 s | every display buffer free hits `WARN_ON(!mutex_is_locked(&dev->struct_mutex))` in `msm_gem_free_object`; each WARN dumps a full backtrace + module list through fbcon, which takes seconds per trace with the console lock held. The composer's HwBinder thread (RT) ran 10 s inside printk → RT throttling, the WLAN scheduler watchdog fired its 10 s BUG, and the system froze |
| diag #52 | Zorix A16 | **usable system**: Settings opens and navigates (About phone shows `5.10.272-gki-g9b42b19601d5`), Wi-Fi connected, mobile data and signal up. After ~3 min of use: **silent freeze, then reset**. logdump ends mid-logcat at 171.6 s with no oops, panic or watchdog bark; the next 2 s bootlog flush never happened, so the whole SoC hung (bus-hang pattern). Last kernel activity: qcacld `pmo_core_enable_wow_in_fw` (WLAN runtime suspend over PCIe), which had cycled fine every 1–2 s before | confirmed WLAN: same build ran 20 min with Wi-Fi off (no freeze), froze again within minutes after turning Wi-Fi on. Freeze not in logdump: the 16 MiB bootlog buffer filled at ~1038 s |
| diag #57 | Zorix A16 | built, not flashed | sound card without BT/FM slimbus links; `haydn-key:` power key logging |
| diag #60 | Zorix A16 | to be tested | WLAN runtime suspend refused (`cnss2.wlan_runtime_pm=0`); bootlog ring buffer |
| diag #61 | Zorix A16 | UI text still disappears after a few minutes (QS panel); switching dark/light theme brings it back until it goes again. Power key still misbehaves. Launcher swipe-up (app drawer) does not open | GPU I/O coherency off did **not** help, so it is not the cause; reverted afterwards |
| diag #66 | Zorix A16 | built, not flashed | GPU without LLCC slices (`msm_kgsl.gpu_llcc=0`, diag only) |
| diag #61 (Wi-Fi on) | Zorix A16 | ran ~25 min with Wi-Fi on, then hung on the first screen-off system suspend (log ends at `__wlan_hdd_bus_suspend: starting bus suspend`); reset when the power key was pressed. Sound still dead (`qcom,wcn-bt` links still added). All SLPI sensors missing except proximity; YouTube frames black/green | see WLAN, Audio |
| diag #68 | Zorix A16 | to be tested | WLAN link kept up across system suspend; `qcom,wcn-bt` links skipped; GPU without LLCC slices |
| diag #70 | Zorix A16 | built | SLPI sensors PD for fastrpc; camera regulator votes released |
| diag #73 | Zorix A16 | **panic at 13.9 s**: the sound card registered for the first time on 5.10 and `soc_new_pcm()` oopsed in `snd_soc_set_runtime_hwparams()` (NULL `substream->runtime` for a `no_host_mode` PCM). #70 has the same audio change and would hit it too. mem_latency/compute governors loaded fine (only the 3 bw_hwmon nodes still fail) | + mem_latency/compute devfreq governors (diag only) |
| diag #76 | Zorix A16 | **sensors: 41, same as 5.4** (SLPI PDR fix confirmed; auto-rotate works). **Media sound works** (speaker, ringtone). YouTube plays with a picture and offers qualities; one brief green preview frame seen. Still broken: camera, call audio both ways, in-call proximity, UI text glyphs (also with GPU LLCC off, so LLCC is not the cause), slow wake, USB to PC/phone | NULL-runtime fix in `snd_soc_set_runtime_hwparams()`; camera powers up even if the shared pm8008i_l7 rejects a voltage |
| diag #76 (session 2) | Zorix A16 | 40 min with Wi-Fi on, screen off/on fine (Wi-Fi suspend fix holds). Call: no voice either way; audio HAL killed by audioserver TimeCheck (`setPhoneState` > 5 s). Power key: release bounce (release, press, release within ~200 us) → camera gesture. Screen wake 5-7 s: every aborted/finished suspend takes 4-7 s to reach `PM: suspend exit`. Camera: sensors acquire and release, never `CAM_START_DEV`. Ended in a hard hang during YouTube, seconds after the WLAN firmware stopped answering (`wlan_hdd_send_ll_stats_req: Target response timed out`) | |
| diag #82 | Zorix A16 | Power key events clean (no bounce). Froze ~1 min after unplugging USB from another phone: NULL deref after `DWC3 exited from low power mode` (host-role start without xhci). Aborted suspends still take ~5 s to reach `PM: suspend exit` | |
| diag #84 | Zorix A16 | USB file transfer to the other device works. Froze on a USB reconnect: host mode started, xhci DMA bypassed the SMMU → `arm-smmu` context fault → BUG → panic. Power key presses each one clean event; double tick also with double-tap-to-sleep (Android feedback). Calls: still no audio either way, no in-call proximity | dwc3 usb-role-switch + host guard; bw_hwmon; diag PM notifier timing and cpufreq dump |
| #86+ | Zorix A16 | to be tested | xhci-plat uses the SMMU-backed `ssusb` parent for DMA |
| diag #90 | Zorix A16 | Call audio works both ways; in-call proximity turns the screen off; fingerprint works. Rest still being tested | hostless PCM start (call audio); us_prox enable logs; diag: camera debug_mdl |
| #92+ | Zorix A16 | to be tested | KGSL cache range op covers the right pages (missing text) |
| diag #90 (more) | Zorix A16 | Text still missing (pre-#92). After ~9 min CPUs 1-7 stayed offline (memlat hotplug -EACCES); wake took ~7 s (fbcon console); USB gadget lost after host use; USB DAC fails enumeration (-71); camera black (CamX never acquires ISP HW) | |
| #96 | Zorix A16 | Lag/stutter fixed (CPUs stay online), slow wake / multi-press fixed, VoIP audio in video calls works. Still open: missing text and blinking images (GPU texture data), USB-C earphones/DAC not detected (audio stays on speaker), camera (also video calls) | memlat hotplug never fails; walt_suspend clock; diag: no fbcon console; USB role logging |
| #96 logs | Zorix A16 | CPUs online all boot (85/85 snapshots). USB: one host session then UCSI GET_PDOS timeout, role never changes again; DAC -71. BT: baud switch stuck | |
| #102 | Zorix A16 | USB device mode + adb work (UCSI fix). Text/images still blink with GPU coherency off (so not KGSL). Camera HAL aborts in FDHwNode. One spontaneous reboot shortly after first boot (logs pending) | UCSI without GET_PDOS; no VT switch on suspend; diag: UART debug to dmesg |
| #102 more | Zorix A16 | USB-C earphones work. BT: 3.2 Mbaud set as 3.125. Two unexplained resets. Camera: CamX FDHwNode abort | |
| #104+ | Zorix A16 | to be tested | BT UART clock table with 102.4 MHz; diag: ion_force_dma_sync=1, UART debug only on BT UART |
| #106 | Zorix A16 | BT still turns off; text/images still blink (diagonal black wedge seen); YouTube green frames (from ion_force_dma_sync) | |
| #108+ | Zorix A16 | UBWC bank-bit logging; KGSL access-check spam silenced | |
| #112+ | Zorix A16 | to be tested | BT UART clock up to 120 MHz at VDD_LOW (exact 3.2 Mbaud) |
| #114 | Zorix A16 | Text still missing (fine after boot, worse over time). Froze at screen timeout, then reset | diag: GPU IFPC and preemption off |
| #116+ | Zorix A16 | to be tested | IFPC/preemption back on |
| #118+ | Zorix A16 | to be tested | diag: DPM watchdog 5 s + pstore readback into bootlog |
| #120+ | Zorix A16 | to be tested | diag: KernelSU-Next v3.4.2 built in |
| #122 | Zorix A16 | KernelSU-Next works (root in Termux). Camera: same FDHwNode abort; CamX log masks did not reach the HAL (bind mount made in Termux's mount namespace; use `su -mm`). Repeated resets with the screen off, both at WLAN WoW enable (see WLAN) | |
| #124+ | Zorix A16 | to be tested | diag: `haydn-wow:` markers with synchronous logdump writes; `pcie_aspm.policy=powersave`; `camera.debug_mdl` dropped (it overflowed dmesg) |
| #125 | Zorix A16 | Reset about 1 min after screen off with Wi-Fi on (same WoW hang). Text tests: see Missing text | |
| #126+ | Zorix A16 | to be tested | diag: `msm_kgsl.tlb_flush_slumber=1` (GPU TLB flush on unmap even in slumber) |
| #125 (pm_async=0) | Zorix A16 | Still resets ~2-3 min after screen off with Wi-Fi on; markers show WoW completes | |
| #128+ | Zorix A16 | to be tested | diag: `haydn_pm_trace=1` per-device suspend markers with logdump writes |
| #125 (Wi-Fi off, mobile data on) | Zorix A16 | Also reset with screen off on mobile data only; that crash log was lost (bootlog slot bug below) | |
| #130+ | Zorix A16 | to be tested | bootlog waits for both logdump partitions before choosing |
| #130 | Zorix A16 | `/sys/fs/pstore` empty even after a normal restart | |
| #132+ | Zorix A16 | to be tested | diag: `reboot=warm` so pstore survives restarts and panics |
| #132 | Zorix A16 | Text still vanishes, parts still blink (TLB flush on unmap did not help) | |
| #134+ | Zorix A16 | to be tested | diag: `dma-coherent` removed from `kgsl_smmu` (non-coherent GPU table walks) |
| #132 (Wi-Fi on, screen off) | Zorix A16 | Hung black after ~5 min and did not reset (needed a long power press); pm trace ends 4 ms after WLAN suspend | |
| #136+ | Zorix A16 | to be tested | diag: `reboot=warm` removed again |
| #138 | Zorix A16 | **Missing text and blinking fixed** | KGSL cmd/obj/sync lists copied the 5.4 way; non-coherent `kgsl_smmu` walks (both moved to main) |
| #138 (pm_test) | Zorix A16 | Hangs at pm_test core/platform/devices, not at freezer; s2idle hangs too | |
| #142 | Zorix A16 | keep_ufs + pm_test devices passes; real suspend still hangs (also with UFS spm_lvl 1 and 2) | `haydn_pm_keep_ufs` switch |
| #144 | Zorix A16 | Still resets with screen off (bootlog ruled out) | bootlog pauses logdump writes during suspend |
| #146 | Zorix A16 | Reset after ~10 min; no breadcrumb (SDAM 23 did not keep the writes) | PMIC SDAM suspend breadcrumb |
| #148 | Zorix A16 | Breadcrumb works: hang right after `cnss_pci` resume noirq returned (resume side) | breadcrumb in SDAM 2 at 0x4a |
| #150 | Zorix A16 | Breadcrumb: hang while re-enabling IRQ 38 `spss` (SPI 352) on resume | breadcrumb per IRQ in resume_irqs() |
| #152 | Zorix A16 | Still resets; breadcrumb again IRQ 38 (not the culprit, see above) | SPSS generic IRQ parked across suspend |
| #154 | Zorix A16 | Still resets; breadcrumb `device IRQ 38 spss`, no wake IRQ after it | breadcrumb records wake IRQs |
| #156 | Zorix A16 | Still resets; breadcrumb "IRQ 38" again but a different device (`sps`) | SPSS disabled in DT |
| #158 | Zorix A16 | Still resets; no threaded handler, hang in the first hard handler on resume | threaded IRQ breadcrumb |
| #160 | Zorix A16 | Still resets (also with breadcrumb/trace off); breadcrumb "IRQ 38" again | `haydn_late_irqs=1` |
| #166 | Zorix A16 | **Still resets** with every suspend debug hook and bootlog off: the hang is in the port itself, not the instrumentation | diag: hooks off, `CONFIG_HAYDN_BOOTLOG=n` |
| #82+ (earlier list) | Zorix A16 | see above | UCSI connector; routing read/write stubs; us_prox poll handlers; 15.6 ms power key debounce; async thermal resume; diag: no L1ss on PCIe0 | GEM objects freed from a worker under struct_mutex (below); diag console `loglevel=4` |

Non-fatal errors seen on screen (being worked through):
sensors-ssc / cdsp-loader (fixed in #17), fastrpc "CMA alloc failed" (minidump
buffer allocated before `adsprpc-mem` probes; harmless), mem-buf missing
`qcom,vmid`, msm_hang_detect, `spmi pmic_arb_wait_for_done: transaction failed (0x3)` and
`qcom-spmi-gpio ...pmr735b: probe failed -5` / `spmi-temp-alarm ...pmr735b` (that PMIC
does not answer on haydn), devfreq qoslat "Unable to find governor", `udc-core:
couldn't find an available UDC`, keymaster@4.0 not found (no vendor partition).

## Known issues and TODO

* SSR notifications: 5.10 drivers (adsprpc, pmic_glink, slimbus NGD, icnss2,
  ipa, ...) register with `qcom_register_ssr_notifier()` (remoteproc), but
  PIL sends `subsys_notif` events. Boot works (the initial state is "up"), but
  recovery after a DSP crash will not be seen. Plan: a shim that forwards
  subsys_notif events to the qcom SSR notifier chain (lpass→adsp, mpss→modem,
  cdsp, slpi).
* BT audio (SCO/A2DP offload): the 5.4 DT uses the msm `qcom,slim-ngd` slimbus
  controller (`SLIMBUS_MSM_NGD` + `BTFM_SLIM` on 5.4). msm-5.10 has only the upstream
  `qcom-ngd-ctrl` (`qcom,slim-ngd-v1.5.0`/`v2.1.0`), and SLIMBUS is not enabled
  in `haydn_GKI.config`. This needs porting `slim-msm-ngd` or converting the DT.
* **Silent reset ~0.7 s after /init starts loading vendor_boot modules** (builds
  #17 and #20). After the reset the bootloader boots recovery mode, where only the
  11 recovery modules load and UFS/USB never probe (no `sd*`, no UDC).
* fastrpc minidump CMA buffer: allocated before `adsprpc-mem` probes.
* Seen on #52 in daily use (user report):
  * Camera dead. The rear sensor (slot 0, id 0x1ad2) probes once, then the
    next power-up fails: `pm8008i_l7: Restricting voltage, 2904000-2800000uV`
    → `cam_vana set voltage failed`; CCI reads then time out and power down
    is unbalanced. cam_vana (2.904 V) and cam_v_custom2 (2.8 V) share
    pm8008i_l7 and the camera driver left a (0, 2.8 V) vote behind. Fixed in
    #70+ (see camera below). The `pm8008j@c ... -107` errors are not the
    cause: haydn's cameras use pm8008i, and pm8008j is not populated.
  * Text randomly missing across the UI (glyphs blank, icons fine). It comes
    back after a theme switch (all UI caches rebuilt) and goes again after a
    few minutes. Not GPU I/O coherency: #61 with coherency off behaved the
    same. kgsl process reclaim is not built for haydn. The pattern in app
    labels: common letters (a, e, l, o, r, s, t...) are blank, rare ones
    (b, k, p, x, y) and capitals still draw, i.e. the glyphs that went into
    the atlas first are lost and later ones survive. Data written by the GPU
    long ago disappears. Suspect: the GPU LLCC slice (5.4 lahaina table, GPU
    slice `retain_on_pc`, `write_scid_en`) dropping dirty lines. Diag test:
    `msm_kgsl.gpu_llcc=0` (default in diag only) runs the GPU without
    GPU/GPUHTW slices. Also `qcom-llcc 9200000.cache-controller: invalid
    resource` ×3 at boot (5.4 DT has only `llcc_base` and
    `llcc_broadcast_base`).
  * App drawer (launcher swipe-up) is open but not drawn while window blurs
    are on: it takes touches and opens apps. With "Allow window-level blurs"
    off it draws (text still has the glyph problem). "Disable HW overlays"
    changes nothing, so SurfaceFlinger GPU composition itself works.
  * Power key: a single press sometimes acts as a double press (camera
    gesture) or a long press (power menu). Diag logs `haydn-key:` lines from
    `qpnp-power-on` for each state change.
  * Screen wake (power key or double tap) takes 3–7 s, sometimes needs a
    second try. Going to sleep is quick.
  * Sensors: only proximity is listed (2 sensors in the HAL; stock 5.4 lists
    41); accelerometer, light, magnetometer, gyroscope and barometer (SLPI
    sensor hub) are missing. Cause: `sscrpcd` could never attach to the SLPI
    sensors PD (`apps_dev_init failed for domain 2`, ENODEV until the SLPI
    is up, then ENOTCONN). Fixed in #70+: see fastrpc below.
  * Video: YouTube frames come out black or green (empty NV12 buffers) and the
    quality menu is empty. Same family as the missing text: data written by a
    hardware block is not there when read back.
  * Ultrasonic proximity (in-call screen off) dead: depends on the sound card.
  * Short UI stutters and warmth: devfreq/bwmon governors missing, ROM Dolby
    app crash-loop (no audio effects), diag logging.
* Seen in the #50 logs (after the lock screen, not fatal on their own):
  * `qti_battery_charger_main`: `battery_chg_write()` sleeps in
    `wait_for_completion_timeout()` from `usb_psy_get_prop()` inside an RCU read
    section (`power_supply_changed_work`) → `rcu_note_context_switch` WARN.
  * Camera EEPROM I2C read times out (-110), then `cam_cc_mclk1_clk already
    disabled` and unbalanced regulator disables in `cam_sensor_util_power_down`.
  * ROM Dolby app (`org.lunaris.dolby`) crash-loops: "Cannot initialize effect
    engine" (audio DSP effect not available yet).
  * `cnss: Failed to wait for response of get info request, err: -110`.
  * adb: `adbd: timed out while waiting for FUNCTIONFS_BIND` (no UDC).
  * lineage health `IChargingControl` AIDL service missing.
* mem-buf: `qcom,vmid` missing in the 5.4 DT.
* After first boot: Wi-Fi, BT, audio and haptics checked against the reference
  tree's DT; LPM/DCVS tuning; then KernelSU + SUSFS; then make `haydn-5.10` the default branch.

## Porting to another kernel: checklist

What was learnt here, useful for 5.15/6.x or another lahaina device:

1. Start from the vendor's tree for the newest kernel (msm-5.10 here), not plain
   ACK. Grafting the whole old vendor tree onto ACK (`wip-5.4-graft`) was much harder to get building.
2. Keep the old DT and the old userspace-facing interfaces. Look at what the ROM
   actually uses (libion vs DMA-BUF heaps, `/dev/subsys_*`, uapi numbering).
3. Diff every per-SoC driver against the old tree and sync the data tables.
4. Grep 5.10 drivers for new DT properties (`qcom,rproc-handle`, memory-region
   names, `qcom,vmid`) and either add them to the DT or use the old driver.
5. Disable drivers for hardware the SoC does not have. Some (dummy_hcd + the dwc3
   kretprobe) crash on hardware that is not there.
6. Ship as AnyKernel3 (boot Image + vendor_boot dtb/modules + dtbo) so the ROM does
   not need to change. Check whether the ROM kernel is monolithic (no modules).
7. Check every DT property count/format a newer driver parses (e.g. rpmh-rsc
   `qcom,tcs-config` went from 4 to 5 entries): a silent `-EINVAL` in an early
   core driver shows up only as hundreds of deferred devices.
8. Have a working on-screen log and logdump before the first flash; pstore
   was empty after panics on haydn.
9. Keep fastboot images of the installed ROM; there is no separate recovery.

## Device tree side

Not needed for the AnyKernel3 zip. For building inside a ROM tree
(android_device_xiaomi_sm8350-common / haydn):

* `TARGET_KERNEL_SOURCE` → this tree.
* `TARGET_KERNEL_CONFIG := gki_defconfig vendor/lahaina_GKI.config` (common) and
  `TARGET_KERNEL_CONFIG += vendor/haydn_GKI.config` (haydn), replacing
  `lahaina-qgki_defconfig`, `debugfs.config`, `xiaomi_QGKI.config` and `haydn_QGKI.config`.
* The vendor_boot module list in haydn `BoardConfig.mk` stays valid.
* 5.10 builds more vendor modules (`=m`). They need to be installed and loaded
  from vendor_boot (as the zip does) or vendor_dlkm.

## Credits

* LineageOS / Qualcomm for msm-5.10, kichukic for the 5.4 haydn tree.
* xiaomi-lahaina-devs (`5.10-sm8350-H`): the reference 5.10 port. `bootlog.c`
  (logdump bootlog) comes from it.
* osm0sis for AnyKernel3.
