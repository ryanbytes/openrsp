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
testing remain required before claiming general recovery stability.

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
