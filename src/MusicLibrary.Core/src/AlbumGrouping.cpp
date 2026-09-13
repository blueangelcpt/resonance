// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlcore/AlbumGrouping.hpp"
#include "mlcore/Text.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace ml {

std::string_view toString(AlbumFlag f) {
	switch (f) {
		case AlbumFlag::MissingTrackNumbers: return "missing_track_numbers";
		case AlbumFlag::DuplicateTrackNumbers: return "duplicate_track_numbers";
		case AlbumFlag::IncompleteTrackRun: return "incomplete_track_run";
		case AlbumFlag::ConflictingAlbumArtist: return "conflicting_album_artist";
		case AlbumFlag::ConflictingDate: return "conflicting_date";
		case AlbumFlag::ConflictingReleaseId: return "conflicting_release_id";
		case AlbumFlag::MixedDiscNumbering: return "mixed_disc_numbering";
		case AlbumFlag::SingleTrackGroup: return "single_track_group";
		case AlbumFlag::LikelyCompilation: return "likely_compilation";
		case AlbumFlag::FolderTitleMismatch: return "folder_title_mismatch";
		case AlbumFlag::NoAlbumTag: return "no_album_tag";
	}
	return "missing_track_numbers";
}

AlbumResolverCore::AlbumResolverCore(GroupingOptions options) : m_options(std::move(options)) {}

bool AlbumResolverCore::sameEdition(std::string_view titleA, std::string_view titleB) const {
	const auto a = text::stripQualifiers(titleA);
	const auto b = text::stripQualifiers(titleB);

	if (text::similarity(a.base, b.base) < m_options.titleSimilarityThreshold) return false;

	// Same base title. Now the qualifiers decide. Two releases whose titles differ
	// only by "(Deluxe)" are different editions and must not be merged.
	const auto editionMarkers = [](const std::vector<std::string>& qualifiers) {
		std::set<std::string> out;
		for (const auto& q : qualifiers) {
			const std::string lower = text::normaliseForMatching(q);
			if (text::isEditionQualifier(lower)) out.insert(lower);
		}
		return out;
	};

	return editionMarkers(a.qualifiers) == editionMarkers(b.qualifiers);
}

std::string AlbumResolverCore::groupKeyFor(const GroupingInput& in) const {
	// A confident release identifier is the strongest available key.
	if (!in.musicBrainzAlbumId.empty()) {
		return "mbid:" + in.musicBrainzAlbumId;
	}

	// Otherwise: directory plus normalised album title plus album artist.
	//
	// The directory is included because a library organised by the user's
	// template puts exactly one album per directory, and because it keeps two
	// different rips of the same album in separate folders apart until provider
	// evidence says otherwise.
	//
	// The album ARTIST is part of the key; the track artist deliberately is not.
	// Using track artist would split a compilation into one group per performer.
	std::string key;
	if (m_options.useDirectoryAsPrimaryKey) {
		key += "dir:";
		key += text::foldCase(in.relativeDirectory);
		key += '|';
	}
	key += "album:";
	key += text::normaliseForMatching(in.album);
	key += '|';
	key += "albumartist:";
	key += text::normaliseForMatching(in.albumArtist);

	// Edition qualifiers stay in the key so a deluxe edition sharing a folder with
	// the standard edition does not merge into it.
	const auto stripped = text::stripQualifiers(in.album);
	for (const auto& q : stripped.qualifiers) {
		const std::string norm = text::normaliseForMatching(q);
		if (text::isEditionQualifier(norm)) {
			key += "|edition:";
			key += norm;
		}
	}
	return key;
}

