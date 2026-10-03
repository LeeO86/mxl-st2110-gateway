// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>

#include "helpers.hpp"
#include "mxlbridge/bootstrap.hpp"
#include "mxlbridge/domaindef.hpp"
#include "mxlbridge/domainscan.hpp"
#include "util/fs.hpp"

using namespace mxlgw;
using mxlbridge::BootstrapError;

namespace
{
    config::Domain domainAt(std::string const& path)
    {
        config::Domain d;
        d.name = "main";
        d.path = path;
        return d;
    }

    std::string eventOf(config::Domain const& d, mxlbridge::BootstrapOptions const& o = {})
    {
        try
        {
            mxlbridge::bootstrapDomain(d, o);
        }
        catch (BootstrapError const& ex)
        {
            return ex.event();
        }
        return {};
    }
}

TEST_CASE("bootstrap: a plain directory on a non-tmpfs filesystem must fail (exit 78)")
{
    testutil::TempDir disk(false);
    if (disk.tmpfs())
    {
        MESSAGE("skipped: /tmp is tmpfs on this host");
        return;
    }
    CHECK(eventOf(domainAt(disk.file("main"))) == "mxl_domain_not_tmpfs");
    CHECK_FALSE(std::filesystem::exists(disk.file("main"))); // nothing created
}

TEST_CASE("bootstrap: creates directory, domain_def.json and options.json on tmpfs")
{
    testutil::TempDir shm;
    if (!shm.tmpfs())
    {
        MESSAGE("skipped: no tmpfs available");
        return;
    }
    auto d = domainAt(shm.file("root/main"));
    d.label = "Studio 1";
    d.historyDurationNs = 300'000'000;
    auto const r = mxlbridge::bootstrapDomain(d);
    CHECK(r.directoryCreated);
    CHECK(r.domainDefCreated);
    CHECK(r.optionsCreated);
    REQUIRE(r.writeBackId);
    CHECK(*r.writeBackId == r.id);
    CHECK(r.label == "Studio 1");
    CHECK(r.fsType == "tmpfs");

    struct stat st
    {};
    REQUIRE(::stat(d.path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0775);

    std::string error;
    auto const def = mxlbridge::parseDomainDef(*util::readFile(mxlbridge::domainDefPath(d.path)), error);
    REQUIRE(def);
    CHECK(def->id == r.id);
    auto const opts = nlohmann::json::parse(*util::readFile(mxlbridge::optionsPath(d.path)));
    CHECK(opts[mxlbridge::historyDurationOption] == 300'000'000);

    SUBCASE("second start adopts the id and changes nothing")
    {
        auto const before = *util::readFile(mxlbridge::domainDefPath(d.path));
        auto d2 = d;
        d2.id = r.id;
        d2.historyDurationNs = 100'000'000; // differs: warning, file wins
        auto const r2 = mxlbridge::bootstrapDomain(d2);
        CHECK_FALSE(r2.domainDefCreated);
        CHECK_FALSE(r2.optionsCreated);
        CHECK(r2.id == r.id);
        CHECK_FALSE(r2.writeBackId);
        CHECK(*util::readFile(mxlbridge::domainDefPath(d.path)) == before);
        CHECK(std::find(r2.warnings.begin(), r2.warnings.end(), "mxl_domain_options_mismatch") != r2.warnings.end());
        CHECK(nlohmann::json::parse(*util::readFile(mxlbridge::optionsPath(d.path)))[mxlbridge::historyDurationOption] == 300'000'000);
    }
    SUBCASE("config id differs: file wins, id written back (C6)")
    {
        auto d2 = d;
        d2.id = util::uuidV4();
        auto const r2 = mxlbridge::bootstrapDomain(d2);
        CHECK(r2.id == r.id);
        REQUIRE(r2.writeBackId);
        CHECK(*r2.writeBackId == r.id);
        CHECK(r2.warnings.front() == "domain_id_mismatch");
    }
    SUBCASE("id from the environment or the seed is never written back, domain_def.json never overwritten")
    {
        auto const before = *util::readFile(mxlbridge::domainDefPath(d.path));
        auto d2 = d;
        d2.id = util::uuidV4();
        mxlbridge::BootstrapOptions o;
        o.idNotPersisted = true;
        auto const r2 = mxlbridge::bootstrapDomain(d2, o);
        CHECK(r2.id == r.id);
        CHECK_FALSE(r2.writeBackId);
        CHECK(r2.warnings.front() == "domain_id_mismatch");
        CHECK(*util::readFile(mxlbridge::domainDefPath(d.path)) == before);
    }
    SUBCASE("seed-derived id creates domain_def.json without write-back")
    {
        std::filesystem::remove_all(d.path);
        auto d2 = d;
        d2.id = util::uuidV5(config::seedNamespaceOf("prod1-gw"), "mxl-domain:" + d.name);
        mxlbridge::BootstrapOptions o;
        o.idNotPersisted = true;
        auto const r2 = mxlbridge::bootstrapDomain(d2, o);
        CHECK(r2.domainDefCreated);
        CHECK(r2.id == *d2.id);
        CHECK_FALSE(r2.writeBackId);
    }
    SUBCASE("tmpfs wiped: the configured id re-creates the same domain_def.json")
    {
        std::filesystem::remove_all(d.path);
        auto d2 = d;
        d2.id = r.id;
        auto const r2 = mxlbridge::bootstrapDomain(d2);
        CHECK(r2.domainDefCreated);
        CHECK(r2.id == r.id);
        CHECK_FALSE(r2.writeBackId);
    }
}

TEST_CASE("bootstrap: mirror domains and invalid domain_def.json are refused")
{
    testutil::TempDir shm;
    if (!shm.tmpfs())
    {
        MESSAGE("skipped: no tmpfs available");
        return;
    }
    CHECK(eventOf(domainAt(shm.file("mirror-0f6e2c9a-1d43-4bb5-9a0e-4c1d2b3a4f55"))) == "mxl_domain_is_mirror");
    CHECK_FALSE(std::filesystem::exists(shm.file("mirror-0f6e2c9a-1d43-4bb5-9a0e-4c1d2b3a4f55")));

    auto const marked = shm.file("main");
    testutil::writeFile(marked + "/domain_def.json",
                        R"({"id":"0f6e2c9a-1d43-4bb5-9a0e-4c1d2b3a4f55","label":"x","description":"","tags":{},
            "x-mxl-fabrics-agent":{"mirror":true,"source_host_id":"host-a","owner_host_id":"host-b"}})");
    CHECK(eventOf(domainAt(marked)) == "mxl_domain_is_mirror");

    auto const invalid = shm.file("broken");
    testutil::writeFile(invalid + "/domain_def.json", R"({"id":"not-a-uuid","label":"x","description":"","tags":{}})");
    auto const before = *util::readFile(invalid + "/domain_def.json");
    CHECK(eventOf(domainAt(invalid)) == "mxl_domain_def_invalid");
    CHECK(*util::readFile(invalid + "/domain_def.json") == before);

    auto const foreign = shm.file("foreign");
    testutil::writeFile(foreign + "/domain_def.json",
                        R"({"id":"0f6e2c9a-1d43-4bb5-9a0e-4c1d2b3a4f56","label":"x","description":"","tags":{},"x-vendor":{"a":1}})");
    auto const r = mxlbridge::bootstrapDomain(domainAt(foreign));
    CHECK(r.id.toString() == "0f6e2c9a-1d43-4bb5-9a0e-4c1d2b3a4f56"); // unknown fields ignored
}

