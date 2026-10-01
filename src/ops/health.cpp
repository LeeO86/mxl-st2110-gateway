// SPDX-License-Identifier: MIT
#include "ops/health.hpp"

#include <cstdlib>

namespace mxlgw::ops
{
    nlohmann::json Readiness::toJson() const
    {
        return nlohmann::json{{"ready", ready}, {"reasons", reasons}, {"warnings", warnings}};
    }

    Readiness evaluateReadiness(ReadinessInputs const& in)
    {
        Readiness r;
        if (in.setupMode)
        {
            r.reasons.push_back("unconfigured");
        }
        if (!in.configValid)
        {
            r.reasons.push_back("config_invalid");
        }
        if (!in.setupMode && !in.mediaUp)
        {
            r.reasons.push_back("media_backend_down");
        }
        if (in.testBackend)
        {
            r.warnings.push_back("test_backend");
        }
        if (!in.setupMode && in.ptpLocked.has_value() && !*in.ptpLocked)
        {
            if (in.requireLock && !in.testBackend)
            {
                r.reasons.push_back("ptp_unlocked");
            }
            else
            {
                r.warnings.push_back("ptp_unlocked");
            }
        }
        if (!in.setupMode && in.clockOffsetNs && std::llabs(*in.clockOffsetNs) > in.maxOffsetNs)
        {
            r.reasons.push_back("clock_mismatch");
        }
        if (!in.domainsOk)
        {
            r.reasons.push_back("mxl_domain_error");
        }
        if (!in.setupMode && !in.nmosRegistered && !in.registryAbsentIntended)
        {
            r.reasons.push_back("nmos_not_registered");
        }
        for (auto const& extra : in.extraReasons)
        {
            r.reasons.push_back(extra);
        }
        r.ready = r.reasons.empty();
        return r;
    }
}