void AlbumResolverCore::analyseGroup(ProvisionalAlbum& album, const std::vector<const GroupingInput*>& members) const {
	std::set<std::string> albumArtists;
	std::set<std::string> trackArtists;
	std::set<std::string> dates;
	std::set<std::string> releaseIds;
	std::set<int> discs;
	std::map<int, int> trackCounts;   // (disc << 16 | track) -> count
	int missingTrackNumbers = 0;
	bool anyCompilationFlag = false;
	std::optional<int> declaredTotal;
	int maxTrack = 0;

	for (const GroupingInput* in : members) {
		if (!in->albumArtist.empty()) albumArtists.insert(text::normaliseForMatching(in->albumArtist));
		if (!in->artist.empty()) trackArtists.insert(text::normaliseForMatching(in->artist));
		if (!in->date.empty()) {
			// Compare on the year only: "1992" and "1992-06-01" are not a conflict.
			dates.insert(in->date.substr(0, 4));
		}
		if (!in->musicBrainzAlbumId.empty()) releaseIds.insert(in->musicBrainzAlbumId);
		const int disc = in->discNumber.value_or(1);
		discs.insert(disc);
		if (in->trackNumber) {
			const int composite = (disc << 16) | *in->trackNumber;
			++trackCounts[composite];
			maxTrack = std::max(maxTrack, *in->trackNumber);
		} else {
			++missingTrackNumbers;
		}
		if (in->compilationFlag) anyCompilationFlag = true;
		if (in->trackTotal && (!declaredTotal || *in->trackTotal > *declaredTotal)) declaredTotal = in->trackTotal;
	}

	album.observedTrackCount = static_cast<int>(members.size());
	album.declaredTrackTotal = declaredTotal;
	album.discCount = static_cast<int>(discs.size());

	// --- Flags -------------------------------------------------------------
	if (album.album.empty()) {
		album.flags.push_back(AlbumFlag::NoAlbumTag);
	}
	if (missingTrackNumbers > 0) {
		album.flags.push_back(AlbumFlag::MissingTrackNumbers);
		album.evidence.push_back({"missing_track_numbers",
			std::to_string(missingTrackNumbers) + " of " + std::to_string(members.size())
				+ " files have no numeric track number", false});
	}
	for (const auto& [composite, count] : trackCounts) {
		if (count > 1) {
			const int disc = composite >> 16;
			const int track = composite & 0xFFFF;
			album.flags.push_back(AlbumFlag::DuplicateTrackNumbers);
			album.evidence.push_back({"duplicate_track_number",
				"disc " + std::to_string(disc) + " track " + std::to_string(track) + " appears "
					+ std::to_string(count) + " times", false});
			break;
		}
	}
	if (m_options.flagIncompleteRuns && declaredTotal && album.discCount == 1
		&& album.observedTrackCount < *declaredTotal) {
		album.flags.push_back(AlbumFlag::IncompleteTrackRun);
		album.evidence.push_back({"incomplete_run",
			std::to_string(album.observedTrackCount) + " tracks present, tags declare "
				+ std::to_string(*declaredTotal), false});
	}
	if (albumArtists.size() > 1) {
		album.flags.push_back(AlbumFlag::ConflictingAlbumArtist);
		album.evidence.push_back({"conflicting_album_artist",
			std::to_string(albumArtists.size()) + " distinct album-artist values in one group", false});
	}
	if (dates.size() > 1) {
		album.flags.push_back(AlbumFlag::ConflictingDate);
		album.evidence.push_back({"conflicting_date",
			std::to_string(dates.size()) + " distinct years in one group", false});
	}
	if (releaseIds.size() > 1) {
		album.flags.push_back(AlbumFlag::ConflictingReleaseId);
		album.evidence.push_back({"conflicting_release_id",
			"files claim " + std::to_string(releaseIds.size()) + " different releases", false});
	}
	if (album.discCount > 1) {
		// Multidisc is normal, but it changes the naming plan, so it is surfaced.
		bool anyMissingDisc = false;
		for (const GroupingInput* in : members) {
			if (!in->discNumber) anyMissingDisc = true;
		}
		if (anyMissingDisc) {
			album.flags.push_back(AlbumFlag::MixedDiscNumbering);
			album.evidence.push_back({"mixed_disc_numbering",
				"some files declare a disc number and some do not", false});
		}
	}
	if (members.size() == 1) {
		album.flags.push_back(AlbumFlag::SingleTrackGroup);
	}

	// Compilation detection: an explicit flag, or many distinct track artists
	// under one album artist. Both are evidence, not proof.
	if (anyCompilationFlag) {
		album.isCompilation = true;
		album.evidence.push_back({"compilation_flag", "TCMP or a compilation tag is set", true});
	} else if (trackArtists.size() > 2 && trackArtists.size() * 2 >= members.size()) {
		album.isCompilation = true;
		album.flags.push_back(AlbumFlag::LikelyCompilation);
		album.evidence.push_back({"many_track_artists",
			std::to_string(trackArtists.size()) + " distinct track artists across "
				+ std::to_string(members.size()) + " tracks", true});
	}

	// Folder name against album title: a mismatch is worth surfacing because the
	// user's library is organised by album folder.
	if (!album.album.empty() && !members.empty()) {
		const std::string& dir = members.front()->relativeDirectory;
		const std::size_t slash = dir.find_last_of('/');
		const std::string leaf = (slash == std::string::npos) ? dir : dir.substr(slash + 1);
		if (!leaf.empty() && text::similarity(leaf, album.album) < 0.6) {
			album.flags.push_back(AlbumFlag::FolderTitleMismatch);
			album.evidence.push_back({"folder_title_mismatch",
				"folder \"" + leaf + "\" does not resemble album tag \"" + album.album + "\"", false});
		}
	}

	// --- Identity confidence ------------------------------------------------
	// A confident release identifier plus a consistent group is strong. Anything
	// with a conflict flag stays at Weak so it surfaces for review.
	const bool hasConflict = std::any_of(album.flags.begin(), album.flags.end(), [](AlbumFlag f) {
		return f == AlbumFlag::ConflictingAlbumArtist || f == AlbumFlag::ConflictingDate
			|| f == AlbumFlag::ConflictingReleaseId || f == AlbumFlag::DuplicateTrackNumbers
			|| f == AlbumFlag::NoAlbumTag;
	});

	if (hasConflict) {
		album.identityConfidence = Confidence::Weak;
	} else if (!album.musicBrainzAlbumId.empty()) {
		album.identityConfidence = Confidence::Strong;
		album.evidence.push_back({"release_id", "all files agree on one MusicBrainz release id", true});
	} else if (!album.album.empty() && !album.albumArtist.empty() && missingTrackNumbers == 0
		&& members.size() > 1) {
		album.identityConfidence = Confidence::Moderate;
		album.evidence.push_back({"consistent_tags",
			"album, album artist and a complete track numbering agree across the group", true});
	} else {
		album.identityConfidence = Confidence::Weak;
	}
}

