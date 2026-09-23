# ElaWidgetTools source provenance

Source: <https://github.com/Liniyous/ElaWidgetTools>

Upstream revision: `454cac2d57a47d3cc28577dc817793aec1881ca7`

The `ElaWidgetTools/` library subtree was vendored without its example
application. Its MIT license and upstream README are preserved alongside
the source. The following ZeroSlack-specific changes are applied:

- Convert icon enum values explicitly when constructing `QChar`, as required
  by Qt 6.10.2.
- Omit four example images from the resource collection. ZeroSlack does not
  use Ela's sample user card, acrylic card, or Mica image features.
- Replace the unverified Fontello-generated `ElaAwesome.ttf` with the
  unmodified Font Awesome Free Solid 6.7.2 font (SIL OFL 1.1) and remap
  internally referenced icon values. The exact mapping is preserved in
  `patches/03-icon-font-swap.patch`; eleven unavailable icons use free alternatives.

Reconstruction: extract the library subtree at the pinned revision, then apply
`patches/01` through `06` in filename order. These patches reproduce all 417
files of the audited comparison source, including its historical formatting.
Use `git -c core.autocrlf=false apply` to retain the recorded file bytes.
Apply `patches/07-zeroslack-control-contracts.patch` next for the basic-control migration:
QIcon/mnemonic/keyboard/checked/focus/default-button painting, selected tool-button
icons, theme repaint, ownership of a removed combo-box layout item, and safe style
detachment before tool/combo/spin-box destruction. The latter includes the shared
popup/view and embedded line-edit styles; visible top-level teardown is tested.

Then apply `patches/08-zeroslack-tree-tab-contracts.patch`. It provides a native
Qt behavior mode for ElaTabBar: Qt owns variable tab sizing, overflow buttons,
selection, reordering and input, while Ela owns painting. The application keeps
its QTabWidget page ownership; ElaTabWidget's automatic close and tear-out
controllers are not used. Close icons use compact strokes.

ElaTreeView exposes a style factory for QTreeWidget hosts and a native item-content
mode that preserves model fonts/brushes, check states, wrapping, elision and focus.
Branch painting accepts Qt's generic style option, row sizes honor content height,
and painting/check-box hit regions use the same padding, applied only once so
automatic column sizing does not elide otherwise fitting labels. Tree/tab/scroll-bar styles
are QObject-owned and detached before widget teardown. Disabling scroll-bar
animation also restores immediate native wheel handling.

Apply `patches/09-zeroslack-radio-contracts.patch` after patch 08. Radio-button
styles are QObject-owned and detached before widget teardown. Disabled radio
labels and indicators use the disabled theme colors and ignore hover feedback.

Apply `patches/10-zeroslack-combo-popup-lifetime.patch` after patch 09. A combo's
shared style is application-owned and released with deleteLater after its popup,
view and line edit have been destroyed. It is not reset during combo teardown:
native popup repolishing from setStyle could invalidate children during Qt's
style propagation. This supersedes patch 07's combo style-detachment sequence.

Apply `patches/11-zeroslack-menu-item-view-contracts.patch` after patch 10. ElaMenu
adds an immediate Qt popup mode, retaining QAction ownership, mnemonics, checked
and exclusive actions, disabled state, icons, shortcuts and submenu navigation.
Qt measures item content and Ela paints the rounded popup and hover background.
ElaListView and ElaTableView expose style factories for existing QListWidget and
QTableWidget models. Native item-content mode preserves model fonts/brushes,
check indicators, wrapping, editing delegates, header labels and sort indicators.
List padding is shared by painting and check-box hit testing. Menu/table and
adapter-created list styles are application-owned and released with deleteLater
after their widgets are destroyed, including visible popup teardown.

