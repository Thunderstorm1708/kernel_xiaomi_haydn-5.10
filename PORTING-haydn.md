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
| Boot on device | panics fixed one by one; see [Boot test history](#boot-test-history) |

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
* Debugging: boot with `printk.devkmsg=on`, otherwise `/dev/kmsg` rate limiting
  drops init's own error lines. Read them from logdump:
  `dd if=/dev/block/by-name/logdump bs=1M count=64 | tr -d '\000' | grep -a init:`.

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
| diag #38 | Zorix A16 | to be tested | `qcom,wlan;` on the lahaina cnss node; QUPv3 `interconnects` (GENI SPI/I2C/UART probe -ENOENT in #35) |

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
