# 2.1 change map

| Area | Files |
| --- | --- |
| Immutable schema 2 storage | src/library/SnapshotLibrary.h, .cpp |
| Shared English Ela surface | src/app/BrowserPanel.h, .cpp |
| Standalone window | src/app/MainWindow.h, .cpp; src/app/main.cpp |
| Native public boundary | include/xips/BrowserApi.h; src/app/BrowserApi.cpp |
| Schema 2 CLI support | src/cli/main.cpp |
| Build and Ela dependency | CMakeLists.txt; src/CMakeLists.txt; thirdparty/elawidgettools |
| Version and native tests | tests/tst_snapshots.cpp; tests/tst_native.cpp |
| English panel and workflow tests | tests/tst_gui_smoke.cpp; tests/tst_user_journey.cpp |
| CLI regressions | tests/tst_cli.cpp |
| AppSuite provider | src/integration/SuiteIntegration.h, .cpp; tests/tst_suite.cpp |
| Application icon | assets/icons; assets/xips.qrc; src/app/Branding.h, .cpp |
| Formal packaging | scripts/package-release.ps1; CHANGELOG.md; THIRD-PARTY-NOTICES.md |
| Product and usage documentation | README.md; GOAL.md; PLAN.md; docs/*.md |

The legacy core, manifest reader, and its regression suite remain for compatibility and explicit migration.
The old table/controller files are no longer part of the application build.

ZeroSlack's matching changes are in src/integrations/xips, its context-provider registration, CMake, and test_sv/xips_context_provider_test.cpp.
