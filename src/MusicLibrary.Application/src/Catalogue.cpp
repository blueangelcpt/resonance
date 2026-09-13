// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlapp/Catalogue.hpp"
#include "mlcore/Text.hpp"
#include "mlcore/NamingTemplate.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace ml {

std::string nowIso8601() {
	const auto now = std::chrono::system_clock::now();
	const auto time = std::chrono::system_clock::to_time_t(now);
	const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
		now.time_since_epoch()) % 1000;

	std::tm utc{};
#ifdef _WIN32
	gmtime_s(&utc, &time);
#else
	gmtime_r(&time, &utc);
#endif

	std::ostringstream out;
	out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S");
	out << '.' << std::setfill('0') << std::setw(3) << milliseconds.count() << 'Z';
	return out.str();
}

namespace {

/// Joins a list for storage in a single text column.
std::string joinList(const std::vector<std::string>& items) {
	return text::join(items, "\x1f");   // Unit separator: cannot appear in tag text.
}

std::vector<std::string> splitList(const std::string& packed) {
	if (packed.empty()) return {};
	auto parts = text::split(packed, '\x1f');
	parts.erase(std::remove_if(parts.begin(), parts.end(),
		[](const std::string& s) { return s.empty(); }), parts.end());
	return parts;
}

std::string joinEvidence(const std::vector<Evidence>& evidence) {
	std::vector<std::string> packed;
	for (const auto& e : evidence) {
		packed.push_back(e.kind + "\x1e" + e.detail + "\x1e" + (e.supporting ? "1" : "0"));
	}
	return joinList(packed);
}

std::vector<Evidence> splitEvidence(const std::string& packed) {
	std::vector<Evidence> out;
	for (const auto& item : splitList(packed)) {
		const auto fields = text::split(item, '\x1e');
		Evidence e;
		if (!fields.empty()) e.kind = fields[0];
		if (fields.size() > 1) e.detail = fields[1];
		e.supporting = fields.size() > 2 && fields[2] == "1";
		out.push_back(std::move(e));
	}
	return out;
}

} // namespace

Catalogue::Catalogue(Database& database) : m_database(database) {}

// ---------------------------------------------------------------------------
// Roots
// ---------------------------------------------------------------------------

Result<RootId> Catalogue::upsertRoot(const fs::path& path, const fs::path& resolved, std::string_view kind,
	std::string_view label, std::string_view volumeIdentity, std::uint64_t deviceId) {
	auto existing = findRoot(path);
	if (!existing) return existing.error();

	if (existing.value()) {
		auto statement = m_database.prepare(
			"UPDATE library_roots SET resolved_path = ?, label = ?, volume_identity = ?, device_id = ?, "
			"last_seen_at = ? WHERE id = ?;");
		if (!statement) return statement.error();
		statement.value().bindAll(resolved.string(), label, volumeIdentity,
			static_cast<std::int64_t>(deviceId), nowIso8601(), existing.value()->value);
		if (auto status = statement.value().execute(); !status) return status.error();
		return *existing.value();
	}

	auto statement = m_database.prepare(
		"INSERT INTO library_roots (path, resolved_path, label, kind, volume_identity, device_id, "
		"added_at, last_seen_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?);");
	if (!statement) return statement.error();

	const std::string timestamp = nowIso8601();
	statement.value().bindAll(path.string(), resolved.string(), label, kind, volumeIdentity,
		static_cast<std::int64_t>(deviceId), timestamp, timestamp);
	if (auto status = statement.value().execute(); !status) return status.error();
	return RootId(m_database.lastInsertRowId());
}

Result<std::optional<RootId>> Catalogue::findRoot(const fs::path& path) const {
	auto statement = const_cast<Database&>(m_database).prepare(
		"SELECT id FROM library_roots WHERE path = ?;");
	if (!statement) return statement.error();
	statement.value().bind(1, path.string());

	auto row = statement.value().step();
	if (!row) return row.error();
	if (!row.value()) return std::optional<RootId>{};
	return std::optional<RootId>(RootId(statement.value().columnInt64(0)));
}

Result<std::vector<std::tuple<RootId, std::string, std::string>>> Catalogue::listRoots(
	std::string_view kind) const {
	auto statement = const_cast<Database&>(m_database).prepare(
		"SELECT id, path, label FROM library_roots WHERE kind = ? ORDER BY id;");
	if (!statement) return statement.error();
	statement.value().bind(1, kind);

	std::vector<std::tuple<RootId, std::string, std::string>> out;
	while (true) {
		auto row = statement.value().step();
		if (!row) return row.error();
		if (!row.value()) break;
		out.emplace_back(RootId(statement.value().columnInt64(0)), statement.value().columnText(1),
			statement.value().columnText(2));
	}
	return out;
}

// ---------------------------------------------------------------------------
// Scan runs
// ---------------------------------------------------------------------------

Result<ScanRunId> Catalogue::beginScan(RootId root, bool rootWasPresent) {
	auto generation = m_database.prepare(
		"SELECT COALESCE(MAX(generation), 0) + 1 FROM scan_runs WHERE root_id = ?;");
	if (!generation) return generation.error();
	generation.value().bind(1, root.value);

	std::int64_t next = 1;
	if (auto row = generation.value().step(); row && row.value()) {
		next = generation.value().columnInt64(0);
	}

	auto statement = m_database.prepare(
		"INSERT INTO scan_runs (root_id, started_at, generation, root_was_present, status) "
		"VALUES (?, ?, ?, ?, 'running');");
	if (!statement) return statement.error();
	statement.value().bindAll(root.value, nowIso8601(), next, rootWasPresent);
	if (auto status = statement.value().execute(); !status) return status.error();
	return ScanRunId(m_database.lastInsertRowId());
}

