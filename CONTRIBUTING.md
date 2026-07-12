# Contributing

This is a clean-room hardware-interoperability project. Contributions must be based on lawful, independently obtained observations, public documentation, or permissively licensed source material.

Do not submit:

- decompiled or disassembled SDRplay binaries;
- copied proprietary implementation code;
- confidential SDK or developer-program material;
- device firmware unless redistribution is explicitly permitted;
- claims of support without reproducible tests.

For every newly identified USB product ID, include the operating system's descriptor output, receiver model/revision, and how the ID was obtained. Redact serial numbers before committing captures. For every control transfer, document request type, request, value, index, payload, observed application action, and at least two repetitions. A correlation is not yet a causal interpretation.

Hardware tests must be opt-in. Discovery must never detach a kernel driver, claim an interface, change firmware, or stop the official SDRplay service. Potentially persistent writes require a separate tool and an explicit warning.