Apply `patches/12-zeroslack-interruptible-scrolling.patch` after patch 11. ElaScrollBar
adds an opt-in wheel mode independent of scroll-range animation, with bounded targets,
immediate direction reversal, direct pixel gestures, and interruption by navigation,
range changes and hiding. ElaTabBar can animate variable-width Qt tabs without taking
over page ownership; close buttons and overflow arrows follow the current offset.
Selection, layout and drag operations supersede motion. Both animation objects are
owned, stopped on teardown, and use 160ms in the product adapter.

Apply `patches/13-zeroslack-appbar-host-contracts.patch` after patch 12. ElaAppBar
adds an opt-in external window-management mode for an existing native frame host.
It leaves flags, content margins, hit testing and close-event policy to the host,
while retaining Ela layout, title updates and window-button actions. Button and
title accessors support accessibility, path menus and sidebar integration.
Maximize icons follow Qt window-state changes; tool-button painting recognizes
native caption hover. Closing requests QWidget::close() only once, respecting a
veto and delete-on-close without nested processEvents or a second native close.
Window-button icons can be supplied without replacing Ela's state handling. The
close icon supports QIcon, content-based sizing, pressed/focus feedback and owned
hover animations. External layout assigns stretch to the title so a shrinkable
path label does not collapse to zero width. The application adapter is
`src/ui/uiwindowchrome.cpp`; ElaWindow
and its navigation/page controllers are not used.

Apply `patches/14-zeroslack-text-view-contracts.patch` after patch 13. ElaPlainTextEdit
adds an opt-in native text mode that leaves palettes and focus policy to the host.
The product adapter uses Qt-generated context actions in an ElaMenu. A single owned
focus animation can reverse immediately, and the style is application-owned until
widget and viewport teardown completes. NoFrame is respected; the native mode uses
the host's Base and Highlight palette roles. ElaScrollArea needs no upstream patch:
the adapter restores as-needed scrollbars without enabling drag-scroll gestures.

Apply `patches/15-zeroslack-label-palette-contracts.patch` after patch 14. ElaText
adds an opt-out from forced theme text colors, including its paint-time reset,
so host semantic and disabled palettes survive theme changes. The default upstream
behavior is unchanged. ZeroSlack's label adapter disables that override, restores
its own typography and native QLabel defaults, and clears constructor QSS/palette
overrides. QLabel continues to handle ordinary text layout, selection and links.

Apply `patches/16-zeroslack-navigation-content-host.patch` after patch 15.
ElaNavigationBar adds optional custom header/content hosting for existing navigation
models. It owns their vertical layout and stable-width sliding viewport, while its
native Maximal/Minimal/Compact modes drive the width transition. A single owned
animation supports reversal and instant settlement; generation-guarded completion
releases custom width constraints after layout has consumed the final frame.
The public mode/completion signals let an external dock synchronize its saved size
once at completion. The host may resize the expanded bar within its configured range.
ZeroSlack's adapter is `src/ui/navigationpanecoordinator.cpp`; it retains QDockWidget
only for existing window layout persistence. ElaWindow and its page routing remain
unused. This content-host API is a local extension, not an unmodified upstream API.

Apply `patches/17-zeroslack-navigation-width-batching.patch` after patch 16.
The owned width driver uses QVariantAnimation and one setFixedWidth per value.
It no longer first writes maximumWidth through QPropertyAnimation and then changes
both constraints in the value callback. Duration, easing, interruption and final
width restoration are unchanged; all animation ownership remains in Ela.

Apply `patches/18-zeroslack-navigation-composition-contract.patch` after patch 17.
The optional display-mode transition handler delegates presentation to a host
compositor, supplying the target width, Ela's 255 ms duration and a generation.
Late completions from a cancelled generation are ignored. Accepted transitions
do not animate the real bar width; the endpoint is applied once at completion.
Fixed endpoint constraints remain in place while the host commits its live layout.
The application compositor uses the same OutCubic curve on Windows DirectComposition;
stock Ela bars retain their existing width animation. This is a local API extension.

