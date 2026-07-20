# RSPduo evidence log

## Locally verified baseline

- USB vendor/product: `1df7:3020`
- USB device class: vendor-specific (`0xff`)
- Configurations: 1
- Interface 0 alternate 1: isochronous IN endpoint `0x81`, raw max-packet field `0x1400`
- Interface 0 alternate 2: isochronous IN endpoint `0x81`, raw max-packet field `0x0400`
- Interface 0 alternate 3: bulk IN endpoint `0x81`, max packet 512 bytes
- Serial descriptor: readable (value intentionally not recorded here)
- Model mapping: SDRplay's published VirtualHere guide states PID `3020` is RSPduo

## Destructive-interference observation

On 2026-07-12, opening and claiming interface 0 and then immediately releasing it used no vendor writes and made no alternate-setting change, but still caused the active SDRplay API client to report physical removal. The proprietary daemon remained running yet subsequently returned `Service Not Responding` on stream initialization. Therefore interface claim is classified as disruptive and must never be part of discovery or an unattended hardware test.

The USB descriptor alone does not establish control-transfer meaning, tuner registers, initialization, or IQ framing. The direct-streaming evidence below comes from the separately licensed GPL Mirics implementation and a hardware capture, not from descriptor inference.

## Direct streaming milestone

On 2026-07-12, the GPL Mirics backend with PID `0x3020` added initialized the connected RSPduo as tuner A with SDRplay's daemon fully unloaded. It set 2.048 MS/s, tuned to 100 MHz, selected bulk endpoint `0x81`, and captured 64,749,568 bytes during an eight-second bounded run. The capture contained changing interleaved 16-bit values rather than an empty or constant stream. This proves direct transport, volatile initialization, tuning calls, and sustained sample delivery on that unit; it does not yet prove frequency calibration, spectrum orientation, RF-path correctness, or sensitivity.

## Capture record template

For each experiment record:

- hardware label and revision;
- host OS and USB topology;
- exact user action and starting state;
- control request type, request, value, index, length, and payload hash;
- bulk endpoint, direction, transfer lengths, timing, and payload fixture hash;
- at least two repeated captures and one negative/control capture;
- the narrow conclusion supported by the observation;
- alternative explanations not yet excluded.

Serial numbers and unrelated host traffic must be removed before committing evidence.

## Standalone API compatibility investigation (2026-07-12)

With SDRTrunk left on its RTL-only path and the proprietary SDRplay service
unloaded, a direct OpenRSP bulk capture at 2.048 MS/s ran for 20 seconds and
wrote 40,632,320 complex signed-16-bit samples (155 MiB).  The capture had a
non-zero peak of 29,696 and changing sample values.  This repeated the direct
transport milestone with a longer bounded run.

The first daemon stream attempts configured the receiver but stalled between
the daemon's libusb callback and the API client's IQ callback.  USB capture
showed complete 65,536-byte payloads entering the daemon.  A local socket test
then established the actual cause: on macOS, the accepted Unix-domain socket
inherited `O_NONBLOCK` from the listening socket.  Small control responses fit,
but an IQ frame could be partially written and then fail with `EAGAIN`, leaving
the client waiting for the rest of the declared frame.  Accepted client sockets
are now explicitly returned to blocking mode before any protocol traffic.

After that fix, a bounded standalone API run delivered 6,356,992 complex
samples through 5,044 callbacks in four seconds.  The same run completed RF and
gain updates, reported the first-stream reset and RF/gain change flags, and
cleanly uninitialized.  USB, daemon protocol, and API callback boundaries were
all observed at 65,536 bytes.

Five consecutive gain-reduction states (20, 30, 40, 50, and 59 dB), LNA states
0 through 9, AGC enable/recovery, and AGC disable all returned success at
10 MS/s and 8 MHz bandwidth without reconnecting the receiver.  The daemon now
applies the RSPduo LNA GPIO sequence for the verified 420--1000 MHz path rather
than folding LNA into a generic total-gain value.

## Bandwidth isolation and transport backpressure (2026-07-12)

A power-clean direct run at 853.9 MHz, 2.048 MS/s, bulk transfer, 252 format,
and the 8 MHz hardware filter streamed for eight seconds and wrote 39 MiB of
changing IQ.  The same direct path with only the hardware bandwidth changed to
1.536 MHz failed during initial bulk submission and wrote zero bytes.  This
isolates the current failure to the reverse-engineered RSPduo bandwidth/control
sequence; it is not caused by SDRTrunk or the API-compatible socket protocol.

Forcing 8 MHz when an application requests 1.536 MHz is explicitly rejected as
a solution.  OpenRSP must implement each advertised filter path correctly and
must not silently substitute a wider bandwidth.

The daemon sends IQ frames synchronously over a blocking client socket.  This is
the hardware-verified path.  It intentionally favors correctness over silent
sample loss; a future decoupled sender must have explicit loss/backpressure
semantics and its own hardware stress evidence before replacing this path.

The official macOS 3.15.1 installer was expanded without installation.  A
temporary ad-hoc-signed copy of its service loaded the local
libusb-control-transfer interposer, and a reference client compiled against the
official headers/library.  The captured 1.536 MHz setup transfers informed the
independently implemented strict profile below; no proprietary executable or
header is distributed with OpenRSP.

The first strict profile now reproduces the official pre-stream transaction
for tuner A at 853.9 MHz, 2.048 MS/s, zero IF, 1.536 MHz bandwidth, gain
reduction 40, and LNA state 3.  It includes two readiness reads followed by the
captured ADC, synthesizer, RF routing, gain, and PLL writes in their observed
order.  This profile is intentionally exact: other startup tuples continue
through the experimental calculated path and are not described as verified.

The official reference produced 4,130,112 samples and 3,073 callbacks during a
two-second run at that tuple, including successful RF and gain updates.  The
OpenRSP direct backend produced 45 MiB during an eight-second run after its ADC
PLL and format words were corrected to the captured values.  The remaining
bandwidth gate is broader tuple coverage: the captured strict 1.536 MHz profile
is verified only for its exact tuner-A tuple, while the calculated path remains
experimental outside the hardware-tested 8 MHz setup.

## SDRTrunk integration and live gain control (2026-07-12)

The system installation under `/Library/OpenRSP/0.1` was loaded by the real
SDRTrunk application with SDRplay's proprietary service absent.  SDRTrunk
discovered the RSPduo, selected tuner A, configured 10 MS/s with an 8 MHz
bandwidth, streamed IQ, and issued RF, sample-rate, bandwidth, gain, LNA, and
AGC updates.  Live P25 control channels and traffic grants decoded through this
path.

The SDRTrunk RSPduo gain guard was corrected in the user's public fork so either
an LNA change or a baseband-gain-reduction change reaches the API.  A packaged
build from public commit `02f73c16` was installed and verified against the
receiver: the UI held an LNA change from 0 to 6, OpenRSP logged a successful
`GAIN` update with `lna=6`, and decoding recovered to 48 messages per second.
Returning the UI to LNA 0 produced the matching successful daemon update.  This
proves the application-to-daemon control path and continued IQ delivery; it is
not a calibrated RF gain measurement.

## ABI evidence (2026-07-12)

The compatibility library exports every documented public symbol in the
SDRplay API 3.15 header used for the audit. The independently written public
headers now expose the documented model IDs, constants, enums, named parameter
fields, callback and API function typedefs, and all ten conventional header
names instead of using opaque padding placeholders for non-RSPduo models. A C
consumer and a C++ consumer both compile and link against those headers.

The layout tool was separately compiled against the official 3.15.1 headers
from the locally expanded installer and against OpenRSP. Its complete output
for device, channel, tuner, control, callback, error, RSP1A, RSP2, RSPduo, and
RSPdx structures and named field offsets is byte-for-byte identical on arm64
macOS. CI retains independent size assertions without requiring or distributing
the official package. Unit tests and an ASan/UBSan build pass; Apple
AddressSanitizer leak detection is unavailable, so the sanitizer run uses
`detect_leaks=0` and must not be described as a leak check.

The documented pre-selection lifecycle is also exercised: debug logging accepts
a null device handle before selection, while heartbeat disabling succeeds after
`Open` and before device selection, matching the reference library.
Debug logging now stores the selected level and gates compatibility-layer
message, warning, and error diagnostics; it is no longer validation-only.
The device API lock is a daemon-owned, cross-process lease rather than only an
in-process mutex. Contending clients receive a busy response, and disconnecting
the owner releases the lease so an application crash cannot strand discovery.

