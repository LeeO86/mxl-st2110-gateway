// SPDX-License-Identifier: MIT
#include "config/schema.hpp"

#include <mutex>
#include <regex>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "config/schema_embedded.hpp"

namespace mxlgw::config
{
    namespace
    {
        std::string escapePointerToken(std::string const& token)
        {
            std::string out;
            for (char const c : token)
            {
                if (c == '~')
                {
                    out += "~0";
                }
                else if (c == '/')
                {
                    out += "~1";
                }
                else
                {
                    out += c;
                }
            }
            return out;
        }

        bool typeMatches(std::string const& type, nlohmann::json const& v)
        {
            if (type == "object")
            {
                return v.is_object();
            }
            if (type == "array")
            {
                return v.is_array();
            }
            if (type == "string")
            {
                return v.is_string();
            }
            if (type == "integer")
            {
                return v.is_number_integer() || (v.is_number_float() && v.get<double>() == static_cast<double>(static_cast<long long>(v.get<double>())));
            }
            if (type == "number")
            {
                return v.is_number();
            }
            if (type == "boolean")
            {
                return v.is_boolean();
            }
            if (type == "null")
            {
                return v.is_null();
            }
            throw std::logic_error("schema: unknown type " + type);
        }

        std::string describe(nlohmann::json const& v)
        {
            auto text = v.dump();
            if (text.size() > 60)
            {
                text = text.substr(0, 57) + "...";
            }
            return text;
        }

        std::regex const& cachedRegex(std::string const& pattern)
        {
            static std::mutex mutex;
            static std::unordered_map<std::string, std::regex> cache;
            std::lock_guard const lock{mutex};
            auto it = cache.find(pattern);
            if (it == cache.end())
            {
                it = cache.emplace(pattern, std::regex(pattern, std::regex::ECMAScript)).first;
            }
            return it->second;
        }

        std::set<std::string> const& annotationKeywords()
        {
            static std::set<std::string> const keys = {"$schema", "$id", "$defs", "$comment", "title", "description", "default", "examples", "format"};
            return keys;
        }
    }

    SchemaValidator::SchemaValidator(nlohmann::json schema)
        : _schema(std::move(schema))
    {}

    nlohmann::json const& SchemaValidator::resolve(std::string const& ref) const
    {
        if (ref.rfind("#", 0) != 0)
        {
            throw std::logic_error("schema: only local $ref supported: " + ref);
        }
        return _schema.at(nlohmann::json::json_pointer(ref.substr(1)));
    }

    ValidationErrors SchemaValidator::validate(nlohmann::json const& instance) const
    {
        ValidationErrors errors;
        check(_schema, instance, "", errors);
        return errors;
    }