Apply `patches/19-zeroslack-hosted-tabs-and-floating.patch` after patch 18.
ElaTabWidget adds an opt-in hosted-document mode. Ela owns drag initiation, guarded
in-process MIME, five-way drop previews, transfer and floating windows with
ElaAppBar. The application supplies split-layout and return-target callbacks;
tab-close requests remain application-owned. Closing a floating container returns
its views without destroying documents. Pointer guards and controller scopes reject
foreign/stale drops, and empty source containers remain alive until drag completion.
The native tab geometry mode, tab visibility and host-provided tab bars are supported.
Stock ElaTabWidget ownership remains unchanged when hosting is not enabled.

ElaDragHandle is a local reusable gesture extension for hosted context panels.
The application retains its resource/placement model while Ela owns threshold,
cancel/release handling and MIME drag loops. Context floating windows use ElaAppBar
window management with the existing native Tool/DWM surface. This patch does not
introduce an ElaWindow shell or replace specialist editor/diagram content.

Apply `patches/20-zeroslack-scoped-acrylic.patch` after patch 19.
ElaApplication adds `applyWindowDisplayMode` for one existing native window,
without changing or registering the application's global display mode. This is
a local compatibility API that delegates to ElaWinShadowHelper's material path.
The caller opts out of global synchronization and retains refresh scheduling.
The native helper reports application failures, requires Windows 11 22H2 for the
system Acrylic attribute, extends the material over the full client area, and
explicitly clears the material and margins when returning to Normal. Theme is
applied after the material, matching Ela's existing global update order.
ZeroSlack's ContextFloatingWindow calls this API for Acrylic and solid fallback;
the application no longer sets native material attributes in its Ela build.
System accessibility/power preferences and the background tint remain host-owned.
The classic build retains its existing DWM path. The original MIT and font OFL
licenses remain unchanged and must accompany this patch in distributed packages.

Apply `patches/21-zeroslack-frameless-context-windows.patch` after patch 20.
ElaAppBar honors explicit Qt::FramelessWindowHint windows without retaining the
Windows non-client border. All four edges and corners use DPI-scaled client-area
resize hit regions; maximized windows use the current monitor's work area. Its
Windows 10 top-border painting is suppressed for this opt-in mode. Other Ela
windows retain their existing native-frame behavior. ZeroSlack enables the mode
on ContextFloatingWindow and keeps the Ela title bar, drag handling and controls.
Hidden native HWND checks exercise border calculation and edge hit testing without
showing windows or moving the cursor. Original MIT and font OFL licenses remain.

Apply `patches/22-zeroslack-compact-context-title-docking.patch` after patch 21.
ElaAppBar adds an opt-in native window-move notification lifecycle and explicit
title-icon visibility. The Windows move loop retains ownership of movement, resizing
and caption double-clicks. Only actual move messages expose docking targets; completion
is deferred until the native loop exits. Cancellation and an unchanged/restored native
rectangle reject docking. Qt title double-clicks use Ela's maximize/restore handler.
ZeroSlack removes floating-window action clutter and consumes these notifications to
preview or accept drops through its existing resource controller. Other Ela windows
do not enable tracking. MIT and font OFL licenses remain unchanged.

Apply `patches/23-zeroslack-checkbox-current-theme.patch` after patch 22.
ElaCheckBox paints checked and partially checked marks with the current theme's
selected foreground color. It no longer reads the other (dark) theme's text color,
so light/dark switches and custom palettes cannot leave stale indicator colors.
MIT and font OFL licenses remain unchanged.

Apply `patches/24-zeroslack-dialog-lifecycle.patch` after patch 23.
ElaContentDialog supports an optional parent and hiding its stock button row so
ZeroSlack can retain Qt button roles and explicit default/cancel decisions.
Accept, reject, close and hide synchronously dismiss the parent mask; closing no
longer enters a nested processEvents loop or closes the native window handle.
Delayed callbacks are scoped to the dialog. The mask follows parent resizing,
uses guarded ownership, and owns/cancels its opacity animations. Parentless
dialogs center on the current screen. Replacing central content releases removed
layout items. The lighter mask and conditional stock footer retain Ela painting.
The five changed files replay byte-for-byte from the preceding revision.
Original MIT and font OFL licenses remain unchanged.

