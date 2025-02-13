#pragma once

#include <Server/HTTP/HTTPServerRequest.h>
#include <Common/Exception.h>
#include <Common/StringUtils.h>
#include <base/find_symbols.h>
#include <Common/re2.h>

#include <Poco/StringTokenizer.h>
#include <Poco/Util/LayeredConfiguration.h>

#include <absl/container/inlined_vector.h>

#include <unordered_map>

namespace DB
{

namespace ErrorCodes
{
    extern const int CANNOT_COMPILE_REGEXP;
}

using CompiledRegexPtr = std::shared_ptr<const re2::RE2>;

static inline bool checkRegexExpression(std::string_view match_str, const CompiledRegexPtr & compiled_regex)
{
    int num_captures = compiled_regex->NumberOfCapturingGroups() + 1;

    absl::InlinedVector<std::string_view, 5> matches(num_captures);
    return compiled_regex->Match(
        {match_str.data(), match_str.size()}, 0, match_str.size(), re2::RE2::Anchor::ANCHOR_BOTH, matches.data(), num_captures);
}

static inline bool checkExpression(std::string_view match_str, const std::pair<String, CompiledRegexPtr> & expression)
{
    if (expression.second)
        return checkRegexExpression(match_str, expression.second);

    return match_str == expression.first;
}

static inline auto methodsFilter(const Poco::Util::AbstractConfiguration & config, const std::string & config_path)
{
    std::vector<String> methods;
    Poco::StringTokenizer tokenizer(config.getString(config_path), ",");

    for (const auto & iterator : tokenizer)
        methods.emplace_back(Poco::toUpper(Poco::trim(iterator)));

    return [methods](const HTTPServerRequest & request) { return std::count(methods.begin(), methods.end(), request.getMethod()); };
}

static inline auto getExpression(const std::string & expression)
{
    if (!startsWith(expression, "regex:"))
        return std::make_pair(expression, CompiledRegexPtr{});

    auto compiled_regex = std::make_shared<const re2::RE2>(expression.substr(6));

    if (!compiled_regex->ok())
        throw Exception(ErrorCodes::CANNOT_COMPILE_REGEXP, "cannot compile re2: {} for http handling rule, error: {}. "
                        "Look at https://github.com/google/re2/wiki/Syntax for reference.",
                        expression, compiled_regex->error());
    return std::make_pair(expression, compiled_regex);
}

static inline auto urlFilter(const Poco::Util::AbstractConfiguration & config, const std::string & config_path)
{
    return [expression = getExpression(config.getString(config_path))](const HTTPServerRequest & request)
    {
        const auto & uri = request.getURI();
        const auto & end = find_first_symbols<'?'>(uri.data(), uri.data() + uri.size());

        return checkExpression(std::string_view(uri.data(), end - uri.data()), expression);
    };
}

static inline auto emptyQueryStringFilter()
{
    return [](const HTTPServerRequest & request)
    {
        const auto & uri = request.getURI();
        return !uri.contains('?');
    };
}

static inline auto headersFilter(const Poco::Util::AbstractConfiguration & config, const std::string & prefix)
{
    std::unordered_map<String, std::pair<String, CompiledRegexPtr>> headers_expression;
    Poco::Util::AbstractConfiguration::Keys headers_name;
    config.keys(prefix, headers_name);

    for (const auto & header_name : headers_name)
    {
        const auto & expression = getExpression(config.getString(prefix + "." + header_name));
        checkExpression("", expression);    /// Check expression syntax is correct
        headers_expression.emplace(std::make_pair(header_name, expression));
    }

    return [headers_expression](const HTTPServerRequest & request)
    {
        for (const auto & [header_name, header_expression] : headers_expression)
        {
            const auto header_value = request.get(header_name, "");
            if (!checkExpression(std::string_view(header_value.data(), header_value.size()), header_expression))
                return false;
        }

        return true;
    };
}

}
