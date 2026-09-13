// SPDX-License-Identifier: GPL-3.0-or-later
// A small RAII adapter over the SQLite C API.
//
// Every handle, statement and transaction is owned by a scope guard. Statements
// are always parameterised: there is no string-concatenation query path in this
// codebase (FRD section 15).
#pragma once

#include "mlcore/Types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace ml {

class Database;

/// Connection options.
///
/// Declared at namespace scope rather than nested in `Database`, because a
/// nested class's default member initialisers are not usable in a default
/// argument of the enclosing class.
struct DatabaseOpenOptions {
	bool readOnly = false;
	bool createIfMissing = true;
	/// Busy timeout in milliseconds before a lock contention is reported.
	int busyTimeoutMs = 10000;
	/// Enable WAL. Disabled automatically for read-only connections.
	bool useWal = true;
	/// Foreign key enforcement. Always on for the catalogue.
	bool foreignKeys = true;
};

/// A prepared statement. Move-only; finalised on destruction.
class Statement {
public:
	Statement() = default;
	~Statement();

	Statement(const Statement&) = delete;
	Statement& operator=(const Statement&) = delete;
	Statement(Statement&& other) noexcept;
	Statement& operator=(Statement&& other) noexcept;

	/// Binding. Indices are 1-based, matching SQLite.
	Statement& bind(int index, std::nullptr_t);
	Statement& bind(int index, std::int64_t value);
	Statement& bind(int index, int value);
	Statement& bind(int index, double value);
	Statement& bind(int index, std::string_view value);
	Statement& bind(int index, const char* value);
	Statement& bind(int index, bool value);
	Statement& bind(int index, const std::vector<std::byte>& blob);
	Statement& bind(int index, const std::optional<std::int64_t>& value);
	Statement& bind(int index, const std::optional<int>& value);
	Statement& bind(int index, const std::optional<double>& value);

	/// Binds every argument in order starting at index 1.
	template <typename... Args>
	Statement& bindAll(Args&&... args) {
		int index = 1;
		(bind(index++, std::forward<Args>(args)), ...);
		return *this;
	}

	/// Advances one row. Returns true when a row is available, false at the end.
	Result<bool> step();

	/// Runs a statement expected to return no rows.
	Status execute();

	void reset();
	void clearBindings();

	/// Column access. Indices are 0-based, matching SQLite.
	std::int64_t columnInt64(int index) const;
	int columnInt(int index) const;
	double columnDouble(int index) const;
	std::string columnText(int index) const;
	std::vector<std::byte> columnBlob(int index) const;
	bool columnIsNull(int index) const;
	int columnCount() const;
	std::string columnName(int index) const;

	sqlite3_stmt* handle() const { return m_statement; }
	bool valid() const { return m_statement != nullptr; }

private:
	friend class Database;
	Statement(sqlite3* db, sqlite3_stmt* statement) : m_db(db), m_statement(statement) {}

	sqlite3* m_db = nullptr;
	sqlite3_stmt* m_statement = nullptr;
};

/// A transaction. Rolls back on destruction unless committed, so an exception or
/// an early return can never leave a half-applied change set.
///
/// Nesting is supported. SQLite has no nested BEGIN, so an inner transaction
/// becomes a SAVEPOINT: committing it releases the savepoint and rolling it back
/// unwinds to it, leaving the outer transaction intact. Without this, a
/// repository method that opens its own transaction would fail whenever a caller
/// had already opened one -- which is exactly what a batched scan does.
class Transaction {
public:
	enum class Kind { Deferred, Immediate };

	Transaction() = default;
	~Transaction();

	Transaction(const Transaction&) = delete;
	Transaction& operator=(const Transaction&) = delete;
	Transaction(Transaction&& other) noexcept;
	Transaction& operator=(Transaction&& other) noexcept;

	Status commit();
	void rollback();
	bool active() const { return m_db != nullptr && !m_finished; }

	/// True when this is a savepoint inside an outer transaction.
	bool isNested() const { return !m_savepointName.empty(); }

private:
	friend class Database;
	Transaction(Database* db, std::string savepointName)
		: m_db(db), m_savepointName(std::move(savepointName)) {}

	Database* m_db = nullptr;
	std::string m_savepointName;
	bool m_finished = false;
};

/// The catalogue connection.
///
/// WAL mode allows concurrent readers with a single writer. The FRD requires the
/// database on a local disk, never an SMB or NFS share; `open` records the
/// journal mode actually achieved so a silent fallback is visible.
class Database {
public:
	Database() = default;
	~Database();

	Database(const Database&) = delete;
	Database& operator=(const Database&) = delete;
	Database(Database&& other) noexcept;
	Database& operator=(Database&& other) noexcept;

	using OpenOptions = DatabaseOpenOptions;

	static Result<Database> open(const std::filesystem::path& path, DatabaseOpenOptions options = {});

	/// An in-memory database, used by tests.
	static Result<Database> openInMemory();

	Result<Statement> prepare(std::string_view sql);

	/// Executes one or more statements with no parameters. Only for migrations
	/// and pragmas, never for user-supplied data.
	Status executeScript(std::string_view sql);

	Result<Transaction> begin(Transaction::Kind kind = Transaction::Kind::Deferred);

	std::int64_t lastInsertRowId() const;
	int changes() const;

	/// Current journal mode, for diagnostics.
	std::string journalMode();

	/// Runs SQLite's own integrity check.
	Result<bool> integrityCheck();

	/// Online backup to another file, using SQLite's backup API so a live
	/// catalogue can be copied safely.
	Status backupTo(const std::filesystem::path& destination);

	sqlite3* handle() const { return m_db; }
	bool valid() const { return m_db != nullptr; }
	const std::filesystem::path& path() const { return m_path; }

	/// Last SQLite error message for this connection.
	std::string lastError() const;

	/// Nesting depth of open transactions. 0 means none.
	int transactionDepth() const { return m_transactionDepth; }

private:
	friend class Transaction;

	sqlite3* m_db = nullptr;
	std::filesystem::path m_path;
	int m_transactionDepth = 0;
	int m_nextSavepoint = 0;
};

/// Applies versioned schema migrations. A downgrade is detected and refused
/// rather than corrupting a newer catalogue (FRD section 16).
class SchemaMigrator {
public:
	/// Schema version this build understands.
	static constexpr int kCurrentVersion = 2;

	struct Migration {
		int version = 0;
		std::string_view name;
		std::string_view sql;
	};

	static Status migrate(Database& db);

	/// Reads `PRAGMA user_version`.
	static Result<int> currentVersion(Database& db);

	static const std::vector<Migration>& migrations();
};

} // namespace ml
