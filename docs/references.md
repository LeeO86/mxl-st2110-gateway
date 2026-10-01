# References

Pinned versions of the documents and sources the implementation was checked against (SPECIFICATION.md §2). `// VERIFIED:` comments in the code cite these pins.

## Software (pins: `docker/Dockerfile`, `.github/workflows/ci.yaml`)

| Source | Pin | Used for |
|---|---|---|
| [Intel MTL](https://github.com/OpenVisualCloud/Media-Transport-Library) | `v26.09` (`VERSION` 26.09.0.REL) | st20p/st30p/st40p pipelines, external frames, kernel-socket backend, built-in PTP; `doc/design.md`, `doc/external_frame.md`, `doc/run.md`, `doc/e800_series_drivers.md`, `doc/kernel_socket.md` |
| [DPDK](https://github.com/DPDK/dpdk) | `v26.07` + MTL `patches/dpdk/26.07` | ice PMD DDP loading (`drivers/net/intel/ice/ice_ethdev.h`, `ice_load_pkg`), ethdev |
| [MXL SDK](https://github.com/dmf-mxl/mxl) | `v1.1.0` | flow API, sync groups, index conversion; `docs/Architecture.md`, `docs/Timing.md`, `docs/Configuration.md`, `docs/Tools.md`, `docs/Addressability.md` |
| [Sony nmos-cpp](https://github.com/sony/nmos-cpp) | `fe303849527394b03bdedc8f161f377fe458bb62` | node server, IS-05 MXL transport (`nmos/mxl.h`), `make_connection_mxl_sender/receiver`, SDP utilities |
| [AMWA nmos-testing](https://github.com/AMWA-TV/nmos-testing) | `90018513758e6fbc096c26d7ef001f700b28b510` | IS-04-01, IS-05-01, IS-05-02, BCP-007-03-01 (`nmostesting/suites/BCP0070301Test.py`) |
| Intel `ice` driver tarball | 2.6.7 (download id 923747) | E810 DDP package `ice-1.3.59.0.pkg` |
| [mxl-fabrics-agent](https://github.com/LeeO86/mxl-fabrics-agent) `SPECIFICATION.md` | `3b981c6` (Draft v0.1) | §5 domain scanning, §7 mirror domains, §9 timing, §10 ports, §11 requirements on media functions |
| [mxl-decklink](https://github.com/LeeO86/mxl-decklink) | `main` at 2026-10-01 (`eb8ea05`) | structure, CI and tagging model, MXL/nmos-cpp patterns |

## Specifications

| Document | Version |
|---|---|
| AMWA BCP-007-03 NMOS With MXL | v1.0.0 (tag `v1.0.0`, commit `76763d56`): `NMOS-With-MXL.md`, `mxl_domain_definition.json`, `sender_transport_params_mxl.json`, `receiver_transport_params_mxl.json`, `constraints-schema-mxl.json` |
| AMWA IS-04 Discovery and Registration | v1.3 |
| AMWA IS-05 Device Connection Management | v1.1 and v1.2 (MXL transport only under v1.2) |
| AMWA BCP-002-01 Natural Grouping | v1.0 |
| AMWA BCP-004-01 Receiver Capabilities | v1.0 |
| SMPTE ST 2110-10 / -20 / -21 / -30 / -40 | 2022 / 2022 / 2022 / 2017 / 2023 |
| SMPTE ST 2022-7 Seamless Protection Switching | 2019 |
| SMPTE ST 2059-1 / -2 | 2021 / 2021 |
| IETF RFC 4175 (uncompressed video over RTP), RFC 8331 (ST 291 ANC over RTP), RFC 7273 (RTP clock source signalling), RFC 8866 (SDP) | — |
| IEEE 1588-2008 (PTPv2) | — |
