#include "stdafx.h"
#include "NqProjectIndex.h"
#include "NqDoc.h"
#include "NqLua.h"
#include "NqUtil.h"
#include "NqCatalog.h"
#include "../EditorProject.h"

namespace
{
	struct SDiskCache
	{
		xr_string	path_key;
		u64			content_hash;
		SNqQuest	quest;
		xr_string	error;
		bool		readable;

		SDiskCache() : content_hash(0), readable(false) {}
	};

	struct SGenerationState
	{
		xr_string	root;
		u64			fingerprint;
		u32			generation;
		bool		valid;

		SGenerationState() : fingerprint(0), generation(0), valid(false) {}
	};

	struct SDirtyDoc
	{
		xr_string	path_key;
		NqDoc*		doc;
	};

	xr_vector<SDiskCache>	s_DiskCache;
	xr_string				s_CacheRoot;
	SGenerationState		s_Generation[2];
	u32					s_InvalidationSerial = 1;

	const u64 kFnvOffset = 14695981039346656037ull;
	const u64 kFnvPrime = 1099511628211ull;

	u64 HashBytes(u64 hash, const void* data, size_t size)
	{
		const u8* bytes = static_cast<const u8*>(data);
		for (size_t i = 0; i < size; ++i)
		{
			hash ^= bytes[i];
			hash *= kFnvPrime;
		}
		return hash;
	}

	u64 HashString(u64 hash, const xr_string& value)
	{
		hash = HashBytes(hash, value.data(), value.size());
		const u8 terminator = 0;
		return HashBytes(hash, &terminator, sizeof(terminator));
	}

	u64 ContentHash(const xr_string& text)
	{
		return HashBytes(kFnvOffset, text.data(), text.size());
	}

	bool SkipEntry(LPCSTR name)
	{
		return EditorProject::IsEditorOnlyEntry(name) || EditorProject::IsSourceOnlyEntry(name);
	}

	const SDiskCache* FindCache(const xr_string& path_key, u64 content_hash)
	{
		const auto found = std::lower_bound(s_DiskCache.begin(), s_DiskCache.end(), path_key,
			[](const SDiskCache& entry, const xr_string& key) { return entry.path_key < key; });
		if (found != s_DiskCache.end() && found->path_key == path_key && found->content_hash == content_hash)
			return &*found;
		return 0;
	}

	NqDoc* FindDirtyDoc(const xr_vector<SDirtyDoc>& docs, const xr_string& path_key)
	{
		const auto found = std::lower_bound(docs.begin(), docs.end(), path_key,
			[](const SDirtyDoc& entry, const xr_string& key) { return entry.path_key < key; });
		if (found != docs.end() && found->path_key == path_key) return found->doc;
		return 0;
	}

	void FoldSearch(LPCSTR text, xr_string& out)
	{
		out.clear();
		if (!text) return;
		for (const u8* p = reinterpret_cast<const u8*>(text); *p; ++p)
		{
			const u8 c = *p;
			if (c >= 'A' && c <= 'Z') { out += char(c + 0x20); continue; }
			if (c == 0xD0 && p[1])
			{
				const u8 d = p[1];
				if (d == 0x81) { out += char(0xD1); out += char(0x91); ++p; continue; }
				if (d >= 0x90 && d <= 0x9F) { out += char(0xD0); out += char(d + 0x20); ++p; continue; }
				if (d >= 0xA0 && d <= 0xAF) { out += char(0xD1); out += char(d - 0x20); ++p; continue; }
			}
			out += char(c);
		}
	}

	struct SSearchTerm
	{
		xr_string	text;
		bool		exclude;
	};

	void ParseSearch(LPCSTR query, xr_vector<SSearchTerm>& out)
	{
		out.clear();
		const char* p = query ? query : "";
		while (*p)
		{
			while (*p && static_cast<u8>(*p) <= 0x20) ++p;
			if (!*p) break;
			SSearchTerm term;
			term.exclude = *p == '-' && p[1] && static_cast<u8>(p[1]) > 0x20;
			if (term.exclude) ++p;
			const bool quoted = *p == '"';
			if (quoted) ++p;
			const char* begin = p;
			if (quoted) while (*p && *p != '"') ++p;
			else while (*p && static_cast<u8>(*p) > 0x20) ++p;
			xr_string raw;
			raw.assign(begin, p - begin);
			if (quoted && *p == '"') ++p;
			FoldSearch(raw.c_str(), term.text);
			if (!term.text.empty()) out.push_back(term);
		}
	}

