# Conformance

AMWA [nmos-testing](https://github.com/AMWA-TV/nmos-testing) at the pinned commit `90018513758e6fbc096c26d7ef001f700b28b510`, run non-interactively by `tests/integration/nmos-testing.sh` (SPECIFICATION.md §7.7): the gateway on the MTL kernel-socket backend with one ingest group (1 video, 8-channel audio, ANC, ST 2022-7) and one egress group, the tool's mock registry announced over multicast DNS-SD through the host's Avahi (owner decision Q13). The gateway runs two NMOS nodes (§7.1); the suites run in two phases, each with only the node under test using DNS-SD (`node.registry.dns_sd` or `node.st2110.registry.dns_sd`), because the tool's mock registry must see a single node — as on the platform, where only the MXL node registers with the MXL registry. The JSON results are CI artifacts (`integration-nmos-testing`).

## Results

Run of 2026-10-03 on the image built for v1.0.0 (branch `cursor/platform-v1-fa5c`):

| Node | Suite | API versions | Pass | Fail | Warning | Could not test | Manual | Not implemented | Disabled | N/A |
|---|---|---|---|---|---|---|---|---|---|---|
| MXL | IS-04-01 Node API | Node v1.3 | 54 | **0** | 0 | 2 | 1 | 4 | 10 | 3 |
| MXL | IS-05-01 Connection API | v1.2 | 53 | **0** | 0 | 9 | 0 | 0 | 7 | 12 |
| MXL | IS-05-02 Interaction with IS-04 | Node v1.3, Connection v1.2 | 55 | **0** | 0 | 1 | 0 | 0 | 14 | 3 |
| MXL | BCP-007-03-01 NMOS With MXL | Node v1.3, Connection v1.2 | 55 | **0** | 0 | 0 | 1 | 0 | 14 | 3 |
| ST 2110 | IS-04-01 Node API | Node v1.3 | 55 | **0** | 0 | 0 | 1 | 5 | 10 | 3 |
| ST 2110 | IS-05-01 Connection API | v1.1 | 61 | **0** | 0 | 0 | 0 | 0 | 8 | 12 |
| ST 2110 | IS-05-01 Connection API | v1.2 | 61 | **0** | 0 | 0 | 0 | 0 | 8 | 12 |
| ST 2110 | IS-05-02 Interaction with IS-04 | Node v1.3, Connection v1.2 | 56 | **0** | 0 | 0 | 0 | 0 | 14 | 3 |

## Non-pass results

| Suite / test | State | Reason |
|---|---|---|
| all `auto_node_17`–`23`, `auto_connection_23`–`29` | Disabled | IS-10 authorization is not implemented (§1.3: no authentication in v1) |
| IS-04-01 `test_02`, `test_02_01` | Disabled | unicast DNS-SD tests; CI uses multicast DNS-SD (Q13) |
| IS-04-01 `test_12` | Disabled | not applicable to Node API v1.3 |
| IS-04-01 (MXL node) `test_13`, `test_14` | Could not test | the MXL node has no RTP Receivers (they are on the ST 2110 node, which passes both) |
| IS-04-01 `test_19_01` | Not implemented | Node `interfaces` carry no `attached_network_device` (LLDP data of the switch port is not known to the gateway) |
| IS-04-01 `test_22` | Manual | "Node resource IDs persist over a reboot": all ids are UUIDv5 derivations of the persisted `node.id` (or `node.seed`) and the essence `uid`s (§7.3; unit tests `test_ids.cpp`, `test_config.cpp`, `test_store.cpp`; `tests/integration/lifecycle.sh` restarts with the same ids) |
| IS-04-01 `test_27_4`–`27_6` | Not implemented | BCP-004-01 constraint sets have no labels, preferences or enabled flags (one exact constraint set per receiver, §7.2) |
| IS-04-01 `test_28` | Not implemented | no BCP-002-02 asset tags |
| IS-05-01 (MXL node) `test_09_01`, `test_11`, `test_11_02`, `test_12`, `test_12_02`, `test_15`, `test_16`, `test_41`, `test_42` | Could not test | "MXL senders/receivers are covered by the BCP-007-03-01 test suite" (the tool skips MXL transport here) |
| IS-05-01 `test_41` (ST 2110 node) | Disabled | SDPoker is not installed on the CI runner |
| IS-05-02 (MXL node) `test_18` | Could not test | no RTP Receivers on the MXL node (covered on the ST 2110 node) |
| BCP-007-03-01 `test_15` | Manual | "whether MXL read/write starts or stops on activation cannot be verified automatically"; covered by `tests/integration/loopback.sh` and `late-flow.sh` |

## Deviations and interpretations

- **Unknown MXL domain ids are accepted** (owner decision C3, §7.4): BCP-007-03 says a Node MUST reject an `mxl_domain_id` it cannot access. The gateway accepts it, logs `mxl_domain_unknown` and waits, because mxl-fabrics-agent in `MIRROR_MODE=on-demand` creates the mirror domain only after the activation. The receivers' `mxl_domain_id` constraint is therefore unconstrained. BCP-007-03-01 tests 01–18 do not exercise this (VERIFIED at the pinned commit).
- **SDP with a different format → 400** (§6.4, Q14): Receivers have fixed formats; an SDP that does not match is rejected at staging with the list of differences. The CI run sets the tool's `SDP_PREFERENCES` to the receivers' formats (1080p50, 8 ch L24 1 ms), as the tool recommends. A syntactically malformed SDP is a 500 (nmos-cpp behaviour, open question O-1).
- **Two NMOS nodes:** the MXL node holds the MXL Senders/Receivers, the ST 2110 node the RTP Senders/Receivers (§7.1). Each device holds either the Receiver or the Sender of an essence, so every resource of a group uses the group hint `<group>:<Role> <n>` and roles are unique per node (IS-04-01 `test_23`).
- **Ports:** IS-07, the Settings and Logging APIs and the other optional nmos-cpp APIs are disabled (§7.1, §10); each node opens only its own HTTP port.
