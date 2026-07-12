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
