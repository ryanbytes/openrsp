# OpenRSP

OpenRSP is the start of a clean-room, open-source userspace driver for SDRplay RSP receivers. The first hardware target is the RSPduo (`1df7:3020`). The goal is to replace both SDRplay's background daemon and proprietary client library, not wrap them. OpenRSP currently performs safe USB discovery and evidence capture. It **does not yet tune or stream IQ samples**.

That limitation is deliberate. SDRplay's public API is documented, but its USB protocol and hardware-control implementation are proprietary. SoapySDRPlay3 and gr-sdrplay3 are open application adapters that still require SDRplay's closed API. Claiming this repository is a working replacement before independently documenting USB initialization, tuner register programming, sample framing, and calibration would be bullshit.

## Current status

| Capability | Status |
| --- | --- |
| Discover USB devices using SDRplay/Mirics vendor ID `0x1df7` | Implemented |
| Read public USB descriptors without claiming an interface | Implemented |
| Machine-readable probe output | Implemented |
| Original RSP1-class/RSP1A/RSP2 model-ID hints | Discovery only |
| RSPduo identification | Verified locally; discovery only |
| RSPdx/RSP1B/RSPdxR2 identification | Awaiting hardware evidence |
| Tuning, gain, filters, antennas, bias-T | Not implemented |
| IQ streaming | Not implemented |
| API 3 ABI compatibility | Not implemented |

## Build and test

Requirements: CMake, a C11 compiler, pkg-config, and libusb 1.0.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/openrsp-probe
```

On macOS with Homebrew: `brew install cmake pkg-config libusb`.

The probe is read-only. The official SDRplay service may prevent string-descriptor access; the tool reports that libusb error instead of stopping or detaching anything. Do not stop a production receiver just to run the probe.

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

OpenRSP is licensed under the MIT License. SDRplay, RSP, and Mirics names identify compatible or investigated hardware; this project is unaffiliated with and not endorsed by SDRplay Limited or Mirics Limited.
