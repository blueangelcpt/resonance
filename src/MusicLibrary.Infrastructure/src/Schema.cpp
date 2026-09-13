// SPDX-License-Identifier: GPL-3.0-or-later
// The catalogue schema (FRD section 4, CAT-001).
//
// Observed, proposed and written state live in separate tables. A failed write
// can therefore never make the catalogue claim that a file was updated: the
// `file_operations` row records its own outcome independently of the
// `tag_snapshots` row that describes what is actually on disk.
#include "mlinfra/Sqlite.hpp"

namespace ml {

namespace {

constexpr std::string_view kMigration001 = R"SQL(
-- ===========================================================================
-- Roots and scans
-- ===========================================================================
CREATE TABLE library_roots (
	id                INTEGER PRIMARY KEY,
	path              TEXT    NOT NULL UNIQUE,
	resolved_path     TEXT    NOT NULL,
	label             TEXT    NOT NULL DEFAULT '',
	kind              TEXT    NOT NULL CHECK (kind IN ('protected_source', 'output', 'staging')),
	volume_identity   TEXT    NOT NULL DEFAULT '',
	device_id         INTEGER NOT NULL DEFAULT 0,
	added_at          TEXT    NOT NULL,
	last_seen_at      TEXT
);

CREATE TABLE scan_runs (
	id                INTEGER PRIMARY KEY,
	root_id           INTEGER NOT NULL REFERENCES library_roots(id) ON DELETE CASCADE,
	started_at        TEXT    NOT NULL,
	finished_at       TEXT,
	generation        INTEGER NOT NULL,
	files_seen        INTEGER NOT NULL DEFAULT 0,
	files_added       INTEGER NOT NULL DEFAULT 0,
	files_changed     INTEGER NOT NULL DEFAULT 0,
	files_unreadable  INTEGER NOT NULL DEFAULT 0,
	-- A scan that ran while a root was absent must never be read as deletion
	-- of that root's contents (FN-SCAN-04).
	root_was_present  INTEGER NOT NULL DEFAULT 1,
	status            TEXT    NOT NULL DEFAULT 'running'
		CHECK (status IN ('running', 'completed', 'failed', 'cancelled')),
	error             TEXT
);
CREATE INDEX idx_scan_runs_root ON scan_runs(root_id, started_at DESC);

-- ===========================================================================
-- Files: filesystem facts and technical audio properties
-- ===========================================================================
CREATE TABLE files (
	id                  INTEGER PRIMARY KEY,
	root_id             INTEGER NOT NULL REFERENCES library_roots(id) ON DELETE CASCADE,
	relative_path       TEXT    NOT NULL,
	relative_directory  TEXT    NOT NULL DEFAULT '',
	file_name           TEXT    NOT NULL DEFAULT '',
	extension           TEXT    NOT NULL DEFAULT '',

	device_id           INTEGER NOT NULL DEFAULT 0,
	inode               INTEGER NOT NULL DEFAULT 0,
	size_bytes          INTEGER NOT NULL DEFAULT 0,
	modified_unix_ms    INTEGER NOT NULL DEFAULT 0,

	codec               TEXT    NOT NULL DEFAULT '',
	duration_ms         INTEGER NOT NULL DEFAULT 0,
	sample_rate_hz      INTEGER NOT NULL DEFAULT 0,
	channels            INTEGER NOT NULL DEFAULT 0,
	bitrate_kbps        INTEGER NOT NULL DEFAULT 0,
	bitrate_mode        TEXT    NOT NULL DEFAULT 'unknown',
	audio_offset        INTEGER NOT NULL DEFAULT 0,
	audio_length        INTEGER NOT NULL DEFAULT 0,
	has_xing_header     INTEGER NOT NULL DEFAULT 0,
	has_lame_header     INTEGER NOT NULL DEFAULT 0,
	encoder_delay       INTEGER NOT NULL DEFAULT 0,
	encoder_padding     INTEGER NOT NULL DEFAULT 0,

	content_sha256      TEXT    NOT NULL DEFAULT '',
	audio_sha256        TEXT    NOT NULL DEFAULT '',

	first_seen_scan     INTEGER REFERENCES scan_runs(id),
	last_seen_scan      INTEGER REFERENCES scan_runs(id),
	read_status         TEXT    NOT NULL DEFAULT 'ok'
		CHECK (read_status IN ('ok', 'unreadable', 'truncated', 'malformed', 'unsupported')),
	read_error          TEXT,

	-- Denormalised display fields, copied from the latest observed snapshot.
	-- The authoritative values remain in tag_frames; these exist so the track
	-- table can page 70,000 rows without joining four tables per row (UI-001).
	display_title           TEXT    NOT NULL DEFAULT '',
	display_artist          TEXT    NOT NULL DEFAULT '',
	display_album_artist    TEXT    NOT NULL DEFAULT '',
	display_album           TEXT    NOT NULL DEFAULT '',
	display_genre           TEXT    NOT NULL DEFAULT '',
	display_date            TEXT    NOT NULL DEFAULT '',
	display_track           INTEGER,
	display_disc            INTEGER,
	display_bpm             REAL,
	has_artwork             INTEGER NOT NULL DEFAULT 0,
	artwork_width           INTEGER NOT NULL DEFAULT 0,
	artwork_height          INTEGER NOT NULL DEFAULT 0,
	has_lyrics              INTEGER NOT NULL DEFAULT 0,
	has_gain_fields         INTEGER NOT NULL DEFAULT 0,
	has_privacy_findings    INTEGER NOT NULL DEFAULT 0,
	primary_container       TEXT    NOT NULL DEFAULT '',

	UNIQUE (root_id, relative_path)
);
CREATE INDEX idx_files_directory ON files(root_id, relative_directory);
CREATE INDEX idx_files_content_hash ON files(content_sha256);
CREATE INDEX idx_files_audio_hash ON files(audio_sha256);
CREATE INDEX idx_files_identity ON files(device_id, inode);
CREATE INDEX idx_files_read_status ON files(read_status) WHERE read_status <> 'ok';
-- Indexes backing the track table's sort order and its filter chips (FN-UI-02).
CREATE INDEX idx_files_browse ON files(display_album_artist, display_album, display_disc, display_track);
CREATE INDEX idx_files_artwork ON files(has_artwork);
CREATE INDEX idx_files_lyrics ON files(has_lyrics);
CREATE INDEX idx_files_gain ON files(has_gain_fields);
CREATE INDEX idx_files_privacy ON files(has_privacy_findings);

-- ===========================================================================
-- Observed tag state. Every discovered field is retained, including fields
-- this application cannot interpret (CAT-001).
-- ===========================================================================
CREATE TABLE tag_snapshots (
	id                    INTEGER PRIMARY KEY,
	file_id               INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
	captured_at           TEXT    NOT NULL,
	-- 'observed' rows describe what is on disk. 'written' rows describe what a
	-- completed write produced, captured by reopening the output.
	kind                  TEXT    NOT NULL DEFAULT 'observed'
		CHECK (kind IN ('observed', 'written')),
	primary_container     TEXT    NOT NULL DEFAULT 'unknown',
	containers            TEXT    NOT NULL DEFAULT '',
	id3v2_tag_bytes       INTEGER NOT NULL DEFAULT 0,
	id3v2_padding_bytes   INTEGER NOT NULL DEFAULT 0,
	ape_tag_bytes         INTEGER NOT NULL DEFAULT 0,
	has_id3v1             INTEGER NOT NULL DEFAULT 0,
	unsynchronised        INTEGER NOT NULL DEFAULT 0,
	extended_header       INTEGER NOT NULL DEFAULT 0,
	content_sha256        TEXT    NOT NULL DEFAULT '',
	audio_sha256          TEXT    NOT NULL DEFAULT '',
	read_warnings         TEXT    NOT NULL DEFAULT ''
);
CREATE INDEX idx_tag_snapshots_file ON tag_snapshots(file_id, kind, captured_at DESC);

CREATE TABLE tag_frames (
	id                INTEGER PRIMARY KEY,
	snapshot_id       INTEGER NOT NULL REFERENCES tag_snapshots(id) ON DELETE CASCADE,
	container         TEXT    NOT NULL DEFAULT 'unknown',
	frame_id          TEXT    NOT NULL,
	owner             TEXT    NOT NULL DEFAULT '',
	description       TEXT    NOT NULL DEFAULT '',
	language          TEXT    NOT NULL DEFAULT '',
	text_value        TEXT,
	-- Exact original payload for binary or uninterpretable frames, so they can
	-- be written back unchanged.
	binary_value      BLOB,
	encoding          TEXT    NOT NULL DEFAULT 'unknown',
	ordinal           INTEGER NOT NULL DEFAULT 0,
	raw_size          INTEGER NOT NULL DEFAULT 0,
	interpreted       INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX idx_tag_frames_snapshot ON tag_frames(snapshot_id);
CREATE INDEX idx_tag_frames_id ON tag_frames(frame_id);

CREATE TABLE tag_pictures (
	id                INTEGER PRIMARY KEY,
	snapshot_id       INTEGER NOT NULL REFERENCES tag_snapshots(id) ON DELETE CASCADE,
	picture_type      TEXT    NOT NULL DEFAULT 'other',
	mime_type         TEXT    NOT NULL DEFAULT '',
	description       TEXT    NOT NULL DEFAULT '',
	byte_length       INTEGER NOT NULL DEFAULT 0,
	width             INTEGER NOT NULL DEFAULT 0,
	height            INTEGER NOT NULL DEFAULT 0,
	content_sha256    TEXT    NOT NULL DEFAULT '',
	frame_ordinal     INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX idx_tag_pictures_snapshot ON tag_pictures(snapshot_id);

-- ===========================================================================
-- Albums
-- ===========================================================================
CREATE TABLE albums (
	id                      INTEGER PRIMARY KEY,
	group_key               TEXT    NOT NULL UNIQUE,
	album                   TEXT    NOT NULL DEFAULT '',
	album_artist            TEXT    NOT NULL DEFAULT '',
	release_date            TEXT    NOT NULL DEFAULT '',
	edition_qualifier       TEXT    NOT NULL DEFAULT '',
	musicbrainz_album_id    TEXT    NOT NULL DEFAULT '',
	musicbrainz_group_id    TEXT    NOT NULL DEFAULT '',
	disc_count              INTEGER NOT NULL DEFAULT 1,
	observed_track_count    INTEGER NOT NULL DEFAULT 0,
	declared_track_total    INTEGER,
	is_compilation          INTEGER NOT NULL DEFAULT 0,
	identity_confidence     TEXT    NOT NULL DEFAULT 'unknown',
	flags                   TEXT    NOT NULL DEFAULT '',
	updated_at              TEXT    NOT NULL
);
CREATE INDEX idx_albums_artist ON albums(album_artist, album);
CREATE INDEX idx_albums_confidence ON albums(identity_confidence);

CREATE TABLE album_tracks (
	album_id      INTEGER NOT NULL REFERENCES albums(id) ON DELETE CASCADE,
	file_id       INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
	disc_number   INTEGER,
	track_number  INTEGER,
	track_artist  TEXT NOT NULL DEFAULT '',
	PRIMARY KEY (album_id, file_id)
);
CREATE INDEX idx_album_tracks_file ON album_tracks(file_id);

CREATE TABLE album_evidence (
	id          INTEGER PRIMARY KEY,
	album_id    INTEGER NOT NULL REFERENCES albums(id) ON DELETE CASCADE,
	kind        TEXT    NOT NULL,
	detail      TEXT    NOT NULL DEFAULT '',
	supporting  INTEGER NOT NULL DEFAULT 1
);
CREATE INDEX idx_album_evidence_album ON album_evidence(album_id);

-- ===========================================================================
-- Proposed metadata. Separate from observed state by construction.
-- ===========================================================================
CREATE TABLE metadata_candidates (
	id              INTEGER PRIMARY KEY,
	file_id         INTEGER REFERENCES files(id) ON DELETE CASCADE,
	album_id        INTEGER REFERENCES albums(id) ON DELETE CASCADE,
	field           TEXT    NOT NULL,
	proposed_value  TEXT    NOT NULL DEFAULT '',
	provider_id     TEXT    NOT NULL DEFAULT '',
	provider_ref    TEXT    NOT NULL DEFAULT '',
	evidence        TEXT    NOT NULL DEFAULT '',
	confidence      TEXT    NOT NULL DEFAULT 'unknown',
	rule_version    TEXT    NOT NULL DEFAULT '',
	selection       TEXT    NOT NULL DEFAULT 'proposed'
		CHECK (selection IN ('proposed', 'accepted', 'rejected', 'locked', 'unresolved')),
	created_at      TEXT    NOT NULL
);
CREATE INDEX idx_candidates_file ON metadata_candidates(file_id, field);
CREATE INDEX idx_candidates_album ON metadata_candidates(album_id, field);

-- ===========================================================================
-- Artwork (ART-001..003)
-- ===========================================================================
CREATE TABLE artwork_assets (
	id                      INTEGER PRIMARY KEY,
	album_id                INTEGER REFERENCES albums(id) ON DELETE CASCADE,
	provider_id             TEXT    NOT NULL DEFAULT '',
	provider_name           TEXT    NOT NULL DEFAULT '',
	page_url                TEXT    NOT NULL DEFAULT '',
	image_url               TEXT    NOT NULL DEFAULT '',
	local_path              TEXT    NOT NULL DEFAULT '',
	content_sha256          TEXT    NOT NULL DEFAULT '',
	mime_type               TEXT    NOT NULL DEFAULT '',
	byte_length             INTEGER NOT NULL DEFAULT 0,

	-- Measured by decoding the received bytes. Distinguished from any claim the
	-- provider or a filename makes about native resolution.
	measured_width          INTEGER NOT NULL DEFAULT 0,
	measured_height         INTEGER NOT NULL DEFAULT 0,
	dimensions_measured     INTEGER NOT NULL DEFAULT 0,
	claimed_width           INTEGER NOT NULL DEFAULT 0,
	claimed_height          INTEGER NOT NULL DEFAULT 0,

	source_type             TEXT    NOT NULL DEFAULT 'unknown',
	cover_match             TEXT    NOT NULL DEFAULT 'unknown',
	match_confidence        TEXT    NOT NULL DEFAULT 'unknown',
	defects                 TEXT    NOT NULL DEFAULT '',
	evidence                TEXT    NOT NULL DEFAULT '',

	selection_state         TEXT    NOT NULL DEFAULT 'candidate'
		CHECK (selection_state IN ('candidate', 'selected', 'rejected', 'locked')),
	selection_reason        TEXT    NOT NULL DEFAULT '',
	rejection_reason        TEXT    NOT NULL DEFAULT '',
	policy_version          TEXT    NOT NULL DEFAULT '',
	retrieved_at            TEXT    NOT NULL DEFAULT ''
);
CREATE INDEX idx_artwork_album ON artwork_assets(album_id, selection_state);
CREATE INDEX idx_artwork_hash ON artwork_assets(content_sha256);

-- The 600x600 derivative. Generated once per (source, encoder configuration)
-- and embedded identically across an album (FN-ART-03).
CREATE TABLE artwork_derivatives (
	id                  INTEGER PRIMARY KEY,
	asset_id            INTEGER NOT NULL REFERENCES artwork_assets(id) ON DELETE CASCADE,
	config_hash         TEXT    NOT NULL,
	local_path          TEXT    NOT NULL DEFAULT '',
	content_sha256      TEXT    NOT NULL DEFAULT '',
	width               INTEGER NOT NULL DEFAULT 0,
	height              INTEGER NOT NULL DEFAULT 0,
	byte_length         INTEGER NOT NULL DEFAULT 0,
	jpeg_quality        INTEGER NOT NULL DEFAULT 0,
	subsampling         TEXT    NOT NULL DEFAULT '',
	created_at          TEXT    NOT NULL,
	UNIQUE (asset_id, config_hash)
);

-- ===========================================================================
-- Tempo and lyrics
-- ===========================================================================
CREATE TABLE tempo_results (
	id                  INTEGER PRIMARY KEY,
	file_id             INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
	raw_bpm             REAL    NOT NULL DEFAULT 0,
	half_tempo          REAL    NOT NULL DEFAULT 0,
	double_tempo        REAL    NOT NULL DEFAULT 0,
	stability           REAL    NOT NULL DEFAULT 0,
	sections_analysed   INTEGER NOT NULL DEFAULT 0,
	section_bpms        TEXT    NOT NULL DEFAULT '',
	character           TEXT    NOT NULL DEFAULT 'unknown',
	engine              TEXT    NOT NULL DEFAULT '',
	settings_hash       TEXT    NOT NULL DEFAULT '',
	decision            TEXT    NOT NULL DEFAULT 'needs_review',
	decided_bpm         REAL    NOT NULL DEFAULT 0,
	tagged_bpm          INTEGER NOT NULL DEFAULT 0,
	existing_bpm        REAL,
	confidence          TEXT    NOT NULL DEFAULT 'unknown',
	reason              TEXT    NOT NULL DEFAULT '',
	analysed_at         TEXT    NOT NULL,
	UNIQUE (file_id, engine, settings_hash)
);
CREATE INDEX idx_tempo_file ON tempo_results(file_id);
CREATE INDEX idx_tempo_decision ON tempo_results(decision);

CREATE TABLE lyrics_results (
	id                  INTEGER PRIMARY KEY,
	file_id             INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
	state               TEXT    NOT NULL DEFAULT 'not_attempted',
	lyrics_text         TEXT,
	synced_text         TEXT,
	language            TEXT    NOT NULL DEFAULT '',
	provider_id         TEXT    NOT NULL DEFAULT '',
	source_url          TEXT    NOT NULL DEFAULT '',
	match_evidence      TEXT    NOT NULL DEFAULT '',
	confidence          TEXT    NOT NULL DEFAULT 'unknown',
	reason              TEXT    NOT NULL DEFAULT '',
	retrieved_at        TEXT    NOT NULL DEFAULT '',
	-- Misses are cached for a configurable period and retried separately from
	-- service errors (FRD section 7).
	retry_after         TEXT
);
CREATE INDEX idx_lyrics_file ON lyrics_results(file_id);
CREATE INDEX idx_lyrics_state ON lyrics_results(state);

-- ===========================================================================
-- Durable jobs (JOB-001)
-- ===========================================================================
CREATE TABLE jobs (
	id                  INTEGER PRIMARY KEY,
	stage               TEXT    NOT NULL,
	entity_kind         TEXT    NOT NULL CHECK (entity_kind IN ('file', 'album', 'change_set')),
	entity_id           INTEGER NOT NULL,
	-- Identity: an unchanged completed plan must not repeat work.
	input_hash          TEXT    NOT NULL DEFAULT '',
	config_hash         TEXT    NOT NULL DEFAULT '',
	engine_version      TEXT    NOT NULL DEFAULT '',

	state               TEXT    NOT NULL DEFAULT 'pending'
		CHECK (state IN ('pending', 'leased', 'succeeded', 'failed', 'cancelled', 'blocked')),
	priority            INTEGER NOT NULL DEFAULT 0,
	attempts            INTEGER NOT NULL DEFAULT 0,
	max_attempts        INTEGER NOT NULL DEFAULT 5,

	lease_owner         TEXT,
	lease_expires_at    TEXT,
	retry_after         TEXT,

	created_at          TEXT    NOT NULL,
	updated_at          TEXT    NOT NULL,
	last_error          TEXT,

	UNIQUE (stage, entity_kind, entity_id, input_hash, config_hash, engine_version)
);
CREATE INDEX idx_jobs_ready ON jobs(state, stage, priority DESC, retry_after);
CREATE INDEX idx_jobs_lease ON jobs(state, lease_expires_at) WHERE state = 'leased';
CREATE INDEX idx_jobs_entity ON jobs(entity_kind, entity_id);

CREATE TABLE job_attempts (
	id            INTEGER PRIMARY KEY,
	job_id        INTEGER NOT NULL REFERENCES jobs(id) ON DELETE CASCADE,
	attempt       INTEGER NOT NULL,
	started_at    TEXT    NOT NULL,
	finished_at   TEXT,
	outcome       TEXT    NOT NULL DEFAULT 'running',
	duration_ms   INTEGER NOT NULL DEFAULT 0,
	error         TEXT
);
CREATE INDEX idx_job_attempts_job ON job_attempts(job_id, attempt);

-- ===========================================================================
-- Change sets and the write journal (FN-SAFE-02, FN-SAFE-03)
-- ===========================================================================
CREATE TABLE change_sets (
	id               INTEGER PRIMARY KEY,
	output_root_id   INTEGER REFERENCES library_roots(id),
	output_root_path TEXT    NOT NULL DEFAULT '',
	created_at       TEXT    NOT NULL,
	config_hash      TEXT    NOT NULL DEFAULT '',
	state            TEXT    NOT NULL DEFAULT 'planned'
		CHECK (state IN ('planned', 'executing', 'completed', 'failed', 'cancelled')),
	note             TEXT    NOT NULL DEFAULT ''
);

CREATE TABLE file_operations (
	id                      INTEGER PRIMARY KEY,
	change_set_id           INTEGER NOT NULL REFERENCES change_sets(id) ON DELETE CASCADE,
	file_id                 INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,

	source_path             TEXT    NOT NULL,
	expected_source_sha256  TEXT    NOT NULL DEFAULT '',
	expected_audio_sha256   TEXT    NOT NULL DEFAULT '',
	source_device_id        INTEGER NOT NULL DEFAULT 0,
	source_inode            INTEGER NOT NULL DEFAULT 0,
	source_size_bytes       INTEGER NOT NULL DEFAULT 0,
	source_modified_unix_ms INTEGER NOT NULL DEFAULT 0,

	destination_relative    TEXT    NOT NULL,
	destination_path        TEXT    NOT NULL DEFAULT '',
	temp_path               TEXT    NOT NULL DEFAULT '',

	plan_json               TEXT    NOT NULL DEFAULT '',
	blockers                TEXT    NOT NULL DEFAULT '',

	-- The recovery state machine. Each transition is committed before the
	-- corresponding filesystem action, so a crash is always recoverable.
	state                   TEXT    NOT NULL DEFAULT 'planned'
		CHECK (state IN ('planned', 'reserved', 'copying', 'copied', 'applying',
						 'applied', 'verifying', 'verified', 'published', 'committed',
						 'failed', 'skipped', 'rolled_back')),

	written_sha256          TEXT    NOT NULL DEFAULT '',
	written_audio_sha256    TEXT    NOT NULL DEFAULT '',
	bytes_before            INTEGER NOT NULL DEFAULT 0,
	bytes_after             INTEGER NOT NULL DEFAULT 0,
	optimisation_savings    INTEGER NOT NULL DEFAULT 0,
	enrichment_growth       INTEGER NOT NULL DEFAULT 0,

	started_at              TEXT,
	finished_at             TEXT,
	error                   TEXT,

	UNIQUE (change_set_id, file_id)
);
CREATE INDEX idx_file_operations_state ON file_operations(change_set_id, state);
CREATE INDEX idx_file_operations_file ON file_operations(file_id);

-- Destination reservation. A UNIQUE constraint is what actually prevents two
-- files being planned onto one path (NAME-002); collision detection in the
-- planner is the friendly early warning, this is the guarantee.
CREATE TABLE output_reservations (
	id                   INTEGER PRIMARY KEY,
	change_set_id        INTEGER NOT NULL REFERENCES change_sets(id) ON DELETE CASCADE,
	destination_folded   TEXT    NOT NULL,
	destination_relative TEXT    NOT NULL,
	file_id              INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
	UNIQUE (change_set_id, destination_folded)
);

-- ===========================================================================
-- Manual decisions. These survive rescans and provider refreshes.
-- ===========================================================================
CREATE TABLE manual_decisions (
	id             INTEGER PRIMARY KEY,
	scope          TEXT    NOT NULL CHECK (scope IN ('file', 'album')),
	scope_id       INTEGER NOT NULL,
	field          TEXT    NOT NULL,
	decision       TEXT    NOT NULL CHECK (decision IN ('lock', 'reject', 'accept', 'unresolved')),
	value          TEXT    NOT NULL DEFAULT '',
	reference_id   INTEGER,
	note           TEXT    NOT NULL DEFAULT '',
	decided_at     TEXT    NOT NULL,
	UNIQUE (scope, scope_id, field, reference_id)
);
CREATE INDEX idx_manual_decisions_scope ON manual_decisions(scope, scope_id);

-- ===========================================================================
-- Provider request accounting, for rate limiting across restarts (FN-JOB-02)
-- ===========================================================================
CREATE TABLE provider_requests (
	id            INTEGER PRIMARY KEY,
	provider_id   TEXT    NOT NULL,
	requested_at  TEXT    NOT NULL,
	status_code   INTEGER NOT NULL DEFAULT 0,
	retry_after   TEXT,
	note          TEXT    NOT NULL DEFAULT ''
);
CREATE INDEX idx_provider_requests ON provider_requests(provider_id, requested_at DESC);

-- ===========================================================================
-- Settings
-- ===========================================================================
CREATE TABLE settings (
	key     TEXT PRIMARY KEY,
	value   TEXT NOT NULL DEFAULT ''
);

-- Full-text search over the fields the track table filters on.
--
-- A regular (not contentless) FTS5 table: the index is rebuilt per file on
-- rescan, and a contentless table cannot be updated in place.
CREATE VIRTUAL TABLE file_search USING fts5(
	title, artist, album_artist, album, relative_path,
	tokenize = 'unicode61 remove_diacritics 2'
);
)SQL";

/// Migration 002.
///
/// Whether a group needs human review is a policy decision made in
/// AlbumResolverCore, not a property of its flag string: some flags are
/// advisory. Recomputing that policy in SQL would duplicate it and let the two
/// drift, so the decision is stored alongside the group and indexed.
constexpr std::string_view kMigration002 = R"SQL(
ALTER TABLE albums ADD COLUMN needs_review INTEGER NOT NULL DEFAULT 1;
CREATE INDEX idx_albums_needs_review ON albums(needs_review);
)SQL";

} // namespace

const std::vector<SchemaMigrator::Migration>& SchemaMigrator::migrations() {
	static const std::vector<Migration> kMigrations = {
		Migration{1, "initial catalogue schema", kMigration001},
		Migration{2, "store the album review decision rather than deriving it in SQL", kMigration002},
	};
	return kMigrations;
}

Result<int> SchemaMigrator::currentVersion(Database& db) {
	auto statement = db.prepare("PRAGMA user_version;");
	if (!statement) return statement.error();
	auto row = statement.value().step();
	if (!row) return row.error();
	if (!row.value()) return 0;
	return statement.value().columnInt(0);
}

Status SchemaMigrator::migrate(Database& db) {
	auto version = currentVersion(db);
	if (!version) return Status(version.error());

	if (version.value() > kCurrentVersion) {
		// Refuse safely rather than operating on a schema written by a newer
		// build (FRD section 16).
		return Status(Error{ErrorCode::Conflict,
			"catalogue schema version " + std::to_string(version.value())
				+ " is newer than this build understands (" + std::to_string(kCurrentVersion)
				+ "); upgrade the application rather than downgrading the catalogue"});
	}

	if (version.value() == kCurrentVersion) return Status::success();

	for (const auto& migration : migrations()) {
		if (migration.version <= version.value()) continue;

		auto transaction = db.begin(Transaction::Kind::Immediate);
		if (!transaction) return Status(transaction.error());

		if (auto status = db.executeScript(migration.sql); !status) {
			return Status(Error{ErrorCode::DatabaseError,
				"migration " + std::to_string(migration.version) + " (" + std::string(migration.name)
					+ ") failed: " + status.error().message});
		}

		// PRAGMA user_version does not accept a bound parameter.
		const std::string setVersion = "PRAGMA user_version = " + std::to_string(migration.version) + ";";
		if (auto status = db.executeScript(setVersion); !status) return status;

		if (auto status = transaction.value().commit(); !status) return status;
	}

	return Status::success();
}

} // namespace ml
