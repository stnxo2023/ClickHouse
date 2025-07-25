#pragma once

#include <base/types.h>
#include <base/defines.h>
#include <Common/JSONParsers/ElementTypes.h>


namespace DB
{

/// This class can be used as an argument for the template class FunctionJSON when we unable to parse JSONs.
/// It can't do anything useful and just throws an exception.
struct DummyJSONParser
{
    class Array;
    class Object;

    /// References an element in a JSON document, representing a JSON null, boolean, string, number,
    /// array or object.
    class Element
    {
    public:
        Element() = default;
        static ElementType type() { return ElementType::NULL_VALUE; }
        static bool isInt64() { return false; }
        static bool isUInt64() { return false; }
        static bool isDouble() { return false; }
        static bool isString() { return false; }
        static bool isArray() { return false; }
        static bool isObject() { return false; }
        static bool isBool() { return false; }
        static bool isNull() { return false; }

        static Int64 getInt64() { return 0; }
        static UInt64 getUInt64() { return 0; }
        static double getDouble() { return 0; }
        static bool getBool() { return false; }
        static std::string_view getString() { return {}; }
        static Array getArray() { return {}; }
        static Object getObject() { return {}; }

        static Element getElement() { return {}; }
    };

    /// References an array in a JSON document.
    class Array
    {
    public:
        class Iterator
        {
        public:
            Element operator*() const { return {}; }
            Iterator & operator++() { return *this; }
            Iterator operator++(int) { return *this; } /// NOLINT
            friend bool operator==(const Iterator &, const Iterator &) { return true; }
            friend bool operator!=(const Iterator &, const Iterator &) { return false; }
        };

        static Iterator begin() { return {}; }
        static Iterator end() { return {}; }
        static size_t size() { return 0; }
        Element operator[](size_t) const { return {}; }
    };

    using KeyValuePair = std::pair<std::string_view, Element>;

    /// References an object in a JSON document.
    class Object
    {
    public:
        class Iterator
        {
        public:
            KeyValuePair operator*() const { return {}; }
            Iterator & operator++() { return *this; }
            Iterator operator++(int) { return *this; } /// NOLINT
            friend bool operator==(const Iterator &, const Iterator &) { return true; }
            friend bool operator!=(const Iterator &, const Iterator &) { return false; }
        };

        static Iterator begin() { return {}; }
        static Iterator end() { return {}; }
        static size_t size() { return 0; }
        bool find(std::string_view, Element &) const { return false; } /// NOLINT
        bool findCaseInsensitive(std::string_view, Element &) const { return false; } /// NOLINT

#if 0
        /// Optional: Provides access to an object's element by index.
        KeyValuePair operator[](size_t) const { return {}; }
#endif
    };

    /// Parses a JSON document, returns the reference to its root element if succeeded.
    bool parse(std::string_view, Element &);

#if 0
    /// Optional: Allocates memory to parse JSON documents faster.
    void reserve(size_t max_size);
#endif
};

inline ALWAYS_INLINE std::ostream& operator<<(std::ostream& out, DummyJSONParser::Element)
{
    return out;
}

}
