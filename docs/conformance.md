# Conformance

AMWA [nmos-testing](https://github.com/AMWA-TV/nmos-testing) at the pinned commit `90018513758e6fbc096c26d7ef001f700b28b510`, run non-interactively by `tests/integration/nmos-testing.sh` (SPECIFICATION.md §7.7): the gateway on the MTL kernel-socket backend with one ingest group (1 video, 8-channel audio, ANC, ST 2022-7) and one egress group, `node.registry.mode = "dns-sd"`, the tool's mock registry announced over multicast DNS-SD through the host's Avahi (owner decision Q13). The JSON results are CI artifacts (`integration-nmos-testing`).

## Results

Run of 2026-10-01 on the image built from commit `46d78bcf`:

| Suite | API versions | Pass | Fail | Warning | Manual | Not implemented | Disabled | N/A |
|---|---|---|---|---|---|---|---|---|
| IS-04-01 Node API | Node v1.3 | 55 | **0** | 0 | 1 | 5 | 10 | 3 |
| IS-05-01 Connection API | v1.1 | 61 | **0** | 0 | 0 | 0 | 8 | 12 |
| IS-05-01 Connection API | v1.2 | 61 | **0** | 0 | 0 | 0 | 8 | 12 |
| IS-05-02 Interaction with IS-04 | Node v1.3, Connection v1.2 | 56 | **0** | 0 | 0 | 0 | 14 | 3 |
| BCP-007-03-01 NMOS With MXL | Node v1.3, Connection v1.2 | 55 | **0** | 0 | 1 | 0 | 14 | 3 |

## Non-pass results

| Suite / test | State | Reason |
|---|---|---|
| all `auto_node_17`–`23`, `auto_connection_23`–`29` | Disabled | IS-10 authorization is not implemented (§1.3: no authentication in v1) |
| IS-04-01 `test_02`, `test_02_01` | Disabled | unicast DNS-SD tests; CI uses multicast DNS-SD (Q13) |
| IS-04-01 `test_12` | Disabled | not applicable to Node API v1.3 |
| IS-04-01 `test_19_01` | Not implemented | Node `interfaces` carry no `attached_network_device` (LLDP data of the switch port is not known to the gateway) |
| IS-04-01 `test_22` | Manual | "Node resource IDs persist over a reboot": all ids are UUIDv5 derivations of the persisted `node.id` and the essence `uid`s (§7.3; unit tests `test_ids.cpp`, and `test_store.cpp` for `node.id` being written back and kept on reload) |
| IS-04-01 `test_27_4`–`27_6` | Not implemented | BCP-004-01 constraint sets have no labels, preferences or enabled flags (one exact constraint set per receiver, §7.2) |
| IS-04-01 `test_28` | Not implemented | no BCP-002-02 asset tags |
| IS-05-01 `test_41` | Disabled | SDPoker is not installed on the CI runner |
| BCP-007-03-01 `test_15` | Manual | "whether MXL read/write starts or stops on activation cannot be verified automatically"; covered by `tests/integration/loopback.sh` and `late-flow.sh` |

## Deviations and interpretations

- **Unknown MXL domain ids are accepted** (owner decision C3, §7.4): BCP-007-03 says a Node MUST reject an `mxl_domain_id` it cannot access. The gateway accepts it, logs `mxl_domain_unknown` and waits, because mxl-fabrics-agent in `MIRROR_MODE=on-demand` creates the mirror domain only after the activation. The receivers' `mxl_domain_id` constraint is therefore unconstrained. BCP-007-03-01 tests 01–18 do not exercise this (VERIFIED at the pinned commit).
- **SDP with a different format → 400** (§6.4, Q14): Receivers have fixed formats; an SDP that does not match is rejected at staging with the list of differences. The CI run sets the tool's `SDP_PREFERENCES` to the receivers' formats (1080p50, 8 ch L24 1 ms), as the tool recommends. A syntactically malformed SDP is a 500 (nmos-cpp behaviour, open question O-1).
- **Group-hint roles:** Receivers use `<group>:<Role> <n> Input`, Senders and Flows `<group>:<Role> <n>`, so that roles are unique within a group (IS-04-01 `test_23`; open question O-5 in `docs/decisions.md`).
- **Single HTTP port:** IS-07, the Settings and Logging APIs and the other optional nmos-cpp APIs are disabled (§7.1, §10).
