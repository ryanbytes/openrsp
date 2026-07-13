# OpenRSP

OpenRSP is an experimental open-source userspace driver for SDRplay RSP receivers. The first hardware target is the RSPduo (`1df7:3020`). The goal is to replace both SDRplay's background daemon and proprietary client library, not wrap them. The direct GPL backend now initializes tuner A, tunes, and streams IQ without SDRplay software. Application compatibility and stability are not finished.

That limitation is deliberate. SDRplay's public API is documented, but its USB protocol and hardware-control implementation are proprietary. SoapySDRPlay3 and gr-sdrplay3 are open application adapters that still require SDRplay's closed API. Claiming this repository is a working replacement before independently documenting USB initialization, tuner register programming, sample framing, and calibration would be bullshit.

## Current status

| Capability | Status |
| --- | --- |
| Discover USB devices using SDRplay/Mirics vendor ID `0x1df7` | Implemented |
| Read public USB descriptors without claiming an interface | Implemented |
| Machine-readable probe output | Implemented |
| Original RSP1-class/RSP1A/RSP2 model-ID hints | Discovery only |
| RSPduo tuner-A direct initialization | Experimental; verified on one unit |
| RSPdx/RSP1B/RSPdxR2 identification | Awaiting hardware evidence |
| Frequency, sample-rate, basic gain and bandwidth | Experimental Mirics implementation |
| IQ streaming | Experimental; bulk capture verified on hardware |
| API 3.15 discovery/selection/parameter ABI | Implemented and loaded by SDRTrunk |
| API 3.15 `Init`/IQ callbacks/`Update` | Experimental; hardware callback client verified |

## Build and test

Requirements: CMake, a C11 compiler, pkg-config, and libusb 1.0.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/openrsp-probe
./build/openrsp-miri-probe
```

On macOS with Homebrew: `brew install cmake pkg-config libusb`.

The probe is read-only. The official SDRplay service may prevent string-descriptor access; the tool reports that libusb error instead of stopping or detaching anything. Do not stop a production receiver just to run the probe.

`openrsp-lifecycle` is disruptive. On the locally tested macOS/RSPduo system, claiming interface 0 caused SDRplay API clients to receive a physical-removal event and left the proprietary daemon unable to start another stream. Run it only on an offline test system after stopping all SDRplay clients and the proprietary daemon. Its two long confirmation flags are intentional.

`openrsp-iq` is the actual direct driver test. It sends volatile device/tuner configuration commands and streams samples without SDRplay's API or daemon. On macOS it currently needs root access after all SDRplay clients and `com.sdrplay.service` are stopped. Do not run it against a live production receiver.

Example offline capture:

```sh
sudo ./build/openrsp-iq -f 100000000 -s 2048000 -T 1 -e 2 -m 252 capture.iq
```

The output is interleaved little-endian signed 16-bit IQ. RSPduo support is presently tuner A only and the RF routing/calibration behavior still needs measurement.

## macOS replacement install

Build first, uninstall SDRplay's proprietary package using its supplied uninstaller, then install OpenRSP:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
sudo ./scripts/install-macos.sh
```

The installer places the versioned library under `/Library/OpenRSP/0.1` and creates `/opt/homebrew/lib/libsdrplay_api.dylib`, which SDRTrunk checks on Apple Silicon. It refuses to proceed while `com.sdrplay.service` is loaded or when the loader path is a regular file.

Some RSPduo USB states do not expose the factory serial descriptor. Set `OPENRSP_SERIAL` in the application environment to preserve a previously known stable identity; otherwise OpenRSP uses the physical USB port path. Do not commit a receiver serial to a public configuration.

## Development path

1. Identify one exact receiver revision from USB descriptors and physical labeling.
2. Capture USB traffic from documented application actions on a sacrificial/test receiver.
3. Record observations as test fixtures without copying proprietary code or confidential material.
4. Implement device initialization and validate every transfer against captures.
5. Implement bounded tuning and gain controls, then verify with RF measurements.
6. Decode bulk IQ framing and test sample rate, loss, DC offset, spectrum orientation, and sustained operation.
7. Add SoapySDR and SDRTrunk integration only after the core transport is stable.

## Definition of better

A replacement is not better merely because its source is available. Before an RSPduo backend is called usable, it must pass automated tests for:

- 24-hour continuous IQ streaming with sequence/loss accounting;
- service-free startup and deterministic cleanup after application crashes;
- repeated open/tune/stream/close cycles without reconnecting the radio;
- unplug/replug recovery without rebooting macOS;
- bounded USB timeouts with cancellation that cannot deadlock shutdown;
- tuner A and tuner B isolation, followed by dual-tuner operation;
- sample-rate, center-frequency, gain, overload, and dropped-sample measurements;
- failure injection for short transfers, stalls, cancellation, and device removal.

Until those measurements exist, OpenRSP is research software—not a stable driver.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the clean-room rules and evidence required for support claims.

## Licensing and trademarks

OpenRSP is licensed under GPL-2.0-or-later because its direct hardware backend derives from the GPL libmirisdr implementation. The imported source and its history are preserved under `third_party/libmirisdr`. SDRplay, RSP, and Mirics names identify compatible or investigated hardware; this project is unaffiliated with and not endorsed by SDRplay Limited or Mirics Limited.
