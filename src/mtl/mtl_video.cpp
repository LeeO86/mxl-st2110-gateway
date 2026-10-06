// SPDX-License-Identifier: MIT
// ST 2110-20 <-> v210 with MTL st20p (§6.1).
#include <atomic>
#include <cerrno>
#include <chrono>

#include "mtl/mtl_sessions.hpp"
#include "util/logging.hpp"

namespace mxlgw::media::mtlimpl
{
    namespace
    {
        FrameMeta metaOf(st_frame const& f)
        {
            FrameMeta m;
            m.rtpTimestamp = f.rtp_timestamp;
            m.receiveTai = static_cast<std::int64_t>(f.receive_timestamp);
            m.complete = f.status == ST_FRAME_STATUS_COMPLETE || f.status == ST_FRAME_STATUS_RECONSTRUCTED;
            m.secondField = f.second_field;
            m.pktsTotal = f.pkts_total;
            m.pktsRecv = {f.pkts_recv[0], f.pkts_recv[1]};
            m.tag = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(f.opaque));
            return m;
        }

        class VideoRx final : public VideoRxSession
        {
        public:
            VideoRx(Context const& ctx, VideoRxParams params, VideoRxHandler& handler)
                : _ctx(ctx)
                , _params(std::move(params))
                , _handler(handler)
            {
                st20p_rx_ops ops{};
                ops.name = _params.name.c_str();
                ops.priv = this;
                fillRxPort(ctx, ops.port, _params.legs, _params.payloadType);
                ops.width = static_cast<std::uint32_t>(_params.format.width);
                ops.height = static_cast<std::uint32_t>(_params.format.height);
                ops.fps = fpsOf(_params.format.grainRate());
                ops.interlaced = _params.format.interlaced();
                ops.transport_fmt = ST20_FMT_YUV_422_10BIT;
                ops.output_fmt = ST_FRAME_FMT_V210;
                // VERIFIED: OpenVisualCloud/Media-Transport-Library@v26.09 lib/src/st2110/pipeline/st20_pipeline_rx.c:204-229 —
                // with EXT_FRAME and query_ext_frame the ext frame is the conversion destination, queried on the lcore
                // when the frame is complete; the internal converter runs in st20p_rx_get_frame (no plugin config is
                // shipped, so device AUTO selects it).
                ops.device = ST_PLUGIN_DEVICE_AUTO;
                ops.framebuff_cnt = 3;
                ops.flags = ST20P_RX_FLAG_EXT_FRAME | ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME | ST20P_RX_FLAG_BLOCK_GET;
                ops.query_ext_frame = &VideoRx::queryExtFrame;
                _handle = st20p_rx_create(ctx.mt, &ops);
                if (_handle == nullptr)
                {
                    throw std::runtime_error("st20p_rx_create failed for " + _params.name);
                }
                st20p_rx_set_block_timeout(_handle, 20'000'000);
            }

            ~VideoRx() override
            {
                if (_current != nullptr)
                {
                    st20p_rx_put_frame(_handle, _current);
                }
                st20p_rx_free(_handle);
            }

            std::optional<FrameMeta> next(std::chrono::nanoseconds timeout) override
            {
                setTimeout(timeout);
                _current = st20p_rx_get_frame(_handle);
                if (_current == nullptr)
                {
                    return std::nullopt;
                }
                return metaOf(*_current);
            }

            void release() override
            {
                if (_current != nullptr)
                {
                    st20p_rx_put_frame(_handle, _current);
                    _current = nullptr;
                }
            }

            bool updateSource(std::vector<LegAddress> const& legs) override
            {
                auto src = rxSource(_ctx, legs);
                return st20p_rx_update_source(_handle, &src) == 0;
            }

            SessionStats stats() const override
            {
                st20_rx_user_stats s{};
                if (st20p_rx_get_session_stats(_handle, &s) != 0)
                {
                    return {};
                }
                auto out = rxStats(s.common, s.stat_frames_incomplete);
                // VERIFIED: OpenVisualCloud/Media-Transport-Library@v26.09 lib/src/st2110/st_rx_video_session.c:1231-1248,1634-1639 —
                // a frame that finds no free transport frame is dropped before st20p (and queryExtFrame) sees it; its slot
                // keeps the timestamp, so it counts once in stat_slot_get_frame_fail whatever the number of packets and legs.
                out.framesDropped += s.stat_slot_get_frame_fail;
                return out;
            }

        private:
            /// REAL-TIME context (MTL lcore).
            static int queryExtFrame(void* priv, st_ext_frame* ext, st20_rx_frame_meta* meta)
            {
                auto* self = static_cast<VideoRx*>(priv);
                FrameMeta m;
                m.rtpTimestamp = meta->rtp_timestamp;
                m.receiveTai = static_cast<std::int64_t>(meta->timestamp_first_pkt);
                m.complete = meta->status == ST_FRAME_STATUS_COMPLETE || meta->status == ST_FRAME_STATUS_RECONSTRUCTED;
                m.secondField = meta->second_field;
                m.pktsTotal = meta->pkts_total;
                m.pktsRecv = {meta->pkts_recv[0], meta->pkts_recv[1]};
                std::uint64_t tag = 0;
                auto* dst = self->_handler.acquire(m, tag);
                if (dst == nullptr)
                {
                    return -EBUSY;
                }
                ext->addr[0] = dst;
                ext->iova[0] = 0; // CPU conversion: no DMA mapping needed
                ext->linesize[0] = self->_params.format.v210Stride();
                ext->size = self->_params.format.grainBytes();
                ext->opaque = reinterpret_cast<void*>(static_cast<std::uintptr_t>(tag));
                return 0;
            }

