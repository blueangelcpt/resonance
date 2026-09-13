// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlinfra/Json.hpp"
#include "mlcore/Text.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace ml {

namespace {

const JsonValue& nullValue() {
	static const JsonValue kNull;
	return kNull;
}

const JsonArray& emptyArray() {
	static const JsonArray kEmpty;
	return kEmpty;
}

const JsonObject& emptyObject() {
	static const JsonObject kEmpty;
	return kEmpty;
}

class Parser {
public:
	Parser(std::string_view text) : m_text(text) {}

	Result<JsonValue> parse() {
		skipWhitespace();
		auto value = parseValue(0);
		if (!value) return value;
		skipWhitespace();
		if (m_position != m_text.size()) {
			return Error{ErrorCode::ParseError,
				"trailing content after the JSON document at offset " + std::to_string(m_position)};
		}
		return value;
	}

private:
	void skipWhitespace() {
		while (m_position < m_text.size()) {
			const char c = m_text[m_position];
			if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
				++m_position;
			} else {
				break;
			}
		}
	}

	bool consume(char expected) {
		if (m_position < m_text.size() && m_text[m_position] == expected) {
			++m_position;
			return true;
		}
		return false;
	}

	Error unexpected(std::string_view what) const {
		return Error{ErrorCode::ParseError,
			std::string("expected ") + std::string(what) + " at offset " + std::to_string(m_position)};
	}

	Result<JsonValue> parseValue(int depth) {
		if (depth > Json::kMaxDepth) {
			return Error{ErrorCode::ParseError, "JSON nesting exceeds the permitted depth"};
		}
		skipWhitespace();
		if (m_position >= m_text.size()) return unexpected("a value");

		switch (m_text[m_position]) {
			case '{': return parseObject(depth);
			case '[': return parseArray(depth);
			case '"': {
				auto s = parseString();
				if (!s) return s.error();
				return JsonValue(s.value());
			}
			case 't':
				if (m_text.compare(m_position, 4, "true") == 0) {
					m_position += 4;
					return JsonValue(true);
				}
				return unexpected("true");
			case 'f':
				if (m_text.compare(m_position, 5, "false") == 0) {
					m_position += 5;
					return JsonValue(false);
				}
				return unexpected("false");
			case 'n':
				if (m_text.compare(m_position, 4, "null") == 0) {
					m_position += 4;
					return JsonValue();
				}
				return unexpected("null");
			default:
				return parseNumber();
		}
	}

	Result<JsonValue> parseObject(int depth) {
		if (!consume('{')) return unexpected("'{'");
		JsonObject object;
		skipWhitespace();
		if (consume('}')) return JsonValue(std::move(object));

		while (true) {
			skipWhitespace();
			auto key = parseString();
			if (!key) return key.error();
			skipWhitespace();
			if (!consume(':')) return unexpected("':'");

			auto value = parseValue(depth + 1);
			if (!value) return value;
			// A duplicate key keeps the last occurrence, matching how most JSON
			// consumers behave.
			object[key.value()] = std::move(value.value());

			skipWhitespace();
			if (consume(',')) continue;
			if (consume('}')) break;
			return unexpected("',' or '}'");
		}
		return JsonValue(std::move(object));
	}

	Result<JsonValue> parseArray(int depth) {
		if (!consume('[')) return unexpected("'['");
		JsonArray array;
		skipWhitespace();
		if (consume(']')) return JsonValue(std::move(array));

		while (true) {
			auto value = parseValue(depth + 1);
			if (!value) return value;
			array.push_back(std::move(value.value()));

			skipWhitespace();
			if (consume(',')) continue;
			if (consume(']')) break;
			return unexpected("',' or ']'");
		}
		return JsonValue(std::move(array));
	}

