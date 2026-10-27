# Poco F6 Pro / Redmi K70

Linux support for Xiaomi `vermeer`, Qualcomm SM8550. This tree contains the
working board DTS and drivers for the display, touch, cameras, audio, modem,
battery telemetry, FastRPC and PM8550B haptics.

## Build

```sh
make O=output ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
make O=output ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- sm8550.config
make O=output ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)"
make O=output ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  INSTALL_MOD_PATH="$PWD/out-modules" modules_install
```

Board description: `arch/arm64/boot/dts/qcom/sm8550-xiaomi-vermeer.dts`.
Build fragment: `arch/arm64/configs/sm8550.config`.
Install the Image, DTB and modules from the same build using the device's
existing boot-image packaging process. Flashing is not performed by this tree.

## Userspace

Kernel drivers need the matching firmware and userspace configuration:

| Repository | Contents |
| --- | --- |
| [vermeer-audio](https://github.com/zuwwik/vermeer-audio) | Working topology and voice firmware, UCM, call controller and session configuration |
| [vermeer-libcamera](https://github.com/zuwwik/vermeer-libcamera) | libcamera 0.7.2 fork with sensor helpers, native AE/AF and CPU colour processing |
| [vermeer-camera](https://github.com/zuwwik/vermeer-camera) | Active camera tuning, pinned libcamera source, build/install tools and deployed musl runtime |
| [vermeer-userspace](https://github.com/zuwwik/vermeer-userspace) | Sensor stack patches, ModemManager, haptics and rootfs integration |

## Current limits

The cameras are OVX8000 main, OV16A1Q front and IMX355 ultrawide. The GC02M1
macro module is not enabled. Main-camera autofocus uses contrast, not Android
PDAF; full 50 MP, OIS, HDR and Qualcomm ICP processing are not implemented.
The panel exposes 60, 90 and 120 Hz modes.

Ordinary capture uses one physical AMIC1 duplicated into two channels. The
audio repository contains native voice routing and media-state restoration.
Bluetooth desktop audio and modem-call Bluetooth routing are separate paths.
GPS fixes and simultaneous SMS reception on both SIMs have not been validated.

The tree includes FastRPC buffer lifetime/probe fixes and one-shot DSP handover
interrupt handling. These do not establish a fix for every spontaneous closed
ADSP charger-firmware exception or SoundWire clock-stop timeout.

Host checks for the DMA-buffer lifetime and battery protocol are under
`tools/testing/vermeer/`. They do not force a live DSP crash.
