#pragma once
// Saved presets for a plugin's own tunable settings: a small file-backed store
// and the "Saved Presets" row (dropdown plus Save, Rename and Delete) built on
// it. Header-only; uses only IModLoaderImGui, no raw ImGui.
//
// The file is <plugin dir>\config\<Plugin>-Presets.ini, one section per preset
// named "[<group>:<name>]" with "key=value" lines (values written as %.6f),
// CRLF line ends and plain ANSI bytes. Several independent groups share one
// file. The format is what BetterDrone and BetterCheats have always written,
// so their players' existing files load unchanged.
//
// Rules the store keeps so a player never loses presets:
//  - LoadFile only reads. Nothing is written until Save, Rename or Delete.
//  - Before the first rewrite it copies the file to "<file>.bak", once, and
//    never replaces a .bak that is already there.
//  - A file with a line it cannot understand is logged once and the store goes
//    read-only for that file. Its readable entries still list and load, but
//    Save, Rename and Delete refuse and leave the file untouched.
//  - A rewrite goes to "<file>.tmp" first and is renamed over the original, so
//    a failed write leaves the old file in place.
//
// Threading: every Store call takes the store's mutex, so the panel (render
// thread) and hotkeys (game tick) may both use one Store. File I/O happens only
// in LoadFile and in Save, Rename and Delete, never per frame.
//
// Not stored here: "built-in" presets. The caller owns those and tells the row
// which names are built in (isBuiltin) and what to call a new copy
// (computeSuggest); built-ins are never written to the file.

