# Qt Creator

Open the root CMakeLists.txt with the Qt 6.10.2 MinGW 64-bit kit.
The compiler must match ZeroSlack's MinGW 13.1 build for native embedding.
Qt private Widgets headers are required by the shared Ela fork.

Set CMAKE_BUILD_TYPE to Debug while developing and BUILD_TESTING to ON.
The runnable targets are xips and xips-cli. The xips_browser target produces xips-browser.dll.
All executable outputs are in the build directory's bin folder.
Set a breakpoint in src/app/BrowserPanel.cpp for the shared UI or src/library/SnapshotLibrary.cpp for version operations.

For a disposable library, create an empty folder outside the repository and run:

```text
--library <absolute folder path>
```

Avoid collecting or converting example or personal libraries merely to inspect the UI.
For CLI debugging:

```text
--action list --library <absolute folder path> --query uart
```

## Command line

```powershell
$env:PATH = "E:/QT6/Tools/mingw1310_64/bin;E:/QT6/6.10.2/mingw_64/bin;$env:PATH"
E:/QT6/Tools/CMake_64/bin/cmake.exe -S . -B build/ela -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DXIPS_WARNINGS_AS_ERRORS=ON -DCMAKE_PREFIX_PATH=E:/QT6/6.10.2/mingw_64 -DCMAKE_MAKE_PROGRAM=E:/QT6/Tools/Ninja/ninja.exe -DCMAKE_CXX_COMPILER=E:/QT6/Tools/mingw1310_64/bin/g++.exe
E:/QT6/Tools/CMake_64/bin/cmake.exe --build build/ela --parallel 6
E:/QT6/Tools/CMake_64/bin/ctest.exe --test-dir build/ela --output-on-failure
```

Qt and MinGW runtimes must be on PATH for a development build. Use windeployqt for a standalone package.
The standalone application and xips-browser-impl.dll share the private XipsEla.dll; ship both DLLs beside the public xips-browser.dll entry point. Do not replace a host application's ElaWidgetTools.dll.
