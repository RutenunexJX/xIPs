# 7-Zip standalone runtime

- Version: 26.03, Windows x64, unmodified `x64/7za.exe` from 7-Zip Extra.
- Official download: https://github.com/ip7z/7zip/releases/download/26.03/7z2603-extra.7z
- Download SHA-256 (checked against the official GitHub release asset digest): `191894e6acb3647ffb69ce630479ff318523b2e2b9890aa7f05c1127c2e59b8f`.
- Executable SHA-256: `edbee35370e14030e4c785cf88200f42dc651c1eb4217c1e3963c38a12f099b0`.
- Corresponding source: https://github.com/ip7z/7zip/releases/download/26.03/7z2603-src.tar.xz (included alongside this notice).
- Source archive SHA-256: `9cbde5099c6deb73691b0579063da5827522ccbbcba3f0020fd04e8c8c16c0d4`.
- Upstream license and distribution information are preserved in `License.txt` and `readme.txt`. This standalone executable has no external 7-Zip DLL dependency.

The application invokes this helper directly for 7z creation and integrity testing. It does not install shell extensions or change the system's archiver configuration.