    void SchemaValidator::check(nlohmann::json const& schema, nlohmann::json const& v, std::string const& ptr, ValidationErrors& errors) const
    {
        if (schema.is_boolean())
        {
            if (!schema.get<bool>())
            {
                errors.push_back({ptr, "not allowed"});
            }
            return;
        }
        if (!schema.is_object())
        {
            throw std::logic_error("schema: subschema must be an object or boolean at " + ptr);
        }

        for (auto it = schema.begin(); it != schema.end(); ++it)
        {
            auto const& key = it.key();
            auto const& value = it.value();
            if (annotationKeywords().count(key) != 0)
            {
                continue;
            }
            if (key == "$ref")
            {
                check(resolve(value.get<std::string>()), v, ptr, errors);
            }
            else if (key == "type")
            {
                bool ok = false;
                std::string expected;
                if (value.is_string())
                {
                    ok = typeMatches(value.get<std::string>(), v);
                    expected = value.get<std::string>();
                }
                else
                {
                    for (auto const& t : value)
                    {
                        ok = ok || typeMatches(t.get<std::string>(), v);
                        expected += (expected.empty() ? "" : " or ") + t.get<std::string>();
                    }
                }
                if (!ok)
                {
                    errors.push_back({ptr, "must be of type " + expected});
                    return; // other keywords would only add noise
                }
            }
            else if (key == "enum")
            {
                bool found = false;
                for (auto const& candidate : value)
                {
                    found = found || candidate == v;
                }
                if (!found)
                {
                    errors.push_back({ptr, "must be one of " + value.dump() + " (is " + describe(v) + ")"});
                }
            }
            else if (key == "const")
            {
                if (value != v)
                {
                    errors.push_back({ptr, "must be " + value.dump()});
                }
            }
            else if (key == "properties")
            {
                if (v.is_object())
                {
                    for (auto p = value.begin(); p != value.end(); ++p)
                    {
                        if (v.contains(p.key()))
                        {
                            check(p.value(), v.at(p.key()), ptr + "/" + escapePointerToken(p.key()), errors);
                        }
                    }
                }
            }
            else if (key == "required")
            {
                if (v.is_object())
                {
                    for (auto const& name : value)
                    {
                        if (!v.contains(name.get<std::string>()))
                        {
                            errors.push_back({ptr + "/" + escapePointerToken(name.get<std::string>()), "is required"});
                        }
                    }
                }
            }
            else if (key == "additionalProperties")
            {
                if (v.is_object())
                {
                    auto const* props = schema.contains("properties") ? &schema.at("properties") : nullptr;
                    for (auto p = v.begin(); p != v.end(); ++p)
                    {
                        if (props != nullptr && props->contains(p.key()))
                        {
                            continue;
                        }
                        auto const childPtr = ptr + "/" + escapePointerToken(p.key());
                        if (value.is_boolean() && !value.get<bool>())
                        {
                            errors.push_back({childPtr, "unknown property"});
                        }
                        else
                        {
                            check(value, p.value(), childPtr, errors);
                        }
                    }
                }
            }
            else if (key == "items")
            {
                if (v.is_array())
                {
                    for (std::size_t i = 0; i < v.size(); ++i)
                    {
                        check(value, v.at(i), ptr + "/" + std::to_string(i), errors);
                    }
                }
            }
            else if (key == "minItems" || key == "maxItems")
            {
                if (v.is_array())
                {
                    auto const n = value.get<std::size_t>();
                    if (key == "minItems" && v.size() < n)
                    {
                        errors.push_back({ptr, "must have at least " + std::to_string(n) + " item(s)"});
                    }
                    if (key == "maxItems" && v.size() > n)
                    {
                        errors.push_back({ptr, "must have at most " + std::to_string(n) + " item(s)"});
                    }
                }
            }
            else if (key == "minimum" || key == "maximum")
            {
                if (v.is_number())
                {
                    auto const limit = value.get<double>();
                    auto const actual = v.get<double>();
                    if (key == "minimum" && actual < limit)
                    {
                        errors.push_back({ptr, "must be >= " + value.dump()});
                    }
                    if (key == "maximum" && actual > limit)
                    {
                        errors.push_back({ptr, "must be <= " + value.dump()});
                    }
                }
            }
            else if (key == "minLength" || key == "maxLength")
            {
                if (v.is_string())
                {
                    auto const n = value.get<std::size_t>();
                    auto const len = v.get<std::string>().size();
                    if (key == "minLength" && len < n)
                    {
                        errors.push_back({ptr, "must be at least " + std::to_string(n) + " character(s)"});
                    }
                    if (key == "maxLength" && len > n)
                    {
                        errors.push_back({ptr, "must be at most " + std::to_string(n) + " character(s)"});
                    }
                }
            }
            else if (key == "pattern")
            {
                if (v.is_string() && !std::regex_search(v.get<std::string>(), cachedRegex(value.get<std::string>())))
                {
                    errors.push_back({ptr, "has an invalid format (" + describe(v) + ")"});
                }
            }
            else if (key == "allOf")
            {
                for (auto const& sub : value)
                {
                    check(sub, v, ptr, errors);
                }
            }
            else if (key == "anyOf" || key == "oneOf")
            {
                std::size_t matches = 0;
                ValidationErrors first;
                for (auto const& sub : value)
                {
                    ValidationErrors local;
                    check(sub, v, ptr, local);
                    if (local.empty())
                    {
                        ++matches;
                    }
                    else if (first.empty())
                    {
                        first = std::move(local);
                    }
                }
                if (matches == 0)
                {
                    // Report the first alternative's errors: usually the intended (non-null) form.
                    errors.insert(errors.end(), first.begin(), first.end());
                }
                else if (key == "oneOf" && matches > 1)
                {
                    errors.push_back({ptr, "matches more than one alternative"});
                }
            }
            else
            {
                throw std::logic_error("schema: unsupported keyword " + key + " at " + ptr);
            }
        }
    }

    std::string_view gatewaySchemaText()
    {
        return embedded::configSchemaJson();
    }

    SchemaValidator const& gatewaySchema()
    {
        static SchemaValidator const validator{nlohmann::json::parse(gatewaySchemaText())};
        return validator;
    }
}
