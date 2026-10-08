// SPDX-License-Identifier: MIT
#include "mtl/mock_backend.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <thread>

#include "codec/audioconv.hpp"
#include "timing/rtpclock.hpp"
#include "util/threading.hpp"

namespace mxlgw::media
{
    namespace
    {
        constexpr std::size_t maxAncPackets = 20; // same limit as MTL ST40_MAX_META

        struct Packet
        {
            std::int64_t deliverTai = 0;
            std::uint64_t seq = 0;
            std::string destination;
            int port = 0;
            int leg = 0;
            FrameMeta meta;
            std::shared_ptr<std::vector<std::uint8_t> const> payload;
            std::shared_ptr<codec::AncFrame const> anc;
            std::size_t samples = 0;
        };

        struct Later
        {
            bool operator()(Packet const& a, Packet const& b) const { return a.deliverTai != b.deliverTai ? a.deliverTai > b.deliverTai : a.seq > b.seq; }
        };

        using Subscriber = std::function<void(Packet const&)>;

        /// In-process "wire": delivers packets at their transmit time to matching subscribers.
        class MockNetwork
        {
        public:
            MockNetwork()
                : _thread([this] { run(); })
            {}

            ~MockNetwork()
            {
                {
                    std::lock_guard const lock{_mutex};
                    _stop = true;
                }
                _cv.notify_all();
                _thread.join();
            }

            std::uint64_t subscribe(std::string const& destination, int port, Subscriber subscriber)
            {
                std::lock_guard const lock{_mutex};
                auto const id = ++_nextId;
                _subscribers[{destination, port}][id] = std::move(subscriber);
                return id;
            }

            void unsubscribe(std::string const& destination, int port, std::uint64_t id)
            {
                std::lock_guard const lock{_mutex};
                auto const it = _subscribers.find({destination, port});
                if (it != _subscribers.end())
                {
                    it->second.erase(id);
                }
                // Wait until no delivery is running so the subscriber may be destroyed safely.
                _cv.notify_all();
            }

            void waitIdle()
            {
                std::unique_lock lock{_mutex};
                _idleCv.wait(lock, [&] { return !_delivering; });
            }

            void send(Packet packet)
            {
                {
                    std::lock_guard const lock{_mutex};
                    packet.seq = ++_seq;
                    _queue.push(std::move(packet));
                }
                _cv.notify_all();
            }

        private:
            void run()
            {
                util::setThreadName("mock-wire");
                std::unique_lock lock{_mutex};
                while (!_stop)
                {
                    if (_queue.empty())
                    {
                        _cv.wait(lock);
                        continue;
                    }
                    auto const now = hostTaiNs();
                    auto const due = _queue.top().deliverTai;
                    if (due > now)
                    {
                        _cv.wait_for(lock, std::chrono::nanoseconds(std::min<std::int64_t>(due - now, 5'000'000)));
                        continue;
                    }
                    Packet packet = _queue.top();
                    _queue.pop();
                    auto const it = _subscribers.find({packet.destination, packet.port});
                    if (it == _subscribers.end() || it->second.empty())
                    {
                        continue;
                    }
                    std::vector<Subscriber> targets;
                    for (auto const& [id, fn] : it->second)
                    {
                        targets.push_back(fn);
                    }
                    _delivering = true;
                    lock.unlock();
                    packet.meta.receiveTai = hostTaiNs();
                    for (auto const& fn : targets)
                    {
                        fn(packet);
                    }
                    lock.lock();
                    _delivering = false;
                    _idleCv.notify_all();
                }
            }

            std::mutex _mutex;
            std::condition_variable _cv;
            std::condition_variable _idleCv;
            bool _stop = false;
            bool _delivering = false;
            std::uint64_t _seq = 0;
            std::uint64_t _nextId = 0;
            std::priority_queue<Packet, std::vector<Packet>, Later> _queue;
            std::map<std::pair<std::string, int>, std::map<std::uint64_t, Subscriber>> _subscribers;
            std::thread _thread;
        };

