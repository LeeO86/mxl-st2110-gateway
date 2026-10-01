# syntax=docker/dockerfile:1
# mxl-st2110-gateway — multi-stage build (SPECIFICATION.md §14.1).
#
# Pins: keep in sync with .github/workflows/ci.yaml (the only other place they live).
ARG MTL_REF=v26.09
ARG DPDK_VER=26.07
ARG MXL_REF=v1.1.0
ARG NMOS_CPP_REF=fe303849527394b03bdedc8f161f377fe458bb62
ARG ICE_VER=2.6.7
ARG ICE_DMID=923747

# ---------------------------------------------------------------------------
# Vue web UI (§11) — single-file dist/index.html for embedding.
FROM node:22-bookworm AS webui
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN npm ci --no-fund --no-audit
COPY web/ ./
RUN npm test && npm run build

# ---------------------------------------------------------------------------
# Dependencies. Changes only when a pin or patches/mtl changes.
FROM ubuntu:24.04 AS deps
ARG MTL_REF
ARG DPDK_VER
ARG MXL_REF
ARG NMOS_CPP_REF
ARG ICE_VER
ARG ICE_DMID
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git pkg-config curl wget zip unzip tar xz-utils \
        ca-certificates python3 python3-pip python3-pyelftools python3-jsonschema meson \
        bison flex byacc autoconf automake libtool patch \
        libnuma-dev libjson-c-dev libpcap-dev libssl-dev \
        libboost-system-dev libboost-thread-dev libboost-chrono-dev libboost-date-time-dev \
        libboost-regex-dev libboost-random-dev libboost-filesystem-dev \
        libcpprest-dev libwebsocketpp-dev nlohmann-json3-dev libavahi-compat-libdnssd-dev \
        clang llvm lld clang-format libclang-rt-18-dev gcovr \
    && rm -rf /var/lib/apt/lists/*

# vcpkg for the MXL dependency manifest (MXL pins its own builtin-baseline).
RUN git clone https://github.com/microsoft/vcpkg /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
ENV VCPKG_ROOT=/opt/vcpkg

# MXL (Apache-2.0), Fabrics OFF, tools ON (mxl-info, mxl-data-probe). No GStreamer is
# installed, so the optional mxl-gst tools are skipped by MXL's CMake.
RUN git clone --depth 1 --branch "${MXL_REF}" https://github.com/dmf-mxl/mxl /tmp/mxl \
    && cmake -S /tmp/mxl -B /tmp/mxl/build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTS=OFF -DBUILD_TOOLS=ON -DBUILD_UTILS=OFF -DBUILD_DOCS=OFF \
        -DMXL_ENABLE_FABRICS_OFI=OFF \
        -DCMAKE_INSTALL_PREFIX=/opt/mxl \
    && cmake --build /tmp/mxl/build -j"$(nproc)" \
    && cmake --install /tmp/mxl/build \
    && mkdir -p /opt/mxl-vcpkg && cp -a /tmp/mxl/build/vcpkg_installed/x64-linux/. /opt/mxl-vcpkg/ \
    && rm -rf /tmp/mxl /root/.cache/vcpkg

# Sony nmos-cpp (Apache-2.0), built once and installed as a CMake package. Built as
# C++17: websocketpp does not compile as C++20.
RUN git -c init.defaultBranch=main init /tmp/nmos-cpp \
    && git -C /tmp/nmos-cpp fetch --depth 1 https://github.com/sony/nmos-cpp.git "${NMOS_CPP_REF}" \
    && git -C /tmp/nmos-cpp -c advice.detachedHead=false checkout FETCH_HEAD \
    && cmake -S /tmp/nmos-cpp/Development -B /tmp/nmos-cpp/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DNMOS_CPP_BUILD_EXAMPLES=OFF -DNMOS_CPP_BUILD_TESTS=OFF \
        -DNMOS_CPP_USE_SUPPLIED_JSON_SCHEMA_VALIDATOR=ON -DNMOS_CPP_USE_SUPPLIED_JWT_CPP=ON \
        -DNMOS_CPP_USE_AVAHI=ON \
        -DCMAKE_INSTALL_PREFIX=/opt/nmos-cpp \
    && cmake --build /tmp/nmos-cpp/build -j"$(nproc)" \
    && cmake --install /tmp/nmos-cpp/build \
    && mkdir -p /opt/nmos-cpp/include/third_party \
    && cp -a /tmp/nmos-cpp/Development/third_party/jwt-cpp /tmp/nmos-cpp/Development/third_party/nlohmann /opt/nmos-cpp/include/third_party/ \
    && rm -rf /tmp/nmos-cpp

# MTL source (needed for the DPDK patches as well).
RUN git clone --depth 1 --branch "${MTL_REF}" \
        https://github.com/OpenVisualCloud/Media-Transport-Library /opt/mtl-src

# DPDK with the MTL patches. Same steps as MTL script/build_dpdk.sh, but with
# -Dplatform=generic so the image does not depend on the build host's CPU
# (build_dpdk.sh would build for -march=native). See docs/decisions.md.
RUN curl -fsSL -o /tmp/dpdk.zip "https://github.com/DPDK/dpdk/archive/refs/tags/v${DPDK_VER}.zip" \
    && cd /tmp && unzip -q dpdk.zip && rm dpdk.zip \
    && cd "/tmp/dpdk-${DPDK_VER}" \
    && for p in /opt/mtl-src/patches/dpdk/"${DPDK_VER}"/*.patch; do patch -p1 -i "$p"; done \
    && meson setup build --prefix=/usr/local -Dplatform=generic -Dtests=false -Denable_docs=false \
    && ninja -C build \
    && ninja -C build install \
    && ldconfig \
    && rm -rf "/tmp/dpdk-${DPDK_VER}"

# E810 DDP package from the pinned ice driver tarball (§14.1).
RUN curl -fsSL "https://downloadmirror.intel.com/${ICE_DMID}/ice-${ICE_VER}.tar.gz" | tar xz -C /tmp \
    && mkdir -p /opt/ddp \
    && cp /tmp/ice-"${ICE_VER}"/ddp/ice-*.pkg /opt/ddp/ \
    && cp /tmp/ice-"${ICE_VER}"/ddp/ice-*.pkg /opt/ddp/ice.pkg \
    && rm -rf /tmp/ice-"${ICE_VER}"

# MTL with the gateway patches. CI also runs `git apply --check` separately.
# MTL's -Werror build needs USDT enabled (variables used only by the trace macros).
RUN apt-get update && apt-get install -y --no-install-recommends systemtap-sdt-dev \
    && rm -rf /var/lib/apt/lists/*
COPY patches/mtl/ /tmp/mtl-patches/
RUN cd /opt/mtl-src \
    && for p in /tmp/mtl-patches/*.patch; do \
         [ -e "$p" ] || continue; \
         git apply --check "$p" && git apply "$p" || exit 1; \
       done \
    && meson setup build --prefix=/usr/local -Dbuildtype=release \
    && ninja -C build \
    && ninja -C build install \
    && ldconfig

# ---------------------------------------------------------------------------
# Gateway build + unit tests. A failing test fails the image build.
FROM deps AS build
COPY . /src
COPY --from=webui /web/dist /src/web/dist
RUN cmake -S /src -B /src/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        "-DCMAKE_PREFIX_PATH=/opt/mxl;/opt/mxl-vcpkg;/opt/nmos-cpp" \
        -DMXLGW_USE_PREBUILT_WEBUI=ON \
        -DMXLGW_WERROR=ON \
    && cmake --build /src/build -j"$(nproc)" \
    && cd /src/build && LD_LIBRARY_PATH=/opt/mxl/lib ctest --output-on-failure

# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime
ARG MTL_REF
ARG DPDK_VER
ARG MXL_REF
ARG NMOS_CPP_REF
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl iproute2 \
        libnuma1 libjson-c5 libpcap0.8t64 libssl3t64 \
        libcpprest2.10 libboost-chrono1.83.0t64 libboost-thread1.83.0 libboost-date-time1.83.0 \
        libboost-regex1.83.0 libboost-random1.83.0 libboost-filesystem1.83.0 libboost-system1.83.0 \
        libavahi-compat-libdnssd1 \
    && rm -rf /var/lib/apt/lists/*

# DPDK + MTL (incl. DPDK PMD plugins) and MXL shared libraries.
COPY --from=deps /usr/local/lib/x86_64-linux-gnu/ /usr/local/lib/x86_64-linux-gnu/
COPY --from=deps /opt/mxl/lib/ /usr/local/lib/
COPY --from=deps /opt/mxl/bin/mxl-info /opt/mxl/bin/mxl-data-probe /usr/local/bin/
COPY --from=deps /opt/nmos-cpp/lib/ /usr/local/lib/
# E810 DDP package where the DPDK ice PMD looks for it (DPDK 26.07 ice_ethdev.h).
COPY --from=deps /opt/ddp/ice.pkg /lib/firmware/updates/intel/ice/ddp/ice.pkg
COPY --from=deps /opt/ddp/ice.pkg /lib/firmware/intel/ice/ddp/ice.pkg

COPY --from=build /src/build/mxl-st2110-gateway /usr/local/bin/mxl-st2110-gateway
COPY --from=build /src/build/tools/mxl-pattern-writer /src/build/tools/mxl-verify /usr/local/bin/
COPY schema/gateway-config.schema.json /usr/share/mxl-st2110-gateway/gateway-config.schema.json
COPY config/examples/ /usr/share/mxl-st2110-gateway/examples/
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN echo /usr/local/lib/x86_64-linux-gnu > /etc/ld.so.conf.d/dpdk.conf \
    && ldconfig && chmod +x /usr/local/bin/entrypoint.sh

LABEL org.opencontainers.image.title="mxl-st2110-gateway" \
      org.opencontainers.image.description="SMPTE ST 2110 <-> MXL gateway (Intel MTL/DPDK, NMOS IS-04/IS-05, BCP-007-03)" \
      org.opencontainers.image.licenses="MIT" \
      io.github.mxlgw.mtl="${MTL_REF}" io.github.mxlgw.dpdk="${DPDK_VER}" \
      io.github.mxlgw.mxl="${MXL_REF}" io.github.mxlgw.nmos-cpp="${NMOS_CPP_REF}"

ENV MXLGW_CONFIG=/config/gateway.json
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