std::vector<ProvisionalAlbum> AlbumResolverCore::group(const std::vector<GroupingInput>& inputs) const {
	// Preserve first-seen ordering so results are reproducible across runs.
	std::vector<std::string> keyOrder;
	std::map<std::string, std::vector<const GroupingInput*>> buckets;

	for (const auto& in : inputs) {
		const std::string key = groupKeyFor(in);
		auto it = buckets.find(key);
		if (it == buckets.end()) {
			keyOrder.push_back(key);
			buckets.emplace(key, std::vector<const GroupingInput*>{&in});
		} else {
			it->second.push_back(&in);
		}
	}

	std::vector<ProvisionalAlbum> albums;
	albums.reserve(keyOrder.size());

	for (const auto& key : keyOrder) {
		const auto& members = buckets[key];

		ProvisionalAlbum album;
		album.groupKey = key;

		// Representative values: the most common non-empty value wins. A tie is
		// resolved by first appearance, and the conflict is flagged separately.
		const auto mostCommon = [&](auto accessor) {
			std::map<std::string, int> counts;
			std::vector<std::string> order;
			for (const GroupingInput* in : members) {
				const std::string v = accessor(*in);
				if (v.empty()) continue;
				if (counts.find(v) == counts.end()) order.push_back(v);
				++counts[v];
			}
			std::string best;
			int bestCount = 0;
			for (const auto& v : order) {
				if (counts[v] > bestCount) {
					bestCount = counts[v];
					best = v;
				}
			}
			return best;
		};

		album.album = mostCommon([](const GroupingInput& i) { return i.album; });
		album.albumArtist = mostCommon([](const GroupingInput& i) { return i.albumArtist; });
		album.date = mostCommon([](const GroupingInput& i) { return i.date; });
		album.musicBrainzAlbumId = mostCommon([](const GroupingInput& i) { return i.musicBrainzAlbumId; });

		const auto stripped = text::stripQualifiers(album.album);
		for (const auto& q : stripped.qualifiers) {
			if (text::isEditionQualifier(text::normaliseForMatching(q))) {
				if (!album.editionQualifier.empty()) album.editionQualifier += "; ";
				album.editionQualifier += q;
			}
		}

		for (const GroupingInput* in : members) album.files.push_back(in->fileId);

		analyseGroup(album, members);
		albums.push_back(std::move(album));
	}

	return albums;
}

} // namespace ml
