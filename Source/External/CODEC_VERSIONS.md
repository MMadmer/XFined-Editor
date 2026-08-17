# Vendored codec sources

This manifest records the exact upstream material vendored for the XFined Editor Windows build. Release archives were verified before extraction, and only library sources, headers, and licenses are retained.

| Component | Version | Authoritative source | SHA-256 | License |
| --- | --- | --- | --- | --- |
| libogg | 1.3.6 | `https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.gz` | `83e6704730683d004d20e21b8f7f55dcb3383cdf84c0daedf30bde175f774638` | `Ogg/COPYING` (`d2ab5758336489da61c12cc5bb757da5339c4ae9001f9bb0562b4370249af814`) |
| libvorbis | 1.3.7 | `https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz` | `b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b` | `Vorbis/COPYING` (`ec1815db59fcd302846df949d7424876cb2e2dc5ed1606c5fb0b36787b1cf43a`) |
| libtheora | 1.2.0 | `https://downloads.xiph.org/releases/theora/libtheora-1.2.0.tar.gz` | `279327339903b544c28a92aeada7d0dcfd0397b59c2f368cc698ac56f515906e` | `Theora/COPYING` (`8417fad7da775735564e209484a2e011e0fa201e94f01fdbee6e4977e478e6fc`) |
| zlib and MiniZip | 1.3.2 | clean local mirror of `madler/zlib` tag `v1.3.2`, commit `da607da739fa6047df13e66a2af6b8bec7c2a498` | source commit | `zlib/LICENSE` (`439c75ab12b340c5362b9d4b08ff05ec3a4d0eb7667a6fff49a9b16d8795c78e`); `zlib/LICENSE.Info-Zip` (`a6b3dd98c9eee4c6213ffcdbbe5fb782815d0f701059e6c9f2af18740ce6ab98`) |

## Retained source subsets

- Ogg retains `src/bitwise.c`, `src/framing.c`, `src/crctable.h`, and the two public Windows-compatible headers from `include/ogg`.
- Vorbis retains the library sources declared by upstream `lib/CMakeLists.txt`, the internal header set, the `books` and `modes` data trees, and all three public headers. Standalone tuning utilities are excluded.
- Theora retains all full encoder/decoder library sources and internal headers from `lib`, except the mutually exclusive `encoder_disabled.c` stub. The official MSVC `lib/x86_vc` subtree is retained as upstream reference material, but is excluded from the Win64 target because MSVC does not support its 32-bit inline assembly.
- zlib retains the core source list declared by upstream `CMakeLists.txt`, plus the MiniZip library and Win32 I/O sources and their required headers.

## Local compatibility patch

The Theora `oc_pack_bytes_left` return type remains 64-bit on `_WIN64`. This reapplies the editor's proven x64 fix from commit `e7438831ce24268a2995d2f7ed4f6c0949580c11` to the 1.2.0 source and prevents truncating a pointer-distance result to Windows' 32-bit `long`.
