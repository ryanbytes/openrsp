# RSPduo evidence log

## Locally verified baseline

- USB vendor/product: `1df7:3020`
- USB device class: vendor-specific (`0xff`)
- Configurations: 1
- Serial descriptor: readable (value intentionally not recorded here)
- Model mapping: SDRplay's published VirtualHere guide states PID `3020` is RSPduo

No control-transfer meaning, endpoint role, tuner register, initialization sequence, or IQ framing has yet been established. Do not infer those from the descriptor alone.

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
