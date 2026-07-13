# OpenRSP

**This project is vibecoded experimental software. Do not mistake rapid development or passing tests for broad hardware validation.**

OpenRSP is an experimental open-source userspace driver for SDRplay RSP receivers. The first hardware target is the RSPduo (`1df7:3020`). The goal is to replace both SDRplay's background daemon and proprietary client library, not wrap them. The direct GPL backend now initializes either RSPduo tuner in single-tuner mode or both tuners in dual low-IF mode, tunes, and streams IQ without SDRplay software. Application compatibility and stability are not finished.

That limitation is deliberate. SDRplay's public API is documented, but its USB protocol and hardware-control implementation are proprietary. SoapySDRPlay3 and gr-sdrplay3 are open application adapters that still require SDRplay's closed API. Claiming this repository is a working replacement before independently documenting USB initialization, tuner register programming, sample framing, and calibration would be bullshit.

## Current status

| Capability | Status |
| --- | --- |
| Discover USB devices using SDRplay/Mirics vendor ID `0x1df7` | Implemented |
| Read public USB descriptors without claiming an interface | Implemented |
| Machine-readable probe output | Implemented |
| Original RSP1-class/RSP1A/RSP2 model-ID hints | Discovery only |
| RSPduo tuner-A direct initialization | Verified on one unit |
| RSPduo tuner-B direct initialization | Single-tuner mode verified on one unit at 2.048 and 10 MS/s; dual low-IF A/B mode verified at 2 MS/s per tuner |
| RSPdx/RSP1B/RSPdxR2 identification | Published RSPdx PID recognized for discovery; newer model USB IDs still need evidence |
| Frequency, sample-rate, gain, AGC and bandwidth | Hardware-verified on RSPduo tuners A and B independently |
| RSPduo LNA routing | All valid A/B states use independently observed register/GPIO plans in the below-60 MHz, 60--420 MHz, 420--1000 MHz, and 1--2 GHz bands |
| IQ streaming | Direct/API paths verified; RSPduo single-tuner complex output uses the official ADC filter-mode words and occupies both negative and positive spectral halves at 10 MS/s |
| Stream allocation | Session-owned fixed IQ buffers; no heap allocation in steady-state API callbacks |
| API 3.15 discovery/selection/parameter ABI | Real VID/PID/model/serial propagation; raw USB indexes are re-resolved from stable identity |
| API 3.15 public headers | Documented entry points, typedefs, enums, fields, sizes, and standard header names provided |
| API 3.15 `Init`/IQ callbacks/`Update` | Hardware callback client and SDRTrunk verified |
| API 3.15 RSPduo modes | Live A/B single-tuner swap, initialized single/dual mode transitions, and simultaneous Stream A/Stream B dual mode hardware-verified |
| RSPduo hardware controls | Bias-T on tuner B, RF/DAB notch on A/B, external-reference output on A/B, and tuner-A AM port/notch match observed API/USB control behavior; electrical voltage/filter/reference output not instrument-measured |
| API 3.15 update-reason constants and validation | Implemented; unsupported controls return errors instead of false success |
| API software decimation | Stateful FIR at x2–x32; automated count plus x2 pass/stop-band tests, not RF-measured |
| API transport-failure event | Unexpected daemon disconnect reports `DeviceFailure`; cleanup suppresses `SIGPIPE` |
| Daemon crash recovery | A replacement daemon clears the halted RSPduo bulk endpoint; three consecutive forced daemon deaths recovered through a new API session without USB reset or replug |
| Socket stalls and shutdown | Five-second send/receive deadlines; shutdown wakes blocked readers; timeout/short-frame fixtures |
| USB cancellation state | Atomic cross-thread state with cancellation visibility fixture |
| IQ loss indication | Daemon frame-sequence gaps set the API stream reset flag and advance sample numbering |
| AGC gain events | Applied software-AGC changes emit API `GainChange` payloads |
| Device API locking | Recursive in-process lock plus daemon-owned cross-process lease with crash release |
| Update error fidelity | RF/gain/sample-rate failures use specific API codes and populate `GetLastError` |
| Overload events | Saturation/correction transitions are hysteretic, acknowledged, and dispatched off the IQ reader |
| Unplug/replug recovery | Same-process SDRTrunk transport and P25 decode recovery verified for three consecutive physical RSPduo cycles; extended-cycle/soak validation remains |
| SoapySDRPlay3 compatibility | The pinned upstream module plus the included dual-control patch builds/loads against OpenRSP; live single/dual RSPduo discovery, independent A/B frequency and gain settings, concurrent 2 MS/s A/B streaming, every advertised single-tuner rate from 62.5 kS/s through 10 MS/s, manual IFGR/RFGR with measured level changes, and AGC restore are verified |
| Linux build | Automated Ubuntu build and test verified |
| macOS build | Automated build/test verified; RSPduo hardware verified on one arm64 host |
| Windows build | Not yet ported; POSIX socket, sleep, and pthread dependencies remain |

