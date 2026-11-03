# Changelog

## 1.0.3 — 2026-11-03

- Support the supplied full game 2.2.145 and updated 2.2.148 ARM64 releases of Meltdown, SubZero, World, and Lite.
- Handle both native C++ string layouts and the updated leaderboard bootstrap.
- Fix UTF-16 JNI startup handling shared by all five updated games, including Unicode lengths, string regions, and critical access.
- Fix A sending an extra bottom-right touch in custom-level menus (issue #7).
- Remove forced 20-second saves from rendering; retain HOME/exit saves (issue #8).
- Build the release with standard Mesa 20.
