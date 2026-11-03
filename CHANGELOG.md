# Changelog

## 1.0.3 — 2026-11-03

- Support the supplied full game 2.2.145 and updated 2.2.148 ARM64 releases of Meltdown, SubZero, World, and Lite.
- Handle both native C++ string layouts and the updated leaderboard bootstrap.
- Fix UTF-16 JNI startup handling shared by all five updated games, including Unicode lengths, string regions, and critical access.
- Fix A sending an extra bottom-right touch in custom-level menus (issue #7).
- Restore the working quick-save path for progress and local levels, with pending writes awaited.
- Restrict autosaves to menus with a 30-second cooldown, avoiding periodic gameplay/editor stutters (issue #8).
- Fix the first-save crash caused by unsupported native thread detachment, and reclaim completed save workers.
- Delay the first autosave by 30 seconds and remove the unreliable HOME save trigger; return to a menu before closing.
- Build the release with standard Mesa 20.