	bool SearchMatches(LPCSTR text, const xr_vector<SSearchTerm>& terms)
	{
		xr_string folded;
		FoldSearch(text, folded);
		for (u32 i = 0; i < terms.size(); ++i)
		{
			const bool found = strstr(folded.c_str(), terms[i].text.c_str());
			if ((terms[i].exclude && found) || (!terms[i].exclude && !found)) return false;
		}
		return !terms.empty();
	}

	bool SearchFieldHit(LPCSTR text, const xr_vector<SSearchTerm>& terms)
	{
		xr_string folded;
		FoldSearch(text, folded);
		for (u32 i = 0; i < terms.size(); ++i)
			if (!terms[i].exclude && strstr(folded.c_str(), terms[i].text.c_str())) return true;
		return false;
	}
}

bool NqProjectIndex::Snapshot(SSnapshot& out, xr_string& err, bool overlay_dirty)
{
	out = SSnapshot();
	err.clear();
	if (!EditorProject::Active()) { err = "no active project"; return false; }

	xr_string root;
	NqUtil::NormalizePath(EditorProject::Root(), root);
	const xr_string root_key = NqUtil::PathKey(root.c_str());
	if (root_key != s_CacheRoot)
	{
		s_DiskCache.clear();
		s_CacheRoot = root_key;
	}

	xr_vector<xr_string> files;
	NqUtil::ListFiles(root.c_str(), ".nqasset", files, SkipEntry);
	std::sort(files.begin(), files.end(), [](const xr_string& left, const xr_string& right)
	{
		return NqUtil::PathKey(left.c_str()) < NqUtil::PathKey(right.c_str());
	});

	xr_vector<NqDoc*> docs;
	if (overlay_dirty) NqDocs::All(docs);
	xr_vector<SDirtyDoc> dirty_docs;
	for (u32 i = 0; i < docs.size(); ++i)
		if (docs[i]->dirty)
		{
			SDirtyDoc entry;
			entry.path_key = NqUtil::PathKey(docs[i]->path.c_str());
			entry.doc = docs[i];
			dirty_docs.push_back(entry);
		}
	std::sort(dirty_docs.begin(), dirty_docs.end(), [](const SDirtyDoc& left, const SDirtyDoc& right)
	{
		return left.path_key < right.path_key;
	});
	xr_vector<SDiskCache> fresh_cache;
	xr_vector<xr_string> seen;

	for (u32 i = 0; i < files.size(); ++i)
	{
		SEntry entry;
		NqUtil::NormalizePath(files[i].c_str(), entry.path);
		entry.relative = NqUtil::ProjectRelative(entry.path.c_str());
		const xr_string path_key = NqUtil::PathKey(entry.path.c_str());
		seen.push_back(path_key);

		if (NqDoc* doc = FindDirtyDoc(dirty_docs, path_key))
		{
			entry.quest = doc->quest;
			entry.readable = true;
			entry.complete = doc->ModelComplete();
			entry.memory = true;
			const xr_string text = doc->Text();
			entry.content_hash = ContentHash(text);
			if (!entry.complete) entry.error = "open document is an incomplete model of a malformed source file";
			out.entries.push_back(entry);
			continue;
		}

		xr_string text, read_error;
		if (!NqLua::ReadFile(entry.path.c_str(), text, read_error))
		{
			entry.error = read_error;
			out.entries.push_back(entry);
			continue;
		}
		entry.content_hash = ContentHash(text);
		const SDiskCache* cached = FindCache(path_key, entry.content_hash);
		SDiskCache cache;
		if (cached) cache = *cached;
		else
		{
			cache.path_key = path_key;
			cache.content_hash = entry.content_hash;
			NqLua::SError parse_error;
			cache.readable = NqLua::ParseQuest(text.c_str(), static_cast<u32>(text.size()),
				NqUtil::BaseName(entry.path.c_str()).c_str(), cache.quest, parse_error);
			if (!cache.readable)
			{
				cache.error = parse_error.message;
				if (parse_error.line > 0 && parse_error.col > 0)
					cache.error += NqUtil::Format(" (line %d:%d)", parse_error.line, parse_error.col);
				else if (parse_error.line > 0)
					cache.error += NqUtil::Format(" (line %d)", parse_error.line);
			}
		}
		fresh_cache.push_back(cache);
		entry.quest = cache.quest;
		entry.error = cache.error;
		entry.readable = cache.readable;
		entry.complete = cache.readable;
		out.entries.push_back(entry);
	}

	if (overlay_dirty)
	{
		for (u32 i = 0; i < docs.size(); ++i)
		{
			NqDoc* doc = docs[i];
			if (!doc->dirty || !NqDocs::InsideProject(doc->path.c_str()) || NqUtil::Extension(doc->path.c_str()) != ".nqasset") continue;
			const xr_string path_key = NqUtil::PathKey(doc->path.c_str());
			if (std::binary_search(seen.begin(), seen.end(), path_key)) continue;
			SEntry entry;
			NqUtil::NormalizePath(doc->path.c_str(), entry.path);
			entry.relative = NqUtil::ProjectRelative(entry.path.c_str());
			entry.quest = doc->quest;
			entry.readable = true;
			entry.complete = doc->ModelComplete();
			entry.memory = true;
			const xr_string text = doc->Text();
			entry.content_hash = ContentHash(text);
			if (!entry.complete) entry.error = "open document is an incomplete model of a malformed source file";
			out.entries.push_back(entry);
		}
	}

	std::sort(out.entries.begin(), out.entries.end(), [](const SEntry& left, const SEntry& right)
	{
		return NqUtil::PathKey(left.path.c_str()) < NqUtil::PathKey(right.path.c_str());
	});
	s_DiskCache = fresh_cache;

	u64 fingerprint = HashString(kFnvOffset, root_key);
	for (u32 i = 0; i < out.entries.size(); ++i)
	{
		const SEntry& entry = out.entries[i];
		fingerprint = HashString(fingerprint, NqUtil::PathKey(entry.path.c_str()));
		fingerprint = HashBytes(fingerprint, &entry.content_hash, sizeof(entry.content_hash));
		const u8 flags = (entry.readable ? 1 : 0) | (entry.complete ? 2 : 0) | (entry.memory ? 4 : 0);
		fingerprint = HashBytes(fingerprint, &flags, sizeof(flags));
		fingerprint = HashString(fingerprint, entry.error);
	}
	out.fingerprint = fingerprint;
	SGenerationState& state = s_Generation[overlay_dirty ? 1 : 0];
	if (!state.valid || state.root != root_key || state.fingerprint != fingerprint)
	{
		state.root = root_key;
		state.fingerprint = fingerprint;
		++state.generation;
		if (!state.generation) ++state.generation;
		state.valid = true;
	}
	out.generation = state.generation;
	return true;
}

