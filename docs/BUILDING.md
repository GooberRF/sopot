Building RF2 Community Patch (SOPOT)
=================

Windows (Visual Studio 2022)
----------------------------

Generate:

```powershell
cmake -S . -B build -A Win32
```

Build launcher and patch DLL:

```powershell
cmake --build build --config Release --target SopotLauncher Sopot
```

Artifacts:

- `build/bin/Release/SopotLauncher.exe`
- `build/bin/Release/Sopot.dll`
