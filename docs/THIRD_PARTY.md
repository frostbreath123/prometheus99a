# Third-party components

| Component | Version (pinned) | Licence | How used | Location |
|---|---|---|---|---|
| LVGL | v9.2.2, commit `7f07a129e8d77f4984fff8e623fd5be18ff42e74` | MIT | statically compiled from source | `third_party/lvgl` (shallow clone, `LICENCE.txt`) |
| libmodbus | v3.1.10, commit `2cbafa3113e276c3697d297f68e88d112b53174d` | LGPL-2.1-or-later | **host/Windows test builds compile modbus.c, modbus-tcp.c, modbus-data.c directly**; the Linux target (`Makefile.linux`) links the system library dynamically | `third_party/libmodbus` (`COPYING.LESSER`), build config in `third_party/libmodbus_cfg` |
| jsmn | v1.1.0 | MIT | single header JSON tokenizer | `third_party/jsmn` |

Licence notes
- LGPL: compiling libmodbus into a test binary on the host is a development
  convenience. A product release must link libmodbus dynamically (or provide
  relinkable objects) to satisfy LGPL-2.1.
- No licence has been legally reviewed. This table is an engineering record,
  not legal advice. An SBOM should be generated before shipping.
- Test-only tools: none beyond the in-repo fake Modbus server.
