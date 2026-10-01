// SPDX-License-Identifier: MIT
#pragma once

#include <memory>

#include "mtl/backend.hpp"
#include "mtl/mtl_internal.hpp"

namespace mxlgw::media::mtlimpl
{
    std::unique_ptr<VideoRxSession> createVideoRx(Context const& ctx, VideoRxParams const& params, VideoRxHandler& handler);
    std::unique_ptr<VideoTxSession> createVideoTx(Context const& ctx, VideoTxParams const& params);
    std::unique_ptr<AudioRxSession> createAudioRx(Context const& ctx, AudioParams const& params);
    std::unique_ptr<AudioTxSession> createAudioTx(Context const& ctx, AudioParams const& params);
    std::unique_ptr<AncRxSession> createAncRx(Context const& ctx, AncParams const& params);
    std::unique_ptr<AncTxSession> createAncTx(Context const& ctx, AncParams const& params);
}