Status Catalogue::finishScan(ScanRunId scan, std::string_view status, std::int64_t seen,
	std::int64_t added, std::int64_t changed, std::int64_t unreadable, std::string_view error) {
	auto statement = m_database.prepare(
		"UPDATE scan_runs SET finished_at = ?, status = ?, files_seen = ?, files_added = ?, "
		"files_changed = ?, files_unreadable = ?, error = ? WHERE id = ?;");
	if (!statement) return Status(statement.error());
	statement.value().bindAll(nowIso8601(), status, seen, added, changed, unreadable,
		error.empty() ? nullptr : std::string(error).c_str(), scan.value);
	return statement.value().execute();
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

namespace {

FileRecord readFileRow(const Statement& s) {
	FileRecord r;
	int i = 0;
	r.id = FileId(s.columnInt64(i++));
	r.rootId = RootId(s.columnInt64(i++));
	r.relativePath = s.columnText(i++);
	r.relativeDirectory = s.columnText(i++);
	r.fileName = s.columnText(i++);
	r.extension = s.columnText(i++);
	r.identity.deviceId = static_cast<std::uint64_t>(s.columnInt64(i++));
	r.identity.inode = static_cast<std::uint64_t>(s.columnInt64(i++));
	r.identity.sizeBytes = s.columnInt64(i++);
	r.identity.modifiedUnixMs = s.columnInt64(i++);
	r.audio.durationMs = s.columnInt64(i++);
	r.audio.sampleRateHz = s.columnInt(i++);
	r.audio.channels = s.columnInt(i++);
	r.audio.bitrateKbps = s.columnInt(i++);
	const std::string mode = s.columnText(i++);
	r.audio.bitrateMode = (mode == "cbr") ? BitrateMode::Cbr
		: (mode == "vbr") ? BitrateMode::Vbr
		: (mode == "abr") ? BitrateMode::Abr : BitrateMode::Unknown;
	r.audio.audioOffset = s.columnInt64(i++);
	r.audio.audioLength = s.columnInt64(i++);
	r.contentSha256 = s.columnText(i++);
	r.audioSha256 = s.columnText(i++);
	r.readStatus = s.columnText(i++);
	r.readError = s.columnText(i++);
	r.title = s.columnText(i++);
	r.artist = s.columnText(i++);
	r.albumArtist = s.columnText(i++);
	r.album = s.columnText(i++);
	r.genre = s.columnText(i++);
	r.date = s.columnText(i++);
	if (!s.columnIsNull(i)) r.trackNumber = s.columnInt(i);
	++i;
	if (!s.columnIsNull(i)) r.discNumber = s.columnInt(i);
	++i;
	if (!s.columnIsNull(i)) r.bpm = s.columnDouble(i);
	++i;
	r.hasArtwork = s.columnInt(i++) != 0;
	r.artworkWidth = s.columnInt(i++);
	r.artworkHeight = s.columnInt(i++);
	r.hasLyrics = s.columnInt(i++) != 0;
	r.hasGainFields = s.columnInt(i++) != 0;
	r.hasPrivacyFindings = s.columnInt(i++) != 0;
	r.primaryContainer = s.columnText(i++);
	r.audio.valid = r.audio.sampleRateHz > 0;
	return r;
}

constexpr std::string_view kFileColumns =
	"id, root_id, relative_path, relative_directory, file_name, extension, "
	"device_id, inode, size_bytes, modified_unix_ms, "
	"duration_ms, sample_rate_hz, channels, bitrate_kbps, bitrate_mode, audio_offset, audio_length, "
	"content_sha256, audio_sha256, read_status, COALESCE(read_error, ''), "
	"display_title, display_artist, display_album_artist, display_album, display_genre, display_date, "
	"display_track, display_disc, display_bpm, "
	"has_artwork, artwork_width, artwork_height, has_lyrics, has_gain_fields, has_privacy_findings, "
	"primary_container";

} // namespace

Result<FileId> Catalogue::upsertFile(RootId root, ScanRunId scan, const FileRecord& record) {
	auto existing = findFile(root, record.relativePath);
	if (!existing) return existing.error();

	const char* sql = existing.value()
		? "UPDATE files SET device_id = ?, inode = ?, size_bytes = ?, modified_unix_ms = ?, "
		  "codec = ?, duration_ms = ?, sample_rate_hz = ?, channels = ?, bitrate_kbps = ?, "
		  "bitrate_mode = ?, audio_offset = ?, audio_length = ?, has_xing_header = ?, "
		  "has_lame_header = ?, encoder_delay = ?, encoder_padding = ?, content_sha256 = ?, "
		  "audio_sha256 = ?, last_seen_scan = ?, read_status = ?, read_error = ?, "
		  "relative_directory = ?, file_name = ?, extension = ?, "
		  "display_title = ?, display_artist = ?, display_album_artist = ?, display_album = ?, "
		  "display_genre = ?, display_date = ?, display_track = ?, display_disc = ?, display_bpm = ?, "
		  "has_artwork = ?, artwork_width = ?, artwork_height = ?, has_lyrics = ?, "
		  "has_gain_fields = ?, has_privacy_findings = ?, primary_container = ? "
		  "WHERE id = ?;"
		: "INSERT INTO files (device_id, inode, size_bytes, modified_unix_ms, codec, duration_ms, "
		  "sample_rate_hz, channels, bitrate_kbps, bitrate_mode, audio_offset, audio_length, "
		  "has_xing_header, has_lame_header, encoder_delay, encoder_padding, content_sha256, "
		  "audio_sha256, last_seen_scan, read_status, read_error, relative_directory, file_name, "
		  "extension, display_title, display_artist, display_album_artist, display_album, "
		  "display_genre, display_date, display_track, display_disc, display_bpm, has_artwork, "
		  "artwork_width, artwork_height, has_lyrics, has_gain_fields, has_privacy_findings, "
		  "primary_container, root_id, relative_path, first_seen_scan) "
		  "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";

	auto statement = m_database.prepare(sql);
	if (!statement) return statement.error();

	Statement& s = statement.value();
	int i = 1;
	s.bind(i++, static_cast<std::int64_t>(record.identity.deviceId));
	s.bind(i++, static_cast<std::int64_t>(record.identity.inode));
	s.bind(i++, record.identity.sizeBytes);
	s.bind(i++, record.identity.modifiedUnixMs);
	s.bind(i++, std::string_view("mp3"));
	s.bind(i++, record.audio.durationMs);
	s.bind(i++, record.audio.sampleRateHz);
	s.bind(i++, record.audio.channels);
	s.bind(i++, record.audio.bitrateKbps);
	s.bind(i++, toString(record.audio.bitrateMode));
	s.bind(i++, record.audio.audioOffset);
	s.bind(i++, record.audio.audioLength);
	s.bind(i++, record.audio.hasXingHeader);
	s.bind(i++, record.audio.hasLameHeader);
	s.bind(i++, record.audio.encoderDelay);
	s.bind(i++, record.audio.encoderPadding);
	s.bind(i++, record.contentSha256);
	s.bind(i++, record.audioSha256);
	s.bind(i++, scan.value);
	s.bind(i++, record.readStatus);
	s.bind(i++, record.readError);
	s.bind(i++, record.relativeDirectory);
	s.bind(i++, record.fileName);
	s.bind(i++, record.extension);
	s.bind(i++, record.title);
	s.bind(i++, record.artist);
	s.bind(i++, record.albumArtist);
	s.bind(i++, record.album);
	s.bind(i++, record.genre);
	s.bind(i++, record.date);
	s.bind(i++, record.trackNumber);
	s.bind(i++, record.discNumber);
	s.bind(i++, record.bpm);
	s.bind(i++, record.hasArtwork);
	s.bind(i++, record.artworkWidth);
	s.bind(i++, record.artworkHeight);
	s.bind(i++, record.hasLyrics);
	s.bind(i++, record.hasGainFields);
	s.bind(i++, record.hasPrivacyFindings);
	s.bind(i++, record.primaryContainer);

	if (existing.value()) {
		s.bind(i++, existing.value()->id.value);
	} else {
		s.bind(i++, root.value);
		s.bind(i++, record.relativePath);
		s.bind(i++, scan.value);
	}

	if (auto status = s.execute(); !status) return status.error();

	const FileId id = existing.value() ? existing.value()->id : FileId(m_database.lastInsertRowId());

	FileRecord stored = record;
	stored.id = id;
	if (auto status = refreshSearchIndex(id, stored); !status) return status.error();

	return id;
}

Status Catalogue::markFileUnreadable(RootId root, ScanRunId scan, std::string_view relativePath,
	std::string_view status, std::string_view error) {
	FileRecord record;
	record.relativePath = std::string(relativePath);
	const fs::path path(record.relativePath);
	record.relativeDirectory = path.parent_path().string();
	record.fileName = path.filename().string();
	record.extension = path.extension().string();
	record.readStatus = std::string(status);
	record.readError = std::string(error);

	auto id = upsertFile(root, scan, record);
	if (!id) return Status(id.error());
	return Status::success();
}

Result<std::optional<FileRecord>> Catalogue::findFile(RootId root, std::string_view relativePath) const {
	const std::string sql = "SELECT " + std::string(kFileColumns)
		+ " FROM files WHERE root_id = ? AND relative_path = ?;";
	auto statement = const_cast<Database&>(m_database).prepare(sql);
	if (!statement) return statement.error();
	statement.value().bindAll(root.value, relativePath);

	auto row = statement.value().step();
	if (!row) return row.error();
	if (!row.value()) return std::optional<FileRecord>{};
	return std::optional<FileRecord>(readFileRow(statement.value()));
}

Result<std::optional<FileRecord>> Catalogue::loadFile(FileId id) const {
	const std::string sql = "SELECT " + std::string(kFileColumns) + " FROM files WHERE id = ?;";
	auto statement = const_cast<Database&>(m_database).prepare(sql);
	if (!statement) return statement.error();
	statement.value().bind(1, id.value);

	auto row = statement.value().step();
	if (!row) return row.error();
	if (!row.value()) return std::optional<FileRecord>{};
	return std::optional<FileRecord>(readFileRow(statement.value()));
}

namespace {

/// Builds the WHERE clause for a filter. Every value is bound, never
/// interpolated: there is no string-concatenation query path in this codebase.
struct FilterClause {
	std::string sql;
	std::vector<std::string> textParameters;
	std::vector<std::int64_t> intParameters;
	std::vector<bool> isText;   // Binding order marker.
};

FilterClause buildFilter(const TrackFilter& filter) {
	FilterClause clause;
	std::vector<std::string> conditions;

	if (!filter.searchText.empty()) {
		conditions.push_back(
			"(display_title LIKE ?1 OR display_artist LIKE ?1 OR display_album LIKE ?1 "
			"OR display_album_artist LIKE ?1 OR relative_path LIKE ?1)");
	}
	if (filter.missingArtwork) {
		conditions.push_back(*filter.missingArtwork ? "has_artwork = 0" : "has_artwork = 1");
	}
	if (filter.missingLyrics) {
		conditions.push_back(*filter.missingLyrics ? "has_lyrics = 0" : "has_lyrics = 1");
	}
	if (filter.missingBpm) {
		conditions.push_back(*filter.missingBpm ? "display_bpm IS NULL" : "display_bpm IS NOT NULL");
	}
	if (filter.hasGainFields) {
		conditions.push_back(*filter.hasGainFields ? "has_gain_fields = 1" : "has_gain_fields = 0");
	}
	if (filter.hasPrivacyFindings) {
		conditions.push_back(*filter.hasPrivacyFindings
			? "has_privacy_findings = 1" : "has_privacy_findings = 0");
	}
	if (!filter.readStatus.empty()) {
		conditions.push_back("read_status = ?2");
	}
	if (filter.albumId) {
		conditions.push_back("id IN (SELECT file_id FROM album_tracks WHERE album_id = ?3)");
	}

	if (!conditions.empty()) {
		clause.sql = " WHERE " + text::join(conditions, " AND ");
	}
	return clause;
}

void bindFilter(Statement& statement, const TrackFilter& filter) {
	// Fixed parameter numbers keep binding independent of which conditions the
	// clause happened to include.
	if (!filter.searchText.empty()) {
		statement.bind(1, "%" + filter.searchText + "%");
	}
	if (!filter.readStatus.empty()) {
		statement.bind(2, filter.readStatus);
	}
	if (filter.albumId) {
		statement.bind(3, filter.albumId->value);
	}
}

} // namespace

Result<std::vector<FileRecord>> Catalogue::queryFiles(const TrackFilter& filter, std::int64_t limit,
	std::int64_t offset) const {
	const FilterClause clause = buildFilter(filter);
	std::string sql = "SELECT " + std::string(kFileColumns) + " FROM files" + clause.sql
		+ " ORDER BY display_album_artist, display_album, display_disc, display_track, relative_path"
		+ " LIMIT ?4 OFFSET ?5;";

	auto statement = const_cast<Database&>(m_database).prepare(sql);
	if (!statement) return statement.error();
	bindFilter(statement.value(), filter);
	statement.value().bind(4, limit > 0 ? limit : 1000000);
	statement.value().bind(5, offset);

	std::vector<FileRecord> out;
	while (true) {
		auto row = statement.value().step();
		if (!row) return row.error();
		if (!row.value()) break;
		out.push_back(readFileRow(statement.value()));
	}
	return out;
}

Result<std::int64_t> Catalogue::countFiles(const TrackFilter& filter) const {
	const FilterClause clause = buildFilter(filter);
	const std::string sql = "SELECT COUNT(*) FROM files" + clause.sql + ";";

	auto statement = const_cast<Database&>(m_database).prepare(sql);
	if (!statement) return statement.error();
	bindFilter(statement.value(), filter);

	auto row = statement.value().step();
	if (!row) return row.error();
	if (!row.value()) return std::int64_t{0};
	return statement.value().columnInt64(0);
}

Result<std::vector<FileId>> Catalogue::filesNeedingRevalidation(RootId root) const {
	auto statement = const_cast<Database&>(m_database).prepare(
		"SELECT id FROM files WHERE root_id = ? AND (content_sha256 = '' OR read_status <> 'ok');");
	if (!statement) return statement.error();
	statement.value().bind(1, root.value);

	std::vector<FileId> out;
	while (true) {
		auto row = statement.value().step();
		if (!row) return row.error();
		if (!row.value()) break;
		out.push_back(FileId(statement.value().columnInt64(0)));
	}
	return out;
}

Status Catalogue::refreshSearchIndex(FileId file, const FileRecord& record) {
	// FTS5 external-content tables need the old row removed before reinsertion.
	auto remove = m_database.prepare("DELETE FROM file_search WHERE rowid = ?;");
	if (!remove) return Status(remove.error());
	remove.value().bind(1, file.value);
	(void)remove.value().execute();

	auto insert = m_database.prepare(
		"INSERT INTO file_search (rowid, title, artist, album_artist, album, relative_path) "
		"VALUES (?, ?, ?, ?, ?, ?);");
	if (!insert) return Status(insert.error());
	insert.value().bindAll(file.value, record.title, record.artist, record.albumArtist,
		record.album, record.relativePath);
	return insert.value().execute();
}

// ---------------------------------------------------------------------------
// Tag snapshots
// ---------------------------------------------------------------------------

Result<SnapshotId> Catalogue::saveSnapshot(FileId file, const TagSnapshot& snapshot, std::string_view kind) {
	auto transaction = m_database.begin(Transaction::Kind::Immediate);
	if (!transaction) return transaction.error();

	auto insert = m_database.prepare(
		"INSERT INTO tag_snapshots (file_id, captured_at, kind, primary_container, containers, "
		"id3v2_tag_bytes, id3v2_padding_bytes, ape_tag_bytes, has_id3v1, unsynchronised, "
		"extended_header, content_sha256, audio_sha256, read_warnings) "
		"VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
	if (!insert) return insert.error();

	std::vector<std::string> containerNames;
	for (auto c : snapshot.containers) containerNames.emplace_back(toString(c));

	insert.value().bindAll(file.value, nowIso8601(), kind, toString(snapshot.primaryContainer),
		joinList(containerNames), static_cast<std::int64_t>(snapshot.id3v2TagBytes),
		static_cast<std::int64_t>(snapshot.id3v2PaddingBytes),
		static_cast<std::int64_t>(snapshot.apeTagBytes), snapshot.hasId3v1,
		snapshot.id3v2Unsynchronised, snapshot.id3v2ExtendedHeader, snapshot.contentSha256,
		snapshot.audioSha256, joinList(snapshot.readWarnings));
	if (auto status = insert.value().execute(); !status) return status.error();

	const SnapshotId id(m_database.lastInsertRowId());

	auto frameStatement = m_database.prepare(
		"INSERT INTO tag_frames (snapshot_id, container, frame_id, owner, description, language, "
		"text_value, binary_value, encoding, ordinal, raw_size, interpreted) "
		"VALUES (?,?,?,?,?,?,?,?,?,?,?,?);");
	if (!frameStatement) return frameStatement.error();

	for (const auto& frame : snapshot.frames) {
		Statement& s = frameStatement.value();
		s.reset();
		s.clearBindings();
		s.bindAll(id.value, toString(frame.container), frame.id, frame.owner, frame.description,
			frame.language, frame.value, frame.binary, toString(frame.encoding), frame.ordinal,
			static_cast<std::int64_t>(frame.rawSize), frame.interpreted);
		if (auto status = s.step(); !status) return status.error();
	}

	auto pictureStatement = m_database.prepare(
		"INSERT INTO tag_pictures (snapshot_id, picture_type, mime_type, description, byte_length, "
		"width, height, content_sha256, frame_ordinal) VALUES (?,?,?,?,?,?,?,?,?);");
	if (!pictureStatement) return pictureStatement.error();

	for (const auto& picture : snapshot.pictures) {
		Statement& s = pictureStatement.value();
		s.reset();
		s.clearBindings();
		s.bindAll(id.value, toString(picture.type), picture.mimeType, picture.description,
			static_cast<std::int64_t>(picture.byteLength), picture.width, picture.height,
			picture.contentSha256, picture.frameOrdinal);
		if (auto status = s.step(); !status) return status.error();
	}

	if (auto status = transaction.value().commit(); !status) return status.error();
	return id;
}

Result<std::optional<TagSnapshot>> Catalogue::latestSnapshot(FileId file, std::string_view kind) const {
	auto statement = const_cast<Database&>(m_database).prepare(
		"SELECT id, primary_container, containers, id3v2_tag_bytes, id3v2_padding_bytes, "
		"ape_tag_bytes, has_id3v1, unsynchronised, extended_header, content_sha256, audio_sha256, "
		"read_warnings FROM tag_snapshots WHERE file_id = ? AND kind = ? "
		"ORDER BY captured_at DESC, id DESC LIMIT 1;");
	if (!statement) return statement.error();
	statement.value().bindAll(file.value, kind);

	auto row = statement.value().step();
	if (!row) return row.error();
	if (!row.value()) return std::optional<TagSnapshot>{};

	TagSnapshot snapshot;
	snapshot.id = SnapshotId(statement.value().columnInt64(0));
	snapshot.fileId = file;
	if (auto c = tagContainerFromString(statement.value().columnText(1))) snapshot.primaryContainer = *c;
	for (const auto& name : splitList(statement.value().columnText(2))) {
		if (auto c = tagContainerFromString(name)) snapshot.containers.push_back(*c);
	}
	snapshot.id3v2TagBytes = static_cast<std::size_t>(statement.value().columnInt64(3));
	snapshot.id3v2PaddingBytes = static_cast<std::size_t>(statement.value().columnInt64(4));
	snapshot.apeTagBytes = static_cast<std::size_t>(statement.value().columnInt64(5));
	snapshot.hasId3v1 = statement.value().columnInt(6) != 0;
	snapshot.id3v2Unsynchronised = statement.value().columnInt(7) != 0;
	snapshot.id3v2ExtendedHeader = statement.value().columnInt(8) != 0;
	snapshot.contentSha256 = statement.value().columnText(9);
	snapshot.audioSha256 = statement.value().columnText(10);
	snapshot.readWarnings = splitList(statement.value().columnText(11));

	auto frames = const_cast<Database&>(m_database).prepare(
		"SELECT container, frame_id, owner, description, language, text_value, binary_value, "
		"encoding, ordinal, raw_size, interpreted FROM tag_frames WHERE snapshot_id = ? ORDER BY id;");
	if (!frames) return frames.error();
	frames.value().bind(1, snapshot.id.value);

	while (true) {
		auto frameRow = frames.value().step();
		if (!frameRow) return frameRow.error();
		if (!frameRow.value()) break;

		TagFrame frame;
		if (auto c = tagContainerFromString(frames.value().columnText(0))) frame.container = *c;
		frame.id = frames.value().columnText(1);
		frame.owner = frames.value().columnText(2);
		frame.description = frames.value().columnText(3);
		frame.language = frames.value().columnText(4);
		frame.value = frames.value().columnText(5);
		frame.binary = frames.value().columnBlob(6);
		const std::string encoding = frames.value().columnText(7);
		frame.encoding = (encoding == "latin1") ? TextEncoding::Latin1
			: (encoding == "utf16") ? TextEncoding::Utf16
			: (encoding == "utf16be") ? TextEncoding::Utf16Be
			: (encoding == "utf8") ? TextEncoding::Utf8 : TextEncoding::Unknown;
		frame.ordinal = frames.value().columnInt(8);
		frame.rawSize = static_cast<std::size_t>(frames.value().columnInt64(9));
		frame.interpreted = frames.value().columnInt(10) != 0;
		snapshot.frames.push_back(std::move(frame));
	}

	auto pictures = const_cast<Database&>(m_database).prepare(
		"SELECT picture_type, mime_type, description, byte_length, width, height, content_sha256, "
		"frame_ordinal FROM tag_pictures WHERE snapshot_id = ? ORDER BY id;");
	if (!pictures) return pictures.error();
	pictures.value().bind(1, snapshot.id.value);

	while (true) {
		auto pictureRow = pictures.value().step();
		if (!pictureRow) return pictureRow.error();
		if (!pictureRow.value()) break;

		EmbeddedPicture picture;
		const std::string type = pictures.value().columnText(0);
		picture.type = (type == "front_cover") ? PictureType::FrontCover
			: (type == "back_cover") ? PictureType::BackCover : PictureType::Other;
		picture.mimeType = pictures.value().columnText(1);
		picture.description = pictures.value().columnText(2);
		picture.byteLength = static_cast<std::size_t>(pictures.value().columnInt64(3));
		picture.width = pictures.value().columnInt(4);
		picture.height = pictures.value().columnInt(5);
		picture.contentSha256 = pictures.value().columnText(6);
		picture.frameOrdinal = pictures.value().columnInt(7);
		snapshot.pictures.push_back(std::move(picture));
	}

	return std::optional<TagSnapshot>(std::move(snapshot));
}

// ---------------------------------------------------------------------------
// Albums
// ---------------------------------------------------------------------------

Status Catalogue::replaceAlbums(const std::vector<ProvisionalAlbum>& albums) {
	auto transaction = m_database.begin(Transaction::Kind::Immediate);
	if (!transaction) return Status(transaction.error());

	// Grouping is derived state and is rebuilt wholesale. Manual decisions live
	// in their own table and are therefore untouched by this (ID-001: locks
	// survive rescans).
	if (auto status = m_database.executeScript("DELETE FROM albums;"); !status) return status;

	auto insert = m_database.prepare(
		"INSERT INTO albums (group_key, album, album_artist, release_date, edition_qualifier, "
		"musicbrainz_album_id, disc_count, observed_track_count, declared_track_total, "
		"is_compilation, identity_confidence, flags, updated_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?);");
	if (!insert) return Status(insert.error());

	auto track = m_database.prepare(
		"INSERT INTO album_tracks (album_id, file_id, disc_number, track_number, track_artist) "
		"SELECT ?, id, display_disc, display_track, display_artist FROM files WHERE id = ?;");
	if (!track) return Status(track.error());

	auto evidence = m_database.prepare(
		"INSERT INTO album_evidence (album_id, kind, detail, supporting) VALUES (?,?,?,?);");
	if (!evidence) return Status(evidence.error());

	const std::string timestamp = nowIso8601();

	for (const auto& album : albums) {
		std::vector<std::string> flagNames;
		for (auto f : album.flags) flagNames.emplace_back(toString(f));

		Statement& s = insert.value();
		s.reset();
		s.clearBindings();
		s.bindAll(album.groupKey, album.album, album.albumArtist, album.date, album.editionQualifier,
			album.musicBrainzAlbumId, album.discCount, album.observedTrackCount,
			album.declaredTrackTotal, album.isCompilation, toString(album.identityConfidence),
			joinList(flagNames), timestamp);
		if (auto status = s.step(); !status) return Status(status.error());

		const std::int64_t albumId = m_database.lastInsertRowId();

		for (FileId file : album.files) {
			Statement& t = track.value();
			t.reset();
			t.clearBindings();
			t.bindAll(albumId, file.value);
			if (auto status = t.step(); !status) return Status(status.error());
		}

		for (const auto& e : album.evidence) {
			Statement& v = evidence.value();
			v.reset();
			v.clearBindings();
			v.bindAll(albumId, e.kind, e.detail, e.supporting);
			if (auto status = v.step(); !status) return Status(status.error());
		}
	}

	return transaction.value().commit();
}

namespace {

ProvisionalAlbum readAlbumRow(Statement& s) {
	ProvisionalAlbum album;
	album.id = AlbumId(s.columnInt64(0));
	album.groupKey = s.columnText(1);
	album.album = s.columnText(2);
	album.albumArtist = s.columnText(3);
	album.date = s.columnText(4);
	album.editionQualifier = s.columnText(5);
	album.musicBrainzAlbumId = s.columnText(6);
	album.discCount = s.columnInt(7);
	album.observedTrackCount = s.columnInt(8);
	if (!s.columnIsNull(9)) album.declaredTrackTotal = s.columnInt(9);
	album.isCompilation = s.columnInt(10) != 0;
	if (auto c = confidenceFromString(s.columnText(11))) album.identityConfidence = *c;
	for (const auto& flag : splitList(s.columnText(12))) {
		if (flag == "missing_track_numbers") album.flags.push_back(AlbumFlag::MissingTrackNumbers);
		else if (flag == "duplicate_track_numbers") album.flags.push_back(AlbumFlag::DuplicateTrackNumbers);
		else if (flag == "incomplete_track_run") album.flags.push_back(AlbumFlag::IncompleteTrackRun);
		else if (flag == "conflicting_album_artist") album.flags.push_back(AlbumFlag::ConflictingAlbumArtist);
		else if (flag == "conflicting_date") album.flags.push_back(AlbumFlag::ConflictingDate);
		else if (flag == "conflicting_release_id") album.flags.push_back(AlbumFlag::ConflictingReleaseId);
		else if (flag == "mixed_disc_numbering") album.flags.push_back(AlbumFlag::MixedDiscNumbering);
		else if (flag == "single_track_group") album.flags.push_back(AlbumFlag::SingleTrackGroup);
		else if (flag == "likely_compilation") album.flags.push_back(AlbumFlag::LikelyCompilation);
		else if (flag == "folder_title_mismatch") album.flags.push_back(AlbumFlag::FolderTitleMismatch);
		else if (flag == "no_album_tag") album.flags.push_back(AlbumFlag::NoAlbumTag);
	}
	return album;
}

constexpr std::string_view kAlbumColumns =
	"id, group_key, album, album_artist, release_date, edition_qualifier, musicbrainz_album_id, "
	"disc_count, observed_track_count, declared_track_total, is_compilation, identity_confidence, flags";

} // namespace

Result<std::vector<ProvisionalAlbum>> Catalogue::listAlbums(bool onlyNeedingReview, std::int64_t limit,
	std::int64_t offset) const {
	std::string sql = "SELECT " + std::string(kAlbumColumns) + " FROM albums";
	if (onlyNeedingReview) {
		sql += " WHERE flags <> '' OR identity_confidence IN ('unknown', 'weak')";
	}
	sql += " ORDER BY album_artist, album";
	if (limit > 0) sql += " LIMIT ?1 OFFSET ?2";
	sql += ";";

	auto statement = const_cast<Database&>(m_database).prepare(sql);
	if (!statement) return statement.error();
	if (limit > 0) {
		statement.value().bind(1, limit);
		statement.value().bind(2, offset);
	}

	std::vector<ProvisionalAlbum> out;
	while (true) {
		auto row = statement.value().step();
		if (!row) return row.error();
		if (!row.value()) break;
		out.push_back(readAlbumRow(statement.value()));
	}

	// Attach members and evidence.
	for (auto& album : out) {
		auto tracks = const_cast<Database&>(m_database).prepare(
			"SELECT file_id FROM album_tracks WHERE album_id = ? ORDER BY disc_number, track_number;");
		if (!tracks) return tracks.error();
		tracks.value().bind(1, album.id.value);
		while (true) {
			auto row = tracks.value().step();
			if (!row) return row.error();
			if (!row.value()) break;
			album.files.push_back(FileId(tracks.value().columnInt64(0)));
		}

		auto evidence = const_cast<Database&>(m_database).prepare(
			"SELECT kind, detail, supporting FROM album_evidence WHERE album_id = ? ORDER BY id;");
		if (!evidence) return evidence.error();
		evidence.value().bind(1, album.id.value);
		while (true) {
			auto row = evidence.value().step();
			if (!row) return row.error();
			if (!row.value()) break;
			album.evidence.push_back({evidence.value().columnText(0), evidence.value().columnText(1),
				evidence.value().columnInt(2) != 0});
		}
	}

	return out;
}

Result<std::optional<ProvisionalAlbum>> Catalogue::loadAlbum(AlbumId id) const {
	const std::string sql = "SELECT " + std::string(kAlbumColumns) + " FROM albums WHERE id = ?;";
	auto statement = const_cast<Database&>(m_database).prepare(sql);
	if (!statement) return statement.error();
	statement.value().bind(1, id.value);

	auto row = statement.value().step();
	if (!row) return row.error();
	if (!row.value()) return std::optional<ProvisionalAlbum>{};

	ProvisionalAlbum album = readAlbumRow(statement.value());

	auto tracks = const_cast<Database&>(m_database).prepare(
		"SELECT file_id FROM album_tracks WHERE album_id = ? ORDER BY disc_number, track_number;");
	if (!tracks) return tracks.error();
	tracks.value().bind(1, id.value);
	while (true) {
		auto trackRow = tracks.value().step();
		if (!trackRow) return trackRow.error();
		if (!trackRow.value()) break;
		album.files.push_back(FileId(tracks.value().columnInt64(0)));
	}

	auto evidence = const_cast<Database&>(m_database).prepare(
		"SELECT kind, detail, supporting FROM album_evidence WHERE album_id = ? ORDER BY id;");
	if (!evidence) return evidence.error();
	evidence.value().bind(1, id.value);
	while (true) {
		auto evidenceRow = evidence.value().step();
		if (!evidenceRow) return evidenceRow.error();
		if (!evidenceRow.value()) break;
		album.evidence.push_back({evidence.value().columnText(0), evidence.value().columnText(1),
			evidence.value().columnInt(2) != 0});
	}

	return std::optional<ProvisionalAlbum>(std::move(album));
}

Result<std::optional<AlbumId>> Catalogue::albumForFile(FileId file) const {
	auto statement = const_cast<Database&>(m_database).prepare(
		"SELECT album_id FROM album_tracks WHERE file_id = ? LIMIT 1;");
	if (!statement) return statement.error();
	statement.value().bind(1, file.value);

	auto row = statement.value().step();
	if (!row) return row.error();
	if (!row.value()) return std::optional<AlbumId>{};
	return std::optional<AlbumId>(AlbumId(statement.value().columnInt64(0)));
}

Result<std::int64_t> Catalogue::countAlbums(bool onlyNeedingReview) const {
	std::string sql = "SELECT COUNT(*) FROM albums";
	if (onlyNeedingReview) sql += " WHERE flags <> '' OR identity_confidence IN ('unknown', 'weak')";
	sql += ";";

	auto statement = const_cast<Database&>(m_database).prepare(sql);
	if (!statement) return statement.error();
	auto row = statement.value().step();
	if (!row) return row.error();
	if (!row.value()) return std::int64_t{0};
	return statement.value().columnInt64(0);
}

// ---------------------------------------------------------------------------
// Artwork
// ---------------------------------------------------------------------------

Result<ArtworkId> Catalogue::saveArtworkCandidate(AlbumId album, const ArtworkCandidate& candidate,
	std::string_view selectionState, std::string_view reason, std::string_view policyVersion) {
	std::vector<std::string> defectNames;
	for (auto d : candidate.defects) defectNames.emplace_back(toString(d));

	auto statement = m_database.prepare(
		"INSERT INTO artwork_assets (album_id, provider_id, provider_name, page_url, image_url, "
		"local_path, content_sha256, mime_type, byte_length, measured_width, measured_height, "
		"dimensions_measured, claimed_width, claimed_height, source_type, cover_match, "
		"match_confidence, defects, evidence, selection_state, selection_reason, policy_version, "
		"retrieved_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
	if (!statement) return statement.error();

	statement.value().bindAll(album.value, candidate.providerId, candidate.providerName,
		candidate.pageUrl, candidate.imageUrl, candidate.localPath, candidate.contentSha256,
		candidate.mimeType, candidate.byteLength, candidate.measuredWidth, candidate.measuredHeight,
		candidate.dimensionsMeasured, candidate.claimedWidth, candidate.claimedHeight,
		toString(candidate.sourceType), toString(candidate.coverMatch),
		toString(candidate.matchConfidence), joinList(defectNames), joinEvidence(candidate.evidence),
		selectionState, reason, policyVersion, nowIso8601());
	if (auto status = statement.value().execute(); !status) return status.error();
	return ArtworkId(m_database.lastInsertRowId());
}

namespace {

ArtworkCandidate readArtworkRow(Statement& s) {
	ArtworkCandidate c;
	c.id = ArtworkId(s.columnInt64(0));
	c.providerId = s.columnText(1);
	c.providerName = s.columnText(2);
	c.pageUrl = s.columnText(3);
	c.imageUrl = s.columnText(4);
	c.localPath = s.columnText(5);
	c.contentSha256 = s.columnText(6);
	c.mimeType = s.columnText(7);
	c.byteLength = s.columnInt64(8);
	c.measuredWidth = s.columnInt(9);
	c.measuredHeight = s.columnInt(10);
	c.dimensionsMeasured = s.columnInt(11) != 0;
	c.claimedWidth = s.columnInt(12);
	c.claimedHeight = s.columnInt(13);

	const std::string sourceType = s.columnText(14);
	c.sourceType = (sourceType == "evidenced_digital_asset") ? ArtworkSourceType::EvidencedDigitalAsset
		: (sourceType == "evidenced_restoration") ? ArtworkSourceType::EvidencedRestoration
		: (sourceType == "scan_or_photo") ? ArtworkSourceType::ScanOrPhoto
		: (sourceType == "user_supplied") ? ArtworkSourceType::UserSupplied
		: (sourceType == "existing_embedded") ? ArtworkSourceType::ExistingEmbedded
		: ArtworkSourceType::Unknown;

	const std::string coverMatch = s.columnText(15);
	c.coverMatch = (coverMatch == "intended_cover") ? CoverMatch::IntendedCover
		: (coverMatch == "faithful_restoration") ? CoverMatch::FaithfulRestoration
		: (coverMatch == "alternate_edition") ? CoverMatch::AlternateEdition
		: (coverMatch == "different_release") ? CoverMatch::DifferentRelease
		: (coverMatch == "wrong_artwork") ? CoverMatch::WrongArtwork
		: CoverMatch::Unknown;

	if (auto confidence = confidenceFromString(s.columnText(16))) c.matchConfidence = *confidence;

	for (const auto& defect : splitList(s.columnText(17))) {
		if (defect == "colour_cast") c.defects.push_back(ArtworkDefect::ColourCast);
		else if (defect == "fading") c.defects.push_back(ArtworkDefect::Fading);
		else if (defect == "sleeve_wear") c.defects.push_back(ArtworkDefect::SleeveWear);
		else if (defect == "ring_wear") c.defects.push_back(ArtworkDefect::RingWear);
		else if (defect == "crease") c.defects.push_back(ArtworkDefect::Crease);
		else if (defect == "scratch") c.defects.push_back(ArtworkDefect::Scratch);
		else if (defect == "sticker") c.defects.push_back(ArtworkDefect::Sticker);
		else if (defect == "unwanted_border") c.defects.push_back(ArtworkDefect::UnwantedBorder);
		else if (defect == "skew") c.defects.push_back(ArtworkDefect::Skew);
		else if (defect == "scan_moire") c.defects.push_back(ArtworkDefect::ScanMoire);
		else if (defect == "compression_blocks") c.defects.push_back(ArtworkDefect::CompressionBlocks);
		else if (defect == "blur") c.defects.push_back(ArtworkDefect::Blur);
		else if (defect == "sharpening_halo") c.defects.push_back(ArtworkDefect::SharpeningHalo);
		else if (defect == "watermark") c.defects.push_back(ArtworkDefect::Watermark);
		else if (defect == "wrong_image") c.defects.push_back(ArtworkDefect::WrongImage);
		else if (defect == "upscaled") c.defects.push_back(ArtworkDefect::Upscaled);
	}
	c.evidence = splitEvidence(s.columnText(18));

	const std::string state = s.columnText(19);
	c.manuallyLocked = (state == "locked");
	c.manuallyRejected = (state == "rejected");
	return c;
}

constexpr std::string_view kArtworkColumns =
	"id, provider_id, provider_name, page_url, image_url, local_path, content_sha256, mime_type, "
	"byte_length, measured_width, measured_height, dimensions_measured, claimed_width, claimed_height, "
	"source_type, cover_match, match_confidence, defects, evidence, selection_state";

} // namespace

Result<std::vector<ArtworkCandidate>> Catalogue::artworkCandidates(AlbumId album) const {
	const std::string sql = "SELECT " + std::string(kArtworkColumns)
		+ " FROM artwork_assets WHERE album_id = ? ORDER BY id;";
	auto statement = const_cast<Database&>(m_database).prepare(sql);
	if (!statement) return statement.error();
	statement.value().bind(1, album.value);

	std::vector<ArtworkCandidate> out;
	while (true) {
		auto row = statement.value().step();
		if (!row) return row.error();
		if (!row.value()) break;
		out.push_back(readArtworkRow(statement.value()));
	}
	return out;
}

Status Catalogue::setArtworkSelection(ArtworkId asset, std::string_view state, std::string_view reason) {
	auto statement = m_database.prepare(
		"UPDATE artwork_assets SET selection_state = ?, selection_reason = ? WHERE id = ?;");
	if (!statement) return Status(statement.error());
	statement.value().bindAll(state, reason, asset.value);
	return statement.value().execute();
}

Result<std::optional<ArtworkCandidate>> Catalogue::selectedArtwork(AlbumId album) const {
	const std::string sql = "SELECT " + std::string(kArtworkColumns)
		+ " FROM artwork_assets WHERE album_id = ? AND selection_state IN ('selected', 'locked') "
		  "ORDER BY CASE selection_state WHEN 'locked' THEN 0 ELSE 1 END, id DESC LIMIT 1;";
	auto statement = const_cast<Database&>(m_database).prepare(sql);
	if (!statement) return statement.error();
	statement.value().bind(1, album.value);

	auto row = statement.value().step();
	if (!row) return row.error();
	if (!row.value()) return std::optional<ArtworkCandidate>{};
	return std::optional<ArtworkCandidate>(readArtworkRow(statement.value()));
}

Status Catalogue::saveDerivative(ArtworkId asset, std::string_view configHash, std::string_view localPath,
	std::string_view sha256, int width, int height, std::int64_t bytes, int quality,
	std::string_view subsampling) {
	auto statement = m_database.prepare(
		"INSERT OR REPLACE INTO artwork_derivatives (asset_id, config_hash, local_path, "
		"content_sha256, width, height, byte_length, jpeg_quality, subsampling, created_at) "
		"VALUES (?,?,?,?,?,?,?,?,?,?);");
	if (!statement) return Status(statement.error());
	statement.value().bindAll(asset.value, configHash, localPath, sha256, width, height, bytes,
		quality, subsampling, nowIso8601());
	return statement.value().execute();
}

Result<std::optional<std::string>> Catalogue::findDerivative(ArtworkId asset,
	std::string_view configHash) const {
	auto statement = const_cast<Database&>(m_database).prepare(
		"SELECT local_path FROM artwork_derivatives WHERE asset_id = ? AND config_hash = ?;");
	if (!statement) return statement.error();
	statement.value().bindAll(asset.value, configHash);

	auto row = statement.value().step();
	if (!row) return row.error();
	if (!row.value()) return std::optional<std::string>{};
	return std::optional<std::string>(statement.value().columnText(0));
}

// ---------------------------------------------------------------------------
// Tempo and lyrics
// ---------------------------------------------------------------------------

Status Catalogue::saveTempo(FileId file, const TempoAnalysis& analysis, const TempoDecision& decision) {
	std::vector<std::string> sections;
	for (double bpm : analysis.sectionBpms) sections.push_back(std::to_string(bpm));

	auto statement = m_database.prepare(
		"INSERT OR REPLACE INTO tempo_results (file_id, raw_bpm, half_tempo, double_tempo, stability, "
		"sections_analysed, section_bpms, character, engine, settings_hash, decision, decided_bpm, "
		"tagged_bpm, existing_bpm, confidence, reason, analysed_at) "
		"VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
	if (!statement) return Status(statement.error());

	statement.value().bindAll(file.value, analysis.bpm, analysis.halfTempo, analysis.doubleTempo,
		analysis.stability, analysis.sectionsAnalysed, joinList(sections),
		toString(analysis.character), analysis.engine, analysis.settings, toString(decision.kind),
		decision.preciseBpm, decision.taggedBpm, decision.existingBpm, toString(decision.confidence),
		decision.reason, nowIso8601());
	return statement.value().execute();
}

Result<std::optional<TempoDecision>> Catalogue::loadTempo(FileId file) const {
	auto statement = const_cast<Database&>(m_database).prepare(
		"SELECT decision, decided_bpm, tagged_bpm, existing_bpm, confidence, reason, half_tempo, "
		"double_tempo FROM tempo_results WHERE file_id = ? ORDER BY analysed_at DESC LIMIT 1;");
	if (!statement) return statement.error();
	statement.value().bind(1, file.value);

	auto row = statement.value().step();
	if (!row) return row.error();
	if (!row.value()) return std::optional<TempoDecision>{};

	TempoDecision d;
	const std::string kind = statement.value().columnText(0);
	d.kind = (kind == "no_change") ? TempoDecisionKind::NoChange
		: (kind == "propose") ? TempoDecisionKind::Propose
		: (kind == "not_applicable") ? TempoDecisionKind::NotApplicable
		: (kind == "failed") ? TempoDecisionKind::Failed
		: TempoDecisionKind::NeedsReview;
	d.preciseBpm = statement.value().columnDouble(1);
	d.taggedBpm = statement.value().columnInt(2);
	if (!statement.value().columnIsNull(3)) d.existingBpm = statement.value().columnDouble(3);
	if (auto c = confidenceFromString(statement.value().columnText(4))) d.confidence = *c;
	d.reason = statement.value().columnText(5);
	if (statement.value().columnDouble(6) > 0) d.alternatives.push_back(statement.value().columnDouble(6));
	if (statement.value().columnDouble(7) > 0) d.alternatives.push_back(statement.value().columnDouble(7));
	return std::optional<TempoDecision>(std::move(d));
}

Status Catalogue::saveLyrics(FileId file, const LyricsDecision& decision) {
	auto statement = m_database.prepare(
		"INSERT OR REPLACE INTO lyrics_results (file_id, state, lyrics_text, language, provider_id, "
		"source_url, match_evidence, confidence, reason, retrieved_at) VALUES (?,?,?,?,?,?,?,?,?,?);");
	if (!statement) return Status(statement.error());
	statement.value().bindAll(file.value, toString(decision.state), decision.text, decision.language,
		decision.providerId, decision.sourceUrl, joinEvidence(decision.evidence),
		toString(decision.confidence), decision.reason, nowIso8601());
	return statement.value().execute();
}

Result<std::optional<LyricsDecision>> Catalogue::loadLyrics(FileId file) const {
	auto statement = const_cast<Database&>(m_database).prepare(
		"SELECT state, lyrics_text, language, provider_id, source_url, match_evidence, confidence, "
		"reason FROM lyrics_results WHERE file_id = ? ORDER BY retrieved_at DESC LIMIT 1;");
	if (!statement) return statement.error();
	statement.value().bind(1, file.value);

	auto row = statement.value().step();
	if (!row) return row.error();
	if (!row.value()) return std::optional<LyricsDecision>{};

	LyricsDecision d;
	if (auto state = lyricsStateFromString(statement.value().columnText(0))) d.state = *state;
	d.text = statement.value().columnText(1);
	d.language = statement.value().columnText(2);
	d.providerId = statement.value().columnText(3);
	d.sourceUrl = statement.value().columnText(4);
	d.evidence = splitEvidence(statement.value().columnText(5));
	if (auto c = confidenceFromString(statement.value().columnText(6))) d.confidence = *c;
	d.reason = statement.value().columnText(7);
	return std::optional<LyricsDecision>(std::move(d));
}

// ---------------------------------------------------------------------------
// Manual decisions
// ---------------------------------------------------------------------------

Status Catalogue::recordManualDecision(std::string_view scope, std::int64_t scopeId,
	std::string_view field, std::string_view decision, std::string_view value,
	std::optional<std::int64_t> referenceId, std::string_view note) {
	auto statement = m_database.prepare(
		"INSERT OR REPLACE INTO manual_decisions (scope, scope_id, field, decision, value, "
		"reference_id, note, decided_at) VALUES (?,?,?,?,?,?,?,?);");
	if (!statement) return Status(statement.error());
	statement.value().bindAll(scope, scopeId, field, decision, value, referenceId, note, nowIso8601());
	return statement.value().execute();
}

Result<std::vector<std::tuple<std::string, std::string, std::string>>> Catalogue::manualDecisions(
	std::string_view scope, std::int64_t scopeId) const {
	auto statement = const_cast<Database&>(m_database).prepare(
		"SELECT field, decision, value FROM manual_decisions WHERE scope = ? AND scope_id = ?;");
	if (!statement) return statement.error();
	statement.value().bindAll(scope, scopeId);

	std::vector<std::tuple<std::string, std::string, std::string>> out;
	while (true) {
		auto row = statement.value().step();
		if (!row) return row.error();
		if (!row.value()) break;
		out.emplace_back(statement.value().columnText(0), statement.value().columnText(1),
			statement.value().columnText(2));
	}
	return out;
}

Status Catalogue::clearManualDecision(std::string_view scope, std::int64_t scopeId,
	std::string_view field) {
	auto statement = m_database.prepare(
		"DELETE FROM manual_decisions WHERE scope = ? AND scope_id = ? AND field = ?;");
	if (!statement) return Status(statement.error());
	statement.value().bindAll(scope, scopeId, field);
	return statement.value().execute();
}

// ---------------------------------------------------------------------------
// Change sets
// ---------------------------------------------------------------------------

Result<ChangeSetId> Catalogue::createChangeSet(std::string_view outputRoot, std::string_view configHash,
	std::string_view note) {
	auto statement = m_database.prepare(
		"INSERT INTO change_sets (output_root_path, created_at, config_hash, state, note) "
		"VALUES (?, ?, ?, 'planned', ?);");
	if (!statement) return statement.error();
	statement.value().bindAll(outputRoot, nowIso8601(), configHash, note);
	if (auto status = statement.value().execute(); !status) return status.error();
	return ChangeSetId(m_database.lastInsertRowId());
}

Status Catalogue::saveFilePlan(ChangeSetId set, const FilePlan& plan) {
	std::vector<std::string> blockerNames;
	for (auto b : plan.blockers) blockerNames.emplace_back(toString(b));

	auto statement = m_database.prepare(
		"INSERT OR REPLACE INTO file_operations (change_set_id, file_id, source_path, "
		"expected_source_sha256, expected_audio_sha256, source_device_id, source_inode, "
		"source_size_bytes, source_modified_unix_ms, destination_relative, plan_json, blockers, "
		"state) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,'planned');");
	if (!statement) return Status(statement.error());

	// The plan is serialised as a single-file change set so the executor can act
	// from the journal alone after a restart.
	ChangeSet single;
	single.files.push_back(plan);
	const std::string planJson = toJson(single, true);

	statement.value().bindAll(set.value, plan.fileId.value, plan.sourcePath, plan.sourceSha256,
		plan.sourceAudioSha256, static_cast<std::int64_t>(plan.sourceIdentity.deviceId),
		static_cast<std::int64_t>(plan.sourceIdentity.inode), plan.sourceIdentity.sizeBytes,
		plan.sourceIdentity.modifiedUnixMs, plan.destinationRelativePath, planJson,
		joinList(blockerNames));
	return statement.value().execute();
}

Result<std::vector<FilePlan>> Catalogue::loadFilePlans(ChangeSetId set, bool onlyWritable) const {
	std::string sql =
		"SELECT file_id, source_path, expected_source_sha256, expected_audio_sha256, "
		"source_device_id, source_inode, source_size_bytes, source_modified_unix_ms, "
		"destination_relative, blockers, state FROM file_operations WHERE change_set_id = ?";
	if (onlyWritable) sql += " AND blockers = '' AND state NOT IN ('committed', 'skipped')";
	sql += " ORDER BY destination_relative;";

	auto statement = const_cast<Database&>(m_database).prepare(sql);
	if (!statement) return statement.error();
	statement.value().bind(1, set.value);

	std::vector<FilePlan> out;
	while (true) {
		auto row = statement.value().step();
		if (!row) return row.error();
		if (!row.value()) break;

		FilePlan plan;
		plan.fileId = FileId(statement.value().columnInt64(0));
		plan.sourcePath = statement.value().columnText(1);
		plan.sourceSha256 = statement.value().columnText(2);
		plan.sourceAudioSha256 = statement.value().columnText(3);
		plan.sourceIdentity.deviceId = static_cast<std::uint64_t>(statement.value().columnInt64(4));
		plan.sourceIdentity.inode = static_cast<std::uint64_t>(statement.value().columnInt64(5));
		plan.sourceIdentity.sizeBytes = statement.value().columnInt64(6);
		plan.sourceIdentity.modifiedUnixMs = statement.value().columnInt64(7);
		plan.destinationRelativePath = statement.value().columnText(8);
		for (const auto& blocker : splitList(statement.value().columnText(9))) {
			if (blocker == "naming_review_required") plan.blockers.push_back(PlanBlocker::NamingReviewRequired);
			else if (blocker == "destination_collision") plan.blockers.push_back(PlanBlocker::DestinationCollision);
			else if (blocker == "artwork_review_required") plan.blockers.push_back(PlanBlocker::ArtworkReviewRequired);
			else if (blocker == "privacy_review_required") plan.blockers.push_back(PlanBlocker::PrivacyReviewRequired);
			else if (blocker == "gain_exception_required") plan.blockers.push_back(PlanBlocker::GainExceptionRequired);
			else if (blocker == "tempo_review_required") plan.blockers.push_back(PlanBlocker::TempoReviewRequired);
			else if (blocker == "lyrics_review_required") plan.blockers.push_back(PlanBlocker::LyricsReviewRequired);
			else if (blocker == "source_unreadable") plan.blockers.push_back(PlanBlocker::SourceUnreadable);
			else if (blocker == "source_changed_since_plan") plan.blockers.push_back(PlanBlocker::SourceChangedSincePlan);
			else if (blocker == "destination_inside_protected_root") {
				plan.blockers.push_back(PlanBlocker::DestinationInsideProtectedRoot);
			}
		}
		out.push_back(std::move(plan));
	}
	return out;
}

Result<std::optional<ChangeSetId>> Catalogue::latestChangeSet() const {
	auto statement = const_cast<Database&>(m_database).prepare(
		"SELECT id FROM change_sets ORDER BY created_at DESC, id DESC LIMIT 1;");
	if (!statement) return statement.error();
	auto row = statement.value().step();
	if (!row) return row.error();
	if (!row.value()) return std::optional<ChangeSetId>{};
	return std::optional<ChangeSetId>(ChangeSetId(statement.value().columnInt64(0)));
}

Status Catalogue::setChangeSetState(ChangeSetId set, std::string_view state) {
	auto statement = m_database.prepare("UPDATE change_sets SET state = ? WHERE id = ?;");
	if (!statement) return Status(statement.error());
	statement.value().bindAll(state, set.value);
	return statement.value().execute();
}

Status Catalogue::reserveDestination(ChangeSetId set, std::string_view relativePath, FileId file) {
	auto statement = m_database.prepare(
		"INSERT INTO output_reservations (change_set_id, destination_folded, destination_relative, "
		"file_id) VALUES (?,?,?,?);");
	if (!statement) return Status(statement.error());
	statement.value().bindAll(set.value, foldPathForComparison(relativePath), relativePath, file.value);

	auto result = statement.value().step();
	if (!result) {
		// The UNIQUE constraint is the actual guarantee behind NAME-002.
		return Status(Error{ErrorCode::Collision,
			"destination \"" + std::string(relativePath) + "\" is already reserved in this change set"});
	}
	return Status::success();
}

// ---------------------------------------------------------------------------
// Operation journal
// ---------------------------------------------------------------------------

Status Catalogue::setOperationState(ChangeSetId set, FileId file, std::string_view state,
	std::string_view tempPath, std::string_view error) {
	auto statement = m_database.prepare(
		"UPDATE file_operations SET state = ?, temp_path = COALESCE(NULLIF(?, ''), temp_path), "
		"error = NULLIF(?, ''), started_at = COALESCE(started_at, ?) "
		"WHERE change_set_id = ? AND file_id = ?;");
	if (!statement) return Status(statement.error());
	statement.value().bindAll(state, tempPath, error, nowIso8601(), set.value, file.value);
	return statement.value().execute();
}

Status Catalogue::completeOperation(ChangeSetId set, FileId file, std::string_view writtenSha256,
	std::string_view writtenAudioSha256, std::int64_t bytesBefore, std::int64_t bytesAfter,
	std::int64_t optimisationSavings, std::int64_t enrichmentGrowth) {
	auto statement = m_database.prepare(
		"UPDATE file_operations SET state = 'committed', written_sha256 = ?, written_audio_sha256 = ?, "
		"bytes_before = ?, bytes_after = ?, optimisation_savings = ?, enrichment_growth = ?, "
		"finished_at = ?, error = NULL WHERE change_set_id = ? AND file_id = ?;");
	if (!statement) return Status(statement.error());
	statement.value().bindAll(writtenSha256, writtenAudioSha256, bytesBefore, bytesAfter,
		optimisationSavings, enrichmentGrowth, nowIso8601(), set.value, file.value);
	return statement.value().execute();
}

Result<std::vector<std::tuple<FileId, std::string, std::string>>> Catalogue::incompleteOperations(
	ChangeSetId set) const {
	auto statement = const_cast<Database&>(m_database).prepare(
		"SELECT file_id, state, COALESCE(temp_path, '') FROM file_operations "
		"WHERE change_set_id = ? AND state NOT IN ('planned', 'committed', 'skipped', 'failed');");
	if (!statement) return statement.error();
	statement.value().bind(1, set.value);

	std::vector<std::tuple<FileId, std::string, std::string>> out;
	while (true) {
		auto row = statement.value().step();
		if (!row) return row.error();
		if (!row.value()) break;
		out.emplace_back(FileId(statement.value().columnInt64(0)), statement.value().columnText(1),
			statement.value().columnText(2));
	}
	return out;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

Status Catalogue::setSetting(std::string_view key, std::string_view value) {
	auto statement = m_database.prepare("INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?);");
	if (!statement) return Status(statement.error());
	statement.value().bindAll(key, value);
	return statement.value().execute();
}

// ---------------------------------------------------------------------------
// Reports (FN-SCAN-03, NAME-001)
// ---------------------------------------------------------------------------

namespace {

/// Runs a "SELECT label, COUNT(*) ... GROUP BY label" query into a list.
Result<std::vector<std::pair<std::string, std::int64_t>>> groupedCounts(Database& database,
	std::string_view sql) {
	auto statement = database.prepare(sql);
	if (!statement) return statement.error();

	std::vector<std::pair<std::string, std::int64_t>> out;
	while (true) {
		auto row = statement.value().step();
		if (!row) return row.error();
		if (!row.value()) break;
		out.emplace_back(statement.value().columnText(0), statement.value().columnInt64(1));
	}
	return out;
}

Result<std::int64_t> scalar(Database& database, std::string_view sql) {
	auto statement = database.prepare(sql);
	if (!statement) return statement.error();
	auto row = statement.value().step();
	if (!row) return row.error();
	if (!row.value()) return std::int64_t{0};
	return statement.value().columnInt64(0);
}

} // namespace

Result<CoverageReport> Catalogue::coverageReport() const {
	Database& db = const_cast<Database&>(m_database);
	CoverageReport report;

	const struct { std::int64_t* target; std::string_view sql; } kScalars[] = {
		{&report.totalFiles, "SELECT COUNT(*) FROM files;"},
		{&report.readable, "SELECT COUNT(*) FROM files WHERE read_status = 'ok';"},
		{&report.unreadable, "SELECT COUNT(*) FROM files WHERE read_status <> 'ok';"},
		{&report.withArtwork, "SELECT COUNT(*) FROM files WHERE has_artwork = 1;"},
		{&report.withLyrics, "SELECT COUNT(*) FROM files WHERE has_lyrics = 1;"},
		{&report.withBpm, "SELECT COUNT(*) FROM files WHERE display_bpm IS NOT NULL;"},
		{&report.withGainFields, "SELECT COUNT(*) FROM files WHERE has_gain_fields = 1;"},
		{&report.withPrivacyFindings, "SELECT COUNT(*) FROM files WHERE has_privacy_findings = 1;"},
		{&report.totalBytes, "SELECT COALESCE(SUM(size_bytes), 0) FROM files;"},
		{&report.totalDurationMs, "SELECT COALESCE(SUM(duration_ms), 0) FROM files;"},
		{&report.albumCount, "SELECT COUNT(*) FROM albums;"},
		{&report.albumsNeedingReview,
			"SELECT COUNT(*) FROM albums WHERE flags <> '' OR identity_confidence IN ('unknown','weak');"},
	};
	for (const auto& entry : kScalars) {
		auto value = scalar(db, entry.sql);
		if (!value) return value.error();
		*entry.target = value.value();
	}

	auto tagVersions = groupedCounts(db,
		"SELECT primary_container, COUNT(*) FROM files WHERE read_status = 'ok' "
		"GROUP BY primary_container ORDER BY COUNT(*) DESC;");
	if (!tagVersions) return tagVersions.error();
	report.tagVersionDistribution = tagVersions.value();

	// Artwork sizes are bucketed against the 600 px output so the report answers
	// the question that matters: how many covers are already adequate.
	auto artworkSizes = groupedCounts(db,
		"SELECT CASE "
		"  WHEN has_artwork = 0 THEN 'none' "
		"  WHEN artwork_width = 0 THEN 'unmeasured' "
		"  WHEN artwork_width < 600 THEN 'below 600' "
		"  WHEN artwork_width = 600 THEN 'exactly 600' "
		"  WHEN artwork_width < 1000 THEN '601-999' "
		"  WHEN artwork_width < 1500 THEN '1000-1499' "
		"  WHEN artwork_width < 2500 THEN '1500-2499' "
		"  ELSE '2500 and above' END AS bucket, COUNT(*) "
		"FROM files GROUP BY bucket ORDER BY COUNT(*) DESC;");
	if (!artworkSizes) return artworkSizes.error();
	report.artworkSizeDistribution = artworkSizes.value();

	auto bitrateModes = groupedCounts(db,
		"SELECT bitrate_mode, COUNT(*) FROM files WHERE read_status = 'ok' "
		"GROUP BY bitrate_mode ORDER BY COUNT(*) DESC;");
	if (!bitrateModes) return bitrateModes.error();
	report.bitrateModeDistribution = bitrateModes.value();

	// Missing fields, reported per field rather than as one "incomplete" count,
	// because the fields have different consequences: a missing album artist
	// blocks the naming template, a missing genre does not.
	const struct { std::string_view label; std::string_view column; } kFields[] = {
		{"title", "display_title"},
		{"artist", "display_artist"},
		{"album artist", "display_album_artist"},
		{"album", "display_album"},
		{"genre", "display_genre"},
		{"date", "display_date"},
	};
	for (const auto& field : kFields) {
		const std::string sql = "SELECT COUNT(*) FROM files WHERE read_status = 'ok' AND "
			+ std::string(field.column) + " = '';";
		auto count = scalar(db, sql);
		if (!count) return count.error();
		if (count.value() > 0) {
			report.missingFieldCounts.emplace_back(std::string(field.label), count.value());
		}
	}
	auto missingTrack = scalar(db,
		"SELECT COUNT(*) FROM files WHERE read_status = 'ok' AND display_track IS NULL;");
	if (!missingTrack) return missingTrack.error();
	if (missingTrack.value() > 0) {
		report.missingFieldCounts.emplace_back("track number", missingTrack.value());
	}

	auto readErrors = groupedCounts(db,
		"SELECT read_status, COUNT(*) FROM files WHERE read_status <> 'ok' GROUP BY read_status;");
	if (!readErrors) return readErrors.error();
	report.readErrorCounts = readErrors.value();

	auto duplicates = scalar(db,
		"SELECT COUNT(*) FROM (SELECT audio_sha256 FROM files WHERE audio_sha256 <> '' "
		"GROUP BY audio_sha256 HAVING COUNT(*) > 1);");
	if (!duplicates) return duplicates.error();
	report.likelyDuplicateGroups = duplicates.value();

	return report;
}

Result<NamingConformity> Catalogue::namingConformity(const NamingTemplate& naming) const {
	Database& db = const_cast<Database&>(m_database);
	NamingConformity conformity;

	auto statement = db.prepare(
		"SELECT relative_path, display_album_artist, display_album, display_artist, display_title, "
		"display_track, display_disc, extension FROM files WHERE read_status = 'ok';");
	if (!statement) return statement.error();

	std::map<std::string, std::int64_t> exceptionCounts;
	std::map<std::string, std::int64_t> grammars;

	while (true) {
		auto row = statement.value().step();
		if (!row) return row.error();
		if (!row.value()) break;

		++conformity.totalFiles;

		const std::string actualPath = statement.value().columnText(0);

		NamingInput input;
		input.albumArtist = statement.value().columnText(1);
		input.album = statement.value().columnText(2);
		input.artist = statement.value().columnText(3);
		input.title = statement.value().columnText(4);
		if (!statement.value().columnIsNull(5)) input.trackNumber = statement.value().columnInt(5);
		if (!statement.value().columnIsNull(6)) input.discNumber = statement.value().columnInt(6);
		input.extension = statement.value().columnText(7);

		const NamingResult result = naming.apply(input);

		if (result.requiresReview) {
			++conformity.wouldNeedReview;
		}
		for (const auto& exception : result.exceptions) {
			++exceptionCounts[std::string(toString(exception.kind))];
		}

		// Conformity is measured by comparing the file's *actual* relative path
		// with the path the template would produce. This is the measured figure
		// the FRD requires to be kept distinct from the template specification.
		if (result.ok() && foldPathForComparison(result.relativePath) == foldPathForComparison(actualPath)) {
			++conformity.matchingTemplate;
			if (conformity.representativeMatches.size() < 10) {
				conformity.representativeMatches.push_back(actualPath);
			}
		} else if (conformity.representativeExceptions.size() < 20) {
			std::string note = actualPath;
			note += "  ->  ";
			note += result.ok() ? result.relativePath : std::string("(review required)");
			conformity.representativeExceptions.push_back(std::move(note));
		}

		// Observed grammar: the shape of the path, with the variable parts
		// replaced. This is what shows which other conventions exist.
		const auto components = text::split(actualPath, '/');
		std::string grammar = std::to_string(components.size()) + " levels";
		if (!components.empty()) {
			const std::string& leaf = components.back();
			if (leaf.size() > 3 && std::isdigit(static_cast<unsigned char>(leaf[0]))
				&& std::isdigit(static_cast<unsigned char>(leaf[1]))) {
				grammar += leaf.compare(2, 2, ". ") == 0 ? ", 'NN. ' prefix"
					: (leaf[2] == ' ' ? ", 'NN ' prefix" : ", 'NN' prefix");
			} else {
				grammar += ", no track prefix";
			}
			grammar += (leaf.find(" - ") != std::string::npos) ? ", ' - ' separator" : ", no ' - '";
		}
		++grammars[grammar];
	}

	for (const auto& [kind, count] : exceptionCounts) {
		conformity.exceptionCounts.emplace_back(kind, count);
	}
	std::sort(conformity.exceptionCounts.begin(), conformity.exceptionCounts.end(),
		[](const auto& a, const auto& b) { return a.second > b.second; });

	for (const auto& [grammar, count] : grammars) {
		conformity.observedGrammars.emplace_back(grammar, count);
	}
	std::sort(conformity.observedGrammars.begin(), conformity.observedGrammars.end(),
		[](const auto& a, const auto& b) { return a.second > b.second; });

	return conformity;
}

Result<std::vector<std::pair<std::string, std::int64_t>>> Catalogue::duplicateAudioGroups(
	std::int64_t limit) const {
	auto statement = const_cast<Database&>(m_database).prepare(
		"SELECT audio_sha256, COUNT(*) AS n FROM files WHERE audio_sha256 <> '' "
		"GROUP BY audio_sha256 HAVING n > 1 ORDER BY n DESC LIMIT ?;");
	if (!statement) return statement.error();
	statement.value().bind(1, limit);

	std::vector<std::pair<std::string, std::int64_t>> out;
	while (true) {
		auto row = statement.value().step();
		if (!row) return row.error();
		if (!row.value()) break;
		out.emplace_back(statement.value().columnText(0), statement.value().columnInt64(1));
	}
	return out;
}

Result<std::optional<std::string>> Catalogue::setting(std::string_view key) const {
	auto statement = const_cast<Database&>(m_database).prepare(
		"SELECT value FROM settings WHERE key = ?;");
	if (!statement) return statement.error();
	statement.value().bind(1, key);

	auto row = statement.value().step();
	if (!row) return row.error();
	if (!row.value()) return std::optional<std::string>{};
	return std::optional<std::string>(statement.value().columnText(0));
}

} // namespace ml
