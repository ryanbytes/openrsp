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
factory serial `1806000E32`, and the existing daemon resolved and reopened it.

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
verify every advertised Soapy rate, manual gain control, antennas, dual-tuner
operation, or the currently unsupported bias-tee/notch/external-reference
controls. SDRTrunk was restarted afterward, rediscovered the RSPduo, restored
the Wabash channel, and loaded JMBE normally.

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
