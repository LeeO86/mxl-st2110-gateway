// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <chrono>
#include <set>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "helpers.hpp"
#include "util/backoff.hpp"
#include "util/cpuset.hpp"
#include "util/fs.hpp"
#include "util/logging.hpp"
#include "util/net.hpp"
#include "util/strings.hpp"
#include "util/uuid.hpp"

using namespace mxlgw;

TEST_CASE("uuid parse, format, v4, v5")
{
    auto const u = util::parseUuid("6BA7B810-9DAD-11D1-80B4-00C04FD430C8");
    REQUIRE(u);
    CHECK(u->toString() == "6ba7b810-9dad-11d1-80b4-00c04fd430c8");
    CHECK_FALSE(util::parseUuid("6ba7b810-9dad-11d1-80b4-00c04fd430c"));
    CHECK_FALSE(util::parseUuid("6ba7b810x9dad-11d1-80b4-00c04fd430c8"));
    CHECK_FALSE(util::parseUuid("6ba7b810-9dad-11d1-80b4-00c04fd430cg"));
    CHECK(util::isUuid("00000000-0000-0000-0000-000000000000"));
    CHECK(util::parseUuid("00000000-0000-0000-0000-000000000000")->isNil());

    auto const a = util::uuidV4();
    auto const b = util::uuidV4();
    CHECK(a != b);
    CHECK((a.bytes[6] >> 4) == 4);
    CHECK((a.bytes[8] & 0xC0) == 0x80);

    // RFC 4122 appendix: v5 of "www.example.com" in the DNS namespace.
    auto const dns = util::parseUuid("6ba7b810-9dad-11d1-80b4-00c04fd430c8");
    CHECK(util::uuidV5(*dns, "www.example.com").toString() == "2ed6657d-e927-568b-95e1-2665a8aea6a2");
    CHECK(util::uuidV5(*dns, "x") == util::uuidV5(*dns, "x"));
    CHECK(util::uuidV5(*dns, "x") != util::uuidV5(*dns, "y"));
}

TEST_CASE("strings")
{
    CHECK(util::split("a, b,,c", ',') == std::vector<std::string>{"a", "b", "c"});
    CHECK(util::split("a,,c", ',', false) == std::vector<std::string>{"a", "", "c"});
    CHECK(util::trim("  x \n") == "x");
    CHECK(util::toUpperSnake("media-p") == "MEDIA_P");
    CHECK(util::toUpperSnake("Studio 1 / A") == "STUDIO_1_A");
    CHECK(util::parseInt("42") == 42);
    CHECK(util::parseInt("+7") == 7);
    CHECK(util::parseInt("-3") == -3);
    CHECK_FALSE(util::parseInt("4x"));
    CHECK_FALSE(util::parseInt(""));
    CHECK(util::parseBool("TRUE") == true);
    CHECK(util::parseBool("off") == false);
    CHECK_FALSE(util::parseBool("maybe"));
    CHECK(util::parseRational("30000/1001")->toString() == "30000/1001");
    CHECK(util::parseRational("25")->den == 1);
    CHECK_FALSE(util::parseRational("0/1"));
    CHECK_FALSE(util::parseRational("1/0"));
    CHECK(util::startsWith("mirror-x", "mirror-"));
    CHECK(util::endsWith("a.mxl-flow", ".mxl-flow"));
    CHECK(util::toLower("AbC") == "abc");
}

TEST_CASE("cpu lists")
{
    auto const set = util::parseCpuList("4-9,12,14-15");
    REQUIRE(set);
    CHECK(set->size() == 9);
    CHECK(util::formatCpuList(*set) == "4-9,12,14-15");
    CHECK_FALSE(util::parseCpuList(""));
    CHECK_FALSE(util::parseCpuList("9-4"));
    CHECK_FALSE(util::parseCpuList("a"));
    CHECK(util::disjoint(*util::parseCpuList("1-3"), *util::parseCpuList("4-5")));
    CHECK_FALSE(util::disjoint(*util::parseCpuList("1-4"), *util::parseCpuList("4-5")));
    CHECK_FALSE(util::presentCpus().empty());
}

TEST_CASE("ipv4")
{
    auto const ip = util::parseIpv4("10.1.1.21");
    REQUIRE(ip);
    CHECK(ip->toString() == "10.1.1.21");
    CHECK_FALSE(util::parseIpv4("10.1.1"));
    CHECK_FALSE(util::parseIpv4("10.1.1.256"));
    CHECK_FALSE(util::parseIpv4("10.1.1.a"));
    CHECK(util::isMulticast(*util::parseIpv4("239.1.1.1")));
    CHECK_FALSE(util::isMulticast(*util::parseIpv4("10.1.1.1")));
    CHECK(util::isNetmask(*util::parseIpv4("255.255.255.0")));
    CHECK_FALSE(util::isNetmask(*util::parseIpv4("255.0.255.0")));
    CHECK(util::prefixLength(*util::parseIpv4("255.255.240.0")) == 20);
    CHECK(util::sameSubnet(*util::parseIpv4("10.1.1.21"), *util::parseIpv4("10.1.1.1"), *util::parseIpv4("255.255.255.0")));
    CHECK_FALSE(util::sameSubnet(*util::parseIpv4("10.1.1.21"), *util::parseIpv4("10.1.2.1"), *util::parseIpv4("255.255.255.0")));
}