#include "plugin_interface.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace PluginPresets
{
	// One named float value within a preset, e.g. { "maxRadius", 500.0f }.
	struct Field
	{
		const char* key;
		float value;
	};

	// Size of a name buffer: up to 64 characters plus the terminator.
	constexpr int kMaxNameLen = 65;

	// The most names ListNames returns for a group (and a dropdown shows).
	constexpr int kMaxListed = 16;

	// Where the store sends its log lines. The plugin passes its own logger,
	// e.g. [](const char* line) { MyLog(line); }. May be empty.
	using LogSink = std::function<void(const char*)>;

	class Store
	{
	public:
		explicit Store(LogSink log = {}) : m_log(std::move(log)) {}

		Store(const Store&) = delete;
		Store& operator=(const Store&) = delete;

		// Reads filePath into memory. A missing file is not an error: the
		// store starts empty and the file is created on the first Save.
		// Never writes. Returns false only for an empty path.
		bool LoadFile(const char* filePath)
		{
			if (!filePath || !filePath[0])
				return false;

			std::lock_guard<std::mutex> lock(m_mutex);
			m_path = filePath;
			m_entries.clear();
			m_readOnly = false;
			m_backupChecked = false;
			Parse(ReadWholeFile(filePath));
			return true;
		}

		// True when the loaded file had a line the reader could not
		// understand. Save, Rename and Delete then return false.
		bool IsReadOnly() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_readOnly;
		}

		// Fills outNames with up to cap names in group, in byte order, and
		// returns how many. A name longer than 64 characters is skipped (it
		// stays in the file).
		int ListNames(const char* group, char outNames[][kMaxNameLen], int cap) const
		{
			if (!group || !outNames || cap <= 0)
				return 0;

			std::lock_guard<std::mutex> lock(m_mutex);
			std::vector<const std::string*> names;
			for (const Entry& e : m_entries)
				if (e.group == group && e.name.size() < static_cast<size_t>(kMaxNameLen))
					names.push_back(&e.name);

			std::sort(names.begin(), names.end(),
				[](const std::string* a, const std::string* b) { return *a < *b; });

			int written = 0;
			for (const std::string* n : names)
			{
				if (written >= cap)
					break;
				snprintf(outNames[written], kMaxNameLen, "%s", n->c_str());
				++written;
			}
			return written;
		}

		bool Exists(const char* group, const char* name) const
		{
			if (!group || !name)
				return false;
			std::lock_guard<std::mutex> lock(m_mutex);
			return Find(group, name) != nullptr;
		}

		// Writes fields as the preset name in group: replaces that entry in
		// place if it exists, otherwise appends a new one. Rewrites the file.
		// Returns false if the store is read-only, the name or group is not
		// valid, or the file could not be written (the old file is kept).
		bool Save(const char* group, const char* name, const Field* fields, int count)
		{
			if (!group || !name || (!fields && count > 0) || !ValidGroup(group) || !ValidName(name))
				return false;

			std::lock_guard<std::mutex> lock(m_mutex);
			if (!CanWrite())
				return false;

			std::vector<Entry> before = m_entries;
			Entry* entry = Find(group, name);
			if (!entry)
			{
				m_entries.push_back({ group, name, {} });
				entry = &m_entries.back();
			}
			entry->fields.clear();
			for (int i = 0; i < count; ++i)
				entry->fields.push_back({ fields[i].key, fields[i].value });

			return Commit(std::move(before));
		}

		// Copies the stored preset name over out[i].value, matched by key. A
		// key the preset does not have is left untouched, so seed out with the
		// live values first. Returns false if there is no such preset.
		bool Get(const char* group, const char* name, Field* out, int count) const
		{
			if (!group || !name)
				return false;

			std::lock_guard<std::mutex> lock(m_mutex);
			const Entry* entry = Find(group, name);
			if (!entry)
				return false;

			for (int i = 0; i < count; ++i)
			{
				if (!out[i].key)
					continue;
				for (const StoredField& f : entry->fields)
				{
					if (f.key == out[i].key)
					{
						out[i].value = f.value;
						break;
					}
				}
			}
			return true;
		}

		// Renames a stored preset. Returns false if the store is read-only,
		// from is missing, to is already used in group or not a valid name.
		bool Rename(const char* group, const char* from, const char* to)
		{
			if (!group || !from || !to || !ValidName(to))
				return false;

			std::lock_guard<std::mutex> lock(m_mutex);
			if (!CanWrite())
				return false;
			if (strcmp(from, to) == 0)
				return Find(group, from) != nullptr; // no-op, not an error

			Entry* entry = Find(group, from);
			if (!entry || Find(group, to))
				return false;

			std::vector<Entry> before = m_entries;
			entry->name = to;
			return Commit(std::move(before));
		}

		// Deletes a stored preset. Returns false if the store is read-only or
		// the preset does not exist.
		bool Delete(const char* group, const char* name)
		{
			if (!group || !name)
				return false;

			std::lock_guard<std::mutex> lock(m_mutex);
			if (!CanWrite() || !Find(group, name))
				return false;

			std::vector<Entry> before = m_entries;
			m_entries.erase(
				std::remove_if(m_entries.begin(), m_entries.end(),
					[&](const Entry& e) { return e.group == group && e.name == name; }),
				m_entries.end());
			return Commit(std::move(before));
		}

		// Fills out with baseName, or baseName + " 2", " 3", ...: the first
		// spelling not already a stored name in group. Never fails; a long
		// baseName is shortened so the result fits 64 characters.
		void SuggestName(const char* group, const char* baseName, char* out, int cap) const
		{
			if (!out || cap <= 0)
				return;
			out[0] = '\0';
			if (!group || !baseName || !baseName[0])
				return;

			std::lock_guard<std::mutex> lock(m_mutex);
			if (!Find(group, baseName))
			{
				snprintf(out, cap, "%s", baseName);
				return;
			}
			for (int suffix = 2; suffix < 1000; ++suffix)
			{
				char tail[16];
				snprintf(tail, sizeof(tail), " %d", suffix);
				std::string candidate = baseName;
				const size_t room = static_cast<size_t>(kMaxNameLen - 1) - strlen(tail);
				if (candidate.size() > room)
					candidate.resize(room);
				candidate += tail;
				if (!Find(group, candidate.c_str()))
				{
					snprintf(out, cap, "%s", candidate.c_str());
					return;
				}
			}
			snprintf(out, cap, "%s", baseName);
		}

	private:
		struct StoredField
		{
			std::string key;
			float value;
		};

		struct Entry
		{
			std::string group;
			std::string name;
			std::vector<StoredField> fields;
		};

		static bool ValidGroup(const char* group)
		{
			return group[0] && !strchr(group, ':') && !strpbrk(group, "\r\n");
		}

		static bool ValidName(const char* name)
		{
			return name[0] && strlen(name) < static_cast<size_t>(kMaxNameLen) && !strpbrk(name, "\r\n");
		}

		Entry* Find(const char* group, const char* name)
		{
			for (Entry& e : m_entries)
				if (e.group == group && e.name == name)
					return &e;
			return nullptr;
		}

		const Entry* Find(const char* group, const char* name) const
		{
			return const_cast<Store*>(this)->Find(group, name);
		}

		void Log(const std::string& line) const
		{
			if (m_log)
				m_log(line.c_str());
		}

		static std::string ReadWholeFile(const std::string& path)
		{
			std::string content;
			FILE* f = nullptr;
			if (fopen_s(&f, path.c_str(), "rb") != 0 || !f)
				return content;

			fseek(f, 0, SEEK_END);
			const long size = ftell(f);
			fseek(f, 0, SEEK_SET);
			if (size > 0)
			{
				content.resize(static_cast<size_t>(size));
				content.resize(fread(&content[0], 1, static_cast<size_t>(size), f));
			}
			fclose(f);
			return content;
		}

		// "[group:name]" body -> group, name. Splits on the first ':', so a
		// group must not contain one but a name may.
		static bool SplitSection(const std::string& section, std::string& group, std::string& name)
		{
			const size_t colon = section.find(':');
			if (colon == std::string::npos)
				return false;
			group = section.substr(0, colon);
			name = section.substr(colon + 1);
			return !group.empty() && !name.empty();
		}

		// Line-based reader. Understood lines: blank, ';' or '#' comments,
		// "[group:name]" headers (closed at the LAST ']', nothing but spaces
		// after it, no repeat of an earlier section), and "key=value" lines
		// under a header whose value is a number. Anything else is
		// unreadable: it is logged once (the first one) and the store goes
		// read-only. Readable entries are kept either way. Caller holds the
		// mutex.
		void Parse(const std::string& content)
		{
			Entry* current = nullptr;
			int lineNo = 0;
			bool reported = false;
			auto bad = [&](const char* why)
			{
				m_readOnly = true;
				if (!reported)
				{
					reported = true;
					Log("Presets file " + m_path + " line " + std::to_string(lineNo) + ": " + why
						+ ". The file is left unchanged and presets cannot be saved, renamed or deleted.");
				}
			};

			size_t pos = 0;
			while (pos < content.size())
			{
				++lineNo;
				const size_t eol = content.find_first_of("\r\n", pos);
				std::string line = (eol == std::string::npos) ? content.substr(pos) : content.substr(pos, eol - pos);
				if (eol == std::string::npos)
					pos = content.size();
				else if (content[eol] == '\r' && eol + 1 < content.size() && content[eol + 1] == '\n')
					pos = eol + 2;
				else
					pos = eol + 1;

				const size_t start = line.find_first_not_of(" \t");
				if (start == std::string::npos)
					continue;
				line = line.substr(start);
				if (line[0] == ';' || line[0] == '#')
					continue;

				if (line[0] == '[')
				{
					current = nullptr;
					const size_t close = line.rfind(']');
					if (close == std::string::npos)
					{
						bad("section header has no closing bracket");
						continue;
					}
					if (line.find_first_not_of(" \t", close + 1) != std::string::npos)
					{
						bad("text after a section header");
						continue;
					}
					std::string group, name;
					if (!SplitSection(line.substr(1, close - 1), group, name))
					{
						bad("section header is not [group:name]");
						continue;
					}
					if (Find(group.c_str(), name.c_str()))
					{
						bad("section appears twice");
						continue;
					}
					m_entries.push_back({ std::move(group), std::move(name), {} });
					current = &m_entries.back();
					continue;
				}

				const size_t eq = line.find('=');
				if (eq == std::string::npos)
				{
					bad("line is not key=value");
					continue;
				}
				if (!current)
				{
					bad("value outside a readable section");
					continue;
				}

				std::string key = line.substr(0, eq);
				while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
					key.pop_back();
				if (key.empty())
				{
					bad("empty key");
					continue;
				}

				const std::string valueStr = line.substr(eq + 1);
				char* end = nullptr;
				const double v = strtod(valueStr.c_str(), &end);
				const bool numeric = end != valueStr.c_str()
					&& valueStr.find_first_not_of(" \t", static_cast<size_t>(end - valueStr.c_str())) == std::string::npos;
				if (!numeric)
				{
					bad("value is not a number");
					continue;
				}
				current->fields.push_back({ std::move(key), static_cast<float>(v) });
			}
		}

		// The file's text for the current entries: the exact layout the
		// plugins have always written.
		std::string Serialize() const
		{
			std::string content;
			for (const Entry& p : m_entries)
			{
				content += "[";
				content += p.group;
				content += ":";
				content += p.name;
				content += "]\r\n";
				for (const StoredField& f : p.fields)
				{
					char valStr[64];
					snprintf(valStr, sizeof(valStr), "%.6f", f.value);
					content += f.key;
					content += "=";
					content += valStr;
					content += "\r\n";
				}
				content += "\r\n";
			}
			return content;
		}

		bool CanWrite() const
		{
			if (m_path.empty())
				return false;
			if (m_readOnly)
			{
				Log("Presets file " + m_path + " was not changed because it has lines this plugin cannot read.");
				return false;
			}
			return true;
		}

		// Writes the entries to disk. On any failure restores the entries to
		// before, so memory and file stay in step. Caller holds the mutex.
		bool Commit(std::vector<Entry> before)
		{
			if (Write())
				return true;
			m_entries = std::move(before);
			return false;
		}

		bool Write()
		{
			namespace fs = std::filesystem;
			std::error_code ec;
			const fs::path path(m_path);

			// First rewrite of this file: keep the player's original beside it.
			if (!m_backupChecked)
			{
				const fs::path bak = fs::path(m_path + ".bak");
				if (fs::exists(path, ec) && !fs::exists(bak, ec))
				{
					fs::copy_file(path, bak, fs::copy_options::none, ec);
					if (ec)
					{
						Log("Could not back up " + m_path + " (" + ec.message() + "); presets were not saved.");
						return false;
					}
				}
				m_backupChecked = true;
			}

			const std::string tmpPath = m_path + ".tmp";
			const std::string content = Serialize();
			FILE* f = nullptr;
			if (fopen_s(&f, tmpPath.c_str(), "wb") != 0 || !f)
			{
				Log("Could not write " + tmpPath);
				return false;
			}
			const bool wrote = fwrite(content.data(), 1, content.size(), f) == content.size();
			const bool closed = fclose(f) == 0;
			if (!wrote || !closed)
			{
				fs::remove(fs::path(tmpPath), ec);
				Log("Could not write " + tmpPath);
				return false;
			}

			fs::rename(fs::path(tmpPath), path, ec);
			if (ec)
			{
				fs::remove(fs::path(tmpPath), ec);
				Log("Could not replace " + m_path);
				return false;
			}
			return true;
		}

		mutable std::mutex m_mutex;
		LogSink m_log;
		std::string m_path;
		std::vector<Entry> m_entries;
		bool m_readOnly = false;
		bool m_backupChecked = false;
	};

	// Loads the stored preset over the live values and applies it: the one
	// body behind picking a preset in the dropdown and stepping to one with a
	// hotkey. Returns false if the preset is gone.
	template <typename GetLiveFields, typename ApplyFields>
	inline bool ApplySaved(Store& store, const char* group, const char* name,
		Field* fields, int fieldCount, GetLiveFields getLive, ApplyFields apply)
	{
		getLive(fields); // seed so a key missing from this preset stays unchanged
		if (!store.Get(group, name, fields, fieldCount))
			return false;
		apply(fields, fieldCount);
		return true;
	}

	// Kept per group between frames: which saved preset is selected, and what
	// the rename field and message line are showing.
	struct RowState
	{
		char selected[kMaxNameLen] = {};
		bool renaming = false;
		char renameBuf[kMaxNameLen] = {};
		char errorMsg[96] = {};
	};

	// A dropdown of the group's saved presets plus Save, Rename and Delete,
	// beside (not replacing) the caller's built-in preset buttons.
	//
	//  Save, a saved preset selected: asks "You're saving over <name>.
	//        Continue?" and on Continue rewrites only that entry.
	//  Save, nothing selected (or a name that is a built-in): adds a new entry
	//        named computeSuggest's base, made unique ("Custom", "Custom 2").
	//  Rename / Delete: as before (Delete asks first).
	//  When the store is read-only each of the three shows a message instead.
	//
	// fields is scratch of fieldCount entries. Callables:
	//   getLive(Field* out)                  fill out with the live values
	//   apply(const Field* f, int count)     put a loaded preset into effect
	//   isBuiltin(const char* name) -> bool  true for the caller's built-in names
	//   computeSuggest(char* out, int cap)   base name for a new entry
	template <typename GetLiveFields, typename ApplyFields, typename IsBuiltinName, typename ComputeSuggestedBase>
	inline void RenderSavedRow(IModLoaderImGui* ui, Store& store, const char* idScope, const char* group,
		Field* fields, int fieldCount,
		GetLiveFields getLive, ApplyFields apply, IsBuiltinName isBuiltin, ComputeSuggestedBase computeSuggest,
		RowState& state)
	{
		auto setError = [&](const char* text) { snprintf(state.errorMsg, sizeof(state.errorMsg), "%s", text); };
		const char* const kReadOnlyMsg = "Presets file has lines this plugin can't read; it was left unchanged.";

		ui->PushIDStr(idScope);

		char names[kMaxListed][kMaxNameLen];
		const int count = store.ListNames(group, names, kMaxListed);

		bool selectedStillValid = false;
		for (int i = 0; i < count; ++i)
			if (strcmp(names[i], state.selected) == 0)
				selectedStillValid = true;
		if (!selectedStillValid)
			state.selected[0] = '\0';

		ui->AlignTextToFramePadding();
		ui->Text("Saved Presets");
		ui->SameLine(0.0f, -1.0f);

		ui->SetNextItemWidth(220.0f);
		const char* preview = state.selected[0] ? state.selected : "(none saved)";
		if (ui->BeginCombo("##saved", preview))
		{
			for (int i = 0; i < count; ++i)
			{
				const bool isSelected = (strcmp(names[i], state.selected) == 0);
				if (ui->Selectable(names[i], isSelected))
				{
					snprintf(state.selected, sizeof(state.selected), "%s", names[i]);
					ApplySaved(store, group, state.selected, fields, fieldCount, getLive, apply);
				}
			}
			ui->EndCombo();
		}

		char overwritePopupId[80];
		snprintf(overwritePopupId, sizeof(overwritePopupId), "Overwrite preset?##%s", group);

		ui->SameLine(0.0f, -1.0f);
		if (ui->SmallButton("Save"))
		{
			if (store.IsReadOnly())
			{
				setError(kReadOnlyMsg);
			}
			else if (state.selected[0] && !isBuiltin(state.selected))
			{
				state.errorMsg[0] = '\0';
				ui->OpenPopup(overwritePopupId, 0);
			}
			else
			{
				char base[kMaxNameLen];
				computeSuggest(base, sizeof(base));
				char suggested[kMaxNameLen];
				store.SuggestName(group, base, suggested, sizeof(suggested));

				getLive(fields);
				if (store.Save(group, suggested, fields, fieldCount))
				{
					snprintf(state.selected, sizeof(state.selected), "%s", suggested);
					state.errorMsg[0] = '\0';
				}
				else
				{
					setError("Could not save -- presets file unavailable.");
				}
			}
		}

		if (ui->BeginPopupModal(overwritePopupId, nullptr, 0))
		{
			char question[kMaxNameLen + 48];
			snprintf(question, sizeof(question), "You're saving over %s. Continue?", state.selected);
			ui->Text(question);
			ui->Spacing();
			if (ui->SmallButton("Continue##overwrite"))
			{
				getLive(fields);
				if (store.Save(group, state.selected, fields, fieldCount))
					state.errorMsg[0] = '\0';
				else
					setError("Could not save -- presets file unavailable.");
				ui->CloseCurrentPopup();
			}
			ui->SameLine(0.0f, -1.0f);
			if (ui->SmallButton("Cancel##overwrite"))
				ui->CloseCurrentPopup();
			ui->EndPopup();
		}

		const bool hasSelection = state.selected[0] != '\0';

		ui->SameLine(0.0f, -1.0f);
		ui->BeginDisabled(!hasSelection);
		if (ui->SmallButton("Rename"))
		{
			if (store.IsReadOnly())
			{
				setError(kReadOnlyMsg);
			}
			else
			{
				state.renaming = true;
				snprintf(state.renameBuf, sizeof(state.renameBuf), "%s", state.selected);
				state.errorMsg[0] = '\0';
			}
		}
		ui->EndDisabled();

		char deletePopupId[80];
		snprintf(deletePopupId, sizeof(deletePopupId), "Delete preset?##%s", group);

		ui->SameLine(0.0f, -1.0f);
		ui->BeginDisabled(!hasSelection);
		if (ui->SmallButton("Delete"))
		{
			if (store.IsReadOnly())
				setError(kReadOnlyMsg);
			else
				ui->OpenPopup(deletePopupId, 0);
		}
		ui->EndDisabled();

		if (ui->BeginPopupModal(deletePopupId, nullptr, 0))
		{
			ui->Text("Delete this saved preset?");
			ui->TextDisabled(state.selected);
			ui->Spacing();
			if (ui->SmallButton("Delete##confirm"))
			{
				if (store.Delete(group, state.selected))
					state.selected[0] = '\0';
				else
					setError("Could not delete -- presets file unavailable.");
				ui->CloseCurrentPopup();
			}
			ui->SameLine(0.0f, -1.0f);
			if (ui->SmallButton("Cancel##delete"))
				ui->CloseCurrentPopup();
			ui->EndPopup();
		}

		if (state.renaming)
		{
			ui->SetNextItemWidth(200.0f);
			ui->InputText("##rename", state.renameBuf, sizeof(state.renameBuf));

			ui->SameLine(0.0f, -1.0f);
			if (ui->SmallButton("OK##rename"))
			{
				if (!state.renameBuf[0])
				{
					setError("A preset needs a name.");
				}
				else if (isBuiltin(state.renameBuf))
				{
					snprintf(state.errorMsg, sizeof(state.errorMsg), "\"%s\" is a built-in preset name.", state.renameBuf);
				}
				else if (store.Rename(group, state.selected, state.renameBuf))
				{
					snprintf(state.selected, sizeof(state.selected), "%s", state.renameBuf);
					state.renaming = false;
					state.errorMsg[0] = '\0';
				}
				else if (store.IsReadOnly())
				{
					setError(kReadOnlyMsg);
				}
				else
				{
					snprintf(state.errorMsg, sizeof(state.errorMsg), "\"%s\" is already used.", state.renameBuf);
				}
			}

			ui->SameLine(0.0f, -1.0f);
			if (ui->SmallButton("Cancel##rename"))
			{
				state.renaming = false;
				state.errorMsg[0] = '\0';
			}
		}

		if (state.errorMsg[0])
			ui->TextColored(1.0f, 0.4f, 0.4f, 1.0f, state.errorMsg);

		ui->PopID();
	}
}
