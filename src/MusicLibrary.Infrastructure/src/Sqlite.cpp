// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlinfra/Sqlite.hpp"

#include <sqlite3.h>

#include <cstring>
#include <utility>

namespace fs = std::filesystem;

namespace ml {

namespace {

Error sqliteError(sqlite3* db, std::string_view context) {
	std::string message(context);
	if (db) {
		message += ": ";
		message += sqlite3_errmsg(db);
	}
	return Error{ErrorCode::DatabaseError, std::move(message)};
}

} // namespace

// ---------------------------------------------------------------------------
// Statement
// ---------------------------------------------------------------------------

Statement::~Statement() {
	if (m_statement) sqlite3_finalize(m_statement);
}

Statement::Statement(Statement&& other) noexcept
	: m_db(other.m_db), m_statement(other.m_statement) {
	other.m_db = nullptr;
	other.m_statement = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept {
	if (this != &other) {
		if (m_statement) sqlite3_finalize(m_statement);
		m_db = other.m_db;
		m_statement = other.m_statement;
		other.m_db = nullptr;
		other.m_statement = nullptr;
	}
	return *this;
}

Statement& Statement::bind(int index, std::nullptr_t) {
	if (m_statement) sqlite3_bind_null(m_statement, index);
	return *this;
}

Statement& Statement::bind(int index, std::int64_t value) {
	if (m_statement) sqlite3_bind_int64(m_statement, index, value);
	return *this;
}

Statement& Statement::bind(int index, int value) {
	if (m_statement) sqlite3_bind_int(m_statement, index, value);
	return *this;
}

Statement& Statement::bind(int index, double value) {
	if (m_statement) sqlite3_bind_double(m_statement, index, value);
	return *this;
}

Statement& Statement::bind(int index, std::string_view value) {
	if (m_statement) {
		// SQLITE_TRANSIENT: SQLite copies the text, so the caller's buffer does
		// not have to outlive the bind.
		sqlite3_bind_text(m_statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
	}
	return *this;
}

Statement& Statement::bind(int index, const char* value) {
	return value ? bind(index, std::string_view(value)) : bind(index, nullptr);
}

Statement& Statement::bind(int index, bool value) {
	return bind(index, value ? 1 : 0);
}

Statement& Statement::bind(int index, const std::vector<std::byte>& blob) {
	if (m_statement) {
		if (blob.empty()) {
			sqlite3_bind_zeroblob(m_statement, index, 0);
		} else {
			sqlite3_bind_blob(m_statement, index, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
		}
	}
	return *this;
}

Statement& Statement::bind(int index, const std::optional<std::int64_t>& value) {
	return value ? bind(index, *value) : bind(index, nullptr);
}

Statement& Statement::bind(int index, const std::optional<int>& value) {
	return value ? bind(index, *value) : bind(index, nullptr);
}

Statement& Statement::bind(int index, const std::optional<double>& value) {
	return value ? bind(index, *value) : bind(index, nullptr);
}

Result<bool> Statement::step() {
	if (!m_statement) {
		return Error{ErrorCode::InvalidArgument, "step on an empty statement"};
	}
	const int rc = sqlite3_step(m_statement);
	if (rc == SQLITE_ROW) return true;
	if (rc == SQLITE_DONE) return false;
	return sqliteError(m_db, "step failed");
}

Status Statement::execute() {
	auto result = step();
	if (!result) return Status(result.error());
	// A statement that unexpectedly returns rows is a programming error, not a
	// data error, but it must not pass silently.
	if (result.value()) {
		reset();
		return Status(Error{ErrorCode::Internal, "execute() used on a statement that returns rows"});
	}
	reset();
	return Status::success();
}

void Statement::reset() {
	if (m_statement) sqlite3_reset(m_statement);
}

void Statement::clearBindings() {
	if (m_statement) sqlite3_clear_bindings(m_statement);
}

std::int64_t Statement::columnInt64(int index) const {
	return m_statement ? sqlite3_column_int64(m_statement, index) : 0;
}

int Statement::columnInt(int index) const {
	return m_statement ? sqlite3_column_int(m_statement, index) : 0;
}

double Statement::columnDouble(int index) const {
	return m_statement ? sqlite3_column_double(m_statement, index) : 0.0;
}

std::string Statement::columnText(int index) const {
	if (!m_statement) return {};
	const auto* text = sqlite3_column_text(m_statement, index);
	if (!text) return {};
	const int bytes = sqlite3_column_bytes(m_statement, index);
	return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
}

std::vector<std::byte> Statement::columnBlob(int index) const {
	std::vector<std::byte> out;
	if (!m_statement) return out;
	const void* data = sqlite3_column_blob(m_statement, index);
	const int bytes = sqlite3_column_bytes(m_statement, index);
	if (!data || bytes <= 0) return out;
	out.resize(static_cast<std::size_t>(bytes));
	std::memcpy(out.data(), data, static_cast<std::size_t>(bytes));
	return out;
}

bool Statement::columnIsNull(int index) const {
	return !m_statement || sqlite3_column_type(m_statement, index) == SQLITE_NULL;
}

int Statement::columnCount() const {
	return m_statement ? sqlite3_column_count(m_statement) : 0;
}

std::string Statement::columnName(int index) const {
	if (!m_statement) return {};
	const char* name = sqlite3_column_name(m_statement, index);
	return name ? std::string(name) : std::string();
}

// ---------------------------------------------------------------------------
// Transaction
// ---------------------------------------------------------------------------

Transaction::~Transaction() {
	rollback();
}

Transaction::Transaction(Transaction&& other) noexcept
	: m_db(other.m_db), m_finished(other.m_finished) {
	other.m_db = nullptr;
	other.m_finished = true;
}

Transaction& Transaction::operator=(Transaction&& other) noexcept {
	if (this != &other) {
		rollback();
		m_db = other.m_db;
		m_finished = other.m_finished;
		other.m_db = nullptr;
		other.m_finished = true;
	}
	return *this;
}

Status Transaction::commit() {
	if (!m_db || m_finished) {
		return Status(Error{ErrorCode::Internal, "commit on an inactive transaction"});
	}
	m_finished = true;
	return m_db->executeScript("COMMIT;");
}

void Transaction::rollback() {
	if (!m_db || m_finished) return;
	m_finished = true;
	// Best effort: a rollback failure during stack unwinding must not throw.
	(void)m_db->executeScript("ROLLBACK;");
}

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------

Database::~Database() {
	if (m_db) sqlite3_close(m_db);
}

Database::Database(Database&& other) noexcept
	: m_db(other.m_db), m_path(std::move(other.m_path)) {
	other.m_db = nullptr;
}

Database& Database::operator=(Database&& other) noexcept {
	if (this != &other) {
		if (m_db) sqlite3_close(m_db);
		m_db = other.m_db;
		m_path = std::move(other.m_path);
		other.m_db = nullptr;
	}
	return *this;
}

Result<Database> Database::open(const fs::path& path, DatabaseOpenOptions options) {
	int flags = options.readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
	if (!options.readOnly && options.createIfMissing) flags |= SQLITE_OPEN_CREATE;
	flags |= SQLITE_OPEN_NOMUTEX;   // One connection per thread; no shared mutex needed.

	sqlite3* handle = nullptr;
	const int rc = sqlite3_open_v2(path.string().c_str(), &handle, flags, nullptr);
	if (rc != SQLITE_OK) {
		std::string message = "cannot open catalogue " + path.string();
		if (handle) {
			message += ": ";
			message += sqlite3_errmsg(handle);
			sqlite3_close(handle);
		}
		return Error{ErrorCode::DatabaseError, std::move(message)};
	}

	Database db;
	db.m_db = handle;
	db.m_path = path;

	sqlite3_busy_timeout(handle, options.busyTimeoutMs);

	if (options.foreignKeys) {
		if (auto status = db.executeScript("PRAGMA foreign_keys = ON;"); !status) return status.error();
	}
	if (options.useWal && !options.readOnly) {
		// A failure here is not fatal: the catalogue still works in the default
		// journal mode. The achieved mode is readable via journalMode().
		(void)db.executeScript("PRAGMA journal_mode = WAL;");
		(void)db.executeScript("PRAGMA synchronous = NORMAL;");
	}
	(void)db.executeScript("PRAGMA temp_store = MEMORY;");

	return db;
}

Result<Database> Database::openInMemory() {
	sqlite3* handle = nullptr;
	const int rc = sqlite3_open_v2(":memory:", &handle,
		SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
	if (rc != SQLITE_OK) {
		if (handle) sqlite3_close(handle);
		return Error{ErrorCode::DatabaseError, "cannot open in-memory catalogue"};
	}
	Database db;
	db.m_db = handle;
	db.m_path = ":memory:";
	(void)db.executeScript("PRAGMA foreign_keys = ON;");
	return db;
}

Result<Statement> Database::prepare(std::string_view sql) {
	if (!m_db) {
		return Error{ErrorCode::DatabaseError, "prepare on a closed database"};
	}
	sqlite3_stmt* statement = nullptr;
	const int rc = sqlite3_prepare_v2(m_db, sql.data(), static_cast<int>(sql.size()), &statement, nullptr);
	if (rc != SQLITE_OK) {
		return sqliteError(m_db, "cannot prepare statement");
	}
	return Statement(m_db, statement);
}

Status Database::executeScript(std::string_view sql) {
	if (!m_db) {
		return Status(Error{ErrorCode::DatabaseError, "execute on a closed database"});
	}
	char* errorMessage = nullptr;
	const std::string text(sql);
	const int rc = sqlite3_exec(m_db, text.c_str(), nullptr, nullptr, &errorMessage);
	if (rc != SQLITE_OK) {
		std::string message = "script failed";
		if (errorMessage) {
			message += ": ";
			message += errorMessage;
			sqlite3_free(errorMessage);
		}
		return Status(Error{ErrorCode::DatabaseError, std::move(message)});
	}
	return Status::success();
}

Result<Transaction> Database::begin(Transaction::Kind kind) {
	const char* sql = (kind == Transaction::Kind::Immediate)
		? "BEGIN IMMEDIATE;"
		: "BEGIN;";
	if (auto status = executeScript(sql); !status) return status.error();
	return Transaction(this);
}

std::int64_t Database::lastInsertRowId() const {
	return m_db ? sqlite3_last_insert_rowid(m_db) : 0;
}

int Database::changes() const {
	return m_db ? sqlite3_changes(m_db) : 0;
}

std::string Database::journalMode() {
	auto statement = prepare("PRAGMA journal_mode;");
	if (!statement) return "unknown";
	auto row = statement.value().step();
	if (!row || !row.value()) return "unknown";
	return statement.value().columnText(0);
}

Result<bool> Database::integrityCheck() {
	auto statement = prepare("PRAGMA integrity_check;");
	if (!statement) return statement.error();
	auto row = statement.value().step();
	if (!row) return row.error();
	if (!row.value()) return false;
	return statement.value().columnText(0) == "ok";
}

Status Database::backupTo(const fs::path& destination) {
	if (!m_db) {
		return Status(Error{ErrorCode::DatabaseError, "backup from a closed database"});
	}

	sqlite3* target = nullptr;
	if (sqlite3_open_v2(destination.string().c_str(), &target,
			SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
		std::string message = "cannot open backup destination " + destination.string();
		if (target) {
			message += ": ";
			message += sqlite3_errmsg(target);
			sqlite3_close(target);
		}
		return Status(Error{ErrorCode::IoError, std::move(message)});
	}

	// Close the destination whatever happens next.
	struct Closer {
		sqlite3* db;
		~Closer() { if (db) sqlite3_close(db); }
	} closer{target};

	sqlite3_backup* backup = sqlite3_backup_init(target, "main", m_db, "main");
	if (!backup) {
		return Status(Error{ErrorCode::DatabaseError,
			std::string("cannot start backup: ") + sqlite3_errmsg(target)});
	}

	const int rc = sqlite3_backup_step(backup, -1);
	sqlite3_backup_finish(backup);

	if (rc != SQLITE_DONE) {
		return Status(Error{ErrorCode::DatabaseError,
			std::string("backup did not complete: ") + sqlite3_errmsg(target)});
	}
	return Status::success();
}

std::string Database::lastError() const {
	return m_db ? sqlite3_errmsg(m_db) : "no connection";
}

} // namespace ml
