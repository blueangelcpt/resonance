// SPDX-License-Identifier: GPL-3.0-or-later
// A small, strict JSON reader for provider responses.
//
// Provider output is untrusted input. This parser has explicit depth and size
// limits and never recurses without bound, because a malicious or malformed
// response must fail cleanly rather than exhaust the stack.
#pragma once

#include "mlcore/Types.hpp"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ml {

class JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue>;

class JsonValue {
public:
	enum class Kind { Null, Boolean, Number, String, Array, Object };

	JsonValue() = default;
	explicit JsonValue(bool value) : m_kind(Kind::Boolean), m_boolean(value) {}
	explicit JsonValue(double value) : m_kind(Kind::Number), m_number(value) {}
	explicit JsonValue(std::string value) : m_kind(Kind::String), m_string(std::move(value)) {}
	explicit JsonValue(JsonArray value) : m_kind(Kind::Array), m_array(std::move(value)) {}
	explicit JsonValue(JsonObject value) : m_kind(Kind::Object), m_object(std::move(value)) {}

	Kind kind() const { return m_kind; }
	bool isNull() const { return m_kind == Kind::Null; }
	bool isObject() const { return m_kind == Kind::Object; }
	bool isArray() const { return m_kind == Kind::Array; }
	bool isString() const { return m_kind == Kind::String; }
	bool isNumber() const { return m_kind == Kind::Number; }
	bool isBoolean() const { return m_kind == Kind::Boolean; }

	/// Accessors that never throw. A type mismatch returns the fallback, because
	/// a provider omitting or retyping a field is a normal condition, not a bug.
	std::string asString(std::string fallback = {}) const;
	double asNumber(double fallback = 0.0) const;
	std::int64_t asInt(std::int64_t fallback = 0) const;
	bool asBool(bool fallback = false) const;

	const JsonArray& asArray() const;
	const JsonObject& asObject() const;

	/// Object member lookup. Returns a null value when absent.
	const JsonValue& operator[](std::string_view key) const;
	/// Array element lookup. Returns a null value when out of range.
	const JsonValue& operator[](std::size_t index) const;

	bool contains(std::string_view key) const;
	std::size_t size() const;

	/// Dotted path lookup, for example "results.0.artistName".
	const JsonValue& at(std::string_view path) const;

private:
	Kind m_kind = Kind::Null;
	bool m_boolean = false;
	double m_number = 0.0;
	std::string m_string;
	JsonArray m_array;
	JsonObject m_object;
};

class Json {
public:
	/// Maximum nesting accepted. Provider responses are shallow; anything deeper
	/// is either malformed or hostile.
	static constexpr int kMaxDepth = 64;
	static constexpr std::size_t kMaxLength = 64u * 1024u * 1024u;

	static Result<JsonValue> parse(std::string_view text);
};

} // namespace ml
