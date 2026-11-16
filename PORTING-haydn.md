# haydn (Mi 11X Pro / Redmi K40 Pro+, SM8350 "lahaina") on Linux 5.10

Branch `haydn-5.10`. Work in progress: compile first, then boot bring-up.

## Base

| Part | Source |
| --- | --- |
| Core kernel + Qualcomm 5.10 drivers | msm-5.10 from LineageOS `android_kernel_qcom_sm8450` (`lineage-20`, ed07fc6e2500): Qualcomm `android13-5.10-waipio`, merged up to Google ACK `ASB-2026-11-02_12-5.10` (5.10.272). It already contains the 5.10 lahaina drivers: GCC/disp/gpu/cam/video clocks, pinctrl, interconnect, LLCC, RPMh clocks, UFS PHY, KGSL with Adreno 660. |
| haydn/lahaina device trees | 5.4 tree (`kichukic/android_kernel_xiaomi_sm8350` b6eeaf3b8), `arch/arm64/boot/dts/vendor` |
| Xiaomi drivers | 5.4 tree: Focaltech SPI touch + `xiaomi_touch`, FPC fingerprint, Xiaomi pmic-glink charger (BQ2597X), hwid, `mi_thermal_interface` + `thermal_message`, IR SPI |
| techpacks | 5.4 tree (`techpack/`), ported to 5.10 one at a time |

KernelSU and SUSFS are not included (to be added once 5.10 boots).

The first attempt (ACK 5.10 + the whole 5.4 vendor tree grafted on top) is kept on
branch `wip-5.4-graft` for reference only.

## Kernel config

```
make ARCH=arm64 LLVM=1 LLVM_IAS=1 O=out \
     gki_defconfig vendor/lahaina_GKI.config vendor/haydn_GKI.config
make ARCH=arm64 LLVM=1 LLVM_IAS=1 O=out -j$(nproc) Image modules dtbs
```

* `vendor/lahaina_GKI.config`: msm-5.10 `waipio_GKI.config` with the SoC options
  switched to lahaina.
* `vendor/haydn_GKI.config`: Xiaomi drivers, plus everything the 5.4 haydn kernel
  had built in stays built in (generated from the 5.4 `.config`). The first-stage
  module list of the haydn device tree therefore stays the same (techpack, touch,
  fingerprint, charger as modules). No LTO/CFI for bring-up.

Outputs:

* `arch/arm64/boot/Image`
* `arch/arm64/boot/dts/vendor/qcom/lahaina{,-v2,-v2.1}.dtb` (vendor_boot dtb)
* `arch/arm64/boot/dts/vendor/qcom/haydn-sm8350-overlay.dtbo` (dtbo.img)

## Status

| Area | State |
| --- | --- |
| Core kernel, lahaina SoC drivers (msm-5.10) | compiles |
| DT: lahaina + haydn overlay | compiles, overlay applies on all three base dtbs |
| Display (`techpack/display`, msm_drm + Xiaomi mi_disp) | compiles |
| GPU (KGSL a660) | msm-5.10 driver |
| Audio (`techpack/audio`, APR/ASoC, cs35l41 amps, MIUS ultrasound) | compiles |
| Camera (`techpack/camera`), video (`techpack/video`) | compiles |
| IPA / rmnet (`techpack/dataipa`, `datarmnet`, `datarmnet-ext`) | compiles |
| WLAN (`drivers/staging/qcacld-3.0`, QCA6490) + cnss2 | compiles (`wlan.ko`) |
| FBE (HWKM v1, wrappedkey_v0) | compiles |
| Boot on device | **not tested yet** |

Build with `TARGET_PRODUCT=haydn` (LineageOS passes it through
`TARGET_KERNEL_ADDITIONAL_FLAGS`): the audio Kbuild picks the cs35l41 amps and
the camera Makefile the haydn sensor config from it.

## Notable 5.10 compatibility changes

* DRM core keeps the android11-5.4 behaviour the lahaina display HAL relies on:
  `drm_display_mode.private/private_flags/vrefresh`, the ACK 5.4 mode flags
  (`SUPPORTS_RGB/YUV`, `VID/CMD_MODE_PANEL`, `SEAMLESS`) and the QCOM DX/TIGHT/TILE
  modifiers.
* power_supply: the android11-5.4 GKI props the Xiaomi charger uses
  (`quick_charge_type`, `tx_adapter`, `signal_strength`, `reverse_chg_mode`).
* thermal: `/sys/class/thermal/thermal_message` for mi_thermald (thermal_message.c).
* lahaina keeps the PIL/SSR subsystem framework (the DT uses `qcom,pil-tz-generic`).
  `subsys-pil-tz` allocates the PAS metadata buffer itself, and cnss2's SSR ramdump
  path builds.
* PDC: `qcom,lahaina-pdc` is matched.
* TSENS: the msm `qcom,tsens24xx` driver is built (lahaina DT).
* FBE: lahaina has no TME, so `drivers/soc/qcom/hwkm_v1` brings the msm-5.4 HWKM v1
  backend (kernel programmed HWKM master, `qcom,hwkm` node) behind the msm-5.10
  crypto-qti API (`CONFIG_QTI_HW_KEY_MANAGER_V1`).
* ALSA/ASoC: `CONFIG_AUDIO_QGKI` carries the msm-5.4 core extensions, and ASoC
  falls back to msm-5.4 style component callbacks (`.ops`, `.compr_ops`,
  `.pcm_new`), so the APR techpack runs mostly unchanged. The msm-5.4 async DPCM ops
  are not ported (BE ops run synchronously).
* uapi kept at msm-5.4 values where the lahaina HALs depend on them: IPA
  (`msm_ipa.h` and friends), rmnet flag bits, the video bitrate mode numbering
  (`V4L2_MPEG_VIDEO_BITRATE_MODE_CQ_MSM`), DRM mode flags and QCOM modifiers.

## Device tree side (android_device_xiaomi_sm8350-common / haydn)

* `TARGET_KERNEL_SOURCE` → this tree.
* `TARGET_KERNEL_CONFIG := gki_defconfig vendor/lahaina_GKI.config` (common) and
  `TARGET_KERNEL_CONFIG += vendor/haydn_GKI.config` (haydn), replacing
  `lahaina-qgki_defconfig`, `debugfs.config`, `xiaomi_QGKI.config` and `haydn_QGKI.config`.
* The vendor_boot module list in haydn `BoardConfig.mk` stays valid.
* 5.10 builds more vendor modules (`=m` in lahaina_GKI.config). They need to be
  installed and loaded (vendor_dlkm / vendor modules.load).
