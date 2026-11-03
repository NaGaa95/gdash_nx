Run `python3 tests/run_tests.py` with Python 3.10+ and a host C compiler (`cc`,
`clang`, or `gcc`). Set `CC` to select another compiler.

The tests compile the production controller, render-loop, menu guards, and native save bridge
with platform stubs. They also compile the complete production JNI dispatcher
and reproduce the startup save-path lookup through its real function table for
Full, Meltdown, SubZero, World, and Lite package identities and save directories.
String checks cover UTF-16, modified UTF-8, embedded NUL, surrogate pairs,
string regions, critical access, malformed input, and all 65,536 code units.
The string ABI follows the [JNI specification](https://docs.oracle.com/en/java/javase/20/docs/specs/jni/functions.html#string-operations).

Save tests exercise 30-second menu scheduling, startup/transition guards, level
and editor deferral, native quick-save ordering and background-write completion,
legacy synchronous saves, disk readback, and config defaults.
Focus notifications only pause/resume: tests explicitly check that HOME does not save.
Thread tests use real host pthreads with an ENOSYS detach stub to reproduce the
Switch limitation. They check join/detach/exit, already-finished workers, detached
attributes, concurrent cleanup, failed creation, TLS reclamation, and the first-save delay.
They use the production save manager with a native-save test backend; Android
save serialization is outside the host harness.

To check compatibility patches against an ARM64 game library:

```
python3 tests/run_tests.py --library /path/to/libcocos2dcpp.so com.robtopx.geometryjump
```

Repeat `--library SO_PATH PACKAGE` for each variant. The harness maps the ELF
segments and invokes the port's patch code; it never executes Android machine
code. It checks package detection, both C++ string ABIs, TLS verification patches,
leaderboard branches, missing-CA behavior, and an invalid branch signature.
It also checks the actual JNI string slots used by each library's JniHelper:
UTF-8 access for the older full game, UTF-16 access for all five newer packages,
the native save entry points, and the scene/play-layer layout signatures.
Game libraries are user-supplied and are not included in the repository.