        /// RX side: subscribes to every enabled leg and merges duplicates by RTP timestamp (2022-7).
        class Subscription
        {
        public:
            Subscription(std::shared_ptr<MockNetwork> net, std::function<void(Packet const&)> onFirst)
                : _net(std::move(net))
                , _onFirst(std::move(onFirst))
            {}

            ~Subscription() { clear(); }

            void set(std::vector<LegAddress> const& legs)
            {
                clear();
                std::lock_guard const lock{_mutex};
                for (std::size_t i = 0; i < legs.size() && i < 2; ++i)
                {
                    if (!legs[i].enabled || legs[i].destination.empty() || legs[i].port <= 0)
                    {
                        continue;
                    }
                    int const leg = static_cast<int>(i);
                    auto const id = _net->subscribe(legs[i].destination, legs[i].port, [this, leg](Packet const& p) { onPacket(leg, p); });
                    _subs.push_back({legs[i].destination, legs[i].port, id});
                }
            }

            void clear()
            {
                std::vector<Sub> subs;
                {
                    std::lock_guard const lock{_mutex};
                    subs.swap(_subs);
                }
                for (auto const& s : subs)
                {
                    _net->unsubscribe(s.destination, s.port, s.id);
                }
                if (!subs.empty())
                {
                    _net->waitIdle();
                }
            }

            SessionStats stats() const
            {
                std::lock_guard const lock{_mutex};
                return _stats;
            }

            void countFrame(bool complete, bool dropped)
            {
                std::lock_guard const lock{_mutex};
                if (dropped)
                {
                    ++_stats.framesDropped;
                }
                else if (complete)
                {
                    ++_stats.framesComplete;
                }
                else
                {
                    ++_stats.framesIncomplete;
                }
            }

        private:
            struct Sub
            {
                std::string destination;
                int port;
                std::uint64_t id;
            };

            void onPacket(int leg, Packet const& p)
            {
                bool first = false;
                {
                    std::lock_guard const lock{_mutex};
                    auto& ls = _stats.legs[static_cast<std::size_t>(leg)];
                    ++ls.packets;
                    ls.bytes += p.payload ? p.payload->size() : 0;
                    // A frame is identified by its RTP timestamp and field; the first leg to deliver wins.
                    auto const key = (static_cast<std::uint64_t>(p.meta.rtpTimestamp) << 1) | (p.meta.secondField ? 1u : 0u);
                    if (_seen.insert(key).second)
                    {
                        first = true;
                        ++_stats.packets;
                        _order.push_back(key);
                        if (_order.size() > 64)
                        {
                            _seen.erase(_order.front());
                            _order.pop_front();
                        }
                    }
                }
                if (first)
                {
                    Packet copy = p;
                    copy.meta.pktsRecv[static_cast<std::size_t>(leg)] = 1;
                    copy.meta.pktsTotal = 1;
                    _onFirst(copy);
                }
            }

            std::shared_ptr<MockNetwork> _net;
            std::function<void(Packet const&)> _onFirst;
            mutable std::mutex _mutex;
            std::vector<Sub> _subs;
            SessionStats _stats;
            std::set<std::uint64_t> _seen;
            std::deque<std::uint64_t> _order;
        };

        template <typename T>
        class ReadyQueue
        {
        public:
            void push(T value, std::size_t capacity, std::atomic<std::uint64_t>* dropped)
            {
                {
                    std::lock_guard const lock{_mutex};
                    if (_items.size() >= capacity)
                    {
                        _items.pop_front();
                        if (dropped != nullptr)
                        {
                            dropped->fetch_add(1);
                        }
                    }
                    _items.push_back(std::move(value));
                }
                _cv.notify_one();
            }

            std::optional<T> pop(std::chrono::nanoseconds timeout)
            {
                std::unique_lock lock{_mutex};
                if (!_cv.wait_for(lock, timeout, [&] { return !_items.empty(); }))
                {
                    return std::nullopt;
                }
                T v = std::move(_items.front());
                _items.pop_front();
                return v;
            }

        private:
            std::mutex _mutex;
            std::condition_variable _cv;
            std::deque<T> _items;
        };

