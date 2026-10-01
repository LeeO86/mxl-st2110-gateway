// SPDX-License-Identifier: MIT
// ST 2110-30 PCM with MTL st30p (§6.2).
#include <algorithm>
#include <atomic>
#include <cstdint>

#include "mtl/mtl_sessions.hpp"

namespace mxlgw::media::mtlimpl
{
    namespace
    {
        constexpr std::int64_t rxBufferNs = 100'000'000;

        enum st30_fmt fmtOf(int bitDepth)
        {
            return bitDepth == 16 ? ST30_FMT_PCM16 : ST30_FMT_PCM24;
        }

        enum st30_ptime ptimeOf(int ptimeUs)
        {
            return ptimeUs == 125 ? ST30_PTIME_125US : ST30_PTIME_1MS;
        }

        std::uint32_t blockBytes(config::AudioFormat const& f)
        {
            return static_cast<std::uint32_t>(f.samplesPerBlock() * f.channels * f.bytesPerSample());
        }

        class AudioRx final : public AudioRxSession
        {
        public:
            AudioRx(Context const& ctx, AudioParams params)
                : _ctx(ctx)
                , _params(std::move(params))
            {
                st30p_rx_ops ops{};
                ops.name = _params.name.c_str();
                ops.priv = this;
                fillRxPort(ctx, ops.port, _params.legs, _params.payloadType);
                ops.fmt = fmtOf(_params.format.bitDepth);
                ops.channel = static_cast<std::uint16_t>(_params.format.channels);
                ops.sampling = ST30_SAMPLING_48K;
                ops.ptime = ptimeOf(_params.format.ptimeUs);
                // §6.2: framebuff_size = block_us worth of samples (an integer multiple of the packet size).
                ops.framebuff_size = blockBytes(_params.format);
                // MTL drops a block when no frame buffer is free, so the pool covers rxBufferNs of blocks to
                // ride out a late ingest worker (a few KiB each).
                ops.framebuff_cnt = static_cast<std::uint16_t>(
                    std::clamp<std::int64_t>((rxBufferNs + _params.format.blockUs * 1000 - 1) / (_params.format.blockUs * 1000), 16, 512));
                ops.flags = ST30P_RX_FLAG_BLOCK_GET | ST30P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
                _handle = st30p_rx_create(ctx.mt, &ops);
                if (_handle == nullptr)
                {
                    throw std::runtime_error("st30p_rx_create failed for " + _params.name);
                }
                st30p_rx_set_block_timeout(_handle, 20'000'000);
            }

            ~AudioRx() override
            {
                release();
                st30p_rx_free(_handle);
            }

            std::optional<AudioBlock> next(std::chrono::nanoseconds timeout) override
            {
                if (timeout != _timeout)
                {
                    _timeout = timeout;
                    st30p_rx_set_block_timeout(_handle, static_cast<std::uint64_t>(std::max<std::int64_t>(1, timeout.count())));
                }
                _current = st30p_rx_get_frame(_handle);
                if (_current == nullptr)
                {
                    return std::nullopt;
                }
                AudioBlock b;
                b.meta.rtpTimestamp = _current->rtp_timestamp;
                b.meta.receiveTai = static_cast<std::int64_t>(_current->receive_timestamp);
                b.meta.complete = _current->status == ST_FRAME_STATUS_COMPLETE || _current->status == ST_FRAME_STATUS_RECONSTRUCTED;
                b.meta.pktsTotal = _current->pkts_total;
                b.meta.pktsRecv = {_current->pkts_recv[0], _current->pkts_recv[1]};
                b.pcm = static_cast<std::uint8_t const*>(_current->addr);
                auto const frameBytes = static_cast<std::size_t>(_params.format.channels * _params.format.bytesPerSample());
                b.samples = frameBytes > 0 ? _current->data_size / frameBytes : 0;
                return b;
            }

            void release() override
            {
                if (_current != nullptr)
                {
                    st30p_rx_put_frame(_handle, _current);
                    _current = nullptr;
                }
            }

            bool updateSource(std::vector<LegAddress> const& legs) override
            {
                auto src = rxSource(_ctx, legs);
                return st30p_rx_update_source(_handle, &src) == 0;
            }

