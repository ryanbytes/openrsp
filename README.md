# OpenRSP

**This project is vibecoded experimental software. Do not mistake rapid development or passing tests for broad hardware validation.**

OpenRSP is an experimental open-source userspace driver for SDRplay RSP receivers. The first hardware target is the RSPduo (`1df7:3020`). The goal is to replace both SDRplay's background daemon and proprietary client library, not wrap them. The direct GPL backend now initializes tuner A, tunes, and streams IQ without SDRplay software. Application compatibility and stability are not finished.

That limitation is deliberate. SDRplay's public API is documented, but its USB protocol and hardware-control implementation are proprietary. SoapySDRPlay3 and gr-sdrplay3 are open application adapters that still require SDRplay's closed API. Claiming this repository is a working replacement before independently documenting USB initialization, tuner register programming, sample framing, and calibration would be bullshit.

## Current status

| Capability | Status |
| --- | --- |
| Discover USB devices using SDRplay/Mirics vendor ID `0x1df7` | Implemented |
| Read public USB descriptors without claiming an interface | Implemented |
| Machine-readable probe output | Implemented |
| Original RSP1-class/RSP1A/RSP2 model-ID hints | Discovery only |
| RSPduo tuner-A direct initialization | Verified on one unit |
| RSPdx/RSP1B/RSPdxR2 identification | Published RSPdx PID recognized for discovery; newer model USB IDs still need evidence |
| Frequency, sample-rate, gain, AGC and bandwidth | Hardware-verified on RSPduo tuner A |
| IQ streaming | Direct and standalone API paths verified on hardware |
| Stream allocation | Session-owned fixed IQ buffers; no heap allocation in steady-state API callbacks |
| API 3.15 discovery/selection/parameter ABI | Real VID/PID/model/raw-index propagation; implemented and loaded by SDRTrunk |
| API 3.15 public headers | Documented entry points, typedefs, enums, fields, sizes, and standard header names provided |
| API 3.15 `Init`/IQ callbacks/`Update` | Hardware callback client and SDRTrunk verified |
| API 3.15 update-reason constants and validation | Implemented; unsupported controls return errors instead of false success |
| API software decimation | Stateful FIR at x2–x32; automated count plus x2 pass/stop-band tests, not RF-measured |
| API transport-failure event | Unexpected daemon disconnect reports `DeviceFailure`; cleanup suppresses `SIGPIPE` |
| Socket stalls and shutdown | Five-second send/receive deadlines; shutdown wakes blocked readers; timeout/short-frame fixtures |
| USB cancellation state | Atomic cross-thread state with cancellation visibility fixture |
| IQ loss indication | Daemon frame-sequence gaps set the API stream reset flag and advance sample numbering |
| AGC gain events | Applied software-AGC changes emit API `GainChange` payloads |
| Device API locking | Recursive in-process lock plus daemon-owned cross-process lease with crash release |
| Update error fidelity | RF/gain/sample-rate failures use specific API codes and populate `GetLastError` |
| Overload events | Saturation/correction transitions are hysteretic, acknowledged, and dispatched off the IQ reader |
| Unplug/replug recovery | Verified once on RSPduo without restarting OpenRSP or SDRTrunk |
| Linux build | Automated Ubuntu build and test verified |
| macOS build | Automated build/test verified; RSPduo hardware verified on one arm64 host |
| Windows build | Not yet ported; POSIX socket, sleep, and pthread dependencies remain |

### API update coverage

The compatibility library implements live sample-rate, RF, bandwidth, IF,
gain/LNA, AGC, PPM, and software-decimation updates for RSPduo tuner A. The
stateful windowed-sinc FIR decimator accepts x2, x4, x8, x16, and x32 and keeps
its filter state across IQ frames. It also accepts the API's required AUTO-LO,
DC/IQ configuration, reset-flag, and overload-message acknowledgement calls.
PPM correction retunes the synthesizer by the inverse crystal-error factor and
reports the completion through `fsChanged`, matching the documented API
callback contract.

RSPduo bias-T/antenna/notch/external-reference switching, RSPdx extensions, and
controls belonging to other RSP models are not implemented. Those calls return
a specific API error. They do not return false success. The complete 3.15
update-reason values are exposed in the compatibility header so applications
can compile against the implemented ABI.

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

Build OpenRSP first. Before uninstalling SDRplay's package, extract the RSPduo
firmware from the official service already installed on the same machine:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/openrsp-extract-firmware /Library/SDRplayAPI/3.15.1 \
  --output /tmp/rspduo-3020.bin
test "$(wc -c < /tmp/rspduo-3020.bin | tr -d ' ')" = 6115
shasum -a 256 /tmp/rspduo-3020.bin
```

The extractor accepts either the versioned official install directory or the
full path to its `bin/sdrplay_apiService` executable. It reads that file only;
it does not launch the proprietary service. The extractor requires both known
firmware signatures, rejects conflicting embedded copies, and writes exactly
6,115 bytes.

If the official package is downloaded but not installed, expand it and point
the extractor at the service inside its payload:

```sh
pkgutil --expand-full /path/to/SDRplayAPI-macos-installer.pkg \
  /tmp/sdrplay-api-expanded
service=$(find /tmp/sdrplay-api-expanded -type f \
  -name sdrplay_apiService -print -quit)
test -n "$service"
./build/openrsp-extract-firmware "$service" --output /tmp/rspduo-3020.bin
test "$(wc -c < /tmp/rspduo-3020.bin | tr -d ' ')" = 6115
```

Keep the extracted firmware local. It is not distributed by OpenRSP and must
not be committed to this repository. Obtain the official package yourself and
comply with its license.

Now uninstall SDRplay's proprietary package using its supplied uninstaller,
install OpenRSP, and place the extracted firmware at the driver's default path:

```sh
sudo ./scripts/install-macos.sh
sudo install -m 0644 /tmp/rspduo-3020.bin \
  /Library/OpenRSP/0.1/firmware/rspduo-3020.bin
```

The installer places the versioned library under `/Library/OpenRSP/0.1` and creates `/opt/homebrew/lib/libsdrplay_api.dylib`, which SDRTrunk checks on Apple Silicon. It refuses to proceed while `com.sdrplay.service` is loaded or when the loader path is a regular file.
When given a library from a non-default build directory, it installs the daemon
and reset utility from that same directory so build configurations cannot be
mixed accidentally.

The RSPduo backend reads `/Library/OpenRSP/0.1/firmware/rspduo-3020.bin` only
when normal frontend initialization indicates a cold-boot firmware load is
needed. Development runs can override that location with the
`OPENRSP_RSPDUO_FIRMWARE` environment variable. A missing or incorrectly sized
file makes cold-boot initialization fail explicitly; OpenRSP does not download
firmware from the network or silently substitute another image.

Some RSPduo USB states do not expose the factory serial descriptor. Set `OPENRSP_SERIAL` in the application environment to preserve a previously known stable identity; otherwise OpenRSP uses the physical USB port path. Do not commit a receiver serial to a public configuration.

## Licensing and trademarks

OpenRSP is licensed under GPL-2.0-or-later because its direct hardware backend derives from the GPL libmirisdr implementation. The imported source and its history are preserved under `third_party/libmirisdr`. SDRplay, RSP, and Mirics names identify compatible or investigated hardware; this project is unaffiliated with and not endorsed by SDRplay Limited or Mirics Limited.