        void sendToLegs(MockNetwork& net, std::vector<LegAddress> const& legs, Packet base, SessionStats& stats, std::mutex& statsMutex)
        {
            std::lock_guard const lock{statsMutex};
            for (std::size_t i = 0; i < legs.size() && i < 2; ++i)
            {
                if (!legs[i].enabled || legs[i].destination.empty() || legs[i].port <= 0)
                {
                    continue;
                }
                Packet p = base;
                p.destination = legs[i].destination;
                p.port = legs[i].port;
                p.leg = static_cast<int>(i);
                net.send(p);
                auto& ls = stats.legs[i];
                ++ls.packets;
                ls.bytes += p.payload ? p.payload->size() : 0;
            }
            ++stats.packets;
            ++stats.framesComplete;
            if (base.deliverTai + 1'000'000 < hostTaiNs())
            {
                ++stats.framesLate;
            }
        }

        // ---------------------------------------------------------------- video
        class MockVideoRx final : public VideoRxSession
        {
        public:
            MockVideoRx(std::shared_ptr<MockNetwork> net, VideoRxParams params, VideoRxHandler& handler)
                : _params(std::move(params))
                , _handler(handler)
                , _sub(std::move(net), [this](Packet const& p) { deliver(p); })
            {
                _sub.set(_params.legs);
            }

            ~MockVideoRx() override { _sub.clear(); }

            std::optional<FrameMeta> next(std::chrono::nanoseconds timeout) override { return _ready.pop(timeout); }
            void release() override {}
            bool updateSource(std::vector<LegAddress> const& legs) override
            {
                _params.legs = legs;
                _sub.set(legs);
                return true;
            }
            SessionStats stats() const override
            {
                auto s = _sub.stats();
                s.framesDropped += _dropped.load();
                return s;
            }

        private:
            void deliver(Packet const& p)
            {
                std::uint64_t tag = 0;
                auto* dst = _handler.acquire(p.meta, tag);
                if (dst == nullptr || !p.payload)
                {
                    _sub.countFrame(false, true);
                    return;
                }
                std::memcpy(dst, p.payload->data(), std::min(p.payload->size(), _params.format.grainBytes()));
                _sub.countFrame(p.meta.complete, false);
                auto meta = p.meta;
                meta.tag = tag;
                _ready.push(meta, 4, &_dropped);
            }

            VideoRxParams _params;
            VideoRxHandler& _handler;
            ReadyQueue<FrameMeta> _ready;
            std::atomic<std::uint64_t> _dropped{0};
            Subscription _sub;
        };

        class MockVideoTx final : public VideoTxSession
        {
        public:
            MockVideoTx(std::shared_ptr<MockNetwork> net, VideoTxParams params)
                : _net(std::move(net))
                , _params(std::move(params))
            {}

            bool send(std::uint8_t const* v210, std::int64_t transmitTai, bool secondField, std::chrono::nanoseconds) override
            {
                auto const bytes = _params.format.grainBytes();
                Packet p;
                p.deliverTai = transmitTai;
                p.meta.rtpTimestamp = timing::rtpAt(transmitTai, timing::videoClockHz);
                p.meta.secondField = secondField;
                p.payload = std::make_shared<std::vector<std::uint8_t> const>(v210, v210 + bytes);
                std::vector<LegAddress> legs;
                {
                    std::lock_guard const lock{_legsMutex};
                    legs = _params.legs;
                }
                sendToLegs(*_net, legs, std::move(p), _stats, _statsMutex);
                return true;
            }

            bool updateDestination(std::vector<LegAddress> const& legs) override
            {
                std::lock_guard const lock{_legsMutex};
                _params.legs = legs;
                return true;
            }

            SessionStats stats() const override
            {
                std::lock_guard const lock{_statsMutex};
                return _stats;
            }

        private:
            std::shared_ptr<MockNetwork> _net;
            VideoTxParams _params;
            std::mutex _legsMutex;
            mutable std::mutex _statsMutex;
            SessionStats _stats;
        };