            SessionStats stats() const override
            {
                st30_rx_user_stats s{};
                if (st30p_rx_get_session_stats(_handle, &s) != 0)
                {
                    return {};
                }
                return rxStats(s.common, s.stat_frames_incomplete);
            }

        private:
            Context _ctx;
            AudioParams _params;
            st30p_rx_handle _handle = nullptr;
            st30_frame* _current = nullptr;
            std::chrono::nanoseconds _timeout{20'000'000};
        };

        class AudioTx final : public AudioTxSession
        {
        public:
            AudioTx(Context const& ctx, AudioParams params)
                : _ctx(ctx)
                , _params(std::move(params))
            {
                st30p_tx_ops ops{};
                ops.name = _params.name.c_str();
                ops.priv = this;
                fillTxPort(ctx, ops.port, _params.legs, _params.payloadType);
                ops.fmt = fmtOf(_params.format.bitDepth);
                ops.channel = static_cast<std::uint16_t>(_params.format.channels);
                ops.sampling = ST30_SAMPLING_48K;
                ops.ptime = ptimeOf(_params.format.ptimeUs);
                ops.framebuff_size = blockBytes(_params.format);
                ops.framebuff_cnt = queueDepth(_params.queueDepth, 64, 512);
                // VERIFIED: OpenVisualCloud/Media-Transport-Library@v26.09 include/st30_pipeline_api.h:28-62 — st30p has no
                // USER_TIMESTAMP flag; with USER_PACING the RTP timestamp follows the pacing TAI
                // (lib/src/st2110/st_tx_audio_session.c:331-341), i.e. T(s) + output_delay (§5.4, Q1).
                ops.flags = ST30P_TX_FLAG_USER_PACING | ST30P_TX_FLAG_BLOCK_GET | ST30P_TX_FLAG_DROP_WHEN_LATE;
                ops.notify_frame_late = &AudioTx::onLate;
                _handle = st30p_tx_create(ctx.mt, &ops);
                if (_handle == nullptr)
                {
                    throw std::runtime_error("st30p_tx_create failed for " + _params.name);
                }
            }

            ~AudioTx() override
            {
                if (_current != nullptr)
                {
                    st30p_tx_put_frame_abort(_handle, _current);
                }
                st30p_tx_free(_handle);
            }

            std::uint8_t* acquire(std::chrono::nanoseconds timeout) override
            {
                if (_current != nullptr)
                {
                    return static_cast<std::uint8_t*>(_current->addr);
                }
                st30p_tx_set_block_timeout(_handle, static_cast<std::uint64_t>(std::max<std::int64_t>(1, timeout.count())));
                _current = st30p_tx_get_frame(_handle);
                return _current == nullptr ? nullptr : static_cast<std::uint8_t*>(_current->addr);
            }

            void send(std::int64_t transmitTai) override
            {
                if (_current == nullptr)
                {
                    return;
                }
                _current->tfmt = ST10_TIMESTAMP_FMT_TAI;
                _current->timestamp = static_cast<std::uint64_t>(transmitTai);
                _current->data_size = blockBytes(_params.format);
                st30p_tx_put_frame(_handle, _current);
                _current = nullptr;
            }

            bool updateDestination(std::vector<LegAddress> const& legs) override
            {
                auto dst = txDestination(_ctx, legs);
                return st30p_tx_update_destination(_handle, &dst) == 0;
            }

            SessionStats stats() const override
            {
                st30_tx_user_stats s{};
                if (st30p_tx_get_session_stats(_handle, &s) != 0)
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
                static_cast<AudioTx*>(priv)->_late.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }

            Context _ctx;
            AudioParams _params;
            st30p_tx_handle _handle = nullptr;
            st30_frame* _current = nullptr;
            std::atomic<std::uint64_t> _late{0};
        };
    }

    std::unique_ptr<AudioRxSession> createAudioRx(Context const& ctx, AudioParams const& params)
    {
        return std::make_unique<AudioRx>(ctx, params);
    }

    std::unique_ptr<AudioTxSession> createAudioTx(Context const& ctx, AudioParams const& params)
    {
        return std::make_unique<AudioTx>(ctx, params);
    }
}
