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
