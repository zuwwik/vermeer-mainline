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

## Optional kernel features

The SM8550 fragment also enables legacy xtables/NAT, IPv4/IPv6 routing,
IP sets, IPVS, connection tracking and the requested optional device/security
support with its Kconfig dependencies. `NF_DEFRAG_IPV4` and `NF_DEFRAG_IPV6`
are built in because `NF_CONNTRACK=y`. `MMCONF_FAM10H` is x86-only and cannot
be enabled on this ARM64 board. The existing LSM order is retained and IPE has
no embedded boot policy. These options do not install firewall or IPE rules.

## Userspace

Kernel drivers need the matching firmware and userspace configuration:

| Repository | Contents |
| --- | --- |
| [vermeer-audio](https://github.com/zuwwik/vermeer-audio) | Working topology and voice firmware, UCM, call controller and session configuration |
| [vermeer-libcamera](https://github.com/zuwwik/vermeer-libcamera) | libcamera 0.7.2 fork with sensor helpers, native AE/AF and CPU colour processing |
| [vermeer-camera](https://github.com/zuwwik/vermeer-camera) | Active camera tuning, pinned libcamera source, build/install tools and deployed musl runtime |
| [vermeer-plasma-camera](https://github.com/zuwwik/vermeer-plasma-camera) | Native RAW still/JPEG 100 workflow and sensor-driven preview orientation |
| [vermeer-userspace](https://github.com/zuwwik/vermeer-userspace) | Sensor stack patches, ModemManager, haptics and rootfs integration |

## Current limits

The cameras are OVX8000 main, OV16A1Q front and IMX355 ultrawide. The GC02M1
macro module is not enabled. Main-camera autofocus uses contrast, not Android
PDAF; full 50 MP, OIS, HDR and Qualcomm ICP processing are not implemented.
The panel exposes 60, 90 and 120 Hz modes.

Goodix touch events are read as complete packets, with ten input slots. In the
physical test the controller itself reported up to five contacts, including in
validated raw packets; ten-finger reporting has not been established.

GPI commands and DMA ring doorbells use ordered MMIO, and event progress reads
order the following DMA-memory accesses. Kernel #79 completed 50,000 read-only
SPI transactions, 23,538 physical touch reports, display runtime-resume cycles
and concurrent GPU/CPU/camera/audio/sensor/UFS checks without SPI/GPI timeouts.
The separate 15-minute health observation kept ADSP running and battery present
in all 90 samples. This is bounded validation, not an endurance guarantee.
Existing early display-clock and SPMI probe warnings remain separate issues.

Ordinary capture uses one physical AMIC1 duplicated into two channels. The
audio repository contains native voice routing and media-state restoration.
Bluetooth desktop audio and modem-call Bluetooth routing are separate paths.
GPS fixes and simultaneous SMS reception on both SIMs have not been validated.

The tree includes FastRPC buffer lifetime/probe fixes, serialized endpoint
removal, single-response ownership and cleanup of parked caller references.
UCSI recovery handles repeated and coalesced GLINK service restarts without
reusing freed connectors. DSP handover interrupts are handled once.
These changes do not establish a fix for every spontaneous closed ADSP
charger-firmware exception. The haptics driver disables the unused PM8550B
SoundWire input; the two WCD9385 audio slaves remain managed normally.

Host checks are under `tools/testing/vermeer/`:

```sh
python3 tools/testing/vermeer/test-goodix-events.py
python3 tools/testing/vermeer/test-gpi-ordering.py
python3 tools/testing/vermeer/test-vermeer-battmgr.py
python3 tools/testing/vermeer/test-fastrpc-buffer.py
python3 tools/testing/vermeer/test-adsp-transport.py
python3 tools/testing/vermeer/test-fastrpc-reply.py
python3 tools/testing/vermeer/test-fastrpc-close.py
```

They exercise actual driver functions with a mock transport and ASan/UBSan.
They do not force a live DSP crash.

For a bounded diagnostic capture on the phone, run as root:

```sh
python3 tools/testing/vermeer/observe-adsp.py --seconds 3600
```

This records kernel messages, stock charger GET logs and device state under
`/var/log/vermeer-adsp/<boot-id>/`. It keeps at most two 8 MiB files, stops
after an hour or at low charge with net discharge, and leaves audio routing
unchanged. The firmware log channel supports one capture per ordinary boot;
the battery transport is not rebound.

## GPU and idle power

A740 IFPC uses its own register restore tables, with the polled HFI/OOB
acknowledgements protected from the interrupt handler. UFS lane-clock
references are balanced, and the unused PCIe1 controller/PHY stay disabled.
Normal GPU/camera activity and sleep/resume were checked on Vermeer.

The measured IFPC saving was modest: roughly 0.04 W in a light inter-frame
rendering test, not a battery-life multiplier. GPU runtime suspend already
powers it down during complete inactivity. Full SoC low-power entry remains
unconfirmed, and the board's development clock/domain boot flags are retained.
