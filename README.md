<div align=center>

<img src="extras/banner.png" alt="Banner" width="60%">

</div>
<h1 align=center>Geometry Dash · Switch Port</h1>

A wrapper/port of the Android releases of **Geometry Dash**, **Meltdown**,
**SubZero**, **World**, and **Lite**. It loads the original arm64 game binaries —
`libcocos2dcpp.so` (RobTop's cocos2d-x 2.2 fork + the game) and the stock
`libfmod.so` — patches them and runs them inside a minimal Android-like
environment natively on the Switch.

### How to install

You need a **2.2.14x arm64** Android release; extract the files from your own
legally-owned copy of the game.

Compatibility has been checked against the supplied **full game 2.2.145**
arm64 package (code 42), **2.2.148** Meltdown (code 15), SubZero (code 13),
World (code 15), and Lite (code 44), alongside the older full-game library.
The port handles both the older GNU and the newer Android libc++ string layouts
used by these releases.

Use wrapper **v1.0.3 or newer** with these updated packages; it fixes their
UTF-16 JNI save-path conversion during startup.

Create a separate folder for each game you install and fill it:

```
/switch/<anything>/
  gdash_nx.nro
  libcocos2dcpp.so        # from the APK's lib/arm64-v8a/
  libfmod.so              # from the APK's lib/arm64-v8a/
  assets/                 # the entire contents of the APK's assets/ folder
  config.txt              # created on first run
  prefs.txt               # created on first run; includes anonymous network ID
  cacert.pem               # generated from the Switch firmware trust store
  save/                    # created on first run; game saves
```

For example, `/switch/gdash_full`, `/switch/gdash_meltdown`,
`/switch/gdash_subzero`, and `/switch/gdash_world` each keep an independent
save. Split APK/XAPK releases usually store the assets in the base APK and the
native libraries in `config.arm64_v8a.apk`. An `armeabi-v7a`-only release is
32-bit and cannot supply libraries for this arm64 port.

For APKMirror `.apkm` bundles, open `base.apk` for `assets/` and
`split_config.arm64_v8a.apk` for the two game libraries. Keep the assets and
libraries from the same game/version together. The ad/crash-reporting native
libraries in the split are not required by this wrapper.

### Notes

This will not run in applet/album mode — it needs the full memory of a game
override. Launch it by holding **R** while opening an installed title, or use
a forwarder.

`config.txt` is created on first run:

* `screen_width` / `screen_height` — render resolution; `-1` auto-picks
  1280x720 handheld / 1920x1080 docked.
* `cursor_speed` — pointer speed of the stick-driven cursor, px/s at 720p.
* `menu_autosave_interval_seconds` — menu save cooldown, **30** seconds by
  default; `0` disables menu autosaves. Old `autosave_interval_seconds` entries
  are ignored so older test configurations use the new menu-only default.

### Controls and saves

Either stick moves the menu cursor. **A** touches its last position, including
after the cursor fades out; it sends one touch, not an extra bottom-right tap.
**L/R/ZL/ZR** retain the dedicated jump touch. D-pad left/right retain the
platformer movement zones, and **B/Plus** go back or pause.

The wrapper uses the original `GameManager::doQuickSave` entry point. The first
checkpoint waits at least 30 seconds after startup; further saves run at most
once every 30 seconds while a menu is open. A save due during a level waits
until you return to a menu. Pause overlays, the level editor, loading screens,
and transitions do not trigger autosaves.
Detached native save workers are reclaimed after they finish. Both progress
and local-level writers are awaited before the next frame is presented.
A clean in-game quit also requests a final checkpoint.

HOME does not trigger a save. Before closing, return to a menu and allow the
30-second cooldown to pass. Keep the same game folder so its `save/` directory
is reused.

### Online features

Public leaderboards and level search/download use RobTop's native Boomlings HTTPS
client embedded in `libcocos2dcpp.so`; they do not require Google Play Games.
The port supplies the missing Switch socket ABI conversions, reports the real
console connection state, and keeps a random anonymous network ID in
`prefs.txt`. It does not read a Nintendo account or console identifier.

TLS peer and hostname verification are enabled at load time. `cacert.pem` is
regenerated from the Switch firmware's enabled public root certificates, so no
certificate file needs to be copied beside the NRO. Google Play sign-in stays
disabled. The public global leaderboard is fetched without the stock client's
anonymous score-upload bootstrap. Level-specific leaderboards require a logged-in
Geometry Dash account because Boomlings requires account authorization for that
endpoint. Score submission, comments, uploads, messaging, and other social
endpoints are outside the supported/tested scope of this port.

Networking is always enabled. Release builds do not write network diagnostic
logs or record request/response payloads, the anonymous ID, or credentials.

### How to build

Install the devkitPro Switch toolchain and portlibs:

```sh
pacman -S devkitA64 switch-tools libnx switch-sdl2 switch-mesa switch-libdrm_nouveau
```

Then `make`.

### Credits

* TheOfficialFloW for the original Android so-loader.
* hatoving for the Geometry Dash PS Vita port this one is based on.
* fgsfds for max_nx / the Switch so-loader groundwork reused here.

### Support

If you enjoy my work and want to support me :

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

### Legal

This project has no affiliation with RobTop Games. "Geometry Dash" and
related marks belong to their respective owners. No assets or program code
from the original game or its Android release are included here. We do not
condone piracy; users must own a legal copy of the game.

Unless noted otherwise, the source in this repository is under the MIT
License (see LICENSE).

### SaltyNX telemetry

FPS and render resolution are published to SaltyNX shared memory for compatible overlays. The port continues normally when SaltyNX is not running. This reports presentation timing and resolution; it does not change the game's frame cap.