        // ---------------------------------------------------------------- audio
        class MockAudioRx final : public AudioRxSession
        {
        public:
            MockAudioRx(std::shared_ptr<MockNetwork> net, AudioParams params)
                : _params(std::move(params))
                , _sub(std::move(net), [this](Packet const& p) { deliver(p); })
            {
                _sub.set(_params.legs);
            }

            ~MockAudioRx() override { _sub.clear(); }

            std::optional<AudioBlock> next(std::chrono::nanoseconds timeout) override
            {
                auto packet = _ready.pop(timeout);
                if (!packet)
                {
                    return std::nullopt;
                }
                _current = std::move(*packet);
                AudioBlock block;
                block.meta = _current.meta;
                block.pcm = _current.payload->data();
                block.samples = _current.samples;
                return block;
            }
            void release() override { _current = Packet{}; }
            bool updateSource(std::vector<LegAddress> const& legs) override
            {
                _params.legs = legs;
                _sub.set(legs);
                return true;
            }
            SessionStats stats() const override { return _sub.stats(); }

        private:
            void deliver(Packet const& p)
            {
                _sub.countFrame(true, false);
                _ready.push(p, 64, nullptr);
            }

            AudioParams _params;
            ReadyQueue<Packet> _ready;
            Packet _current;
            Subscription _sub;
        };

        class MockAudioTx final : public AudioTxSession
        {
        public:
            MockAudioTx(std::shared_ptr<MockNetwork> net, AudioParams params)
                : _net(std::move(net))
                , _params(std::move(params))
                , _buffer(static_cast<std::size_t>(_params.format.samplesPerBlock() * _params.format.channels * _params.format.bytesPerSample()))
            {}

            std::uint8_t* acquire() override { return _buffer.data(); }

            void send(std::int64_t transmitTai) override
            {
                Packet p;
                p.deliverTai = transmitTai;
                p.meta.rtpTimestamp = timing::rtpAt(transmitTai, _params.format.sampleRate);
                p.samples = static_cast<std::size_t>(_params.format.samplesPerBlock());
                p.payload = std::make_shared<std::vector<std::uint8_t> const>(_buffer);
                std::vector<LegAddress> legs;
                {
                    std::lock_guard const lock{_legsMutex};
                    legs = _params.legs;
                }
                sendToLegs(*_net, legs, std::move(p), _stats, _statsMutex);
            }

            bool updateDestination(std::vector<LegAddress> const& legs) override
            {
                std::lock_guard const lock{_legsMutex};
                _params.legs = legs;
                return true;
            }

            SessionStats stats() const override
            {
                std::lock_guard const lock{_statsMutex};
                return _stats;
            }

        private:
            std::shared_ptr<MockNetwork> _net;
            AudioParams _params;
            std::vector<std::uint8_t> _buffer;
            std::mutex _legsMutex;
            mutable std::mutex _statsMutex;
            SessionStats _stats;
        };

        // ---------------------------------------------------------------- ANC
        class MockAncRx final : public AncRxSession
        {
        public:
            MockAncRx(std::shared_ptr<MockNetwork> net, AncParams params)
                : _params(std::move(params))
                , _sub(std::move(net), [this](Packet const& p) { deliver(p); })
            {
                _sub.set(_params.legs);
            }

            ~MockAncRx() override { _sub.clear(); }

            std::optional<AncReceived> next(std::chrono::nanoseconds timeout) override { return _ready.pop(timeout); }
            bool updateSource(std::vector<LegAddress> const& legs) override
            {
                _params.legs = legs;
                _sub.set(legs);
                return true;
            }
            SessionStats stats() const override { return _sub.stats(); }

        private:
            void deliver(Packet const& p)
            {
                AncReceived r;
                r.meta = p.meta;
                if (p.anc)
                {
                    r.frame = *p.anc;
                }
                _sub.countFrame(true, false);
                _ready.push(std::move(r), 16, nullptr);
            }

            AncParams _params;
            ReadyQueue<AncReceived> _ready;
            Subscription _sub;
        };