	Result<std::string> parseString() {
		if (!consume('"')) return unexpected("'\"'");

		std::string out;
		while (m_position < m_text.size()) {
			const char c = m_text[m_position];
			if (c == '"') {
				++m_position;
				return out;
			}
			if (c == '\\') {
				++m_position;
				if (m_position >= m_text.size()) return unexpected("an escape sequence");
				const char escape = m_text[m_position++];
				switch (escape) {
					case '"': out.push_back('"'); break;
					case '\\': out.push_back('\\'); break;
					case '/': out.push_back('/'); break;
					case 'b': out.push_back('\b'); break;
					case 'f': out.push_back('\f'); break;
					case 'n': out.push_back('\n'); break;
					case 'r': out.push_back('\r'); break;
					case 't': out.push_back('\t'); break;
					case 'u': {
						auto cp = parseHex4();
						if (!cp) return cp.error();
						char32_t codepoint = cp.value();
						// Surrogate pair.
						if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
							if (m_position + 1 < m_text.size() && m_text[m_position] == '\\'
								&& m_text[m_position + 1] == 'u') {
								m_position += 2;
								auto low = parseHex4();
								if (!low) return low.error();
								if (low.value() >= 0xDC00 && low.value() <= 0xDFFF) {
									codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low.value() - 0xDC00);
								} else {
									codepoint = 0xFFFD;
								}
							} else {
								codepoint = 0xFFFD;
							}
						} else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
							// A lone low surrogate is invalid.
							codepoint = 0xFFFD;
						}
						out += text::encodeUtf8({codepoint});
						break;
					}
					default:
						return unexpected("a valid escape character");
				}
				continue;
			}
			// Unescaped control characters are not legal JSON, but rejecting a
			// whole provider response over one is not useful; replace instead.
			if (static_cast<unsigned char>(c) < 0x20) {
				out.push_back(' ');
				++m_position;
				continue;
			}
			out.push_back(c);
			++m_position;
		}
		return unexpected("a closing quote");
	}

	Result<char32_t> parseHex4() {
		if (m_position + 4 > m_text.size()) return unexpected("four hex digits");
		char32_t value = 0;
		for (int i = 0; i < 4; ++i) {
			const char c = m_text[m_position + static_cast<std::size_t>(i)];
			value <<= 4;
			if (c >= '0' && c <= '9') value |= static_cast<char32_t>(c - '0');
			else if (c >= 'a' && c <= 'f') value |= static_cast<char32_t>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') value |= static_cast<char32_t>(c - 'A' + 10);
			else return unexpected("a hex digit");
		}
		m_position += 4;
		return value;
	}

	Result<JsonValue> parseNumber() {
		const std::size_t begin = m_position;
		if (m_position < m_text.size() && (m_text[m_position] == '-' || m_text[m_position] == '+')) {
			++m_position;
		}
		while (m_position < m_text.size()
			&& (std::isdigit(static_cast<unsigned char>(m_text[m_position]))
				|| m_text[m_position] == '.' || m_text[m_position] == 'e' || m_text[m_position] == 'E'
				|| m_text[m_position] == '+' || m_text[m_position] == '-')) {
			++m_position;
		}
		if (m_position == begin) return unexpected("a number");

		const std::string token(m_text.substr(begin, m_position - begin));
		try {
			std::size_t consumed = 0;
			const double value = std::stod(token, &consumed);
			if (consumed != token.size()) return unexpected("a well-formed number");
			return JsonValue(value);
		} catch (...) {
			return unexpected("a number within range");
		}
	}

	std::string_view m_text;
	std::size_t m_position = 0;
};

} // namespace

std::string JsonValue::asString(std::string fallback) const {
	if (m_kind == Kind::String) return m_string;
	// A provider that returns a number where a string is documented is common
	// enough that coercing is more useful than failing.
	if (m_kind == Kind::Number) {
		if (m_number == std::floor(m_number) && std::abs(m_number) < 1e15) {
			return std::to_string(static_cast<std::int64_t>(m_number));
		}
		return std::to_string(m_number);
	}
	return fallback;
}

double JsonValue::asNumber(double fallback) const {
	if (m_kind == Kind::Number) return m_number;
	if (m_kind == Kind::String) {
		try {
			std::size_t consumed = 0;
			const double value = std::stod(m_string, &consumed);
			if (consumed > 0) return value;
		} catch (...) {
		}
	}
	return fallback;
}

std::int64_t JsonValue::asInt(std::int64_t fallback) const {
	const double value = asNumber(static_cast<double>(fallback));
	if (!std::isfinite(value)) return fallback;
	return static_cast<std::int64_t>(std::llround(value));
}

bool JsonValue::asBool(bool fallback) const {
	if (m_kind == Kind::Boolean) return m_boolean;
	if (m_kind == Kind::Number) return m_number != 0.0;
	return fallback;
}

const JsonArray& JsonValue::asArray() const {
	return (m_kind == Kind::Array) ? m_array : emptyArray();
}

const JsonObject& JsonValue::asObject() const {
	return (m_kind == Kind::Object) ? m_object : emptyObject();
}

const JsonValue& JsonValue::operator[](std::string_view key) const {
	if (m_kind != Kind::Object) return nullValue();
	auto it = m_object.find(std::string(key));
	return (it == m_object.end()) ? nullValue() : it->second;
}

const JsonValue& JsonValue::operator[](std::size_t index) const {
	if (m_kind != Kind::Array || index >= m_array.size()) return nullValue();
	return m_array[index];
}

bool JsonValue::contains(std::string_view key) const {
	return m_kind == Kind::Object && m_object.find(std::string(key)) != m_object.end();
}

std::size_t JsonValue::size() const {
	if (m_kind == Kind::Array) return m_array.size();
	if (m_kind == Kind::Object) return m_object.size();
	if (m_kind == Kind::String) return m_string.size();
	return 0;
}

const JsonValue& JsonValue::at(std::string_view path) const {
	const JsonValue* current = this;
	std::size_t start = 0;

	while (start <= path.size()) {
		const std::size_t dot = path.find('.', start);
		const std::string_view segment = (dot == std::string_view::npos)
			? path.substr(start)
			: path.substr(start, dot - start);

		if (segment.empty()) break;

		if (current->isArray()) {
			std::size_t index = 0;
			const auto result = std::from_chars(segment.data(), segment.data() + segment.size(), index);
			if (result.ec != std::errc()) return nullValue();
			current = &(*current)[index];
		} else {
			current = &(*current)[segment];
		}
		if (current->isNull()) return nullValue();

		if (dot == std::string_view::npos) break;
		start = dot + 1;
	}
	return *current;
}

Result<JsonValue> Json::parse(std::string_view text) {
	if (text.size() > kMaxLength) {
		return Error{ErrorCode::ParseError, "JSON document exceeds the permitted length"};
	}
	if (text.empty()) {
		return Error{ErrorCode::ParseError, "empty JSON document"};
	}
	// Tolerate a UTF-8 byte order mark; some providers emit one.
	if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
		&& static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
		text.remove_prefix(3);
	}
	Parser parser(text);
	return parser.parse();
}

} // namespace ml
