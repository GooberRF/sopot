RF2 Community Patch (SOPOT) Changelog
=====================================

Version 1.0.0: Not yet released
-------------------------------------
### Features, changes, and enhancements
[@GooberRF](https://github.com/GooberRF)
- Added launcher settings for:
  - game executable path (`rf2.exe`)
  - window mode (`fullscreen`, `windowed`, `borderless`)
  - game resolution
  - fast start (skip startup BIK videos)
  - auto close launcher after launch
  - vsync toggle
  - direct input mouse toggle
  - aim slowdown on target toggle
  - enemy crosshair indicator toggle
- Added in-game developer console overlay with:
  - command execution
  - help output
  - command search (`. <string>`)
  - tab completion
  - scroll controls (`PgUp`, `PgDown`, `Home`, `End`)
- Added custom console commands:
  - `fov`
  - `maxfps`
  - `r_showfps`
  - `directinput` / `dinput`
  - `aimslow`
  - `enemycrosshair`
- Added launcher About dialog with links to local project documentation.
- Added SOPOT launcher and game patch bootstrap flow for Red Faction II.
- Added launcher-managed settings in `sopot_settings.ini`.
- Added runtime DLL injection flow for `rf2.exe`.

### Compatibility and fixes
[@GooberRF](https://github.com/GooberRF)
- Added RF2 window mode support improvements for modern systems.
- Added startup movie skip support (`fast_start`).
- Disabled broken stock video memory startup requirement check.
- Added background activity handling improvements to reduce alt-tab lockups.
- Added input flush handling on focus loss to avoid latched keys.
- Added widescreen FOV autoscaling support (90 hFOV baseline at 4:3).
