# AGENTS.md

Instructions for coding agents (Claude Code) working on `mxl-st2110-gateway`.

## Start here

1. Read `SPECIFICATION.md` completely before writing code. It is the source of truth; `IMPLEMENTATION_PLAN.md` defines the order of work.
2. Work one phase at a time. Do not start a phase before the previous one meets its acceptance criteria.
3. The sibling repository `LeeO86/mxl-decklink` (C++20, nmos-cpp, MXL v1.1.0, Vue web UI, same CI/tagging model) is the style and structure reference. Clone it read-only into `/tmp/mxl-decklink` and reuse patterns, not code blindly.

## Hard rules

- **No GStreamer**, anywhere.
- **Never write RFC 4175 wire data into a `video/v210` grain.** Intel's `ecosystem/MTL_with_MXL` PoC does exactly that — do not copy that part.
- Keep the pins (`MTL v26.09`, `DPDK 26.07`, `MXL v1.1.0`, `nmos-cpp fe303849527394b03bdedc8f161f377fe458bb62`, nmos-testing `90018513…`) in exactly two places: `docker/Dockerfile` build args and `.github/workflows/ci.yaml` (with a "keep in sync" comment).
- All media-path time is TAI since the ST 2059-1 epoch. Never add or subtract the UTC offset in the media path.
- MTL lcore callbacks must not block, allocate, log synchronously or take contended locks.
- Never overwrite an existing `domain_def.json` or `options.json`.
- Metric names/labels and config schema are public interfaces: changes require a `CHANGELOG.md` entry; breaking changes require a schema version bump.
- Every **VERIFY** in the spec must be resolved with a test or a `// VERIFIED: <repo>@<pin> <file>:<line> — <finding>` comment. Deviations go to `docs/decisions.md` (date, context, decision, consequence).
- If the spec is ambiguous or contradicts the pinned sources, do not guess silently: record the question in `docs/decisions.md` under "Open questions", choose the most conservative option, and mention it in the PR/commit message.

## Environment notes

- GitHub API may rate-limit; download sources via `https://codeload.github.com/<owner>/<repo>/tar.gz/<ref>` or `git clone --depth 1 --branch <tag>`.
- Building DPDK + MTL + MXL is slow: build the `deps` Docker stage once and reuse it (`docker build --target deps -t mxlgw-deps .`), then iterate on the `build` stage.
- Without an E810 use `nic.backend = "kernel"` with a veth pair (needs hugepages: `sudo sysctl vm.nr_hugepages=1024`). This backend has no pacing or HW PTP and is test-only.
- MXL domains for local tests: `sudo mount -t tmpfs -o size=2g tmpfs /tmp/mxl-test` (a plain directory must fail the bootstrap — that is a feature).
- The Dockerfile lives in `docker/`: always pass `-f docker/Dockerfile` with the repository root as context.
- Kernel backend: put the second veth end into another network namespace (a `sleep` container, see `tests/integration/lib.sh`), otherwise the kernel drops the multicast as martian. Simulate leg loss with `nft … numgen random` (not every kernel has `xt_statistic`).
- MXL tests in a container need a large tmpfs: `--tmpfs /mnt/mxltest:size=2g -e MXLGW_TEST_TMPFS=/mnt/mxltest` (the 64 MiB `/dev/shm` is too small for video flows).
- DNS-SD tests need avahi-daemon on the host and the container mounts `/run/dbus` + `/run/avahi-daemon`; without systemd start `dbus-daemon --system --fork` and `avahi-daemon -D` first (`tests/integration/nmos-testing.sh` does this).
- If `vm.nr_hugepages` does not reach the target, memory is fragmented: drop caches and `sysctl vm.compact_memory=1`, then retry.

## Commands (fill in as they come into existence)

```bash
docker build --target build -t mxlgw-build .        # compile + unit tests
docker build -t mxlgw:dev .                         # runtime image
tests/integration/loopback.sh mxlgw:dev             # kernel-backend media loopback
tests/integration/nmos-testing.sh mxlgw:dev         # AMWA suites
python3 monitoring/tools/gen_dashboard.py --check   # dashboard up to date
```

## Conventions

- English for code, comments, docs and commit messages; Conventional Commits (`feat:`, `fix:`, `docs:`, `ci:` …).
- Small commits, each building and passing tests.
- New dependencies: justify in `docs/decisions.md`, add license to `THIRD_PARTY_NOTICES.md`.

## Cursor Cloud specific instructions

Phases 0–8 of `IMPLEMENTATION_PLAN.md` are implemented; build and test through `docker/Dockerfile` (commands above).

The image already provides CMake, Ninja, Clang 18 (the default `c++`), clang-format 18, doctest 2.4 (`doctest-dev`), `libstdc++-14-dev` (Clang cannot link without it), Python 3.12 with `jsonschema`, and Node 22. Docker Engine uses `fuse-overlayfs` and iptables-legacy. The environment start command brings the daemon up and waits until `docker info` succeeds, and `ubuntu` is in the `docker` group. `sudo service docker start` exits non-zero when the daemon is already running, so start checks `docker info` first.

Do not build DPDK, MTL, or MXL on the host. Those pins are built in the `deps` stage of `docker/Dockerfile`. Kernel-backend hugepages and the MXL tmpfs domain are not mounted at boot; the commands are in the environment notes above.
