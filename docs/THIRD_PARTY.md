# Third-party components

| Component | Version (pinned) | Licence | How used | Location |
|---|---|---|---|---|
| LVGL | v9.2.2, commit `7f07a129e8d77f4984fff8e623fd5be18ff42e74` | MIT | statically compiled from source | `third_party/lvgl` (shallow clone, `LICENCE.txt`) |
| libmodbus | v3.1.10, commit `2cbafa3113e276c3697d297f68e88d112b53174d` | LGPL-2.1-or-later | **host/Windows test builds compile modbus.c, modbus-tcp.c, modbus-data.c directly**; the Linux target (`Makefile.linux`) links the system library dynamically | `third_party/libmodbus` (`COPYING.LESSER`), build config in `third_party/libmodbus_cfg` |
| jsmn | v1.1.0 | MIT | single header JSON tokenizer | `third_party/jsmn` |
| Monocypher | 4.0.2 | BSD-2-Clause or CC0-1.0 (dual, choose either) | Argon2id password hashing and constant-time helpers, compiled from source; used only through `src/hmi/hmi_kdf.c` | `third_party/monocypher` (`monocypher.c`, `monocypher.h`, `LICENCE.md`) |

Licence notes
- LGPL: compiling libmodbus into a test binary on the host is a development
  convenience. A product release must link libmodbus dynamically (or provide
  relinkable objects) to satisfy LGPL-2.1.
- No licence has been legally reviewed. This table is an engineering record,
  not legal advice. An SBOM should be generated before shipping.
- Test-only tools: none beyond the in-repo fake Modbus server.

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
- Correctness check in this repository: `tests/hmi/test_kdf.c` reproduces the RFC 9106
  section 5.3 Argon2id test vector through the wrapper.
