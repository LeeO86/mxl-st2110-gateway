# Third-party notices

`mxl-st2110-gateway` is licensed under the MIT license (`LICENSE`). The container image and the source tree contain or link the following third-party components. Versions are pinned in `docker/Dockerfile` (mirrored in `.github/workflows/ci.yaml`). The full license texts ship with each component's sources; the image keeps them under `/usr/share/doc` where the Ubuntu packages install them.

## Linked into the gateway binary or shipped in the image

| Component | Version | License | Use |
|---|---|---|---|
| [Intel Media Transport Library (MTL)](https://github.com/OpenVisualCloud/Media-Transport-Library) | v26.09 + `patches/mtl/*` | BSD-3-Clause | ST 2110 sessions, built-in PTP |
| [DPDK](https://www.dpdk.org/) | 26.07 + MTL's DPDK patches | BSD-3-Clause (user-space libraries and PMDs; no kernel modules are shipped) | NIC access |
| [MXL SDK](https://github.com/dmf-mxl/mxl) | v1.1.0, Fabrics OFF | Apache-2.0 | MXL domains, flows, `mxl-info`, `mxl-data-probe` |
| MXL build dependencies via vcpkg (statically linked into `libmxl`): spdlog, fmt, picojson, stduuid, Microsoft GSL, ada-url, CLI11, libuuid, PcapPlusPlus, libpcap | MXL's vcpkg baseline | MIT (spdlog, fmt, GSL, stduuid, CLI11), BSD-2-Clause (picojson), Apache-2.0 / MIT (ada), BSD-3-Clause (libuuid, libpcap), Unlicense (PcapPlusPlus) | MXL internals and tools |
| [Sony nmos-cpp](https://github.com/sony/nmos-cpp) | `fe303849527394b03bdedc8f161f377fe458bb62` | Apache-2.0 | IS-04 / IS-05 node, BCP-007-03 MXL transport |
| nmos-cpp bundled: json-schema-validator (pboettch), jwt-cpp, mdns (Apple mDNSResponder client shim) | as bundled at the nmos-cpp pin | MIT, MIT, Apache-2.0 | schema validation, IS-10 support code, DNS-SD |
| [C++ REST SDK (cpprestsdk)](https://github.com/microsoft/cpprestsdk) | Ubuntu 24.04 `libcpprest2.10` | MIT | HTTP server/client for nmos-cpp |
| [WebSocket++](https://github.com/zaphoyd/websocketpp) | Ubuntu 24.04 `libwebsocketpp-dev` (headers) | BSD-3-Clause | nmos-cpp |
| [Boost](https://www.boost.org/) (system, thread, chrono, date_time, regex, random, filesystem) | Ubuntu 24.04 1.83 | BSL-1.0 | cpprestsdk / nmos-cpp |
| Avahi (`libavahi-compat-libdnssd`, `libavahi-client`) | Ubuntu 24.04 | LGPL-2.1-or-later (dynamically linked) | DNS-SD registry discovery |
| [OpenSSL](https://www.openssl.org/) | Ubuntu 24.04 3.0 | Apache-2.0 | UUIDv5 (SHA-1), ETags (SHA-256), TLS |
| [nlohmann/json](https://github.com/nlohmann/json) | Ubuntu 24.04 3.11 (headers) | MIT | JSON |
| json-c, libnuma, libpcap | Ubuntu 24.04 | MIT, LGPL-2.1 (dynamically linked), BSD-3-Clause | MTL / DPDK runtime dependencies |
| Intel E810 DDP package (`ice.pkg`) | from the `ice` 2.6.7 driver tarball | Intel Limited License (redistributed with its license text, `/usr/share/doc/intel-ice-ddp/LICENSE` in the image) | DPDK ice PMD |
| [Vue.js](https://vuejs.org/) | 3.5 (bundled into the embedded admin UI) | MIT | admin web UI |

## Build and test only (not shipped)

| Component | License | Use |
|---|---|---|
| [doctest](https://github.com/doctest/doctest) 2.4.12 (`third_party/doctest`) | MIT | unit tests |
| Vite, `@vitejs/plugin-vue`, `vite-plugin-singlefile`, Vitest | MIT | building and testing the web UI |
| LLVM / clang compiler-rt (libFuzzer, sanitizers) | Apache-2.0 WITH LLVM-exception | `tests/fuzz` |
| vcpkg | MIT | MXL dependency build |
| AMWA [nmos-testing](https://github.com/AMWA-TV/nmos-testing) | Apache-2.0 | conformance tests (`tests/integration/nmos-testing.sh`) |
| Python `jsonschema`, `PyYAML` | MIT | `tools/validate_configs.py` |

## Not used

No GStreamer, no libfabric / rdma-core (host-to-host replication is done by mxl-fabrics-agent in its own container).