TEST_CASE("bootstrap: own stale flows are removed only when unlocked")
{
    testutil::TempDir shm;
    auto const a = *util::parseUuid("5fbec3b1-1b0f-4e5b-b5c4-1a2b3c4d5e01");
    auto const b = *util::parseUuid("5fbec3b1-1b0f-4e5b-b5c4-1a2b3c4d5e02");
    auto const foreign = *util::parseUuid("5fbec3b1-1b0f-4e5b-b5c4-1a2b3c4d5e03");
    for (auto const& id : {a, b, foreign})
    {
        testutil::writeFile(mxlbridge::flowDirPath(shm.path(), id) + "/data", "x");
    }
    auto const lockedPath = mxlbridge::flowDirPath(shm.path(), b) + "/data";
    int const fd = ::open(lockedPath.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    REQUIRE(::flock(fd, LOCK_SH) == 0); // a reader or writer holds the flow
    CHECK(mxlbridge::flowInUse(shm.path(), b));
    CHECK_FALSE(mxlbridge::flowInUse(shm.path(), a));

    auto const removed = mxlbridge::removeStaleFlows(shm.path(), {a, b});
    ::close(fd);
    REQUIRE(removed.size() == 1);
    CHECK(removed[0] == a);
    CHECK_FALSE(std::filesystem::exists(mxlbridge::flowDirPath(shm.path(), a)));
    CHECK(std::filesystem::exists(mxlbridge::flowDirPath(shm.path(), b)));
    CHECK(std::filesystem::exists(mxlbridge::flowDirPath(shm.path(), foreign))); // not ours: untouched
}
