// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <filesystem>

#include "config/schema.hpp"
#include "helpers.hpp"
#include "util/fs.hpp"

using namespace mxlgw;
using nlohmann::json;

namespace
{
    bool hasError(config::ValidationErrors const& errors, std::string const& pointer)
    {
        for (auto const& e : errors)
        {
            if (e.pointer == pointer)
            {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("schema validator keywords")
{
    config::SchemaValidator v(json::parse(R"({
      "type": "object",
      "required": ["a"],
      "additionalProperties": false,
      "properties": {
        "a": {"type": "integer", "minimum": 1, "maximum": 5},
        "b": {"type": ["string", "null"], "minLength": 2, "maxLength": 3, "pattern": "^x"},
        "c": {"enum": ["p", "q"]},
        "d": {"const": 7},
        "e": {"type": "array", "items": {"$ref": "#/$defs/n"}, "minItems": 1, "maxItems": 2},
        "f": {"anyOf": [{"type": "boolean"}, {"type": "null"}]},
        "g": {"oneOf": [{"type": "integer"}, {"type": "number"}]},
        "h": {"allOf": [{"type": "object"}, {"required": ["z"]}]},
        "i": {"type": "object", "additionalProperties": {"type": "string"}},
        "j": false
      },
      "$defs": {"n": {"type": "number"}}
    })"));

    CHECK(v.validate(json::parse(R"({"a": 3})")).empty());
    CHECK(hasError(v.validate(json::parse(R"({})")), "/a"));
    CHECK(hasError(v.validate(json::parse(R"({"a": 0})")), "/a"));
    CHECK(hasError(v.validate(json::parse(R"({"a": 9})")), "/a"));
    CHECK(hasError(v.validate(json::parse(R"({"a": "x"})")), "/a"));
    CHECK(hasError(v.validate(json::parse(R"({"a": 1, "b": "y1"})")), "/b"));
    CHECK(hasError(v.validate(json::parse(R"({"a": 1, "b": "x"})")), "/b"));
    CHECK(hasError(v.validate(json::parse(R"({"a": 1, "b": "xabc"})")), "/b"));
    CHECK(v.validate(json::parse(R"({"a": 1, "b": null})")).empty());
    CHECK(hasError(v.validate(json::parse(R"({"a": 1, "c": "r"})")), "/c"));
    CHECK(hasError(v.validate(json::parse(R"({"a": 1, "d": 8})")), "/d"));
    CHECK(hasError(v.validate(json::parse(R"({"a": 1, "e": []})")), "/e"));
    CHECK(hasError(v.validate(json::parse(R"({"a": 1, "e": [1, 2, 3]})")), "/e"));
    CHECK(hasError(v.validate(json::parse(R"({"a": 1, "e": ["s"]})")), "/e/0"));
    CHECK(hasError(v.validate(json::parse(R"({"a": 1, "f": 1})")), "/f"));
    CHECK(hasError(v.validate(json::parse(R"({"a": 1, "g": 1})")), "/g"));
    CHECK(v.validate(json::parse(R"({"a": 1, "g": 1.5})")).empty());
    CHECK(hasError(v.validate(json::parse(R"({"a": 1, "h": {}})")), "/h/z"));
    CHECK(hasError(v.validate(json::parse(R"({"a": 1, "i": {"k": 1}})")), "/i/k"));
    CHECK(hasError(v.validate(json::parse(R"({"a": 1, "j": 1})")), "/j"));
    CHECK(hasError(v.validate(json::parse(R"({"a": 1, "zz": 1})")), "/zz"));

    config::SchemaValidator bad(json::parse(R"({"frobnicate": 1})"));
    CHECK_THROWS(bad.validate(json(1)));
}

TEST_CASE("gateway schema accepts the shipped examples")
{
    auto const& schema = config::gatewaySchema();
    for (auto const* name : {"gateway.minimal.json", "gateway.example.json", "gateway.fabrics-host-a.json", "gateway.fabrics-host-b.json"})
    {
        auto const text = util::readFile(std::string(MXLGW_SOURCE_DIR) + "/config/examples/" + name);
        REQUIRE(text);
        auto const errors = schema.validate(json::parse(*text));
        INFO(name << ": " << (errors.empty() ? std::string() : errors.front().pointer + " " + errors.front().message));
        CHECK(errors.empty());
    }
}

TEST_CASE("gateway schema rejects bad shapes with JSON pointers")
{
    auto const& schema = config::gatewaySchema();
    auto j = testutil::sampleConfig();
    j["groups"][0]["video"][0]["width"] = 1280;
    CHECK(hasError(schema.validate(j), "/groups/0/video/0/width"));

    j = testutil::sampleConfig();
    j["groups"][0]["video"][0]["bogus"] = 1;
    CHECK(hasError(schema.validate(j), "/groups/0/video/0/bogus"));

    j = testutil::sampleConfig();
    j["nic"]["port_pairs"].push_back(j["nic"]["port_pairs"][0]);
    CHECK(hasError(schema.validate(j), "/nic/port_pairs"));

    j = testutil::sampleConfig();
    j["schema_version"] = 2;
    CHECK(hasError(schema.validate(j), "/schema_version"));

    j = testutil::sampleConfig();
    j["groups"][0]["uid"] = "nope";
    CHECK(hasError(schema.validate(j), "/groups/0/uid"));
    CHECK_FALSE(config::gatewaySchemaText().empty());
}