Apply `patches/25-zeroslack-native-interaction-routing.patch` after patch 24.
This supersedes patch 18's optional external navigation compositor callback,
patch 19's hosted-tab gesture replacement, and patches 21/22's Context window
controller selection. It does not remove those APIs from other ElaAppBar users.

ElaNavigationBar again runs its own 255 ms OutCubic width animation. The 225 ms
OutCubic overlay-position animation is shared by ElaWindow and the application's
QMainWindow adapter through new overlay methods. The custom Files/Design content
host remains a local extension; it is not replaced by Ela's page-node model.

Hosted document tabs retain the original ElaTabBar gesture signals and
ElaTabWidgetPrivate enter/leave/drop routing. The ownership adapter floats the
real page immediately, merges on tab-bar entry, detaches on leave, and tracks
floating content at the upstream 10 ms cadence. Cancellation restores the original
index and tab metadata. Scope, page and source destruction and hidden workspace
tabs invalidate the drag. The application still owns split layout, document close
decisions, and workspace visibility. This is an adapted upstream interaction,
not an unmodified ElaFloatingWidget document owner.

ElaCentralStackedWidget exposes immediate settlement and switching state. Its
existing Popup, Scale, Flip and Blur effects use owned, interruptible animations;
rapid selection, reentrant selection, page deletion, hide and resize discard stale
pixmaps and restore the current live page. Popup begins immediately rather than
after the former 180 ms timer. ZeroSlack uses Popup for settings categories and
Problems/Activity page switches; editor and diagram rendering remain specialized.

ElaDockWidget exposes Qt dock-drag lifecycle signals, a gesture handoff, and
cancellation using QDockWidget's abort path. Qt owns floating movement, dock-area
preview and AnimatedDocks landing. The application subsequently transfers the same
view to its existing ordered/resizable Context sections. Resource ownership,
parallel layout persistence, insertion ordering, scroll preservation, compact
title controls and scoped Acrylic remain application responsibilities. The
previous ContextDockTransition snapshot landing is inactive in the Ela build.
The original MIT and font OFL licenses are unchanged.

For the patch-25 baseline, the product adapter in `src/ui/uicontrols.cpp` releases fixed dimensions, restores
ZeroSlack typography, updates per-button theme colors, supplies focus outlines,
and uses Qt's immediate combo popup lifecycle with Ela's style. This avoids
upstream's non-interruptible popup animation. No recursive application event
filter or protected-surface traversal is installed for control styling. Hosted tabs
and drag handles use scoped application input filters for cross-window gestures;
they do not traverse or restyle document content.
Selected browsing viewports route precision pixel gestures to the matching Ela
scrollbar and cancel motion on key or pointer input. This filter is local to those views.
The numeric-control adapter sizes the inline step buttons and input area together,
including prefixes, suffixes and input padding; it does not assume Qt's narrower
native step-button geometry.

The ordinary tooltip adapter intercepts Qt help events and explicit gutter hints
to present an ElaToolTip. It does not traverse or restyle editor/diagram content.
Qt retains hover timing; the adapter bounds the popup to the screen, wraps long
paths, avoids focus activation, and clears stale tips on input and owner changes.
Interactive symbol information cards remain application-owned.

This integration is pinned to Qt 6.10.2 because ElaTabBar and the dock-drag state
observer include Qt private headers. The observer reads QDockWidgetPrivate state;
it does not invoke or link private Qt functions. Rebuild both DLLs and rerun
validation before changing the Qt version.
Upstream CMake declares version 2.0.0 while its public header declares 2.0.3;
the commit identifier above is the authoritative source version.

