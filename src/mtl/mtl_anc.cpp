// SPDX-License-Identifier: MIT
// ST 2110-40 with MTL st40p (§6.3).
#include <algorithm>

#include "mtl/mtl_sessions.hpp"

namespace mxlgw::media::mtlimpl
{
    namespace
    {
        constexpr std::uint32_t udwBufferSize = 4096;

        class AncRx final : public AncRxSession
        {
        public:
            AncRx(Context const& ctx, AncParams params)
                : _ctx(ctx)
                , _params(std::move(params))
            {
                st40p_rx_ops ops{};
                ops.name = _params.name.c_str();
                ops.priv = this;
                fillRxPort(ctx, ops.port, _params.legs, _params.payloadType);
                ops.interlaced = _params.format.interlace != config::Interlace::Progressive;
                ops.framebuff_cnt = 8;
                ops.max_udw_buff_size = udwBufferSize;
                ops.flags = ST40P_RX_FLAG_BLOCK_GET;
                _handle = st40p_rx_create(ctx.mt, &ops);
                if (_handle == nullptr)
                {
                    throw std::runtime_error("st40p_rx_create failed for " + _params.name);
                }
            }

            ~AncRx() override { st40p_rx_free(_handle); }

            std::optional<AncReceived> next(std::chrono::nanoseconds timeout) override
            {
                (void)timeout; // st40p uses its default block timeout
                auto* info = st40p_rx_get_frame(_handle);
                if (info == nullptr)
                {
                    return std::nullopt;
                }
                AncReceived r;
                r.meta.rtpTimestamp = info->rtp_timestamp;
                r.meta.receiveTai = static_cast<std::int64_t>(info->receive_timestamp);
                r.meta.complete = info->status == ST_FRAME_STATUS_COMPLETE || info->status == ST_FRAME_STATUS_RECONSTRUCTED;
                r.meta.secondField = info->second_field;
                r.meta.pktsTotal = info->pkts_total;
                r.meta.pktsRecv = {info->pkts_recv[0], info->pkts_recv[1]};
                if (info->interlaced)
                {
                    r.frame.field = info->second_field ? codec::AncField::Field2 : codec::AncField::Field1;
                }
                for (std::uint32_t i = 0; i < info->meta_num; ++i)
                {
                    auto const& m = info->meta[i];
                    codec::AncPacket p;
                    p.c = m.c != 0;
                    p.line = m.line_number;
                    p.hOffset = m.hori_offset;
                    p.s = m.s != 0;
                    p.streamNum = static_cast<std::uint8_t>(m.stream_num);
                    p.did = static_cast<std::uint8_t>(m.did);
                    p.sdid = static_cast<std::uint8_t>(m.sdid);
                    if (m.udw_offset + m.udw_size <= info->udw_buffer_fill)
                    {
                        p.udw.assign(info->udw_buff_addr + m.udw_offset, info->udw_buff_addr + m.udw_offset + m.udw_size);
                    }
                    r.frame.packets.push_back(std::move(p));
                }
                st40p_rx_put_frame(_handle, info);
                return r;
            }

            bool updateSource(std::vector<LegAddress> const& legs) override
            {
                auto src = rxSource(_ctx, legs);
                return st40p_rx_update_source(_handle, &src) == 0;
            }

            SessionStats stats() const override
            {
                st40_rx_user_stats s{};
                if (st40p_rx_get_session_stats(_handle, &s) != 0)
                {
                    return {};
                }
                return rxStats(s.common, 0);
            }

        private:
            Context _ctx;
            AncParams _params;
            st40p_rx_handle _handle = nullptr;
        };

