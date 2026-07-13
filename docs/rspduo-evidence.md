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
a null device handle before selection, while heartbeat disabling fails unless
the calling thread holds the device API lock and no device has been selected.
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
successful SAFE-T, Wabash, Somerset, and Howard County upload counts. The only
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
restarted afterward, rediscovered the RSPduo, restored the Wabash channel, and
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
and restored the Wabash channel and JMBE on the same installed artifacts.

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

## Analytic IQ and spectrum orientation (2026-07-13)

A known 853.8625 MHz signal was captured with the receiver centered at
853.7125 MHz. The original unpacker placed the signal at both +149.84 and
-149.84 kHz with -0.044 dB image rejection. Inspection and measurement showed
that the RSPduo single-tuner USB words are two real ADC lanes rather than one
complex pair; tuner A was routed to the second lane. Calling those lanes I and
Q was wrong even though a real-signal decoder could still process the result.

The core direct backend now converts tuner A's real lane to analytic IQ with a
stateful 63-tap Hilbert FIR. Its antisymmetric tap pairs use 16 multiplies per
sample and a duplicated ring buffer, preserving filter quality without modulo
operations in the hot loop. A synthetic split-buffer regression requires more
than 30 dB rejection of the negative-frequency image and proves filter state
continues across USB callback boundaries.

The final physical capture placed the signal peak at +149,906.25 Hz and its
occupied-spectrum centroid at +149,834.57 Hz, 165.43 Hz below the expected
offset. Integrated power in the positive 25 kHz window exceeded its negative
image by 61.51 dB. This proves spectrum orientation and useful image rejection
for that 2.048 MS/s tuple. The frequency comparison uses the locally identified
signal, not a traceable laboratory frequency standard, so it is field evidence
rather than an absolute oscillator calibration.

The [NWS county-coverage table](https://www.weather.gov/nwr/county_coverage?State=IN)
currently lists Marion WXM-98 on 162.450 MHz as normal and covering all of
Wabash County. It was also tested at a +50 kHz offset with 200 kHz RF bandwidth
and the highest verified VHF gain state. The corrected analytic path rejected
the negative image by 32.62 dB, but the expected channel was only 0.30 dB above
adjacent spectrum. Reception of WXM-98 on the connected antenna is therefore
inconclusive and is not used as the orientation proof. The upstream Soapy
adapter did pass its physical IFGR/RFGR and AGC-restoration test with the
receiver centered at 162.300 MHz.

After analytic conversion was enabled, the native API again passed 2, 2.048,
3, 4, 5, 6, 7, 8, 9, 10, and 10.66 MS/s with SDRTrunk stopped to remove host
load; wall-clock error ranged from 0.0173 to 0.7926 percent. The upstream Soapy
adapter independently passed all 19 advertised rates from 62.5 kS/s through
10 MS/s, with error from 0.0030 to 0.3433 percent, and restored AGC. Running the
6 MS/s gate while the normal high-CPU SDRTrunk workload remained active limited
both the pre-fix and analytic daemons to about 4.03 MS/s, proving that result was
host-load interference rather than an analytic-filter regression.

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
sent. RSPduo limits use SDRplay's published 1 kHz--2 GHz coverage and
2--10.66 MSPS sample-frequency range.

API events now use a bounded dispatcher queue instead of invoking application
event callbacks on the daemon socket reader or AGC worker. This prevents an
event callback that calls `Update` from blocking the same reader needed to
finish that update. The overload fixture injects a saturated IQ frame, receives
`Overload_Detected`, acknowledges it by calling `Update` from inside the event
callback, then requires a hysteretic `Overload_Corrected` event from the next
normal frame. Gain and device-failure events use the same dispatcher.

## Typed last-error behavior (2026-07-13)

The official 3.15.1 library was behavior-probed with its service absent and no
receiver attached. `GetLastErrorByType` returned null for types -1 and 4 and
left the caller's timestamp unchanged. Type 0 returned the recorded DLL error
with a nonzero microsecond timestamp; types 1, 2, and 3 returned null and also
left the timestamp unchanged because no error existed in those categories.

OpenRSP previously ignored the requested type, returned its one record for all
integers, and always overwrote the timestamp with zero. The compatibility layer
now returns its in-process error only for DLL category 0, records its UTC time
in microseconds, and leaves the caller's timestamp untouched for unsupported or
currently empty categories. The compatibility fixture covers -1 through 4,
then injects a rejected RF update and requires the typed DLL record, message,
and nonzero timestamp. Separate DLL-device and daemon-side error histories are
not implemented yet; those categories return null instead of fabricating data.

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

An initial implementation incorrectly inferred that B occupied USB lane 0.
Direct raw-lane measurements disproved that: after the startup transient, lane
0 was nearly empty while lane 1 measured about 234 RMS counts, matching the
official B reference of about 245. Both A and B single-tuner routes therefore
use lane 1 before OpenRSP's analytic-IQ conversion.

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