Apply `patches/26-zeroslack-interruptible-drawers-controls.patch` after patch 25.
ElaDrawerArea retains its upstream translated/faded body snapshot and header
rotation, with owned animations, four drawer edges, header visibility, immediate
settlement, completion signals and bounded capture diagnostics. Reversal reuses
the current body snapshot; resize, hide and input settle the latest requested
state. Snapshots are limited to 32 MiB and released on completion. The application
uses these APIs for the right dock, bottom drawer and individual Context sections;
the old PanelCompositor is not used for those Ela paths. This is an extension of
the upstream drawer, not an unmodified upstream dock-layout controller.

ElaComboBox again uses its original popup-height, view-position and indicator
animations. Owned groups replace unowned callbacks and the non-interruptible hide
gate. Opening takes 180 ms, indicators 150 ms; selection, dismissal and Escape
close immediately and restore the live view layout. Input, hide, resize and
destruction cancel pending transitions. The shared popup style remains alive
through Qt popup/view teardown, without repolishing children from the destructor.
ElaToolBar applies the same deferred style-lifetime protection to action widgets.

ElaTreeView exposes finishExpansion(QTreeView*) to finish Qt's existing expansion
animation before new input. Qt otherwise ignores some pointer input while that
animation is active. This reads QTreeViewPrivate::animatedOperation and advances
the existing animation to completion; it does not link private Qt functions.
The existing exact Qt 6.10.2/WidgetsPrivate dependency therefore also covers trees.

The application enables Ela smooth wheel scrolling on ordinary browsing views,
while programmatic value changes remain immediate. Professional editor/diagram
coordinates retain their own interaction semantics. Ordered Context resizing now
uses Qt QSplitter, which is not an Ela class; document and workspace ownership,
selection and persistence remain application responsibilities. Patch 26 replays
13 vendor source files byte-for-byte after line-ending normalization. MIT and
font OFL license text and attribution are unchanged.

Apply `patches/27-zeroslack-interruptible-menu-popup.patch` after patch 26.
ElaMenu restores its upstream snapshot-position reveal even when native Qt item
content is enabled. The animation is owned by the menu, runs for 160 ms with
OutCubic easing, and is settled immediately on pointer/keyboard input, resize,
hide, action or theme changes. Qt continues to own placement, action dispatch,
shortcuts, checked/disabled state and submenu navigation. Submenus inherit the
native-item policy. Menu capture is bounded to 8 MiB; menus containing live
QWidgetAction editors remain live without a snapshot. Destruction cancels the
animation. This is an adapted upstream reveal, not a replacement menu gesture
system. The three source files replay byte-for-byte. MIT/OFL remain unchanged.

The shared ZeroSlack/xIPs native browser contract identifies this capability
baseline as `ela=454cac2d-p27`; rebuild both packages together. This supplements
the existing Qt, pointer-size and compiler ABI checks and does not change the
native surface v1 data contract.

Apply `patches/28-xips-list-style-lifetime.patch` after patch 27.
ElaListView retains its style until Qt has finished destroying the view, clearing
focus and releasing children. The style is owned by QApplication and queued for
deletion after the view's destroyed signal, matching the combo/toolbar lifetime
protection. A focused, scrolled list previously accessed the deleted style from
QWidget teardown. This changes no public API or object layout; the capability ABI
remains `454cac2d-p27`, while the source fingerprint identifies this additional fix.
The xIPs interaction regression retains focused list teardown and pending input.

Apply `patches/29-wave-overlay-origin-lifetime.patch` after patch 28.
Overlay scrollbar origin/area references are QPointers. Replacing or destroying
the origin removes the observer, stops pending wheel motion and hides the overlay;
queued value/geometry callbacks guard expired objects. This private implementation
fix preserves the p27 public capability ABI. The application keeps QPlainTextEdit
pixel gestures in Qt's native text units while angle-only input uses Ela motion.

Both `LICENSE` (ElaWidgetTools) and `Font/FontAwesome-LICENSE.txt` must
accompany redistributed binaries.
