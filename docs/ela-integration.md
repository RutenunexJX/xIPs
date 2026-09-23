# Ela integration in xIPs 2.2

The application retains one English BrowserPanel, used by standalone xIPs and the
ZeroSlack native surface. No new navigation hierarchy is introduced.

| Surface | Actual component and enabled behavior | Application responsibility |
| --- | --- | --- |
| Window | ElaWidget / ElaAppBar native window events | Title, branding, standalone activation |
| Commands and categories | ElaPushButton / ElaToolButton | Command meaning, checked category, enabled state |
| Search and notes | ElaLineEdit / ElaPlainTextEdit | English actions connected to Qt undo/redo/clipboard; nonblocking owned popup |
| Revision/category choice | ElaComboBox original height, position and indicator animations, with p26 interruption | Model, selection, fixed revision identity |
| Actions and editing menus | ElaMenu original reveal plus p27 input/hide/theme interruption; native action rendering retained | English labels, anchors, action authorization, owner lifetime |
| Assets and files | ElaListView / ElaScrollBar, 160 ms wheel target animation | Cached catalog model, selection and file list |
| Precision gestures | ElaScrollBar pixel input for lists; Qt native wheel handling for text | Axis choice, text line units, no smoothing of already precise pixel deltas |
| Dialogs | ElaContentDialog mask and dismissal lifecycle | English buttons, validation; reject/detach a stack dialog when its owner is destroyed |
| Feedback | ElaProgressRing / ElaText / ElaToolTip | Actual task state, error text, Qt help timing, screen bounds, no focus activation |
| Resizing | Qt QSplitter | Independent horizontal/vertical user ratios, backward-compatible saved state |

The tooltip and wheel event adapters are local extensions. TextUnitScrollBar keeps
QPlainTextEdit pixel gestures in Qt's native line units and uses Ela only for
angle-wheel motion; the viewport router lets text gestures pass through Qt.
Explicit view palettes
compose Ela's translucent central color over its window base. Ela retains list
selection/hover painting; application QSS is limited to the details container and
Qt splitter. Busy animation stops when the panel is hidden. Long issue reports use
a read-only ElaPlainTextEdit. Common note fields also enable ordinary smooth scrolling.

## Data and lifetime boundaries

CatalogModel caches case-folded searchable metadata and display text once per
refresh. Filtering changes visible indices only; unchanged result sets preserve
selection and avoid resetting the model. QStringListModel displays revision files.
Selecting schema 2 metadata is immediate and uses the catalog already loaded.
The Refresh command reloads external changes. At most one legacy describe request
runs per panel; only the latest pending selection is retained. Stale results cannot
replace the current selection. Pending context changes are applied after operations.

Immutable snapshots, library locks, digest verification, transactional writes,
migration, deletion boundaries and pinned exports remain in SnapshotLibrary.
Worker jobs capture values; UI connections end with their QObject owner. A started
transaction retains its existing filesystem completion semantics after panel closure.
Native callers keep the DLL loaded, as ZeroSlack does with PreventUnloadHint.

There is no specialist editor/diagram canvas in xIPs. System file dialogs and
local-file drag/drop retain their Qt/OS behavior. Drawers, ribbon, calendars and
extra navigation are not applicable. The existing bounded SuiteApp runtime/provider
startup contract is unchanged; this release does not claim a cold-start benchmark.

## Provenance and native compatibility

Upstream: `454cac2d57a47d3cc28577dc817793aec1881ca7`.
Shared source reference: ZeroSlack `8f7abf69e464c582387394944b873b8364d85bec`.
Patches 25, 26 and 27 are imported incrementally. Patch 28 fixes ElaListView's
style ownership: QApplication keeps it alive until Qt completes focused-view
destruction, with deleteLater after destroyed and application-exit cleanup as fallback.
No destructor repolishing or nested processEvents is introduced. All 34 source
files touched by patches 25–28 replay identically after line-ending normalization
from the previous xIPs `c5bdd8e` baseline. MIT and font OFL attribution is retained.
Patch 29, `29-wave-overlay-origin-lifetime.patch`, adds guarded overlay origin/area
references and callback cancellation. Its three additional files match the shared
validated patch. It is recorded as implementation patch level 29 without changing
the public p27 capability ABI.

The ABI remains native surface v1 with `;ela=454cac2d-p27`. This names the required
API level; the source digest identifies additional compatible lifetime fixes.
The DLL directly references new combo/menu lifecycle exports. Capabilities JSON
and the formal build record include the normalized source SHA-256; the package
also records the actual Ela DLL hash and carries the complete patch chain.

## Validation

All eight CTest groups pass at both scales. The interaction executable includes
eleven scenarios plus setup/cleanup, with no skipped cases. An early integration run
also loaded this component with ZeroSlack 0.31.11's clean formal core/Ela DLLs:
four checks passed, including collection, pinned Use, provenance hash and workspace
closure. The coordinating task rechecks the final packaged DLL before deployment.

Build: Qt 6.10.2 exact / WidgetsPrivate, MinGW 13.1, C++20, Release optimization,
strict xIPs warnings treated as errors. Separate production builds disable tests
and test hooks. Eight CTest groups cover core, snapshots, native, CLI, GUI smoke,
user journey, interactions and SuiteApp at 100% and 200% scaling.

Interaction cases cover rapid combo reversal and Escape/Enter, owner destruction,
English edit action states/undo/read-only clipboard, pixel and angle wheel input,
keyboard/hide cancellation, splitter drag and restoration, background context
changes, modern selection with the worker pool occupied, bounded legacy requests,
Form owner destruction, tooltip focus, light/dark viewport pixels, and text
pixel/angle scrolling compared with unmodified QPlainTextEdit. Offscreen
screenshots cover wide and narrow layouts. Offscreen tests and CPU timings do not
establish desktop frame rate, compositor behavior or physical multi-monitor gestures.

## Catalog measurements

The same Release/O3 executable harness uses 2,000 synthetic catalog records with
64 file paths per revision, a 1000 by 700 logical-pixel panel, five query patterns
repeated four times, and 200 ms settling per query. Source baseline is c5bdd8e;
only metadata is generated, and no fake payload is exported. Dispatch timings
measure restore/filter entry CPU time. Paint/layout counts include descendant
widgets during settling, rather than display presentation. Runs are serialized
with this task's regression jobs.

| Scale | Median dispatch, before / after (ms) | P95 dispatch (ms) | Paint events | Layout requests |
| --- | --- | --- | --- | --- |
| 100% | 18.2163 / 0.9411 | 26.0778 / 4.2677 | 1205 / 454 | 301 / 68 |
| 200% | 19.3250 / 0.8108 | 24.0427 / 2.8234 | 1224 / 488 | 304 / 76 |

Raw samples and replay verification are in
[the validation record](validation/ela-2.2-performance.json). These are local
observations, not a throughput guarantee or a screen-FPS measurement.
Measurements were recorded at f377dda before the final compatible overlay/text-unit
fix; the catalog/model implementation measured here is unchanged by that fix.

The formal delivery is a runnable directory under AppSuite/Apps/xIPs. Build a
clean tagged release into a new staging directory, verify startup and native loading,
then let the coordinating task replace that application directory and update the
shared suite manifest/checksum inventory serially. No ZIP or old-package backup.
