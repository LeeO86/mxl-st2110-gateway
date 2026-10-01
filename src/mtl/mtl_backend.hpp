// SPDX-License-Identifier: MIT
#pragma once

#include <memory>

#include "config/config.hpp"
#include "mtl/backend.hpp"

namespace mxlgw::media
{
    /// MTL backend for nic.backend = dpdk (E810 via vfio-pci) and kernel (MTL_PMD_KERNEL_SOCKET,
    /// test-only). Throws std::runtime_error if MTL cannot be initialised.
    std::unique_ptr<MediaBackend> createMtlBackend(config::Config const& config);
}
