# Architecture

OpenRSP is a userspace libusb driver. A macOS kernel extension is unnecessary and would add signing, security, and crash risk without solving the undocumented hardware protocol.

The intended layers are:

1. `usb`: enumeration, interface ownership, bounded asynchronous transfers, cancellation, and hotplug.
2. `rspduo`: independently documented initialization, tuner control, calibration, and IQ framing for PID `0x3020`.
3. `core`: stable device/session/stream state machines with no global daemon.
4. `compat`: optional SoapySDR and SDRplay API 3-compatible adapters.

The core owns no unbounded wait. Every submitted transfer has one owner, one completion path, and a cancellation path. Device removal is a normal state transition. Callbacks never perform blocking control transfers. Recovery happens inside a device session, not through a machine-wide privileged service restart.

## RSPduo state model

```text
absent -> discovered -> opened -> configured -> streaming
   ^          |           |          |            |
   +----------+-----------+----------+------------+
                    removal or close
```

Errors must preserve their source: USB transport, protocol rejection, invalid caller state, unsupported hardware, or sample discontinuity. A generic `fail` result is insufficient for diagnosing an unstable receiver stack.

## Compatibility policy

Protocol support is model- and revision-specific. Unknown product IDs may be enumerated but cannot be opened. Firmware or calibration writes are forbidden until their persistence and rollback behavior are independently established.