### API update coverage

The compatibility library implements live sample-rate, RF, bandwidth, IF,
gain/LNA, AGC, PPM, and software-decimation updates for either RSPduo tuner in
single-tuner mode. In dual mode, RF, gain/LNA, AGC, overload acknowledgement,
and software-decimation state are independent per tuner. The shared ADC rate,
IF, and bandwidth cannot be changed with a per-tuner hot update. The
stateful windowed-sinc FIR decimator accepts x2, x4, x8, x16, and x32 and keeps
its filter state across IQ frames. It also accepts the API's required AUTO-LO,
DC/IQ configuration, reset-flag, and overload-message acknowledgement calls.
PPM correction retunes the synthesizer by the inverse crystal-error factor and
reports the completion through `fsChanged`, matching the documented API
callback contract.

RSPdx extensions and controls belonging to other RSP models are not
implemented. Bias-T (tuner B only), RF notch, DAB notch,
external-reference output, and tuner-A AM-port/AM-notch switching are
implemented from independently observed API 3.15.1 behavior. Tuner-B AM
control updates return `OutOfRange`, matching the reference API. Dual-tuner mode
requires a shared 6 or 8 MHz ADC clock, low IF at 1.620 or 2.048 MHz, and no
more than 1.536 MHz requested RF bandwidth; OpenRSP then delivers approximately
2 MS/s per tuner through the separate Stream A and Stream B callbacks. Invalid
mode combinations return a specific API error instead of false success. The
complete 3.15 update-reason values are exposed in the compatibility header so
applications can compile against the implemented ABI.

`sdrplay_api_SwapRspDuoMode()` supports initialized single-to-dual and
dual-to-single transitions. It updates the caller's device enumeration and
channel pointers before resuming IQ, produces a fresh reset on each newly
active stream, and keeps single-tuner B on Stream A. Direct dual sessions return
`InvalidParam` from `sdrplay_api_SwapRspDuoDualTunerModeSampleRate()` because
the documented 6/8 MHz hot-swap entry point is restricted to master/slave mode,
which OpenRSP does not yet expose.

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

If SoapySDR development files are installed, the build also produces
`openrsp-soapy-control-probe`. With the upstream SoapySDRPlay3 module on
`SOAPY_SDR_PLUGIN_PATH`, this disruptive hardware test disables AGC, sweeps
RSPduo IFGR and RFGR, requires IQ to continue, verifies monotonic measured
sample levels, and restores AGC. Stop other radio applications before running
it:

The pinned upstream SoapySDRPlay3 revision keeps only one channel-parameter
pointer and sends dual-mode updates to tuner `Both`, so its channel 1 frequency
and gain methods actually modify channel 0. Apply OpenRSP's small MIT-licensed
adapter patch before building that revision:

```sh
git -C /path/to/SoapySDRPlay3 checkout 6cc31316b730503cee3e30906ff1975175a16400
git -C /path/to/SoapySDRPlay3 apply \
  /path/to/openrsp/contrib/soapysdrplay3-rspduo-dual-controls.patch
```

```sh
SOAPY_SDR_PLUGIN_PATH=/path/to/SoapySDRPlay3/build \
  ./build/openrsp-soapy-control-probe --serial YOUR_SERIAL
```

Use `--rates` to measure every rate advertised by the module, or `--rate SPS`
to test one rate. These modes compare delivered IQ against wall-clock time and
restore the normal 2 MS/s configuration and AGC after a successful run. Use
`--frequency HZ` to run the same control test at a specific RF center.
Use `--dual` to require the patched module to discover `mode=DT`, expose two
RX channels, retain distinct live frequency/IFGR/RFGR settings, and deliver IQ
concurrently through both Soapy streams.

The standalone compatibility path has an independent verifier that does not
load SoapySDR. With other radio applications stopped, it measures native API
callback throughput from 2 through the API maximum of 10.66 MS/s and restores
2 MS/s plus AGC on success or failure:

```sh
./build/sdrplay-rate-probe --rates
```

The RSPduo gain-plan verifier accepts one tuner, RF frequency, and either one
LNA state or `all`. It prints a timestamped boundary around every update so an
independent control-boundary trace can distinguish initialization, each live
gain change, and cleanup:

```sh
./build/sdrplay-rspduo-gain-probe A 100000000 all
./build/sdrplay-rspduo-gain-probe B 1500000000 8
./build/sdrplay-rspduo-mode-probe mode-to-dual
./build/sdrplay-rspduo-mode-probe mode-to-single
```

This is a disruptive hardware test. Stop radio applications first. A successful
run requires continued IQ, successful updates, and complete API/device cleanup;
it does not measure absolute RF gain without a calibrated source.

The native stream verifier can optionally retain up to one second of
interleaved little-endian signed 16-bit IQ for independent spectrum analysis.
The final positional arguments select the output file, RF bandwidth in kHz,
gain reduction, and LNA state:

```sh
./build/sdrplay-compat-stream-test \
  853712500 853712500 4 2048000 0 4 0 capture.iq 1536 20 0 A
```

Use final argument `B` to select tuner B in RSPduo single-tuner mode. The
verifier measures callback throughput from the first callback through the last
measurement boundary, so synchronous update time is included rather than
misreported as sample-rate error.

When more than one update is requested, the verifier distributes updates
across the second half of the run. It requires one acknowledgement per RF,
sample-rate, and gain change and always attempts API/device cleanup on failure.

To exercise complete API teardown and reacquisition repeatedly in one process:

```sh
./build/sdrplay-lifecycle-probe --cycles 10
```

Each cycle must stream before and after a live rate/RF/gain update, preserve
sample numbering, report only the initial reset indication, and release every
API/device/stream resource without reconnecting the receiver.

To kill a disposable streaming child process and verify that the daemon releases
its ownership for a new complete lifecycle on the same receiver:

```sh
./build/sdrplay-lifecycle-probe --crash-recovery 10
```

The `openrsp-probe` USB descriptor probe is read-only. The official SDRplay
service may prevent string-descriptor access; that tool reports the libusb
error instead of stopping or detaching anything. Do not stop a production
receiver just to run the descriptor probe.

`openrsp-lifecycle` is disruptive. On the locally tested macOS/RSPduo system, claiming interface 0 caused SDRplay API clients to receive a physical-removal event and left the proprietary daemon unable to start another stream. Run it only on an offline test system after stopping all SDRplay clients and the proprietary daemon. Its two long confirmation flags are intentional.

`openrsp-iq` is the actual direct driver test. It sends volatile device/tuner configuration commands and streams samples without SDRplay's API or daemon. On macOS it currently needs root access after all SDRplay clients and `com.sdrplay.service` are stopped. Do not run it against a live production receiver.

Example offline capture:

```sh
sudo ./build/openrsp-iq -f 100000000 -s 2048000 -T 1 -e 2 -m 252 capture.iq
```

The output is interleaved little-endian signed 16-bit IQ. This command-line tool
supports either RSPduo tuner in single-tuner mode; simultaneous dual operation
is exposed through the daemon protocol and API compatibility library.

## Linux replacement install

Linux builds install a standard ELF library with the loader names
`libsdrplay_api.so`, `libsdrplay_api.so.3`, and
`libsdrplay_api.so.3.15`, the public API headers, a `sdrplay_api.pc`
pkg-config file, `openrspd`, command-line tools, and a systemd unit. Build with
the final prefix because that prefix is compiled into the firmware fallback
and service unit:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Before uninstalling SDRplay's package, locate its official
`sdrplay_apiService` executable and extract the firmware without launching it:

```sh
service=$(command -v sdrplay_apiService || true)
if test -z "$service"; then
  service=$(find /usr/local -type f -name sdrplay_apiService -print -quit)
fi
test -n "$service"
./build/openrsp-extract-firmware "$service" --output /tmp/rspduo-3020.bin
test "$(wc -c < /tmp/rspduo-3020.bin | tr -d ' ')" = 6115
test "$(sha256sum /tmp/rspduo-3020.bin | awk '{print $1}')" = \
  f2a9451acd81fd8f09c5a0e506335d5d1ec9ab2c4ed53dd98de5cfff2a249387
```

Use SDRplay's supplied uninstaller to remove its API and service before
installing OpenRSP. Do not run two services against the same receiver. Then:

```sh
sudo cmake --install build
sudo install -m 0644 /tmp/rspduo-3020.bin \
  /usr/local/share/openrsp/firmware/rspduo-3020.bin
sudo ldconfig
sudo systemctl daemon-reload
sudo systemctl enable --now openrspd.service
systemctl --no-pager --full status openrspd.service
```

The systemd service runs the standalone driver as root so it can claim the USB
interface without a permissive global udev rule. Application processes use the
local Unix socket and do not need root. Linux installation is build/CI
verified; RSPduo hardware operation has only been verified on macOS so far.

## macOS replacement install

Build OpenRSP first. Before uninstalling SDRplay's package, extract the RSPduo
firmware from the official service already installed on the same machine:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/openrsp-extract-firmware /Library/SDRplayAPI/3.15.1 \
  --output /tmp/rspduo-3020.bin
test "$(wc -c < /tmp/rspduo-3020.bin | tr -d ' ')" = 6115
firmware_sha256=$(shasum -a 256 /tmp/rspduo-3020.bin | awk '{print $1}')
test "$firmware_sha256" = \
  f2a9451acd81fd8f09c5a0e506335d5d1ec9ab2c4ed53dd98de5cfff2a249387
```

The extractor accepts either the versioned official install directory or the
full path to its `bin/sdrplay_apiService` executable. It reads that file only;
it does not launch the proprietary service. The extractor requires both known
firmware signatures, rejects conflicting embedded copies, and writes exactly
6,115 bytes. The checksum above is the image extracted from SDRplay API 3.15.1;
do not assume a different official API release contains the same image.

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

Some RSPduo USB states do not expose the factory serial descriptor. In that
case OpenRSP uses the physical USB port path reported by the backend, so reconnect
the receiver to the same port for deterministic recovery. `OPENRSP_SERIAL`
changes only the serial shown to API applications; it is never trusted to choose
hardware. Do not commit a receiver serial to a public configuration.

## Licensing and trademarks

OpenRSP is licensed under GPL-2.0-or-later because its direct hardware backend derives from the GPL libmirisdr implementation. The imported source and its history are preserved under `third_party/libmirisdr`. SDRplay, RSP, and Mirics names identify compatible or investigated hardware; this project is unaffiliated with and not endorsed by SDRplay Limited or Mirics Limited.

## Dedication

Thanks Carolyn
