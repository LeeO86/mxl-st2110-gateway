// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "app/connection_state.hpp"
#include "app/control_queue.hpp"
#include "app/services.hpp"
#include "config/env.hpp"
#include "config/store.hpp"
#include "group/domain_resolver.hpp"
#include "group/group_manager.hpp"
#include "mtl/backend.hpp"
#include "mxlbridge/bootstrap.hpp"
#include "mxlbridge/domainscan.hpp"
#include "mxlbridge/instance.hpp"
#include "nmos/node_api.hpp"
#include "ops/clock_supervisor.hpp"
#include "ops/http.hpp"
#include "ops/metrics_export.hpp"

namespace mxlgw::app
{
    struct Options
    {
        std::string configPath = "/config/gateway.json";
        config::EnvLookup env = config::processEnvironment();
        bool preflightOnly = false;
        /// Tests: skip the HTTP/NMOS layer.
        bool withNmos = true;
    };

    /// Exit codes (§14.4).
    inline constexpr int exitOk = 0;
    inline constexpr int exitRuntime = 1;
    inline constexpr int exitTempFail = 75; // EX_TEMPFAIL: a listening port cannot be bound
    inline constexpr int exitConfig = 78;   // EX_CONFIG
    inline constexpr int exitSigint = 130;  // 128 + SIGINT
    inline constexpr int exitSigterm = 143; // 128 + SIGTERM

    /// The gateway process: configuration, domains, media backend, groups, NMOS node and the
    /// HTTP routes on one port (§3.1). run() blocks until stop()/a signal and returns the exit code.
    class Application final : public Services
    {
    public:
        explicit Application(Options options);
        ~Application() override;
        Application(Application const&) = delete;
        Application& operator=(Application const&) = delete;

        int run();
        /// Starts everything; returns 0 on success or the exit code to terminate with.
        int start();
        void stop(int exitCode = exitOk);
        /// SIGTERM/SIGINT (§14.4): graceful shutdown, exit 128 + signal, own domains removed with mxl.cleanup_on_exit.
        void terminate(int signal);
        void shutdown();
        bool stopping() const { return _stopRequested.load(); }
        int exitCode() const { return _exitCode.load(); }
        /// node.shutdown_timeout_s of the loaded configuration (10 before it is loaded).
        int shutdownTimeoutS() const { return _shutdownTimeoutS.load(); }
        group::GroupManager* groups() { return _groups.get(); }
        nmosnode::Node* node() { return _node.get(); }
        nmosnode::Node* st2110Node() { return _st2110Node.get(); }
        ops::Router const& router() const { return _router; }

        // Services
        config::ConfigStore& store() override { return *_store; }
        bool setupMode() const override { return _setupMode; }
        bool restartRequired() const override;
        std::vector<std::string> restartReasons() const override;
        void markRestartRequired(std::string const& reason) override;
        nlohmann::json applyGroups() override;
        nlohmann::json status() override;
        nlohmann::json nic() override;
        nlohmann::json ptp() override;
        nlohmann::json domains() override;
        std::optional<nlohmann::json> flows(std::string const& domainId) override;
        nlohmann::json nmos() override;
        std::vector<ops::CheckResult> preflight() override;
        ops::Readiness readiness() override;
        std::string metrics() override;
        std::string_view adminHtml() const override;
        void requestRestart() override;

    private:
        int bootstrapDomains(config::Config const& cfg);
        int startHttp(config::Config const& cfg);
        void deregister(std::chrono::steady_clock::time_point deadline);
        void cleanupDomains();
        void onActivation(nmosnode::Activation const& activation);
        std::string checkMxlFlow(util::Uuid const& essenceUid, std::optional<util::Uuid> const& domainId, util::Uuid const& flowId);
        void housekeeping();
        void refreshDomainUsage();
        nmosnode::ClockInfo clockInfo() const;
        std::vector<nmosnode::InterfaceInfo> interfaces() const;
        nlohmann::json backendJson() const;
        void countActivation(std::string const& kind, std::string const& transport, std::string const& result);

        Options _options;
        std::unique_ptr<config::ConfigStore> _store;
        bool _setupMode = false;
        std::atomic<bool> _stopRequested{false};
        std::atomic<bool> _signalled{false};
        std::atomic<int> _exitCode{exitOk};
        std::atomic<int> _shutdownTimeoutS{10};
        std::string _hostAddress;
        std::mutex _stopMutex;
        std::condition_variable _stopCv;

        mutable std::mutex _restartMutex;
        std::set<std::string> _restartReasons;

        mxlbridge::InstanceRegistry _instances;
        std::vector<mxlbridge::BootstrapResult> _bootstrap;
        std::vector<group::DomainRuntime> _domains;
        std::unique_ptr<mxlbridge::DomainDirectory> _directory;
        std::unique_ptr<group::DomainResolver> _resolver;
        std::unique_ptr<media::MediaBackend> _backend;
        std::unique_ptr<ops::ClockSupervisor> _clock;
        std::unique_ptr<group::GroupManager> _groups;
        std::unique_ptr<ConnectionState> _connections;
        std::unique_ptr<ControlQueue> _control;
        ops::Router _router;
        std::unique_ptr<nmosnode::Node> _node;       // MXL node
        std::unique_ptr<nmosnode::Node> _st2110Node; // ST 2110 node (node.st2110.enabled)
        std::unique_ptr<nmosnode::HttpServer> _http; // setup mode, or the gateway routes on node.web_port

        mutable std::mutex _cacheMutex;
        std::vector<ops::ConfiguredDomainUsage> _domainUsage;
        bool _domainsOk = true;
        std::map<ops::ActivationKey, std::uint64_t> _activations;
        std::string _lastGmid;

        std::thread _housekeeping;
        bool _started = false;
    };
}
