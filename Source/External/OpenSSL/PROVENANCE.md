# OpenSSL 3.5.7 provenance

XFined Editor uses the unmodified OpenSSL 3.5.7 `libcrypto` binary built from
the official upstream source release. OpenSSL 3.5 is the upstream LTS branch.

## Source authentication

- Source archive:
  `https://github.com/openssl/openssl/releases/download/openssl-3.5.7/openssl-3.5.7.tar.gz`
- Official checksum file:
  `https://github.com/openssl/openssl/releases/download/openssl-3.5.7/openssl-3.5.7.tar.gz.sha256`
- Source archive SHA-256:
  `A8C0D28A529CA480F9F36CF5792E2CD21984552A3C8E4AA11A24AA31AEAC98E8`

The bundled `LICENSE.txt` is copied byte-for-byte from that archive. Its
SHA-256 is
`7D5450CB2D142651B8AFA315B5F238EFC805DAD827D91BA367D8516BC9D49E7A`.

## Rebuild

Run `build-openssl-3.5.7.ps1` from PowerShell 7 with fresh work and install
directories. The recipe downloads and verifies every input before extraction.

Build inputs used for the bundled artifacts:

- Strawberry Perl 5.42.2.1 portable x64, SHA-256
  `32D83BE90CF04B807CFB9477482BC36302CDEE6F5B04CF57E81ADECBD8F07898`
  from the official Strawberry Perl release.
- NASM 3.02 x64, SHA-256
  `161D0BFAFF53C2F9E9F3E69FD0672323EBABAFD1268976A5CEC11BE92A19AEE7`
  from the official NASM release server.
- Visual Studio Community 2026, MSVC 19.50.35729, x64 tools.

Configure arguments:

```text
VC-WIN64A shared no-makedepend
```

The recipe runs `nmake`, the complete upstream `nmake test` suite, and
`nmake install_sw`. The 2026-08-17 build completed 344 test files and 4,272
tests with `Result: PASS`.

## Bundled artifacts

- `bin/x64/libcrypto-3-x64.dll`:
  `C9043E500A5D3480506A0FFE491092C5C28A04E005EE2FFA61CF2B40A3CF0CFA`
- `lib/x64/libcrypto.lib`:
  `CB633D6EE1CA8E571C131BCEFBC7D2EE84FF47E23071978FB1E5E906B8238983`
- `include/openssl/configuration.h`:
  `FD7A1AFEC84DC20BED06740932580C69F81F974B7F6D0AAA468E5ADFAB33959E`
- `include/openssl/opensslv.h`:
  `15005B91BAF4983436C87B415675B3DB6EEB3DBF9E6655DB3CBAFC386A3E02CC`

The complete installed public header set is included. OpenSSL applications,
`libssl`, engines, external provider modules, configuration files, and debug
symbols are not distributed.

## Compatibility boundary

OpenSSL supplies provider-backed DSA and operating-system RNG access. The
historical public `crypto::xr_sha256` type intentionally remains a first-party
SHA-0 implementation because old XFined/game signatures serialize that exact
20-byte digest. The wrapper disables OpenSSL configuration loading and uses
the built-in default provider, so no external provider or configuration file
is required at runtime.