TEST_CASE("host address detection (G5)")
{
    auto const routes = std::string("Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
                                    "cni0\t0000F40A\t00000000\t0001\t0\t0\t0\t0000FFFF\t0\t0\t0\n"
                                    "eth1\t00000000\t0102A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
                                    "eth0\t00000000\t0101A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n");
    CHECK(util::defaultRouteInterface(routes) == std::string("eth0"));
    CHECK_FALSE(util::defaultRouteInterface("Iface\tDestination\n"));

    std::vector<util::InterfaceAddress> const addresses{{"lo", "127.0.0.1"}, {"docker0", "172.17.0.1"}, {"eth0", "192.168.1.20"}, {"eth1", "169.254.3.3"}};
    CHECK(util::pickHostAddress(addresses, std::string("eth0")) == std::string("192.168.1.20"));
    CHECK(util::pickHostAddress(addresses, std::nullopt) == std::string("172.17.0.1"));        // first non-loopback
    CHECK(util::pickHostAddress(addresses, std::string("eth1")) == std::string("172.17.0.1")); // link-local is skipped
    CHECK_FALSE(util::pickHostAddress({{"lo", "127.0.0.1"}}, std::nullopt));

    CHECK(util::isAnnounceable(*util::parseIpv4("10.0.0.1")));
    CHECK_FALSE(util::isAnnounceable(*util::parseIpv4("0.0.0.0")));
    CHECK_FALSE(util::isAnnounceable(*util::parseIpv4("127.1.2.3")));
    CHECK_FALSE(util::isAnnounceable(*util::parseIpv4("224.0.0.251")));
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    int const port = ntohs(addr.sin_port);
    CHECK_FALSE(util::processListensOn(port)); // bound, not listening
    REQUIRE(::listen(fd, 1) == 0);
    CHECK(util::processListensOn(port));
    ::close(fd);
    CHECK_FALSE(util::processListensOn(port));

    auto const live = util::defaultHostAddress();
    if (live)
    {
        CHECK(util::isAnnounceable(*util::parseIpv4(*live)));
    }
    CHECK_FALSE(util::allowedCpus().empty());
}

TEST_CASE("atomic write, mtime, fs inspection")
{
    testutil::TempDir dir;
    auto const path = dir.file("a.json");
    util::atomicWrite(path, "{}\n");
    CHECK(util::readFile(path) == std::string("{}\n"));
    auto const m1 = util::mtimeNs(path);
    REQUIRE(m1);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    util::atomicWrite(path, "{\"a\":1}\n");
    CHECK(util::mtimeNs(path) != m1);
    CHECK_FALSE(util::readFile(dir.file("missing")));
    CHECK_FALSE(util::mtimeNs(dir.file("missing")));
    CHECK_THROWS(util::atomicWrite(dir.file("no/such/dir/x"), "x"));
    auto const info = util::inspectFs(dir.file("not/yet/there"));
    CHECK_FALSE(info.exists);
    CHECK_FALSE(info.typeName.empty());
    CHECK(util::deviceOf(dir.path()).has_value());
}

TEST_CASE("backoff 500 ms doubling to 5 s with jitter")
{
    using namespace std::chrono;
    util::Backoff b(milliseconds(500), seconds(5), 0.1, 42);
    CHECK(b.nominal(0) == milliseconds(500));
    CHECK(b.nominal(1) == milliseconds(1000));
    CHECK(b.nominal(3) == milliseconds(4000));
    CHECK(b.nominal(4) == seconds(5));
    CHECK(b.nominal(40) == seconds(5));
    for (int i = 0; i < 10; ++i)
    {
        auto const d = b.next();
        auto const nominal = b.nominal(static_cast<std::uint32_t>(i));
        CHECK(d >= nominal * 9 / 10);
        CHECK(d <= nominal * 11 / 10);
    }
    CHECK(b.attempts() == 10);
    b.reset();
    CHECK(b.attempts() == 0);
}

TEST_CASE("logging: levels, history, realtime ring, rate limiter")
{
    log::configure(log::Level::Debug, log::Format::Json);
    log::info("unit_test_event", {{"k", 1}});
    auto lines = log::recent(5);
    REQUIRE_FALSE(lines.empty());
    auto const last = nlohmann::json::parse(lines.back());
    CHECK(last["event"] == "unit_test_event");
    CHECK(last["level"] == "info");
    CHECK(last["k"] == 1);

    log::trace("hidden_event");
    CHECK(nlohmann::json::parse(log::recent(1).back())["event"] == "unit_test_event");

    log::enqueueRealtimeF(log::Level::Warn, "mtl", "port %d link %s\n", 0, "up");
    log::drainNow();
    auto const rt = nlohmann::json::parse(log::recent(1).back());
    CHECK(rt["component"] == "mtl");
    CHECK(rt["message"] == "port 0 link up");

    log::startDrain();
    log::enqueueRealtimeF(log::Level::Info, "mtl", "async");
    log::stopDrain();
    CHECK(nlohmann::json::parse(log::recent(1).back())["message"] == "async");

    log::configure(log::Level::Info, log::Format::Text);
    log::info("text_event", {{"a", "b"}});
    CHECK(log::recent(1).back().find("text_event a=b") != std::string::npos);
    log::configure(log::Level::Info, log::Format::Json);

    CHECK(log::parseLevel("warning") == log::Level::Warn);
    CHECK_FALSE(log::parseLevel("loud"));
    CHECK(std::string(log::levelName(log::Level::Error)) == "error");

    log::RateLimiter limiter(std::chrono::hours(1));
    CHECK(limiter.allow("x"));
    CHECK_FALSE(limiter.allow("x"));
    CHECK(limiter.allow("y"));
    limiter.reset("x");
    CHECK(limiter.allow("x"));
}
