# UI boost host tests

Run from this directory:

```sh
python3 run-ui-boost-tests.py
```

The runner extracts the production helper functions, limiter wrapper, proc parser,
proc write handler, state declarations, and relevant header types on every run.
It compiles them with a fake hardware boundary, pthread mutexes, and small kernel
API shims. The implementation under test is not copied into the test harness.

AddressSanitizer and UndefinedBehaviorSanitizer are enabled by default. In a
ptrace-based sandbox, LeakSanitizer cannot run; use
`ASAN_OPTIONS=detect_leaks=0 python3 run-ui-boost-tests.py` there. The harness does
not allocate heap memory. Use `--sanitizer undefined` for UBSan alone, or
`--sanitizer ''` for a compiler without sanitizer support. `CC` selects the host
compiler. `--output /absolute/output/directory` preserves generated source and
the binary; otherwise temporary outputs are removed after the run.

The cases cover:

- Exact, between-entry, minimum, maximum, and above-maximum frequency requests.
- Invalid OPP counts, zero frequencies, duplicate or unordered entries, and the
  largest supported table.
- UI release while the working OPP table is invalid.
- Independent legacy APIBOOST floors and ceilings, keep/reset requests, and
  capturing requests made before the first UI command.
- Remapping stored kHz after the published working table changes.
- DEBUG suspension and restoration, including a pre-existing DEBUG limit.
- Failed apply/release and each step of DEBUG transitions, followed by retries.
- Unchanged requests for unrelated limiters and dual-buck targets.
- Concurrent legacy and UI release calls serialized through hardware completion.
- Negative, overflowing, malformed, overlong, empty, and embedded-NUL proc
  commands, user-copy failure, and existing set/switch syntax.

These host tests verify wrapper behavior and requested hardware arguments. They
do not validate the actual firmware transport, device clocks, power consumption,
thermal response, or Android hint timing; those need device validation.