        class AncTx final : public AncTxSession
        {
        public:
            AncTx(Context const& ctx, AncParams params)
                : _ctx(ctx)
                , _params(std::move(params))
            {
                st40p_tx_ops ops{};
                ops.name = _params.name.c_str();
                ops.priv = this;
                fillTxPort(ctx, ops.port, _params.legs, _params.payloadType);
                ops.fps = fpsOf(_params.format.grainRate()); // fields per second for interlaced formats
                ops.interlaced = _params.format.interlace != config::Interlace::Progressive;
                ops.framebuff_cnt = queueDepth(_params.queueDepth, 4, 16);
                ops.max_udw_buff_size = udwBufferSize;
                ops.flags = ST40P_TX_FLAG_USER_PACING | ST40P_TX_FLAG_USER_TIMESTAMP | ST40P_TX_FLAG_DROP_WHEN_LATE | ST40P_TX_FLAG_BLOCK_GET;
                // No notify_frame_late: MTL v26.09 hands it to the transport session with the pipeline's own context as
                // priv (lib/src/st2110/pipeline/st40_pipeline_tx.c, ops_tx.priv = ctx), so a late frame in the transport
                // called it with a foreign pointer. Frames dropped as late are in the session stats (stat_frames_dropped).
                _handle = st40p_tx_create(ctx.mt, &ops);
                if (_handle == nullptr)
                {
                    throw std::runtime_error("st40p_tx_create failed for " + _params.name);
                }
            }

            ~AncTx() override { st40p_tx_free(_handle); }

            std::size_t send(codec::AncFrame const& frame, std::int64_t transmitTai, bool secondField, std::chrono::nanoseconds timeout) override
            {
                st40p_tx_set_block_timeout(_handle, static_cast<std::uint64_t>(std::max<std::int64_t>(1, timeout.count())));
                auto* info = st40p_tx_get_frame(_handle);
                if (info == nullptr)
                {
                    return frame.packets.size();
                }
                // Accepted v1 limit (§6.3, Q10): at most ST40_MAX_META packets per frame/field, 8-bit UDW.
                auto const count = std::min<std::size_t>(frame.packets.size(), ST40_MAX_META);
                std::uint32_t fill = 0;
                std::uint32_t used = 0;
                for (std::size_t i = 0; i < count; ++i)
                {
                    auto const& p = frame.packets[i];
                    if (fill + p.udw.size() > info->udw_buffer_size)
                    {
                        break;
                    }
                    auto& m = info->meta[used++];
                    m.c = p.c ? 1 : 0;
                    m.line_number = p.line;
                    m.hori_offset = p.hOffset;
                    m.s = p.s ? 1 : 0;
                    m.stream_num = p.streamNum;
                    m.did = p.did;
                    m.sdid = p.sdid;
                    m.udw_size = static_cast<std::uint16_t>(p.udw.size());
                    m.udw_offset = static_cast<std::uint16_t>(fill);
                    std::copy(p.udw.begin(), p.udw.end(), info->udw_buff_addr + fill);
                    fill += static_cast<std::uint32_t>(p.udw.size());
                }
                info->meta_num = used;
                info->udw_buffer_fill = fill;
                info->tfmt = ST10_TIMESTAMP_FMT_TAI;
                info->timestamp = static_cast<std::uint64_t>(transmitTai);
                info->second_field = secondField;
                st40p_tx_put_frame(_handle, info);
                return frame.packets.size() - used;
            }

            bool updateDestination(std::vector<LegAddress> const& legs) override
            {
                auto dst = txDestination(_ctx, legs);
                return st40p_tx_update_destination(_handle, &dst) == 0;
            }

            SessionStats stats() const override
            {
                st40_tx_user_stats s{};
                if (st40p_tx_get_session_stats(_handle, &s) != 0)
                {
                    return {};
                }
                return txStats(s.common);
            }

        private:
            Context _ctx;
            AncParams _params;
            st40p_tx_handle _handle = nullptr;
        };
    }

    std::unique_ptr<AncRxSession> createAncRx(Context const& ctx, AncParams const& params)
    {
        return std::make_unique<AncRx>(ctx, params);
    }

    std::unique_ptr<AncTxSession> createAncTx(Context const& ctx, AncParams const& params)
    {
        return std::make_unique<AncTx>(ctx, params);
    }
}
