// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlcore/ChangePlan.hpp"
#include "mlcore/Text.hpp"

#include <sstream>

namespace ml {

std::string_view toString(PlanBlocker b) {
	switch (b) {
		case PlanBlocker::None: return "none";
		case PlanBlocker::NamingReviewRequired: return "naming_review_required";
		case PlanBlocker::DestinationCollision: return "destination_collision";
		case PlanBlocker::ArtworkReviewRequired: return "artwork_review_required";
		case PlanBlocker::PrivacyReviewRequired: return "privacy_review_required";
		case PlanBlocker::GainExceptionRequired: return "gain_exception_required";
		case PlanBlocker::TempoReviewRequired: return "tempo_review_required";
		case PlanBlocker::LyricsReviewRequired: return "lyrics_review_required";
		case PlanBlocker::SourceUnreadable: return "source_unreadable";
		case PlanBlocker::SourceChangedSincePlan: return "source_changed_since_plan";
		case PlanBlocker::DestinationInsideProtectedRoot: return "destination_inside_protected_root";
	}
	return "none";
}

std::vector<const FrameDecision*> FilePlan::changingDecisions() const {
	std::vector<const FrameDecision*> out;
	for (const auto& d : tagDecisions) {
		if (d.action != FrameAction::Keep) out.push_back(&d);
	}
	return out;
}

ChangeSet::Summary ChangeSet::summarise() const {
	Summary s;
	s.total = files.size();
	for (const auto& f : files) {
		if (f.writable()) ++s.writable;
		else ++s.blocked;

		bool review = false;
		for (auto b : f.blockers) {
			switch (b) {
				case PlanBlocker::NamingReviewRequired:
				case PlanBlocker::ArtworkReviewRequired:
				case PlanBlocker::PrivacyReviewRequired:
				case PlanBlocker::GainExceptionRequired:
				case PlanBlocker::TempoReviewRequired:
				case PlanBlocker::LyricsReviewRequired:
					review = true;
					break;
				default:
					break;
			}
		}
		if (review) ++s.needingReview;

		s.optimisationSavings += f.size.optimisationSavings;
		s.enrichmentGrowth += f.size.enrichmentGrowth;

		if (f.artwork.replaceFrontCover) ++s.artworkReplacements;
		if (f.lyrics.writesTag()) ++s.lyricsAdded;
		if (f.tempo.writesTag()) ++s.bpmProposed;

		for (const auto& d : f.tagDecisions) {
			if (d.action != FrameAction::Remove) continue;
			if (d.ruleId.rfind("priv.", 0) == 0 || d.ruleId.rfind("ufid.", 0) == 0
				|| d.ruleId.rfind("comm.", 0) == 0) {
				++s.privacyRemovals;
			} else if (d.ruleId.rfind("gain.", 0) == 0) {
				++s.gainRemovals;
			}
		}
	}
	return s;
}

namespace {

void appendJsonString(std::ostringstream& out, std::string_view key, std::string_view value, bool& first) {
	if (!first) out << ",";
	first = false;
	out << "\"" << text::jsonEscape(key) << "\":\"" << text::jsonEscape(value) << "\"";
}

void appendJsonNumber(std::ostringstream& out, std::string_view key, long long value, bool& first) {
	if (!first) out << ",";
	first = false;
	out << "\"" << text::jsonEscape(key) << "\":" << value;
}

void appendJsonBool(std::ostringstream& out, std::string_view key, bool value, bool& first) {
	if (!first) out << ",";
	first = false;
	out << "\"" << text::jsonEscape(key) << "\":" << (value ? "true" : "false");
}

} // namespace

std::string toJson(const ChangeSet& set, bool includeUnchanged) {
	std::ostringstream out;
	const auto summary = set.summarise();

	out << "{";
	bool first = true;
	appendJsonString(out, "output_root", set.outputRootPath, first);
	appendJsonString(out, "created_at", set.createdAtIso8601, first);
	appendJsonString(out, "config_hash", set.configHash, first);

	out << ",\"summary\":{";
	bool sf = true;
	appendJsonNumber(out, "total", static_cast<long long>(summary.total), sf);
	appendJsonNumber(out, "writable", static_cast<long long>(summary.writable), sf);
	appendJsonNumber(out, "blocked", static_cast<long long>(summary.blocked), sf);
	appendJsonNumber(out, "needing_review", static_cast<long long>(summary.needingReview), sf);
	appendJsonNumber(out, "optimisation_savings_bytes", static_cast<long long>(summary.optimisationSavings), sf);
	appendJsonNumber(out, "enrichment_growth_bytes", static_cast<long long>(summary.enrichmentGrowth), sf);
	appendJsonNumber(out, "artwork_replacements", static_cast<long long>(summary.artworkReplacements), sf);
	appendJsonNumber(out, "lyrics_added", static_cast<long long>(summary.lyricsAdded), sf);
	appendJsonNumber(out, "bpm_proposed", static_cast<long long>(summary.bpmProposed), sf);
	appendJsonNumber(out, "privacy_removals", static_cast<long long>(summary.privacyRemovals), sf);
	appendJsonNumber(out, "gain_removals", static_cast<long long>(summary.gainRemovals), sf);
	out << "}";

	out << ",\"collisions\":[";
	for (std::size_t i = 0; i < set.collisions.size(); ++i) {
		if (i > 0) out << ",";
		out << "{\"path\":\"" << text::jsonEscape(set.collisions[i].relativePath) << "\","
			<< "\"case_only\":" << (set.collisions[i].caseOnly ? "true" : "false") << ","
			<< "\"file_count\":" << set.collisions[i].files.size() << "}";
	}
	out << "]";

	out << ",\"files\":[";
	bool firstFile = true;
	for (const auto& f : set.files) {
		const auto changing = f.changingDecisions();
		if (!includeUnchanged && changing.empty() && !f.artwork.replaceFrontCover && !f.lyrics.writesTag()
			&& !f.tempo.writesTag() && f.blockers.empty()) {
			continue;
		}
		if (!firstFile) out << ",";
		firstFile = false;

		out << "{";
		bool ff = true;
		appendJsonNumber(out, "file_id", f.fileId.value, ff);
		appendJsonString(out, "source_path", f.sourcePath, ff);
		appendJsonString(out, "source_sha256", f.sourceSha256, ff);
		appendJsonString(out, "destination_relative_path", f.destinationRelativePath, ff);
		appendJsonBool(out, "writable", f.writable(), ff);
		appendJsonString(out, "output_container", toString(f.outputContainer), ff);
		appendJsonNumber(out, "optimisation_savings_bytes", static_cast<long long>(f.size.optimisationSavings), ff);
		appendJsonNumber(out, "enrichment_growth_bytes", static_cast<long long>(f.size.enrichmentGrowth), ff);

		out << ",\"blockers\":[";
		for (std::size_t i = 0; i < f.blockers.size(); ++i) {
			if (i > 0) out << ",";
			out << "\"" << toString(f.blockers[i]) << "\"";
		}
		out << "]";

		out << ",\"naming_exceptions\":[";
		for (std::size_t i = 0; i < f.naming.exceptions.size(); ++i) {
			if (i > 0) out << ",";
			out << "{\"kind\":\"" << toString(f.naming.exceptions[i].kind) << "\","
				<< "\"advisory\":" << (f.naming.exceptions[i].advisory ? "true" : "false") << ","
				<< "\"detail\":\"" << text::jsonEscape(f.naming.exceptions[i].detail) << "\"}";
		}
		out << "]";

		out << ",\"tag_changes\":[";
		for (std::size_t i = 0; i < changing.size(); ++i) {
			const FrameDecision& d = *changing[i];
			if (i > 0) out << ",";
			out << "{\"action\":\"" << toString(d.action) << "\","
				<< "\"frame\":\"" << text::jsonEscape(d.frameId) << "\","
				<< "\"container\":\"" << toString(d.container) << "\","
				<< "\"description\":\"" << text::jsonEscape(d.description) << "\","
				<< "\"rule\":\"" << text::jsonEscape(d.ruleId) << "\","
				<< "\"reason\":\"" << text::jsonEscape(d.reason) << "\","
				<< "\"sensitive\":" << (d.sensitive ? "true" : "false") << ","
				<< "\"byte_delta\":" << d.byteDelta();
			// Sensitive values are redacted from routine reports (FRD section 8).
			if (!d.sensitive) {
				out << ",\"before\":\"" << text::jsonEscape(d.beforeValue) << "\"";
				out << ",\"after\":\"" << text::jsonEscape(d.afterValue) << "\"";
			}
			out << "}";
		}
		out << "]";

		out << ",\"artwork\":{"
			<< "\"replace_front_cover\":" << (f.artwork.replaceFrontCover ? "true" : "false") << ","
			<< "\"outcome\":\"" << toString(f.artwork.outcome) << "\","
			<< "\"reason\":\"" << text::jsonEscape(f.artwork.selectionReason) << "\","
			<< "\"derivative_sha256\":\"" << text::jsonEscape(f.artwork.derivativeSha256) << "\","
			<< "\"derivative_width\":" << f.artwork.derivativeWidth << ","
			<< "\"derivative_height\":" << f.artwork.derivativeHeight << ","
			<< "\"policy_version\":\"" << text::jsonEscape(f.artwork.policyVersion) << "\"}";

		out << ",\"tempo\":{"
			<< "\"decision\":\"" << toString(f.tempo.kind) << "\","
			<< "\"precise_bpm\":" << f.tempo.preciseBpm << ","
			<< "\"tagged_bpm\":" << f.tempo.taggedBpm << ","
			<< "\"confidence\":\"" << toString(f.tempo.confidence) << "\","
			<< "\"reason\":\"" << text::jsonEscape(f.tempo.reason) << "\"}";

		out << ",\"lyrics\":{"
			<< "\"state\":\"" << toString(f.lyrics.state) << "\","
			<< "\"provider\":\"" << text::jsonEscape(f.lyrics.providerId) << "\","
			<< "\"language\":\"" << text::jsonEscape(f.lyrics.language) << "\","
			<< "\"confidence\":\"" << toString(f.lyrics.confidence) << "\","
			<< "\"reason\":\"" << text::jsonEscape(f.lyrics.reason) << "\"}";

		out << "}";
	}
	out << "]}";
	return out.str();
}

std::string toCsv(const ChangeSet& set) {
	std::ostringstream out;
	out << "source_path,destination_relative_path,writable,blockers,tag_changes,artwork_outcome,"
		   "tempo_decision,lyrics_state,optimisation_savings_bytes,enrichment_growth_bytes\n";

	for (const auto& f : set.files) {
		std::string blockers;
		for (std::size_t i = 0; i < f.blockers.size(); ++i) {
			if (i > 0) blockers += "|";
			blockers += toString(f.blockers[i]);
		}
		out << text::csvEscape(f.sourcePath) << ","
			<< text::csvEscape(f.destinationRelativePath) << ","
			<< (f.writable() ? "true" : "false") << ","
			<< text::csvEscape(blockers) << ","
			<< f.changingDecisions().size() << ","
			<< toString(f.artwork.outcome) << ","
			<< toString(f.tempo.kind) << ","
			<< toString(f.lyrics.state) << ","
			<< f.size.optimisationSavings << ","
			<< f.size.enrichmentGrowth << "\n";
	}
	return out.str();
}

} // namespace ml