            void setTimeout(std::chrono::nanoseconds timeout)
            {
                if (timeout != _timeout)
                {
                    _timeout = timeout;
                    st20p_rx_set_block_timeout(_handle, static_cast<std::uint64_t>(std::max<std::int64_t>(1, timeout.count())));
                }
            }

            Context _ctx;
            VideoRxParams _params;
            VideoRxHandler& _handler;
            st20p_rx_handle _handle = nullptr;
            st_frame* _current = nullptr;
            std::chrono::nanoseconds _timeout{20'000'000};
        };

        class VideoTx final : public VideoTxSession
        {
        public:
            VideoTx(Context const& ctx, VideoTxParams params)
                : _ctx(ctx)
                , _params(std::move(params))
            {
                st20p_tx_ops ops{};
                ops.name = _params.name.c_str();
                ops.priv = this;
                fillTxPort(ctx, ops.port, _params.legs, _params.payloadType);
                ops.width = static_cast<std::uint32_t>(_params.format.width);
                ops.height = static_cast<std::uint32_t>(_params.format.height);
                ops.fps = fpsOf(_params.format.grainRate());
                ops.interlaced = _params.format.interlaced();
                ops.input_fmt = ST_FRAME_FMT_V210;
                ops.transport_fmt = ST20_FMT_YUV_422_10BIT;
                ops.transport_pacing = _params.pacing == config::Pacing::Linear ? ST21_PACING_LINEAR
                                       : _params.pacing == config::Pacing::Wide ? ST21_PACING_WIDE
                                                                                : ST21_PACING_NARROW;
                ops.transport_packing = _params.packing == config::Packing::Gpm     ? ST20_PACKING_GPM
                                        : _params.packing == config::Packing::GpmSl ? ST20_PACKING_GPM_SL
                                                                                    : ST20_PACKING_BPM;
                ops.device = ST_PLUGIN_DEVICE_AUTO;
                // MTL accepts 2..ST20_FB_MAX_COUNT (8) frames for an ST 2110-20 sender; more fails st20p_tx_create.
                ops.framebuff_cnt = queueDepth(_params.queueDepth, 4, ST20_FB_MAX_COUNT);
                // §5.7: user pacing at T(i) + output_delay with the same TAI as RTP timestamp (Q1).
                ops.flags =
                    ST20P_TX_FLAG_EXT_FRAME | ST20P_TX_FLAG_USER_PACING | ST20P_TX_FLAG_USER_TIMESTAMP | ST20P_TX_FLAG_DROP_WHEN_LATE | ST20P_TX_FLAG_BLOCK_GET;
                ops.notify_frame_late = &VideoTx::onLate;
                _handle = st20p_tx_create(ctx.mt, &ops);
                if (_handle == nullptr)
                {
                    throw std::runtime_error("st20p_tx_create failed for " + _params.name);
                }
            }

            ~VideoTx() override { st20p_tx_free(_handle); }

            bool send(std::uint8_t const* v210, std::int64_t transmitTai, bool secondField, std::chrono::nanoseconds timeout) override
            {
                st20p_tx_set_block_timeout(_handle, static_cast<std::uint64_t>(std::max<std::int64_t>(1, timeout.count())));
                auto* frame = st20p_tx_get_frame(_handle);
                if (frame == nullptr)
                {
                    return false;
                }
                frame->tfmt = ST10_TIMESTAMP_FMT_TAI;
                frame->timestamp = static_cast<std::uint64_t>(transmitTai);
                frame->second_field = secondField;
                st_ext_frame ext{};
                ext.addr[0] = const_cast<std::uint8_t*>(v210);
                ext.iova[0] = 0;
                ext.linesize[0] = _params.format.v210Stride();
                ext.size = _params.format.grainBytes();
                // VERIFIED: OpenVisualCloud/Media-Transport-Library@v26.09 lib/src/st2110/pipeline/st20_pipeline_tx.c:991-998 — with the
                // internal converter st20p_tx_put_ext_frame converts v210 -> RFC 4175 synchronously into MTL's own buffer,
                // so the (read-only, mmapped) grain only needs to stay valid during this call.
                return st20p_tx_put_ext_frame(_handle, frame, &ext) == 0;
            }

            bool updateDestination(std::vector<LegAddress> const& legs) override
            {
                auto dst = txDestination(_ctx, legs);
                return st20p_tx_update_destination(_handle, &dst) == 0;
            }

            SessionStats stats() const override
            {
                st20_tx_user_stats s{};
                if (st20p_tx_get_session_stats(_handle, &s) != 0)
                {
                    return {};
                }
                auto out = txStats(s.common);
                out.framesLate += _late.load(std::memory_order_relaxed);
                return out;
            }

        private:
            static int onLate(void* priv, std::uint64_t)
            {
                static_cast<VideoTx*>(priv)->_late.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }

            Context _ctx;
            VideoTxParams _params;
            st20p_tx_handle _handle = nullptr;
            std::atomic<std::uint64_t> _late{0};
        };
    }

    std::unique_ptr<VideoRxSession> createVideoRx(Context const& ctx, VideoRxParams const& params, VideoRxHandler& handler)
    {
        return std::make_unique<VideoRx>(ctx, params, handler);
    }

    std::unique_ptr<VideoTxSession> createVideoTx(Context const& ctx, VideoTxParams const& params)
    {
        return std::make_unique<VideoTx>(ctx, params);
    }
}
