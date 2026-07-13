# Architecture

OpenRSP is a userspace libusb driver. A macOS kernel extension is unnecessary and would add signing, security, and crash risk without solving the undocumented hardware protocol.

The intended layers are:

1. `usb`: enumeration, interface ownership, bounded asynchronous transfers, cancellation, and hotplug.
2. `rspduo`: independently documented initialization, tuner control, calibration, and IQ framing for PID `0x3020`.
3. `core`: stable device/session/stream state machines with no global daemon.
4. `compat`: optional SoapySDR and SDRplay API 3-compatible adapters.

Daemon protocol version 7 represents RSPduo mode explicitly. A single-tuner
configuration selects A or B and emits one IQ event consumed as API Stream A.
A dual configuration carries a shared ADC rate plus separate A and B RF/gain
configuration and emits distinguishable A and B IQ events. Update requests
retain their tuner identity. This prevents a dual session from collapsing two
independent control and callback timelines into the old single-channel state.
Each channel configuration also carries its bias-T, RF-notch, DAB-notch,
external-reference, AM-port, and AM-notch state so recovery and tuner swaps can
restore controls without a second out-of-band state model. AM controls are
tuner-A-only; a direct protocol update that addresses them to tuner B is
rejected.

An initialized API mode transition is a two-phase daemon operation. The swap
request stops and closes the old hardware session, opens and configures the
target mode, and leaves IQ paused. Only after the compatibility layer has
atomically changed its A/B parameter pointers, DSP state, callback queue,
sample counters, and reset state does it send the resume request. This prevents
the first target-mode B frame from racing the application-visible mode change.
If hardware reconfiguration fails, the daemon attempts to restore and restart
the old mode before returning the error.

API discovery reports RSPduo tuner and mode fields as capability masks, not as
an already selected operating mode. OpenRSP advertises both tuners and the
direct single/dual modes it implements. It deliberately omits the official
master capability until cross-process master/slave ownership exists, preventing
SoapySDR and other clients from presenting a mode that cannot be selected.

The core owns no unbounded wait. Every submitted transfer has one owner, one completion path, and a cancellation path. Device removal is a normal state transition. Callbacks never perform blocking control transfers. Recovery happens inside a device session, not through a machine-wide privileged service restart.

## RSPduo state model

```text
absent -> discovered -> opened -> configured -> streaming
   ^          |           |          |            |
   +----------+-----------+----------+------------+
                    removal or close
```

Errors must preserve their source: USB transport, protocol rejection, invalid caller state, unsupported hardware, or sample discontinuity. A generic `fail` result is insufficient for diagnosing an unstable receiver stack.

Dual mode owns two callback sequences, reset states, sample counters, low-IF
DSP states, decimators, gain/AGC states, and overload acknowledgement states.
The USB reader uses session-owned completed-transfer buffers and separates the
two ADC lanes without allocating in the steady-state completion path. A live
single-tuner swap stops the stream, changes the selected frontend while
preserving the active channel settings, restarts it, and continues to expose
only Stream A. A live single/dual mode transition preserves the stored A and B
channel settings, resets the newly active callback timelines, and starts dual
IQ only after both application parameter pointers are valid.

## Compatibility policy

Protocol support is model- and revision-specific. Unknown product IDs may be enumerated but cannot be opened. Firmware or calibration writes are forbidden until their persistence and rollback behavior are independently established.

## Receiver identity

The compatibility API passes the daemon the receiver's USB vendor ID, product
ID, and actual descriptor identity captured during discovery. The daemon
resolves that tuple against a fresh enumeration before opening or recovering a
receiver; it does not assume that libusb's raw device index remains stable after
an unplug/replug. Duplicate matches fail as ambiguous rather than selecting an
arbitrary radio. If no serial or physical-port identity is available, the raw
index is retained as a limited fallback and stable replug recovery is not
claimed.

An RSPduo physically reappears first as a cold `1df7:3020` firmware-loader
device with `iSerialNumber == 0`. The daemon recognizes only that explicit
descriptor state, loads firmware from the configured local file, waits for USB
re-enumeration, and then performs normal factory-serial matching. It does not
select the first device with the same product ID. This ordering avoids the
circular requirement that a cold device already expose the serial produced by
its firmware.

Application-facing serial overrides are deliberately excluded from hardware
selection. A caller-controlled display name must not be able to redirect a
session to another attached receiver.
