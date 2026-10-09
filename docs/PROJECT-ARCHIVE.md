# Archive project

The toolbar Archive project icon is available in standalone xIPs and native
BrowserPanel even without an open library. It opens one owned tool window. Choose
a single XPR or scan a folder, select projects, edit archive names, configure the
exact Vivado versions, choose an output directory, then create complete folders
and 7z packages. This tool does not create library assets or versions.

The engine inventories the source read-only, copies into an isolated diagnostic
workspace, preserves registered implementation outputs and the active block
design's HWH, relocates generated/local sources, validates XCIX contents, invokes
Vivado reset on the copy, prunes generated files, compresses, tests the archive,
and publishes with no-overwrite checks. Failed/cancelled workspaces are retained.
Two output renames provide rollback, not crash-atomic publication. Six progress
stages and detailed logs remain available. Batch failures continue to the next
selected project; cancellation stops the remaining queue.

Windows execution uses an isolated profile/temp and minimal system PATH while
retaining licensing variables. The suspended launcher enters a kill-on-close Job
Object before creating children. Success requires the Tcl marker and exit zero.
Missing IP repositories are cleaned only in the copy and require an IP definition
check before reset.

## State and lifecycle

State is owned by the explicit INI file
QStandardPaths::GenericConfigLocation/xIPs/archive.ini, independent of host
application/organization names. If the state key is absent, the schema-1
tools/project.archive/state map is copied once from
GenericConfigLocation/FpgaToolbox/settings.ini. Existing xIPs state wins, including
an empty state. The old file is read-only; missing legacy settings are valid.

Closing while archiving/scanning is blocked with a Cancel message. Standalone
close has the same guard. Native hosts consult isCatalogBusy, which includes
archive work. Context/state changes queue until idle. Forced owner destruction
interrupts and joins owned workers. Paths, selections, names and installs persist.

## Build and deployment

Qt 6.10.2 Xml and matching CorePrivate ZIP reader are required. The unmodified
7-Zip Extra 26.03 executable is hash-pinned in CMake and packaging. It is resolved
relative to ArchiveRuntime's module (xips.exe or xips-browser-impl.dll), never
the host executable, PATH or Toolbox.

package-release.ps1 includes tools/7zip/7za.exe, Qt6Xml.dll and original 7-Zip
notices plus corresponding sources. xips-native-runtime.json declares component,
helper, host Qt/plugin and license dependencies. package-native-component.ps1
exports a new staging overlay: components/xips, host Qt6Xml.dll and
licenses/components/xips. It never installs or changes a host. The coordinator
must check host Qt identity and merge owned checksums/metadata while preserving
the host and other components.

## Provenance and tests

Source: FPGA Toolbox 0.4.5, https://github.com/RutenunexJX/FpgaToolbox.git,
commit 129479a23beaf23c2eeff9470432f7135a7d1f22.
modules/project-archive maps to src/archive. ToolPage/ToolRegistry/ToolWorkspace
are replaced by ArchiveWindow, ArchiveState and BrowserPanel ownership. Engine,
scan, artifact selection, XCIX validation, resources, tests and fixtures remain.
Internal FTB_* Tcl/test variables remain private compatibility identifiers; they
do not depend on Toolbox. Diagnostics now use .xips-archive-*.

tst_archive retains every original archive test method, adapting the two shell
tests to owned window/state; it adds state migration and fresh real fixture
creation. tst_archive_host exercises standalone and real native DLL ownership,
helper location, close/cancel/destroy, scan, context queue, host identity and
narrow light/dark views. Original library/native/IPC tests remain.

Opt-in real tests use FTB_TEST_VIVADO, FTB_TEST_XPR, FTB_TEST_OUTPUT and optional
FTB_TEST_ARCHIVE_NAME. XIPS_REAL_FIXTURE enables fresh fixture creation in a new
directory. Never point destructive test fixtures at user projects.