        class MockAncTx final : public AncTxSession
        {
        public:
            MockAncTx(std::shared_ptr<MockNetwork> net, AncParams params)
                : _net(std::move(net))
                , _params(std::move(params))
            {}

            std::size_t send(codec::AncFrame const& frame, std::int64_t transmitTai, bool secondField, std::chrono::nanoseconds) override
            {
                auto limited = std::make_shared<codec::AncFrame>(frame);
                std::size_t dropped = 0;
                if (limited->packets.size() > maxAncPackets)
                {
                    dropped = limited->packets.size() - maxAncPackets;
                    limited->packets.resize(maxAncPackets);
                }
                Packet p;
                p.deliverTai = transmitTai;
                p.meta.rtpTimestamp = timing::rtpAt(transmitTai, timing::videoClockHz);
                p.meta.secondField = secondField;
                p.anc = limited;
                std::vector<LegAddress> legs;
                {
                    std::lock_guard const lock{_legsMutex};
                    legs = _params.legs;
                }
                sendToLegs(*_net, legs, std::move(p), _stats, _statsMutex);
                return dropped;
            }

            bool updateDestination(std::vector<LegAddress> const& legs) override
            {
                std::lock_guard const lock{_legsMutex};
                _params.legs = legs;
                return true;
            }

            SessionStats stats() const override
            {
                std::lock_guard const lock{_statsMutex};
                return _stats;
            }

        private:
            std::shared_ptr<MockNetwork> _net;
            AncParams _params;
            std::mutex _legsMutex;
            mutable std::mutex _statsMutex;
            SessionStats _stats;
        };

        class MockBackend final : public MediaBackend
        {
        public:
            explicit MockBackend(config::Config const& cfg)
                : _net(std::make_shared<MockNetwork>())
            {
                _status.backend = "mock";
                _status.testBackend = true;
                _status.mtlVersion = "mock";
                int index = 0;
                auto addPort = [&](config::NicPort const& p)
                {
                    PortStatus s;
                    s.name = p.name;
                    s.pci = p.pci;
                    s.ifname = p.ifname;
                    s.ip = p.ip;
                    char mac[18];
                    std::snprintf(mac, sizeof(mac), "02:00:00:00:00:%02x", ++index);
                    s.mac = mac;
                    s.linkUp = true;
                    s.linkSpeedMbps = 25000;
                    s.bindMode = "mock";
                    s.driver = "mock";
                    _status.ports.push_back(s);
                };
                if (!cfg.nic.portPairs.empty())
                {
                    addPort(cfg.nic.portPairs.front().primary);
                    if (cfg.nic.portPairs.front().redundant)
                    {
                        addPort(*cfg.nic.portPairs.front().redundant);
                    }
                }
            }

            std::string name() const override { return "mock"; }
            std::int64_t ptpTimeNs() const override { return hostTaiNs(); }
            BackendStatus status() const override { return _status; }

            std::unique_ptr<VideoRxSession> createVideoRx(VideoRxParams const& params, VideoRxHandler& handler) override
            {
                return std::make_unique<MockVideoRx>(_net, params, handler);
            }
            std::unique_ptr<VideoTxSession> createVideoTx(VideoTxParams const& params) override { return std::make_unique<MockVideoTx>(_net, params); }
            std::unique_ptr<AudioRxSession> createAudioRx(AudioParams const& params) override { return std::make_unique<MockAudioRx>(_net, params); }
            std::unique_ptr<AudioTxSession> createAudioTx(AudioParams const& params) override { return std::make_unique<MockAudioTx>(_net, params); }
            std::unique_ptr<AncRxSession> createAncRx(AncParams const& params) override { return std::make_unique<MockAncRx>(_net, params); }
            std::unique_ptr<AncTxSession> createAncTx(AncParams const& params) override { return std::make_unique<MockAncTx>(_net, params); }

        private:
            std::shared_ptr<MockNetwork> _net;
            BackendStatus _status;
        };
    }

    std::unique_ptr<MediaBackend> createMockBackend(config::Config const& config)
    {
        return std::make_unique<MockBackend>(config);
    }
}
