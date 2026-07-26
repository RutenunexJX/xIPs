# Qt Creator build and debug

The project is a standard CMake project. Open the repository-root
`CMakeLists.txt`; do not create or open a qmake `.pro` project.

## Verified Windows kit

The following local toolchain was verified without starting a visible Qt
Creator window:

- Qt Creator 18.0.2:
  `E:\QT6\Tools\QtCreator\bin\qtcreator.exe`
- Qt 6.10.2 MinGW 64-bit:
  `E:\QT6\6.10.2\mingw_64`
- CMake 3.30.5:
  `E:\QT6\Tools\CMake_64\bin\cmake.exe`
- Ninja:
  `E:\QT6\Tools\Ninja\ninja.exe`
- MinGW g++ 13.1.0:
  `E:\QT6\Tools\mingw1310_64\bin\g++.exe`
- GDB 11.2:
  `E:\QT6\Tools\mingw1310_64\bin\gdb.exe`

Use the equivalent paths when the Qt SDK is installed elsewhere. In
**Preferences > Kits**, the selected desktop kit must use the Qt, CMake,
compiler, debugger, and device architecture from the same MinGW 64-bit
installation. Do not mix an MSVC Qt build with the MinGW compiler.

## Configure the project

1. Select **File > Open File or Project** and open the root `CMakeLists.txt`.
2. Select the **Desktop Qt 6.10.2 MinGW 64-bit** kit.
3. Enable the **Debug** build configuration and retain Qt Creator's default
   out-of-source build directory.
4. In **Projects > Build Settings**, verify that `CMAKE_BUILD_TYPE` is `Debug`
   and `BUILD_TESTING` is `ON`.
5. Run CMake configuration when prompted.

Qt Creator stores the selected kit, build directory, and run arguments in
`CMakeLists.txt.user*`. Those files are workstation-specific and are
intentionally ignored. A checked-in `CMakePresets.json` is not required because
the Qt Creator kit supplies the compiler, Qt prefix, CMake, Ninja, and debugger.

## Build and debug

The CMake configuration marks only these application targets as Qt Creator run
targets:

- `xips`: desktop application; primary interactive debug target.
- `xips-cli`: command-line interface; suitable for argument and JSON-contract
  debugging.

The two internal static libraries and `tst_*` executables remain available in
the Build target list, but they do not create application run configurations.
CTest and Qt Creator's Tests view can run the `core`, `gui_smoke`, and
`user_journey` suites. Both GUI suites are configured for Qt's offscreen
platform and do not operate the visible desktop.

For desktop debugging:

1. In **Projects > Run Settings**, select the `xips` run configuration.
2. Set **Command line arguments** to
   `--library %{Project:DirName}/examples/library`.
3. Keep the run environment set to **Build Environment** so the kit-provided Qt
   and MinGW runtime directories remain on `PATH`.
4. Select **Build**. Set a breakpoint in `src/app/MainWindow.cpp` or another
   source file, then select **Start Debugging**.

For CLI debugging, select `xips-cli` and use an argument set such as:

```text
--action list --library %{Project:DirName}/examples/library --query reset
```

The desktop target is a Windows GUI executable, so the absence of a separate
console window is expected and does not prevent GDB debugging. The lean build
has no Slang, SQLite, Git, Vivado, or external test-runner dependency. Its
application build graph consists of `xips_core`, `xips_app`, `xips`, and
`xips-cli`.

## Silent command-line equivalent

The following configuration is equivalent to the verified Qt Creator Debug kit
and is useful for diagnosing kit configuration failures:

```powershell
$env:PATH = "E:\QT6\Tools\mingw1310_64\bin;E:\QT6\6.10.2\mingw_64\bin;$env:PATH"
E:\QT6\Tools\CMake_64\bin\cmake.exe -S . -B build-qtcreator-debug -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DBUILD_TESTING=ON `
  -DCMAKE_PREFIX_PATH=E:\QT6\6.10.2\mingw_64 `
  -DCMAKE_MAKE_PROGRAM=E:\QT6\Tools\Ninja\ninja.exe `
  -DCMAKE_CXX_COMPILER=E:\QT6\Tools\mingw1310_64\bin\g++.exe
E:\QT6\Tools\CMake_64\bin\cmake.exe --build build-qtcreator-debug --target xips
```

If CMake cannot find Qt, correct the Qt version selected in the kit rather than
hard-coding a different compiler. If Qt Creator reports that no debugger is
available, assign the MinGW `gdb.exe` listed above to the kit.
