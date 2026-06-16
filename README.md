# vulcpp-predicate-expand

Restricted VUL-style C++ to Predicate IR / OutputExpressionMap.

## Directories

- `src/`: predicate-expand source
- `tests/`: unit, e2e, and fixture tests
- `gui/`: Windows GUI
- `scripts/`: build, test, run, and publish scripts
- `third_party/`: bundled VUL headers used by tests and releases

## Build

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\configure_vs2026.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\build_release.ps1
```

## Run

```powershell
.\scripts\run_real_case.ps1 -BuildDir build -Input "path\module.logic.cpp" -Top hls_main -Format text -Output "path\module.txt" -ClangArg "-Ipath\include"
.\scripts\run_real_case.ps1 -BuildDir build -Input "path\module.logic.cpp" -Top hls_main -Format json -Output "path\module.json" -ClangArg "-Ipath\include"
```

## GUI

```powershell
dotnet build .\gui\RTLzz.Gui\RTLzz.Gui.csproj -c Release
dotnet run --project .\gui\RTLzz.Gui\RTLzz.Gui.csproj -c Release
```

## Test

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\test_all.ps1 -BuildDir build
powershell -ExecutionPolicy Bypass -File .\scripts\test_gui.ps1 -BuildDir build
```
