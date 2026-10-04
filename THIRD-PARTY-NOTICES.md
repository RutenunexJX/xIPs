# Third-party components

Original xIPs application code is Apache-2.0; see LICENSE and NOTICE. The
following components retain their own licenses.

xIPs uses Qt 6.10.2, ElaWidgetTools from the ZeroSlack fork, the local SuiteApp
SDK, and the MinGW 13.1 compiler runtime. Qt and Ela remain separate libraries.
The SuiteApp SDK version is recorded in build-info.json; the shared AppSuite
runtime is distributed separately under Apps/Runtime.

The portable package's licenses directory contains Qt's LGPLv3 text, Ela's MIT
license and source provenance, Font Awesome's license, and the compiler runtime
licenses and exceptions. Ela source and its upstream revision are retained in
thirdparty/elawidgettools in the source repository.

The xIPs icon was produced with the built-in image generation tool. Its design
brief and final generation prompt are in assets/icons/DESIGN.md.

The compatibility changes imported from ZeroSlack are attributed separately
under Apache-2.0 in `thirdparty/elawidgettools/ZeroSlack-Apache-2.0.txt`.
The Ela upstream remains MIT and its unmodified Font Awesome Free font remains
SIL OFL 1.1. Keep the source provenance and patch records with these notices.

The SuiteApp SDK/runtime is separately supplied. This project's license does
not grant rights to redistribute it. Any public binary release must also provide
corresponding sources and required relinking/installation information for the
actual Qt libraries distributed; including LGPL text alone is not sufficient.
See https://www.qt.io/development/open-source-lgpl-obligations,
[asset provenance](docs/ASSET-PROVENANCE.md) and the remaining
[public-release checks](docs/PUBLIC-RELEASE-REVIEW.md).
