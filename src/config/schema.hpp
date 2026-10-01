// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace mxlgw::config
{
    struct ValidationError
    {
        std::string pointer; // JSON pointer into the configuration ("" = root)
        std::string message;
    };

    using ValidationErrors = std::vector<ValidationError>;

    /// Validator for the JSON Schema (draft 2020-12) keywords used by schema/gateway-config.schema.json:
    /// type, enum, const, properties, required, additionalProperties, items, minItems, maxItems,
    /// minimum, maximum, minLength, maxLength, pattern, allOf, anyOf, oneOf, $ref (local "#/...").
    /// Annotation keywords are ignored. Unknown assertion keywords are a programming error and throw.
    class SchemaValidator
    {
    public:
        explicit SchemaValidator(nlohmann::json schema);

        ValidationErrors validate(nlohmann::json const& instance) const;

        nlohmann::json const& schema() const { return _schema; }

    private:
        void check(nlohmann::json const& schema, nlohmann::json const& instance, std::string const& pointer, ValidationErrors& errors) const;
        nlohmann::json const& resolve(std::string const& ref) const;

        nlohmann::json _schema;
    };

    /// The gateway's own schema (embedded at build time).
    SchemaValidator const& gatewaySchema();
    std::string_view gatewaySchemaText();
}
