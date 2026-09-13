// SPDX-License-Identifier: GPL-3.0-or-later
// A minimal assertion harness.
//
// Deliberately dependency-free: the FRD requires reproducible builds with pinned
// dependencies, and a test framework is one more thing to pin, vendor and audit
// for four platforms. This is enough for assertions, named cases and a CTest
// exit code.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace mltest {

struct Failure {
	std::string caseName;
	std::string expression;
	std::string detail;
	std::string file;
	int line = 0;
};

class Registry {
public:
	static Registry& instance() {
		static Registry registry;
		return registry;
	}

	void add(std::string name, std::function<void()> body) {
		m_cases.push_back({std::move(name), std::move(body)});
	}

	void beginCase(const std::string& name) { m_currentCase = name; }

	void recordFailure(std::string expression, std::string detail, std::string file, int line) {
		m_failures.push_back({m_currentCase, std::move(expression), std::move(detail),
			std::move(file), line});
	}

	void recordAssertion() { ++m_assertions; }

	int run(const char* suiteName) {
		std::cout << "== " << suiteName << " ==\n";

		std::size_t failedCases = 0;
		for (auto& testCase : m_cases) {
			const std::size_t before = m_failures.size();
			beginCase(testCase.name);

			try {
				testCase.body();
			} catch (const std::exception& error) {
				recordFailure("unexpected exception", error.what(), __FILE__, __LINE__);
			} catch (...) {
				recordFailure("unexpected exception", "non-standard exception", __FILE__, __LINE__);
			}

			const bool passed = m_failures.size() == before;
			if (!passed) ++failedCases;
			std::cout << (passed ? "  PASS  " : "  FAIL  ") << testCase.name << "\n";
		}

		if (!m_failures.empty()) {
			std::cout << "\nFailures:\n";
			for (const auto& failure : m_failures) {
				std::cout << "  " << failure.caseName << "\n";
				std::cout << "    " << failure.file << ":" << failure.line << "\n";
				std::cout << "    " << failure.expression << "\n";
				if (!failure.detail.empty()) std::cout << "    " << failure.detail << "\n";
			}
		}

		std::cout << "\n" << m_cases.size() << " cases, " << m_assertions << " assertions, "
			<< failedCases << " failed\n";
		return failedCases == 0 ? 0 : 1;
	}

private:
	struct Case {
		std::string name;
		std::function<void()> body;
	};

	std::vector<Case> m_cases;
	std::vector<Failure> m_failures;
	std::string m_currentCase;
	std::size_t m_assertions = 0;
};

struct Registrar {
	Registrar(std::string name, std::function<void()> body) {
		Registry::instance().add(std::move(name), std::move(body));
	}
};

template <typename T>
std::string describe(const T& value) {
	std::ostringstream out;
	out << value;
	return out.str();
}

inline std::string describe(const std::string& value) { return "\"" + value + "\""; }
inline std::string describe(bool value) { return value ? "true" : "false"; }

} // namespace mltest

#define ML_TEST_CONCAT_INNER(a, b) a##b
#define ML_TEST_CONCAT(a, b) ML_TEST_CONCAT_INNER(a, b)

/// Declares a test case.
#define TEST_CASE(name)                                                            \
	static void ML_TEST_CONCAT(mlTestBody, __LINE__)();                            \
	static ::mltest::Registrar ML_TEST_CONCAT(mlTestRegistrar, __LINE__)(           \
		name, ML_TEST_CONCAT(mlTestBody, __LINE__));                               \
	static void ML_TEST_CONCAT(mlTestBody, __LINE__)()

#define CHECK(expression)                                                          \
	do {                                                                           \
		::mltest::Registry::instance().recordAssertion();                          \
		if (!(expression)) {                                                       \
			::mltest::Registry::instance().recordFailure(                          \
				"CHECK(" #expression ")", {}, __FILE__, __LINE__);                  \
		}                                                                          \
	} while (false)

#define CHECK_MESSAGE(expression, message)                                         \
	do {                                                                           \
		::mltest::Registry::instance().recordAssertion();                          \
		if (!(expression)) {                                                       \
			::mltest::Registry::instance().recordFailure(                          \
				"CHECK(" #expression ")", (message), __FILE__, __LINE__);           \
		}                                                                          \
	} while (false)

#define CHECK_EQUAL(actual, expected)                                              \
	do {                                                                           \
		::mltest::Registry::instance().recordAssertion();                          \
		/* By value, not by reference: binding a reference to something like       */\
		/* *optional keeps a reference into a temporary that is already gone.      */\
		const auto mlActual = (actual);                                            \
		const auto mlExpected = (expected);                                        \
		if (!(mlActual == mlExpected)) {                                           \
			::mltest::Registry::instance().recordFailure(                          \
				"CHECK_EQUAL(" #actual ", " #expected ")",                          \
				"actual " + ::mltest::describe(mlActual)                            \
					+ ", expected " + ::mltest::describe(mlExpected),                \
				__FILE__, __LINE__);                                               \
		}                                                                          \
	} while (false)

#define ML_TEST_MAIN(suiteName)                                                    \
	int main() { return ::mltest::Registry::instance().run(suiteName); }
