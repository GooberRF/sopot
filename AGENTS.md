# AGENTS.md

Guidance for coding agents working in `sopot`.

## Project Summary

- Project: **RF2 Community Patch (SOPOT)**.
- Output binaries:
- `Sopot.dll` (patch DLL from target `Sopot`)
  - `SopotLauncher.exe` (launcher from target `SopotLauncher`)
- Platform constraints:
  - **Win32/x86 only** (enforced in top-level `CMakeLists.txt`).
  - Visual Studio 2022 toolchain is the expected Windows setup.

## Build Commands

- Configure:
  - `cmake -S . -B build -A Win32`
- Build launcher + DLL:
  - `cmake --build build --config Release --target SopotLauncher Sopot`
- Build DLL only:
  - `cmake --build build --config Release --target Sopot`

Primary build outputs:
- `build/bin/Release/SopotLauncher.exe`
- `build/bin/Release/Sopot.dll`

## Repository Layout

- `game_patch/`: injected patch DLL logic and RF2 hooks.
- `launcher/`: standalone launcher executable UI and flow.
- `launcher_common/`: shared launcher helpers (process launch/injection).
- `patch_common/`: reusable hook/injection primitives (`FunHook`, `CallHook`, `CodeInjection`, `AsmWriter`).
- `common/`: config, error, and utility code used across targets.
- `crash_handler_stub/`: crash/watchdog integration support.
- `xlog/`: in-tree logging library.
- `vendor/`:
  - `subhook/`
  - `d3d8/` headers
- `research/`: reference material. Treat as read-only unless explicitly asked.

## Current Naming/Branding State

- Product name string is **RF2 Community Patch (SOPOT)**.
- Launcher target is `SopotLauncher`.
- Patch DLL output name is `Sopot`.
- Some legacy identifiers/macros may still use historical names for compatibility; do not rename blindly.

## Critical Engineering Rules

- Preserve hook/functionality behavior unless the task explicitly asks for behavioral changes.
- For address-based patches, treat constants and calling conventions as high risk:
  - validate signatures/patterns before patching.
  - avoid changing `__cdecl` / `__stdcall` / `__fastcall` types.
- Prefer minimal diffs around hooking code and trampolines.
- Keep `patch_common` style aligned with `alpinefaction` when touching shared hook primitives.

## Editing and Style Notes

- C++ standard: C++20.
- Keep formatting consistent with surrounding file style.
- For SOPOT hook instance declarations in `game_patch/*`, keep initializers concise where unambiguous.
- Use comments sparingly and only for non-obvious intent.
- Avoid broad refactors unless explicitly requested.

## Validation Expectations

For code changes, prefer:
1. Build `Sopot`.
2. If launcher-related changes were made, build `SopotLauncher` too.
3. Report exact build command and result.

If you cannot run the build, state why and what remains unverified.

## Licensing and Third-Party

- Keep `licensing-info.txt` in sync with third-party code actually present/used.

## Agent Workflow Suggestions

When implementing changes:
1. Read relevant local module(s) and CMake target file first.
2. Check `alpinefaction` for style/parity when touching patch/hook internals.
3. Make smallest safe edit.
4. Build affected target(s).
5. Summarize exactly what changed and why.
