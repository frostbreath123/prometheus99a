# Third-party components

| Component | Version (pinned) | Licence | How used | Location |
|---|---|---|---|---|
| LVGL | v9.2.2, commit `7f07a129e8d77f4984fff8e623fd5be18ff42e74` | MIT | statically compiled from source; the configuration (`config/lv_conf.h`) keeps only the object, label, button and chart widgets, the RGB565 software renderer and no theme of its own | `third_party/lvgl` (git submodule, see `.gitmodules`; `LICENCE.txt`) |
| libmodbus | v3.1.10, commit `2cbafa3113e276c3697d297f68e88d112b53174d` | LGPL-2.1-or-later | the development build (`Makefile.linux`) links the system library dynamically; the small build (`STATIC=1`) and the Windows test builds compile `modbus.c`, `modbus-tcp.c` and `modbus-data.c` from the source below and link them into one static executable (see the LGPL note) | `third_party/libmodbus` (git submodule, `COPYING.LESSER`), build config in `third_party/libmodbus_cfg` |
| jsmn | v1.1.0 | MIT | single header JSON tokenizer | `third_party/jsmn` |
| Monocypher | 4.0.2 | BSD-2-Clause or CC0-1.0 (dual, choose either) | SHA-512, HMAC-SHA-512, constant-time comparison and memory wipe, compiled from source; used only through `src/hmi/hmi_mac.c` and `src/hmi/hmi_util.c`. The Argon2id password hash it also contains is not used and not linked (the password hash was removed, `docs/AUTHENTICATION.md`) | `third_party/monocypher` (`monocypher.c`, `monocypher.h`, `LICENCE.md`) |
| musl libc | Debian/Ubuntu package `musl-tools` 1.2.4 | MIT | the C library of the small build: linked statically into the executable | system package, not in the repository |
| DejaVu Sans (as a bitmap font) | the file `scripts/built_in_font/DejaVuSans.ttf` of the LVGL checkout | Bitstream Vera licence with the DejaVu changes (free to use, copy, modify and embed; the name "DejaVu" or "Bitstream Vera" may not be used for a modified font) | the only font of the UI: rendered to 14 px, 1 bit per pixel, printable ASCII, into `src/ui/font/p99_font.c` with `lv_font_conv`; the generated array is named `p99_font` | `src/ui/font` (`README.md` gives the command and the licence note) |

Licence notes
- LGPL and static linking: the small build contains libmodbus in the same file as the rest of the program. LGPL-2.1
  allows that only if the recipient can relink the program with a modified libmodbus. For a delivered device that means
  delivering the object files (or the source and these Makefiles) together with the executable; `make -f Makefile.linux
  STATIC=1 CC=musl-gcc hmi` does the relinking from source. The development build links the system library dynamically and
  needs nothing extra. This has not been reviewed by a lawyer.
- No licence has been legally reviewed. This table is an engineering record, not legal advice. An SBOM should be
  generated before shipping.
- Test-only tools: none beyond the in-repo fake Modbus server (`tests/hmi/fake_modbus_server*.c`, `fake_plc_main.c`).
- Build tools used once, not part of the product: `lv_font_conv` (npm, MIT) to make the font file.

Monocypher provenance (recorded 2026-10-22)
- Source: release tarball https://monocypher.org/download/monocypher-4.0.2.tar.gz,
  SHA-256 of the tarball `38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c`.
- Vendored files, SHA-256: `monocypher.c`
  `afe2b098c8569577a84488e0b98d276d1fba6506adea68bb9241a52111734c59`, `monocypher.h`
  `f78bb31255cfb7beba66afd2137f5194c8a025cf40488b6cc1e295234d43f374`, `LICENCE.md`
  `5f8360e4c06ddcc584bdb4b210c6af824c4bb301e6a9a521869b6d90795ca4b3`.
- Cross-check: the GitHub tag archive for 4.0.2 (https://github.com/LoupVaillant/Monocypher/archive/refs/tags/4.0.2.tar.gz)
  differs in `monocypher.c` and `monocypher.h` only in the first line (version stamp
  `__git__` instead of `4.0.2`). The upstream site publishes no checksum, so the
  tarball hash above is a local record, not an upstream-signed value.
- Files are unmodified. The licence file says the project is dual-licensed (2-clause BSD or CC0).
- Correctness check in this repository: the Argon2id known-answer test (RFC 9106) was removed with the hash;
  the SHA-512 and HMAC tests below remain.

Monocypher optional files (added for milestone M2, recorded 2026-10-22)
- `third_party/monocypher/monocypher-ed25519.c` and `monocypher-ed25519.h` come from
  `src/optional/` of the same 4.0.2 release tarball, unmodified. They provide
  SHA-512 and HMAC-SHA-512, used only through `src/hmi/hmi_mac.c`. The ed25519
  signature functions in the same file are compiled but not used.
- SHA-256: `monocypher-ed25519.c`
  `7c9b16056cbd27521919e8a6f56a228808b9e718afc42e3d33f28c08e5abdee2`, `monocypher-ed25519.h`
  `bd546edcd468d64e28caa3dbf4b1d6bfad7435c0ce994723fd81aae26405121b`.
- Same dual licence as the rest of Monocypher (2-clause BSD or CC0).
- Correctness check in this repository: `tests/hmi/test_mac.c` reproduces RFC 4231
  test cases 1, 2, 3 and 6 for HMAC-SHA-512 (also cross-checked with Python's `hmac`).