Enumeration preserves each libmirisdr device's actual VID, PID, serial, and raw
index instead of stamping every record as an RSPduo at index zero. The daemon
filters non-SDRplay devices, and the compatibility API maps the published
RSP1/RSP1A/RSP2/RSPduo hardware identifiers. SDRplay's published PID list also
identifies RSPdx as `0x3030`; OpenRSP
recognizes that identity in direct discovery but does not claim streaming
support for it. See the [SDRplay VirtualHere guide](https://www.sdrplay.com/docs/VirtualHere.pdf).

## Software processing and synchronized-update coverage (2026-07-14)

The compatibility stream now applies independent per-tuner DC removal and IQ
imbalance correction before ADS-B filtering and normal decimation. Correction
coefficients and periodic calibration schedules are retained independently of
callback frame boundaries. Synthetic
fixtures exercise correction enable/disable state, convergence, retained frame
state, calibration windows, and all four documented ADS-B modes with pass-band
and stop-band signals. The no-decimation ADS-B path uses a 17-tap fixed-point
FIR with duplicated circular history so its inner loop has no modulo or
floating-point division. These tests establish deterministic software behavior;
they are not calibrated RF measurements against the proprietary implementation.

Gain, RF, sample-rate, and AGC updates can be scheduled at wrap-safe one-shot or
periodic sample boundaries. Reset flags cancel only their selected pending
categories. The compatibility layer enforces the documented sample-rate and
gain-limit-dependent AGC setpoint bounds, including extended gain-reduction
setpoints through 0 dBFS. The internal daemon protocol accepts the complete
`-72..0` envelope because configuration messages do not carry the API's
`minGr` policy and unchanged AGC fields accompany unrelated updates. Unit and
mock-daemon tests cover the boundary and reset behavior; live RSPduo timing and
RF-output validation remain outstanding.

Protocol version 8 adds a typed device-status frame. A transport/socket loss
still maps to `DeviceFailure`. When fresh USB inventories cannot resolve the
selected stable identity for five seconds, the daemon emits one removal status
and the compatibility layer maps it to `DeviceRemoved`; shorter absences retain
the previously verified transparent replug recovery path. Hardware-free tests
cover grace timing, one-shot notification, backend propagation, and separation
from `DeviceFailure`. A new long-unplug live test has not yet been run.

## Live unplug/replug recovery (2026-07-12)

The first live replug attempt exposed a stale-handle failure: libusb ended the
bulk stream, the daemon retained the old device handle, and later updates
returned an I/O error.  Reopening the re-enumerated device restored IQ, but a
second attempt showed that a transient update error made SDRTrunk permanently
discard the tuner.  Queuing the latest requested configuration avoided that
error, but a third attempt exposed SDRTrunk's two-second wait for the matching
RF-change callback while no IQ callbacks existed to carry the change flag.

The final recovery path closes the failed handle, polls for the selected device
index, reapplies the latest queued configuration, restarts the stream, restores
gain after first IQ, and explicitly acknowledges recovery-queued update flags
through a zero-sample API callback.  A fourth physical unplug/replug passed on
the live scanner: OpenRSP and SDRTrunk retained their original PIDs, the daemon
resumed 65,536-byte IQ frames and restored GR 50/LNA 0, SDRTrunk logged no
terminal update failure or tuner removal, and decoding resumed.  This is one
successful recovery cycle on one RSPduo; repeated-cycle and long-disconnect
testing remain required before claiming general recovery stability. That live
test still reused the discovery-time raw USB index; the correction below has
software-test coverage but still needs a physical replug validation.

## Stable receiver selection across re-enumeration (2026-07-13)

Protocol version 2 carries the selected receiver's actual USB VID, PID, serial
or physical-port identity, and discovery-time raw index. Before initial open or
recovery, the daemon takes a fresh discovery snapshot and resolves the stable
tuple again. A changed raw index is accepted only when the stable tuple matches;
duplicate tuples and missing receivers fail closed. The raw index is used as a
fallback only when no identity string exists.

The hardware-free identity fixture moves an RSPduo from raw index 13 to 7,
excludes a different product with the same serial, rejects duplicate matches
without altering the caller's output, exercises the no-serial fallback, and
rejects malformed non-terminated identities. The compatibility mock also
requires the full RSPduo VID/PID/serial tuple in its acquire request. Debug,
Release, and ASan/UBSan test suites pass; this section does not claim a new live
unplug/replug result.

### Live protocol-v2 deployment

Commit `e3d652b` passed the hosted macOS and Ubuntu jobs before deployment. The
installed compatibility library and daemon matched the tested Release build at
SHA-256 `feb2d497fc447646c3658f4105a2b43291456a03044d85dd43e166bee7f5c9c5`
and `eceb6013a871078c8d623c1fa8830263702f3423cb3bcde9aaa8ffd21c0990a4`,
respectively. The fresh daemon session reported protocol 2, discovered the
RSPduo by factory serial, configured 10 MSPS at GR 50/LNA 0, delivered its first
65,536-byte USB and socket IQ frames, applied the initial gain update, and
accepted repeated RF updates without a recovery or I/O error.

SDRTrunk's live tuner view then showed the RSPduo as the control tuner centered
at 855.01250 MHz with eight locked P25 channels. The eight listed RSPduo-backed
channels included six control channels and two active traffic channels, with
live signal and frequency-error measurements. The session also accumulated
successful upload counts across multiple P25 sites. The only
other active tuner was an RTL receiver at 155 MHz, so the displayed 800 MHz P25
chains were specifically allocated to the RSPduo. This proves post-deployment
streaming, tuning, gain application, and live P25 use. At that stage,
stable-identity recovery after a new physical replug remained unverified; the
later replug evidence below supersedes that limitation in part.

### Cold replug, client backpressure, and bounded cleanup

A physical replug on 2026-07-13 exposed two distinct failures. The receiver
first enumerated as `1df7:3020` with no serial descriptor, so factory-serial
resolution correctly failed closed but could not reopen the radio. After the
firmware bootstrap path ran, the same physical receiver re-enumerated with
factory serial (redacted), and the existing daemon resolved and reopened it.

That reopen produced USB IQ, but SDRTrunk had stopped draining the stream
socket after quarantining the removed tuner. A process stack sample showed the
IQ callback blocked indefinitely in `write()`, holding libusb's event lock,
while the daemon main thread waited inside a synchronous gain control transfer.
Commit `72940c4` bounds each accepted client's socket writes to two seconds and
evicts a client after an IQ write timeout. It also bootstraps only an RSPduo
whose USB descriptor explicitly reports the cold no-serial state before doing
stable identity resolution.

The exact Release daemon from that commit was deployed at SHA-256
`42f41982b06ff6af77637952e2dcf109238dffd4fc6a893c45c8abffa9deedb7`.
On startup it encountered two real bulk-transfer start failures, reopened the
same receiver without a physical replug, delivered 65,536-byte USB and socket
IQ frames, and restored GR 50/LNA 0. A post-deployment process sample showed
the main thread idle in `poll()` and the stream thread servicing libusb rather
than the previous cross-thread deadlock.

This is evidence for cold firmware preparation, stable identity resolution,
bounded client backpressure, and driver-side stream/gain recovery. It is not a
claim of transparent end-to-end recovery in every application: this SDRTrunk
build removed and quarantined the logical tuner during the outage and did not
reattach it until restart. Repeated same-process replug cycles and application
reattachment remain open gates.

The next failure trace showed why the API client stopped draining the daemon
socket after recovery: its socket-reader thread invoked the application's IQ
callback inline. A callback paused by application lifecycle handling therefore
blocked both incoming IQ and control responses. The compatibility layer now
does deinterleaving, sequence accounting, overload detection, AGC measurement,
and decimation in socket order, then places the processed frame on a bounded
application-callback queue. A slow callback can no longer stop socket reads or
RF/gain responses. If the queue fills, it drops the oldest processed frame and
the callback dispatcher derives a reset from the resulting sample-number gap.

The regression fixture deliberately blocks an application IQ callback, sends
an RF update from another thread, and requires the update to complete while the
callback remains blocked. Exact decimation accounting and filter measurements
still pass, proving that the queue was not incorrectly placed ahead of ordered
IQ processing. Debug, Release, and ASan/UBSan tests pass. This change has not
yet passed a new physical replug cycle, so transparent SDRTrunk survival is
still unverified.

The first live deployment of that queue loaded the exact Release library,
restored GR 50/LNA 0, and produced fresh P25 grants. An uncommanded USB dropout
then exposed a separate five-second boundary: `openrsp_client_receive` treated
a socket receive timeout during the daemon's otherwise successful recovery as
EOF. The API emitted `DeviceFailure` and SDRTrunk removed the tuner before the
daemon cold-booted the receiver and resumed USB IQ. Client receive now returns
distinct success, idle-timeout, and fatal-error results. The streaming reader
continues after a timeout with no frame; synchronous command waits still expire
after five seconds, and EOF, partial frames, and malformed frames remain fatal.
The timeout fixture separately verifies silent timeout, shutdown wakeup, and
truncated-frame classification. A new physical replug is still required to
validate this change end to end.

That replug kept the same SDRTrunk process alive and the daemon successfully
cold-booted, identity-resolved, reopened, and resumed socket IQ without an API
`DeviceFailure`. It did not restore usable reception: after channels returned
to the RSPduo, both P25 control channels reported no meaningful decode for
29.5 seconds and SDRTrunk moved them away again. Transport continuity is
therefore verified, but transparent RF recovery is not.

The trace also identified a concrete ordering difference. Every successful
fresh SDRTrunk session configured the receiver near 101.1 MHz, started the
bulk endpoint, received IQ, and only then tuned into the active 800 MHz window.
The recovery path instead configured the reopened frontend directly at its
last 855 MHz channel. It produced correctly framed IQ bytes but no decodable
RF. Recovery now records the session's successful initial configuration,
replays that bootstrap state after reopen, waits for first IQ, and only then
restores the newest sample rate, RF, bandwidth, IF, gain-reduction, and LNA
state. A post-first-IQ restore failure cancels the stream and re-enters bounded
recovery instead of reporting false success.

The first physical test of that ordering retained the same SDRTrunk PID across
the unplug/replug. The daemon cold-booted and identity-resolved the receiver,
configured the recorded 101.1 MHz bootstrap state, resumed 65,536-byte IQ at
sequence 18,649, then successfully restored 855.312383 MHz, 10 MSPS, 8 MHz
bandwidth, GR 50, and LNA state 0. No API `DeviceFailure`, logical tuner
removal, callback-queue drop, or application restart occurred.

The SDRTrunk build under test runs its standard P25 control decode watchdog
every five seconds and rebalances a processing channel after 25 seconds with
no meaningful P25 control/trunking event. The same RSP-backed channel
processors remained allocated after replug and the watchdog did not fire for
more than two minutes. In the immediately preceding direct-at-855-MHz recovery
test it fired for both RSP-backed control channels after 29.5 seconds. This is
evidence for one same-process physical transport-and-decode recovery cycle. It
does not establish repeated-cycle reliability or long-duration stability.

A second consecutive physical cycle retained the same SDRTrunk and daemon
PIDs. IQ resumed at sequence 153,650, bootstrap replay restored 855.312313 MHz
with GR 50/LNA state 0, no failure or reallocation event appeared, and the
five-second watchdog again remained quiet beyond its 25-second stall limit.
Two cycles are useful regression evidence, not a soak or broad reliability
claim.

A third consecutive physical cycle again retained both PIDs, resumed IQ at
sequence 304,405, restored 855.312333 MHz with GR 50/LNA state 0, and passed
the same decoder-watchdog interval without failure or reallocation. All three
cycles cold-booted the RSPduo firmware and returned to meaningful P25 activity
without restarting SDRTrunk or `openrspd`. Extended cycling and soak time are
still required.

## SoapySDRPlay3 compatibility (2026-07-13)

Upstream `pothosware/SoapySDRPlay3` commit
`6cc31316b730503cee3e30906ff1975175a16400` compiled and linked against the
OpenRSP public headers and compatibility library with no missing types,
fields, constants, or symbols. `SoapySDRUtil --info` loaded that exact module
and registered its `sdrplay` factory. A read-only `--find=driver=sdrplay`
enumeration through the running OpenRSP daemon reported the physical unit as
an RSPduo in single-tuner mode with its factory serial.

The first live `SoapySDRUtil` receive test acquired that unit and produced IQ,
but it also exposed two API-compatibility defects. The upstream module requests
6 MS/s at a 1.620 MHz low IF and expects the API to shift and decimate that to
2 MS/s. OpenRSP initially passed the low-IF samples through unchanged, so the
utility reported about 4.48 MS/s for a 2 MS/s request. Adding a continuous
complex mixer and FIR decimator corrected the API semantics, but the resulting
rate was only about 1.49 MS/s. A temporary official 3.15.1 reference run on the
same receiver established the comparison: 23,999,472 zero-IF samples and
7,967,316 low-IF output samples in separate four-second runs, confirming that
the expected rates are approximately 6 MS/s and 2 MS/s respectively.

The remaining error was the RSPduo's 252-word USB-format register. The captured
`0x05` value remains in use for previously tested rates, while the hardware-
verified 6,000,000-sample tuple uses `0x94`. With that change, the same upstream
Soapy module measured 1.98459 MS/s after startup convergence for a 2.00000 MS/s
request during a bounded live run. The daemon logged first USB and socket IQ,
and repeated successful gain updates from the active AGC path. No API callback
drop, IQ-gap, device-failure, or stream-stop error was observed. This verifies
single-tuner RSPduo receive streaming at that one rate and mode; it does not
verify every advertised Soapy rate, antennas, dual-tuner operation, or the
currently unsupported bias-tee/notch/external-reference controls. SDRTrunk was
restarted afterward, rediscovered the RSPduo, restored its assigned channel, and
loaded JMBE normally.

Ubuntu CI now checks out that pinned upstream commit, builds it against the
current OpenRSP artifacts, loads the module, and requires the `sdrplay`
factory. This verifies source/ABI integration but not Soapy stream behavior or
future unpinned upstream changes. Hardware streaming remains a separate local
gate because hosted CI has no receiver.

Commit `c1d1bfc` then passed hosted macOS and Ubuntu run `29232008268`; the
Ubuntu job also built and loaded the pinned upstream Soapy module. The deployed
Release daemon and compatibility library matched the tested artifacts at
SHA-256 `6dd862abcace409026f3fbfd69583351f0845165af9a113e3f0305211d4de444`
and `d72141528e9805132725f1b069f81a4a667e6822912147c3ca9efef07de3482e`.
A fresh Soapy module linked with an rpath to `/Library/OpenRSP/0.1/lib` then
repeated the physical receive test against the installed service and converged
to 1.98897 MS/s. The installed daemon logged 6 MS/s, 1.620 MHz IF, first IQ,
and continuing AGC gain updates without a callback drop, IQ gap, device
failure, or stream-stop error. SDRTrunk subsequently rediscovered the RSPduo
and restored its assigned channel and JMBE on the same installed artifacts.

The installed artifacts then passed an independent Soapy control probe at
101.1 MHz. The probe disabled AGC through `setGainMode`, applied IFGR/RFGR
states 59/9, 40/5, and 20/0, required exact Soapy readback after every update,
and consumed one million physical IQ samples at each state. AC RMS increased
monotonically from 14.0094 to 24.0909 to 403.812, while the daemon recorded
each corresponding `GAIN` update with the requested GR and LNA values. The
probe restored AGC, consumed another 500,000 samples, and exited successfully.
This proves both manual Soapy gain controls affected the physical receive path
without interrupting streaming; it is not an absolute gain-calibration
measurement. The reusable `openrsp-soapy-control-probe` is built automatically
when SoapySDR development files are present. Debug, Release, and ASan/UBSan
builds, all 13 automated tests, and compilation of that probe passed afterward.
SDRTrunk then rediscovered the same RSPduo on the still-running daemon.

A subsequent full-rate probe exposed a native packet-decoder error at exactly
2.048 MS/s and then at 3 MS/s. Register 7 value `0x05` produces 336 IQ pairs
per USB packet on the tested RSPduo, while the inherited Mirics thresholds had
selected the 252-pair converter and discarded exactly 25 percent of the
samples. OpenRSP now selects the RSPduo register word and packet converter as
one rate tuple in the core `libmirisdr` backend. With SDRTrunk stopped to remove
host-load interference, the physical receiver passed all 19 rates advertised
by SoapySDRPlay3: 62.5, 96, 125, 192, 250, 384, 500, and 768 kS/s, and 1,
2, 2.048, 3, 4, 5, 6, 7, 8, 9, and 10 MS/s. Measured wall-clock error ranged
from 0.0026 percent at 6 MS/s to 0.353 percent at 2.048 MS/s. The probe restored
2 MS/s and AGC after the sweep. This verifies single-tuner sample delivery rate
on one RSPduo; it does not establish RF bandwidth flatness or dual-tuner support.

The same core mappings then passed an independent verifier through OpenRSP's
standalone SDRplay-compatible API, without loading SoapySDR. One continuous
stream was updated through 2, 2.048, 3, 4, 5, 6, 7, 8, 9, and 10 MS/s. Native
callback throughput stayed within 0.0071 percent at 9 MS/s and 0.5204 percent
at 2.048 MS/s of wall-clock rate. The tool restored 2 MS/s and AGC before
uninitializing, and SDRTrunk subsequently restarted with its saved RSPduo
disabled state, channel configuration, and JMBE library intact.

The API's upper boundary of 10.66 MS/s was then measured separately at
10.6466 MS/s, a 0.1253 percent error, with the same successful restoration and
SDRTrunk restart. This covers the accepted API sample-rate endpoints as well as
the rate set advertised by the current Soapy adapter.

## Single-tuner complex width correction (2026-07-13)

The earlier analytic-IQ conclusion was wrong. A 10 MS/s SDRTrunk display made
the failure visible: OpenRSP populated only the positive-frequency half of the
complex spectrum. Callback throughput still measured 10 MS/s, so the previous
rate tests did not establish 10 MHz spectral coverage. The earlier 61.51 dB
image-rejection measurement was evidence of that one-sided Hilbert
output, not evidence that the complete complex stream was correct.

Clean-room control tracing at 10 MS/s and 8 MHz bandwidth found that API 3.15.1
programs ADC register 3 as `0x01ca07` and packet-format register 7 as
`0x000c94`. OpenRSP used `0x01da07`: the inherited MSi2500 filter-mode nibble
was one bit higher at every sample-rate range. The same discrepancy was
observed at 2.048 MS/s (`0x01081f` official versus `0x01181f` old OpenRSP).
OpenRSP now uses the observed `0`, `4`, `8`, and `c` filter-mode nibbles and
passes the hardware's complex I/Q pair directly. The official service exports
an `IQSumDiff` helper, but the installed reference binary has no call site for
that helper in its receive pipeline. The single-lane Hilbert path and its
synthetic image-rejection test were removed.

The public-API rate probe now also reports I/Q RMS, I/Q correlation, and
aggregate power at seven symmetric FFT bins in each spectral half; it retains
no samples. At 10 MS/s the official API measured 40.763 I RMS, 37.512 Q RMS,
and a negative-to-positive aggregate ratio of +0.440 dB. The corrected OpenRSP
path measured both halves within 0.351 dB while the installed Release daemon
delivered 10,015,003 samples/s (0.1500 percent wall-clock error). These field
measurements establish that the missing spectral half is restored. Rates at or
above 8 MS/s now fail the probe if the two aggregates differ by more than 20 dB
or if the bounded spectrum sample is incomplete. This does not establish
laboratory-grade passband flatness at the outer edges of the nominal 8 MHz RF
filter.

A five-minute post-conversion native API soak ran with the normal SDRTrunk
workload active and alternated 2.048/3.072 MS/s, RF frequency, and gain 20 times
over the second half. It delivered 766,050,304 samples with 0.2539 percent
wall-clock error, acknowledged all 20 changes in each category, reported the
single expected stream reset, and uninitialized cleanly. The selected gain
clipped the known strong signal, so this is transport and control-stability
evidence rather than a linearity measurement.

All three non-zero IF paths were then exercised on hardware with the host radio
application stopped: 450 kHz at 6 MS/s delivered 24,936,448 samples in four
seconds; 1.620 MHz at 6 MS/s delivered 8,317,611 samples after the required
divide-by-three conversion; and 2.048 MHz at 8 MS/s delivered 8,335,360 samples
after divide-by-four conversion. Each run acknowledged RF, sample-rate, and
gain changes, reported one initial reset, and uninitialized cleanly. The native
verifier now compares the 1.620 and 2.048 MHz paths with their actual 2 MS/s
API callback rate rather than the pre-decimation ADC rate.

Ten complete post-conversion Open/select/Init/update/Uninit/release/Close cycles
also passed without reconnecting the receiver. Each streamed before and after
the update acknowledgement with zero discontinuities and device-failure
events. Three additional stream-owner processes were killed with `SIGKILL`;
after each forced exit, a new full lifecycle acquired and streamed from the
same receiver without a replug.

A 20-second native API stress run then applied 100 consecutive combined sample-
rate, RF, and gain updates on one active stream while the normal SDRTrunk/RTL
workload remained running. The driver delivered 40,828,928 samples in 32,396
callbacks, acknowledged all 100 changes for each field, reported one expected
initial stream-reset indication, and uninitialized successfully. The
first run exposed a verifier bug: it expected only one `fsChanged` callback no
matter how many updates were requested. The verifier now requires one callback
per requested update, and the identical 100-update run passes.

A separate five-minute run under the live SDRTrunk/RTL workload delivered
607,666,176 samples in 482,157 callbacks. At its midpoint it applied ten
combined 2.048 MS/s, RF, and gain updates; all ten acknowledgements for each
field arrived, streaming continued for the remaining half, and uninitialization
succeeded. The OpenRSP daemon and SDRTrunk retained their original process IDs
throughout. This is useful bounded evidence, but it is not a long-duration soak.
The native verifier now treats any reset after the initial stream indication or
more than five percent error from the configured wall-clock sample total as a
failure. An eight-second, ten-update hardware run passed those stricter gates
with one reset and 1.9953 percent sample-count error.

The standalone lifecycle probe completed 50 consecutive full API Open, device
discovery, selection, Init, streaming, live sample-rate/RF/gain update, Uninit,
release, and Close cycles without reconnecting the RSPduo. Every cycle reported
one initial reset, zero sample-number discontinuities, exactly one acknowledgement
for each updated field, and zero device failures. Per-cycle delivery ranged from
3,358,720 to 3,457,024 samples. Daemon RSS was 12,128 KB before and after the
run, and both the daemon and SDRTrunk retained their process IDs. A subsequent
ten-cycle run additionally required separate delivery on both sides of every
update; pre-update counts ranged from 1,490,944 to 1,540,096 samples and post-
update counts from 1,835,008 to 1,916,928 samples. The mock API regression now
also repeats complete Open/Close lifecycles so hosted CI covers the teardown
contract without hardware.

The final verifier also starts a separate counter only when the `fsChanged`
acknowledgement reaches the application callback. Three additional cycles each
delivered 1,867,776 samples at or after that acknowledgement, ruling out a pass
based only on IQ already queued before the update completed.

Crash cleanup was then tested separately. The lifecycle probe forks a disposable
API client, waits until that child has received physical IQ, kills it with
`SIGKILL` without calling Uninit or Close, waits for the daemon to observe the
dead socket, and requires a new complete lifecycle on the same receiver. Ten
consecutive crash/recovery cycles passed. Every replacement stream had one
initial reset, zero discontinuities, exact rate/RF/gain acknowledgements, no
device-failure event, and at least 1,867,776 samples delivered at or after the
update acknowledgement. The daemon retained its PID and returned to 12,128 KB
RSS; SDRTrunk also remained running. This proves bounded recovery from abrupt
client death, not recovery from a daemon crash or host power loss.

Daemon death exposed a different failure. Killing `openrspd` during an active
bulk stream caused the launchd replacement's submissions to fail with
`LIBUSB_ERROR_PIPE`; the old API client's expected `DeviceFailure` was followed
by a new client's `Init` timing out after two seconds. OpenRSP now clears the
halt on the claimed RSPduo bulk endpoint before submitting transfers, and a new
API session retains its lease for up to 30 seconds while the replacement daemon
performs bounded stop, clear, and reopen attempts. A hardware-free regression
delays first IQ for 2.2 seconds so the former timeout would fail in CI.

Three consecutive physical tests then killed the daemon while a disposable API
client was receiving IQ. Launchd supplied a different daemon PID each time; the
old client failed as expected, and a new complete lifecycle acquired and
streamed from the same RSPduo without a USB reset, physical replug, or host
reboot. Each replacement lifecycle reported one initial reset, zero sample
discontinuities, exact sample-rate/RF/gain acknowledgements, and zero device
failures. This is recovery by a replacement daemon and new API session, not
transparent continuation of the client connection that died with the daemon.
The compatibility regression also kills its mock daemon, waits for
`DeviceFailure`, tears down the failed session, starts a replacement daemon,
and requires the same application process to Open, select, Init, receive IQ,
Uninit, release, and Close again. This covers in-process API recreation in CI;
the three physical daemon-kill tests used a fresh verifier process.

The API backend now also has a hardware-free recovery-silence regression test.
Its mock daemon sends IQ, remains silent across three socket receive deadlines,
then resumes IQ on the same connection. The backend must deliver both frames
without invoking its failure callback. This protects the application layer,
where the original low-level timeout fixture alone was insufficient.

## API update-reason audit (2026-07-12)

The public API 3.15 update-reason surface was compared with SDRplay's published
API specification and SDRTrunk's generated 3.15 bindings. The compatibility
header now exposes all 32 primary reason bits and all seven extension bits.
Previously, reasons outside sample rate, RF, bandwidth, IF, gain, and AGC were
silently accepted as no-ops. That was incorrect application behavior.

The compatibility layer now validates values and distinguishes wrong-model,
invalid-mode, invalid-parameter, and out-of-range requests. Required RSPduo
tuner-A setup calls for AUTO LO, DC/IQ configuration, reset flags, and overload
acknowledgement remain accepted. PPM correction is applied to the synthesizer
command and acknowledged using the API's `fsChanged` callback convention.
Software decimation uses a stateful windowed-sinc FIR for x2, x4, x8, x16, and
x32. Automated transport tests verify exact output accounting for every
supported factor and rejection of unsupported factors. The x2 fixture runs
eight continuous pass-band frames at 0.0625 cycles/input sample followed by
eight stop-band frames at 0.375 cycles/input sample; the settled pass-band peak
must remain above 10,000 for a 12,000-count input and the settled stop-band peak
must fall below 100. RF bandwidth and alias rejection have not yet been
measured on hardware. Unsupported hardware switches fail explicitly instead of
pretending to work.

Debug, Release, and ASan/UBSan builds each pass all five tests. The sanitizer
run uses `ASAN_OPTIONS=detect_leaks=0` because Apple AddressSanitizer does not
support leak detection on this platform.

The compatibility test also terminates its mock daemon during an active stream.
The socket reader reports one API `DeviceFailure` event, and `Uninit` completes
without terminating the host process. The POSIX client suppresses `SIGPIPE` on
both Linux (`MSG_NOSIGNAL`) and macOS (`SO_NOSIGPIPE`) so a vanished daemon is
returned as a transport error instead of crashing the application.

IQ frame sequence numbers now reach the compatibility layer instead of being
discarded. An injected forward gap sets the stream callback reset flag and
advances `firstSampleNum` by the estimated missing output samples. The test
checks both values and repeats the combined gap/decimation case to catch
ordering races. A sequence restart (such as daemon recovery) sets reset without
inventing a forward sample count.

Software AGC now keeps its applied mode and set point in atomic shadow state;
the worker no longer races application writes to the public parameter block.
After a successful gain update it emits an API `GainChange` event containing
baseband reduction, LNA reduction, and calculated current gain. The mock test
enables AGC from a disabled state, supplies IQ, and requires a matching event;
reloading the applied mode after the worker sleep prevents the first new peak
from being consumed under a stale disabled mode.

Before exercising updates and failure injection, the mock compatibility test
now performs ten consecutive `Init`/first-IQ/`Uninit` cycles while retaining the
same selected device and daemon. Each cycle requires a new initial reset
callback and clean reader-thread shutdown. This covers repeated API lifecycle
behavior without pretending it is a substitute for the still-needed long
hardware cycle test.

`LockDeviceApi` now uses an owner-aware recursive process mutex instead of
returning false success. A competing test thread remains blocked until the
owning thread releases both recursive levels, and `Close` refuses to invalidate
the API while its caller holds that lock. `Open`, `Close`, and a newly acquiring
thread serialize on the same mutex and recheck open state after acquisition.

Injected daemon rejection now verifies that `Update` does not flatten every
failure into generic `HwError`. RF, gain, and sample-rate/PPM requests map to
their API-specific update errors, and the thread-safe `GetLastError` record
identifies `sdrplay_api_Update`, the rejected reason mask, and backend result.

Update validation now rejects non-finite/out-of-range sample rate and RF,
unknown bandwidth and IF values, invalid low-IF/bandwidth combinations, gain or
band-specific LNA states outside their tables, malformed boolean controls, and
AGC modes/set points outside the public API ranges before any daemon command is
sent. The documented RSPduo RF coverage is 1 kHz--2 GHz, but differential API
3.15.1 probes show that the reference implementation accepts 0--999 Hz too;
OpenRSP matches that observable boundary and its measured gain calibration.
2--10.66 MSPS sample-frequency range.

API events now use a bounded dispatcher queue instead of invoking application
event callbacks on the daemon socket reader or AGC worker. This prevents an
event callback that calls `Update` from blocking the same reader needed to
finish that update. The overload fixture injects a saturated IQ frame, receives
`Overload_Detected`, acknowledges it by calling `Update` from inside the event
callback, then requires a hysteretic `Overload_Corrected` event from the next
normal frame. Gain and device-failure events use the same dispatcher.

## Typed last-error behavior (2026-07-13)

The official 3.15.1 library was behavior-probed both without its service and in
an initialized RSPduo session. `GetLastErrorByType` returns null for types -1
and 4 and leaves the caller's timestamp unchanged. Valid empty categories 0--3
return null and zero the timestamp in an initialized session. Device-side RF or
gain validation failures populate category 3 with a nonzero microsecond
timestamp, and `GetLastError` returns that newest record.

OpenRSP previously retained only one category-0 record. The compatibility layer
now keeps independent histories for DLL, DLL-device, service, and
service-device categories, returns null for an empty category, and leaves
unsupported-type timestamps untouched. Validation, synchronized-update, and
daemon rejection failures tied to the selected receiver populate the
service-device history. The fixture covers -1 through 4, empty timestamps,
latest-record selection, and injected validation/backend failures.
With the service absent, the reference library fails `Open` and retains a
category-0 DLL record; OpenRSP now verifies daemon reachability at `Open` and
does the same instead of exposing a lazily broken API session.

The API compatibility callback path now allocates its deinterleaving buffers
once per `Init` session at the protocol's bounded maximum frame size and frees
them only after the backend reader has stopped. The mock fixture requires the
same I/Q buffer addresses across successive non-empty callbacks. This removes
two allocations and two frees from every steady-state IQ frame while retaining
the existing ten-cycle `Init`/stream/`Uninit` allocation-and-cleanup exercise.

Every POSIX client socket now has a five-second send and receive deadline, so
synchronous acquire/configure/start calls and a stalled streaming reader cannot
wait forever. A private-socket fixture uses a 100 ms test deadline and requires
a silent peer to time out, `shutdown` to wake and join a blocked reader in under
one second, and a truncated response payload to fail instead of being accepted.

The inherited libmirisdr asynchronous cancellation state was a plain integer
read and written by the daemon and USB event threads, which is a C data race.
It is now atomic. A hardware-free fixture requires a running-to-canceling
transition to be visible across threads and requires synchronous cancellation
to return after the event-thread side marks the state inactive.

The first live deployment exposed command-response starvation under continuous
10 MS/s IQ output. The daemon had correctly applied the initial gain command,
but its small response competed with the stream thread for the same socket
write mutex and missed the API's five-second timeout. SDRTrunk therefore saw a
hardware initialization error despite active IQ. Control responses now raise a
writer-priority gate before taking that mutex, and the IQ callback briefly
waits behind the pending response. A second live launch completed discovery,
initial gain, repeated RF updates, and channel startup without the false
hardware error or update timeout.

The same launch also confirmed that the inherited libmirisdr USB-path fallback
is not an acceptable receiver identity: bypassing the normal launch environment
changed the apparent serial from the saved receiver serial to its bus path.
Device discovery now reads the actual USB serial descriptor when available and
uses the physical path only for hardware that has no readable serial.

## RSPduo tuner-B single-tuner support (2026-07-13)

Matched tuner-A and tuner-B captures were made with the official 3.15.1 API at
the same RF, sample rate, bandwidth, IF, gain reduction, and LNA state. Both
sessions delivered 8,258,880 samples in four seconds, acknowledged RF, sample
rate, and gain changes, and uninitialized cleanly. Their PLL, ADC, packet
format, timing, streaming, and register transfers were identical. Only the
frontend GPIO routing, LNA transitions, and shutdown routing differed. OpenRSP
uses those behavioral differences without distributing official binaries,
headers, firmware, or raw captures.

Protocol version 3 carries tuner A/B selection in the radio configuration. The
API compatibility layer accepts tuner B only for an RSPduo, uses
`rxChannelB`, and preserves the official single-tuner callback contract by
delivering B through `StreamACbFn`. The daemon opens the selected frontend, and
the core applies B-specific initialization, routing, gain-state, and shutdown
GPIO sequences. Hot updates cannot silently change tuners; changing the tuner
requires a new device session.

An initial implementation incorrectly inferred that B occupied a separate USB
lane, and a later implementation incorrectly treated the single-tuner words as
real lanes requiring analytic conversion. Those measurements were made while
the ADC was in the wrong filter mode and do not identify the final complex I/Q
mapping. Tuner A and B single mode now use the same verified complex packet
format; their differences remain confined to frontend routing and gain state.

With SDRTrunk stopped to remove known host-load interference, tuner B at
853.8625 MHz and 10 MS/s delivered 80,625,664 samples over 8.062949 seconds,
for 0.0047 percent wall-clock error. Eight alternating RF and gain changes were
all acknowledged, the stream reported one initial reset and no additional
reset, and `Uninit` succeeded. A tuner-A regression delivered 10,321,920
samples over 5.038295 seconds at 2.048 MS/s, for 0.0338 percent error, while
acknowledging five RF/gain updates and cleaning up normally.

The tuner-B hardware gain sweep accepted LNA states 0 through 9 and produced
large measured level changes: at GR 55, mean-square power fell from 21,529.2 at
LNA 0 to 107.9 at LNA 3 and 2.8 at LNA 5. IF gain-reduction updates at LNA 0
and LNA 5 accepted GR 20, 30, 40, 50, and 59. Software AGC moved GR from 20 to
31, then recovered a forced GR 59 to 30 before disable held the final value.
These measurements verify tuner B independently in single-tuner mode. They do
not verify dual-tuner operation, B spectrum orientation against a known
connected carrier, or other RSPduo frequency-band GPIO tables.

A later offset-tuned FM check used 101.1 MHz as a known connected signal at
10 MS/s. Equal-looking energy on the opposite side of the display was not
sufficient evidence of an image: the official API's demodulated positive and
negative channels had correlation `0.000097`, showing that they were separate
stations. OpenRSP measured `0.000760` under the same center-frequency,
bandwidth, gain, LNA, tuner-B, and demodulation procedure. Its native callback
rate error was `0.0280%`, and the installed Release daemon's output had both I
and Q populated. This verifies that tuner B receives the FM station without
duplicating its audio at the image frequency; it does not measure calibrated
laboratory image rejection.

## RSPduo live swap and dual-tuner mode (2026-07-13)

The official API 3.15.1 was exercised with a purpose-built public-API client,
then observed at its process and USB control boundary. No official source,
private header, firmware, receiver identity, or raw capture was copied into the
repository. In initialized single-tuner mode, swapping A to B and back returned
success, changed the exposed channel pointer to the active tuner, preserved the
channel settings, and produced a reset on each transition. All IQ continued
through Stream A; Stream B was never called. A gain update addressed to tuner B
after the first swap succeeded.

The official dual-mode reference accepted a shared 6 MHz ADC rate with 1.620
MHz low IF and separate A/B RF and gain updates. Both stream callbacks received
6,451,200 samples during the same bounded run, each reported one initial reset,
and each independently acknowledged its RF and gain update. Cleanup succeeded.
The observed startup, routing, update, and shutdown transfers established that
dual mode uses both ADC lanes and a distinct B frontend route; they did not
support the incorrect assumption that dual mode can deliver 10 MHz per tuner.

OpenRSP daemon protocol version 4 adds an explicit both-tuner selection, a dual
configuration containing shared clock plus separate channel settings, distinct
A/B IQ event types, and a single-mode swap request. The direct backend decodes
the two ADC lanes into separate fixed buffers, the daemon preserves their event
identity, and the compatibility library maintains separate callback queues,
sample numbering, reset state, low-IF DSP, decimation, gain/AGC state, overload
acknowledgement state, and pending update acknowledgements for A and B.

With SDRTrunk stopped for an exclusive hardware window, OpenRSP dual mode used
an 8 MHz ADC rate and 2.048 MHz low IF. Three consecutive four-second sessions
delivered between 8,031,744 and 8,058,624 samples on each tuner, or approximately
2.01 MS/s per tuner. Every session had one reset per stream, separate RF/gain
acknowledgements, nonzero signal power on both streams, and successful
`Uninit`, release, and close. A release-build dual session immediately followed
by a single-mode A-to-B-to-A swap also completed without a USB reset or replug;
the swap produced three Stream A resets and zero Stream B callbacks.

Abruptly terminating the test daemon caused the initialized client to receive a
`DeviceFailure` event. A replacement daemon then reopened the same receiver and
completed fresh dual and swap sessions without a physical replug. A sanitizer
build completed dual initialization, both-stream delivery, and cleanup without
an AddressSanitizer or UndefinedBehaviorSanitizer report. It could not sustain
the full 8 MHz control-plus-IQ workload on this host: update responses timed out
while the daemon was still applying them, so full-rate performance claims are
based on the Debug and Release hardware runs, not the instrumented build.

The pinned upstream SoapySDRPlay3 module was rebuilt against this protocol-v4
compatibility library after the mode changes. It linked successfully, and
`SoapySDRUtil --info` loaded the resulting module and registered the `sdrplay`
factory. That is a source/ABI integration result; dual-channel Soapy behavior
was not claimed because the upstream adapter's application-level mode choices
were not exercised in this hardware window.

## RSPduo bias, notch, and reference controls (2026-07-13)

A clean-room public-API probe exercised bias-T, RF notch, DAB notch, and
external-reference output in initialized single-tuner sessions against API
3.15.1. Tuner-A bias-T enable and disable both returned `InvalidParam`; tuner-B
bias-T and the other controls on A and B returned success while IQ continued.
The successful bounded sessions delivered approximately 3.0 million samples,
reported one initial reset, and uninitialized, released, and closed cleanly.

The official process boundary was observed at `libusb_control_transfer` with
the proprietary daemon isolated from OpenRSP. Only request metadata and state
deltas were recorded; no raw USB capture, private header, firmware, receiver
identity, or proprietary code was retained. At the tested 853.8625 MHz,
GR 45/LNA 2 configuration, the independently observed active-low deltas were:

- tuner A RF notch: request `0x4b`, `0x12df` to `0x12cf`;
- tuner B RF notch: request `0x4b`, `0x12ff` to `0x127f`;
- tuner A DAB notch: request `0x4b`, `0x12df` to `0x129f`;
- tuner B DAB notch: request `0x4b`, `0x13de` to `0x13dc`;
- tuner B bias-T: request `0x4b`, `0x12ff` to `0x12fd`;
- external reference: request `0x4a`, A `0x12a4` to `0x1284`, B `0x123f` to `0x121f`.

Protocol version 5 adds the four persistent control fields to each channel
configuration and distinct update flags. The compatibility layer maps the
public update reasons and reproduces the tuner-A bias error. The daemon applies
and retains the state across recovery; the direct backend merges active-low
control bits with gain/LNA GPIO state and performs no steady-state allocation.
Deterministic compatibility fixtures verify A/B routing, flag mapping, combined
updates, and the invalid tuner-A bias request.

On the physical RSPduo, every supported single-tuner enable/disable update
returned success, continued streaming roughly 3.1 million samples per session,
and emitted the expected GPIO delta. An 8 MHz dual low-IF Debug run delivered
8,042,496 A samples and 8,037,120 B samples while independent RF/gain and A/B
control updates all succeeded. The Release run delivered 8,042,496 samples on
each tuner with the same successful updates and clean cleanup. Killing the
daemon during an active tuner-B bias session produced `DeviceFailure`; a fresh
protocol-v5 daemon and client session immediately streamed and toggled bias-T
without a USB reset or physical replug.

The ASan/UBSan build passed all 14 hardware-free tests and a live single-tuner
RF-notch session without a sanitizer report. Its dual control run delivered
both streams and applied the A controls, but the instrumented client did not
complete the full control/cleanup sequence before its update timeout; this is
not counted as a passing dual ASan hardware run. No voltmeter, signal source,
spectrum analyzer, or frequency counter was attached, so the evidence proves
API return parity, GPIO command parity, stream continuity, cleanup, and
recovery—not bias voltage, notch attenuation, or reference-output amplitude and
frequency.

## RSPduo tuner-1 AM controls (2026-07-13)

A public-API probe ran at 10 MHz, 2.048 MS/s, 1.536 MHz bandwidth, zero IF,
GR 45, and LNA state 2. Against API 3.15.1, AM-port and tuner-1 AM-notch enable
and disable returned success on tuner A. The same initialized calls on tuner B
returned `OutOfRange`. Every bounded reference session continued streaming,
reported one initial reset, and uninitialized, released, and closed cleanly.

The official service was isolated from OpenRSP and observed only at the
`libusb_control_transfer` argument boundary. AM-port selection reprogrammed the
tuner and changed register 0 bit 11: the independently reconstructed register
word was `0x04f610` for AM port 1 and `0x04fe10` for AM port 2 at the tested
frequency. OpenRSP stores the selection and derives the complete register word
from the active frequency instead of replaying a frequency-specific capture.
Enabling the AM notch emitted the following GPIO pulse sequence: request
`0x4b` value `0x12ff` three times, then `0x4a` values `0x00df` and `0x01ff`,
then `0x4b` values `0x00ff` and `0x01ff`. Disabling returned success without an
observable control transfer. No raw capture or receiver identity was retained.

Protocol version 6 adds persistent AM-port and AM-notch fields plus distinct
update flags. The compatibility library maps both API reasons, enforces the
observed tuner-A-only behavior, and preserves the controls through the daemon
configuration and recovery state. Deterministic fixtures verify flag mapping,
combined A updates, and tuner-B rejection.

The installed Release build was exercised on the physical RSPduo. Each tuner-A
AM-port and AM-notch enable/disable session delivered 3,112,960 samples in
2,470 callbacks with one reset and clean cleanup. Tuner-B sessions delivered
the same stream volume while both updates returned `OutOfRange`. Killing the
daemon during an active AM-notch session produced `DeviceFailure`; launchd
started a new protocol-v6 daemon, and a fresh AM-port session reopened and
streamed without a USB reset or physical replug. A subsequent dual-control run
delivered 8,069,376 A samples and 8,064,000 B samples, independently
acknowledged both RF/gain updates, and cleaned up successfully.

No AM antenna, calibrated signal source, or spectrum analyzer was connected.
These results verify API return behavior, register/GPIO command parity, stream
continuity, cleanup, and recovery, not AM-port sensitivity or notch attenuation.

## RSPduo all-band gain and LNA routing (2026-07-13)

A timestamped public-API sweep captured every valid LNA state on tuners A and B
at 10 MHz, 100 MHz, and 1.5 GHz. Together with the previously verified
853.8625 MHz sweep, these frequencies exercise all four published LNA
state-count groups:
below 60 MHz, 60--420 MHz, 420--1000 MHz, and 1--2 GHz. The official API
accepted states 0--6, 0--9, 0--9, and 0--8 respectively. All six new reference
sessions streamed between 8.9 and 11.1 million samples, reported one initial
reset, accepted every update, and uninitialized, released, and closed cleanly.

The official service was isolated from OpenRSP and observed only at the
`libusb_control_transfer` argument boundary. Timestamped update boundaries
separated initialization and cleanup traffic from each gain change. The
independently reconstructed plans establish the complete register-9 base word,
first GPIO-bank value, frontend GPIO value, and final GPIO-bank value for all
52 tuner/state combinations. They also show that tuner B has no bank-bit
transition below 60 MHz, switches banks before state 4 in the two middle
bands, and switches before state 3 above 1 GHz. No official binary, header,
firmware, receiver identity, proprietary source, or raw USB capture is stored
in the repository.

The direct backend now selects the band plan from the active RF frequency and
programs IF gain reduction separately from the physical LNA route. The daemon
no longer falls back to a generic total-gain approximation outside the
420--1000 MHz band. Gain planning is allocation-free and merges the persistent
notch and external-reference bits so a gain update cannot silently undo a
control update. A deterministic fixture checks the exact reconstructed words
for every A/B state in those groups, invalid next states, and the
active-low control-bit merge.

The installed Release build then swept every valid state at 10 MHz, 100 MHz,
and 1.5 GHz on both physical tuners. Each of the six OpenRSP sessions continued
streaming, delivered between 5.47 and 6.75 million samples, reported one reset,
accepted every update, and cleaned up normally. A dual low-IF regression
delivered 8,058,624 samples on each stream while acknowledging independent A/B
RF, gain, and control updates. A subsequent A-to-B-to-A swap delivered
8,568,832 Stream A samples, three Stream A resets, and no Stream B callbacks,
matching the single-mode callback contract. Finally, a forced daemon death
recovered through a new streaming lifecycle without a USB reset or physical
replug.

### Exact routing boundaries and optional controls (2026-07-14)

An exhaustive follow-up placed official-API probes immediately below, at, and
above every observed selector transition. The front end has ten routing ranges:
1 kHz--12 MHz, 12--30 MHz, 30--60 MHz, 60--120 MHz, 120--250 MHz,
250--300 MHz, 300--380 MHz, 380--420 MHz, 420 MHz--1 GHz, and 1--2 GHz.
The lower bound is inclusive, each internal upper bound belongs to the next
range, and exactly 2 GHz is accepted.

Independent sweeps reconstructed every valid LNA row in each range on both
tuners. Combined RF-notch, DAB-notch, and external-reference tests showed that
the active-low masks compose. On tuner B the DAB mask applies to both GPIO-4B
writes, not only the final write. Tuner-A AM port 1 below 60 MHz uses five LNA
states with distinct register-9 bases; port 2 uses seven. The AM notch clears
GPIO bit 0x0008 in subsequent gain plans. The deterministic fixture now checks
all ten exact lower boundaries, both physical selector banks, last valid and
first invalid states, the inclusive 2 GHz limit, combined control masks, and
the AM-port-specific rows.

Debug, Release, and ASan/UBSan builds each passed all 13 hardware-free tests.
No calibrated RF source or spectrum analyzer was attached for the new band
sweeps, so this evidence proves accepted state coverage, reconstructed command
parity, stream continuity, cleanup, and recovery--not the absolute gain or
noise figure of each LNA state.

## Initialized RSPduo mode transitions (2026-07-13)

The public API 3.15 guide documents `sdrplay_api_SwapRspDuoMode()` as an
initialized master-application operation that updates the current device
enumeration and device-parameter pointer. It also limits
`sdrplay_api_SwapRspDuoDualTunerModeSampleRate()` to 6/8 MHz master/slave
operation. In a directly owned dual session, the official 3.15.1 library
returned `InvalidParam` for both requested sample-rate changes and left the
6 MHz rate and both streams unchanged.

Clean-room reference probes then started valid direct single mode at 2.048 MHz,
zero IF and valid direct dual mode at a shared 6 MHz ADC rate with 1.620 MHz low
IF. In both single-to-dual and dual-to-single directions, the official function
returned success and updated the mode, tuner, sample rate, A/B pointers, and
device-parameter pointer. Both IQ streams then stopped. A second `Init` returned
`AlreadyInitialised`, and cleanup terminated the probe with signal 10 instead
of returning from `Uninit`. Three single-to-dual observations and one
dual-to-single observation reproduced the stopped-stream behavior. This is
recorded as an API 3.15.1 direct-mode defect; OpenRSP does not intentionally
reproduce the dead stream or cleanup signal.

Protocol version 7 adds an explicit whole-mode swap request and a separate
resume command. The daemon reconfigures the direct hardware while IQ is paused.
The compatibility layer then changes its mode and parameter pointers,
reconfigures per-tuner low-IF DSP and decimation, clears queued old-mode frames,
resets A/B sample timelines and gain/AGC state, and only then resumes the
daemon. A deterministic mock-daemon fixture verifies single A to dual to single
B, separate A/B callbacks and resets, an independent tuner-B frequency update,
the direct-dual sample-rate error, pointer changes, and Stream-A routing after
returning to single B.

The installed Release build completed separate live single-to-dual and
dual-to-single probes on the physical RSPduo without a USB reset or replug. The
single-to-dual run delivered 9,477,888 A samples and 6,381,312 B samples, added
one reset to each stream, and cleaned up normally. The dual-to-single run
delivered 9,547,264 A samples, stopped B at 3,042,816 samples, added one A reset,
and cleaned up normally. A combined single-to-dual-to-single run reported
success for both transitions, 8,093,440 A samples, 2,768,640 B samples, three A
resets, one B reset, and complete cleanup.

Independent dual RF, gain, and hardware-control updates subsequently delivered
8,053,248 A samples and 8,047,872 B samples with separate acknowledgements. A
single A-to-B-to-A regression delivered 8,552,448 samples through Stream A,
three resets, no Stream B callbacks, and delivered tuner B's gain-change
acknowledgement through Stream A. A separate single-B 6 MHz/1.620 MHz low-IF
run delivered 8,039,083 samples over 4.016 seconds (0.0872% rate error),
acknowledged its RF/gain update, and cleaned up normally. Killing the daemon
during a disposable stream produced a replacement daemon PID; three fresh
lifecycle cycles then streamed, acknowledged rate/RF/gain changes, and cleaned
up with one reset and
zero discontinuities per cycle, without a physical replug. Debug, Release, and
ASan/UBSan builds each passed all 14 hardware-free tests after the transition
work. The pinned upstream SoapySDRPlay3 module was rebuilt against the installed
library after the protocol-v7 change; `SoapySDRUtil --info` loaded the module
and registered the `sdrplay` factory.

## SoapySDR direct-dual discovery and streaming (2026-07-13)

An isolated public-API reference probe found one idle physical RSPduo record.
API 3.15.1 reported tuner mask `Both` (`3`) and mode capability mask `7`:
single tuner, direct dual tuner, and master. Selecting direct dual at 6 MHz and
calling `GetDeviceParams` before modifying any settings returned a 6 MHz device
rate, 1.620 MHz IF on both channels, 0.200 MHz default bandwidth on both
channels, and disabled x1 decimation. No receiver identity was retained.

OpenRSP discovery reports tuner mask `Both` and capability mask `7`, containing
single, direct-dual, and master operation. Dual selection installs the observed 6
or 8 MHz shared clock and matching 1.620 or 2.048 MHz IF defaults on both
channels before the application receives its parameter pointers. The
hardware-free compatibility fixture verifies the capability mask, rejects a
dual selection without a valid shared clock, and verifies the 6 MHz dual
defaults.

After rebuilding the current upstream SoapySDRPlay3 module against the installed
Release library, `SoapySDRUtil --find` exposed separate `mode=ST` and `mode=DT`
choices for the same physical receiver. The extended Soapy verifier selected
`mode=DT`, observed two RX channels, opened separate channel 0 and channel 1
streams, and alternated reads for three seconds. Each stream delivered
5,935,104 samples (1.976 MS/s measured), with nonzero RMS on both, then closed
and released cleanly. This verifies Soapy discovery and concurrent A/B stream
routing. It does not yet verify independent per-channel frequency/gain control
through the upstream adapter's Soapy control surface.

## Dual `Both` updates and independent Soapy controls (2026-07-13)

A follow-up public-API reference probe tested the ambiguous tuner target used
by SoapySDRPlay3. In initialized direct-dual mode, the probe changed only A's
public RF/gain fields and called `sdrplay_api_Update` with tuner `Both`, then
changed only B's fields and repeated the call. API 3.15.1 returned success both
times. Each call reported `rfChanged` and `grChanged` through both Stream A and
Stream B. The observable contract therefore treats `Both` as a two-tuner
update; it does not mean “choose whichever block changed.” Both streams delivered
5,805,072 samples during the bounded reference run and cleanup succeeded.

OpenRSP now accepts the same `Both` target in direct-dual mode. It validates A
and B before sending either command, then applies each tuner configuration and
routes the corresponding update acknowledgement to its stream. A deterministic
fixture first gives B an invalid frequency and proves the combined call fails
before either update, then installs valid, distinct A/B RF and LNA settings and
requires RF/gain acknowledgements on both callbacks. The OpenRSP hardware probe
repeated the two `Both` calls successfully; both callbacks reported both update
flags and each delivered 4,064,256 samples before clean shutdown.

Source inspection of pinned upstream SoapySDRPlay3 commit
`6cc31316b730503cee3e30906ff1975175a16400` found a separate application bug:
the adapter stores one `chParams` pointer, selects A for dual mode, ignores the
Soapy channel argument in frequency/gain methods, and passes tuner `Both` to
the API. The compatibility library cannot infer the discarded channel number.
The included MIT-licensed patch selects `rxChannelA`/tuner A for Soapy channel
0 and `rxChannelB`/tuner B for channel 1. CI applies this patch to that exact
upstream revision before compiling it.

With the patched adapter and installed Release library, the hardware verifier
set channel 0 to 853.7125 MHz, IFGR 35, RFGR 1 and channel 1 to 853.8625 MHz,
IFGR 55, RFGR 5 after both streams were active. All six getters retained their
independent values. The two streams then each delivered 5,935,104 samples in
the same three-second interval (1.978 MS/s measured), with nonzero RMS, and
closed cleanly. This verifies the complete Soapy-to-API-to-daemon live control
path for independent A/B frequency and gain settings; it does not imply that
unpatched upstream SoapySDRPlay3 has corrected its channel-selection bug.

## Master/slave and final public-API differential audit (2026-07-16)

Protocol version 9 gives master and slave clients separate ownership records
over one RSPduo direct-dual hardware session. Tuner A is the master lane, tuner
B is the discoverable slave lane, and the daemon routes stream and lifecycle
events to the owning sockets. Live 6 MHz and 8 MHz sessions attached and
initialized the slave, streamed both tuners, delivered attach/init and
uninit/detach events, and cleaned up. Reciprocal RF captures moved a strong
101.1 MHz broadcast signature from B to A when the requested frequencies were
swapped; the signature did not remain in both lanes. This disproves mirrored
A/B RF routing on the tested unit. Three consecutive 6 MHz master sessions also
completed without a USB disappearance after stop-before-cancel teardown was
restored.

The 20 documented API 3.15 exports are present. The vendor library has three
additional undocumented symbols (`GetInternalDeviceParams`, `InternalUpdate`,
and `SelectDeviceCB`) whose opaque parameter types and private update enum are
absent from the installed public headers. OpenRSP deliberately does not export
guessed signatures for those private hooks. Official and OpenRSP default dumps
otherwise match exactly for single A, single B, direct dual at 6 MHz, and
master at 6 MHz.

A final initialized differential probe established the observable RF edge
semantics. API 3.15.1 accepts 0, 1, 500, 999, 1000, and 2,000,000,000 Hz; it
rejects negative, over-2-GHz, infinite, and NaN requests. The 50-ohm gain
outputs vary linearly from the measured 0 Hz knot through 999 Hz, then use the
previously measured nearest-kHz behavior. Tuner-A AM Port 1 has its own measured
0 Hz knot. The current OpenRSP library and daemon matched those return codes and
gain values to floating-point tolerance on the physical RSPduo while streaming,
then uninitialized, released, and closed normally. The same probe confirmed
independent typed last-error histories and category-3 device validation errors.