void NqProjectIndex::Invalidate()
{
	s_DiskCache.clear();
	s_CacheRoot.clear();
	++s_InvalidationSerial;
	if (!s_InvalidationSerial) ++s_InvalidationSerial;
}

u32 NqProjectIndex::InvalidationSerial()
{
	return s_InvalidationSerial;
}

void NqProjectIndex::FindNodes(const SSnapshot& snapshot, LPCSTR query, SFindResponse& out)
{
	out = SFindResponse();
	out.generation = snapshot.generation;
	out.fingerprint = snapshot.fingerprint;
	xr_vector<SSearchTerm> terms;
	ParseSearch(NqUtil::Trim(query ? query : "").c_str(), terms);

	for (u32 i = 0; i < snapshot.entries.size(); ++i)
	{
		const SEntry& entry = snapshot.entries[i];
		if (!entry.readable || !entry.complete)
		{
			SDiagnostic diagnostic;
			diagnostic.path = entry.path;
			diagnostic.code = entry.readable ? "incomplete_model" : "unreadable";
			diagnostic.message = entry.error.empty() ? "quest could not be inspected" : entry.error;
			diagnostic.source = entry.Source();
			out.diagnostics.push_back(diagnostic);
			out.complete = false;
			continue;
		}
		if (terms.empty()) continue;
		for (u32 n = 0; n < entry.quest.nodes.size(); ++n)
		{
			const SNqNode& node = entry.quest.nodes[n];
			const NqCatalog::SKind* kind = NqCatalog::Find(node.kind.c_str());
			const xr_string title = kind ? kind->title : node.kind;
			xr_string searchable = NqLua::WriteNode(node, 0);
			searchable += "\n";
			searchable += title;
			if (!SearchMatches(searchable.c_str(), terms)) continue;

			SFindResult result;
			result.path = entry.path;
			result.node = node.id;
			result.kind = node.kind;
			result.title = title;
			result.source = entry.Source();
			if (SearchFieldHit(node.id.c_str(), terms)) result.match = "id";
			else if (SearchFieldHit(node.kind.c_str(), terms) || SearchFieldHit(title.c_str(), terms)) result.match = "kind";
			else if (SearchFieldHit(node.comment.c_str(), terms)) result.match = "comment";
			else result.match = "content";
			out.results.push_back(result);
		}
	}
}

xr_string NqProjectIndex::FingerprintText(u64 fingerprint)
{
	char text[24];
	sprintf_s(text, "%016llx", static_cast<unsigned long long>(fingerprint));
	return text;
}
