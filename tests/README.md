Run `python3 tests/run_tests.py` with Python 3.10+ and a host C compiler (`cc`,
`clang`, or `gcc`). Set `CC` to select another compiler.

The tests compile the production controller, render-loop, and focus-save code
with platform stubs. They also compile the complete production JNI dispatcher
and reproduce the startup save-path lookup through its real function table for
Full, Meltdown, SubZero, World, and Lite package identities and save directories.
String checks cover UTF-16, modified UTF-8, embedded NUL, surrogate pairs,
string regions, critical access, malformed input, and all 65,536 code units.
The string ABI follows the [JNI specification](https://docs.oracle.com/en/java/javase/20/docs/specs/jni/functions.html#string-operations).

To check compatibility patches against an ARM64 game library:

```
python3 tests/run_tests.py --library /path/to/libcocos2dcpp.so com.robtopx.geometryjump
```

Repeat `--library SO_PATH PACKAGE` for each variant. The harness maps the ELF
segments and invokes the port's patch code; it never executes Android machine
code. It checks package detection, both C++ string ABIs, TLS verification patches,
leaderboard branches, missing-CA behavior, and an invalid branch signature.
It also checks the actual JNI string slots used by each library's JniHelper:
UTF-8 access for the older full game, UTF-16 access for all five newer packages.
Game libraries are user-supplied and are not included in the repository.
