// plugin_presets.h tests. A console program: the store and the saved-presets row
// only need the standard library and IModLoaderImGui, so a recording stand-in
// for the ImGui table (below) and the shared Basic.hpp stub are enough and no
// game is needed.
//
// Fixtures are hand-written in the exact format the plugins have always written
// (fixtures\Synthetic-Presets.ini). Each test works on a temp copy.
//
// Optional: pass a presets file as argv[1], or set PLUGIN_PRESETS_REAL_FILE, to
// run the load-then-save round trip on a real player file. It is only read; the
// check works on a temp copy and is skipped when no path is given.
#include <Basic.hpp>
#include <plugin_interface.h>
#include <plugin_presets.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace fs = std::filesystem;
using namespace PluginPresets;

static int g_checks = 0;
static int g_failed = 0;

static void Check(bool ok, const char* what)
{
	++g_checks;
	if (!ok)
	{
		++g_failed;
		printf("    FAIL %s\n", what);
	}
}

static std::string ReadBytes(const fs::path& p)
{
	std::string s;
	FILE* f = nullptr;
	if (fopen_s(&f, p.string().c_str(), "rb") != 0 || !f)
		return s;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		s.append(buf, n);
	fclose(f);
	return s;
}

static void WriteBytes(const fs::path& p, const std::string& s)
{
	FILE* f = nullptr;
	if (fopen_s(&f, p.string().c_str(), "wb") != 0 || !f)
		return;
	fwrite(s.data(), 1, s.size(), f);
	fclose(f);
}

static fs::path MakeTempDir()
{
	static int n = 0;
	fs::path d = fs::temp_directory_path() / ("plugin_presets_tests_" + std::to_string(::rand()) + "_" + std::to_string(++n));
	fs::create_directories(d);
	return d;
}

static const std::string kFixture = ReadBytes(fs::path(__FILE__).parent_path() / "fixtures" / "Synthetic-Presets.ini");

// A fresh temp dir holding Presets.ini with the given text.
struct Sandbox
{
	fs::path dir = MakeTempDir();
	fs::path file = dir / "Plugin-Presets.ini";
	fs::path bak = dir / "Plugin-Presets.ini.bak";
	fs::path tmp = dir / "Plugin-Presets.ini.tmp";
	explicit Sandbox(const std::string& content, bool create = true)
	{
		if (create)
			WriteBytes(file, content);
	}
	~Sandbox() { std::error_code ec; fs::remove_all(dir, ec); }
	std::string Path() const { return file.string(); }
};

struct Logs
{
	std::vector<std::string> lines;
	LogSink Sink() { return [this](const char* l) { lines.push_back(l); }; }
};

// Every (group, name, keys) in a presets text, read independently of the store
// so the tests can save each entry back with its own values.
struct Section { std::string group, name; std::vector<std::string> keys; };

static std::vector<Section> ScanSections(const std::string& text)
{
	std::vector<Section> out;
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t eol = text.find("\r\n", pos);
		std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
		pos = (eol == std::string::npos) ? text.size() : eol + 2;
		if (line.empty() || line[0] == ';' || line[0] == '#')
			continue;
		if (line[0] == '[')
		{
			const std::string body = line.substr(1, line.rfind(']') - 1);
			const size_t colon = body.find(':');
			out.push_back({ body.substr(0, colon), body.substr(colon + 1), {} });
		}
		else if (!out.empty())
			out.back().keys.push_back(line.substr(0, line.find('=')));
	}
	return out;
}

// Saves every entry back with the values the store holds for it, in file order.
static bool SaveAllUnchanged(Store& store, const std::string& original)
{
	for (const Section& s : ScanSections(original))
	{
		std::vector<Field> fields;
		for (const std::string& k : s.keys)
			fields.push_back({ k.c_str(), 0.0f });
		if (!store.Get(s.group.c_str(), s.name.c_str(), fields.data(), static_cast<int>(fields.size())))
			return false;
		if (!store.Save(s.group.c_str(), s.name.c_str(), fields.data(), static_cast<int>(fields.size())))
			return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// A recording stand-in for IModLoaderImGui: buttons are "clicked" by label,
// popups open and close, and text is captured.
// ---------------------------------------------------------------------------
namespace Fake
{
	static std::set<std::string> clicks;       // labels to report as clicked on the next frame
	static std::set<std::string> openPopups;
	static std::vector<std::string> texts;
	static std::string currentPopup;
	static std::string comboPick;              // item to pick when the combo opens

	static bool SmallButton(const char* label) { return clicks.erase(label) > 0; }
	static void OpenPopup(const char* id, int) { openPopups.insert(id); }
	static bool BeginPopupModal(const char* id, bool*, int)
	{
		if (!openPopups.count(id))
			return false;
		currentPopup = id;
		return true;
	}
	static void CloseCurrentPopup() { openPopups.erase(currentPopup); }
	static void Text(const char* t) { texts.push_back(t); }
	static void TextColored(float, float, float, float, const char* t) { texts.push_back(t); }
	static void TextDisabled(const char* t) { texts.push_back(t); }
	static bool BeginCombo(const char*, const char*) { return !comboPick.empty(); }
	static bool Selectable(const char* label, bool) { return comboPick == label; }
	static void EndCombo() { comboPick.clear(); }
	static void Nop0() {}
	static void NopBool(bool) {}
	static void NopStr(const char*) {}
	static void NopFloat(float) {}
	static void NopSame(float, float) {}
	static bool NopInput(const char*, char*, size_t) { return false; }

	static IModLoaderImGui Make()
	{
		IModLoaderImGui ui{};
		ui.SmallButton = SmallButton;
		ui.OpenPopup = OpenPopup;
		ui.BeginPopupModal = BeginPopupModal;
		ui.CloseCurrentPopup = CloseCurrentPopup;
		ui.Text = Text;
		ui.TextColored = TextColored;
		ui.TextDisabled = TextDisabled;
		ui.BeginCombo = BeginCombo;
		ui.Selectable = Selectable;
		ui.EndCombo = EndCombo;
		ui.EndPopup = Nop0;
		ui.Spacing = Nop0;
		ui.PopID = Nop0;
		ui.AlignTextToFramePadding = Nop0;
		ui.EndDisabled = Nop0;
		ui.BeginDisabled = NopBool;
		ui.PushIDStr = NopStr;
		ui.SetNextItemWidth = NopFloat;
		ui.SameLine = NopSame;
		ui.InputText = NopInput;
		return ui;
	}

	static void Reset() { clicks.clear(); openPopups.clear(); texts.clear(); currentPopup.clear(); comboPick.clear(); }
	static bool Said(const char* t) { for (const std::string& s : texts) if (s == t) return true; return false; }
}

// Live values the row reads and applies.
static float g_live[2] = { 111.0f, 2.5f };
static float g_applied[2] = { 0, 0 };
static const char* g_suggest = "Custom";

static void GetLive(Field* out) { out[0] = { "speedPerSec", g_live[0] }; out[1] = { "boostMultiplier", g_live[1] }; }
static void ApplyFields(const Field* f, int count) { for (int i = 0; i < count && i < 2; ++i) g_applied[i] = f[i].value; }
static bool IsBuiltin(const char* n) { return strcmp(n, "Normal") == 0 || strcmp(n, "Slow") == 0; }
static void SuggestBase(char* out, int cap) { snprintf(out, cap, "%s", g_suggest); }

// One frame of the row for group "Speed".
static void Frame(Store& store, RowState& state)
{
	IModLoaderImGui ui = Fake::Make();
	Field fields[2] = { { "speedPerSec", 0 }, { "boostMultiplier", 0 } };
	RenderSavedRow(&ui, store, "speed", "Speed", fields, 2, GetLive, ApplyFields, IsBuiltin, SuggestBase, state);
}

// The text of fixture with section [group:name] replaced by body.
static std::string ReplaceSection(const std::string& text, const std::string& header, const std::string& body)
{
	const size_t at = text.find(header);
	const size_t end = text.find("\r\n\r\n", at) + 4;
	return text.substr(0, at) + header + body + "\r\n" + text.substr(end);
}

int main(int argc, char** argv)
{
	printf("plugin_presets tests\n");
	Check(kFixture.size() > 500, "fixture is present");
	Check(kFixture.find("\r\n") != std::string::npos && kFixture.find('\xE9') != std::string::npos, "fixture has CRLF and high-ANSI bytes");

	// Load and list.
	{
		Sandbox sb(kFixture);
		Logs logs;
		Store store(logs.Sink());
		Check(store.LoadFile(sb.Path().c_str()), "load succeeds");
		Check(!store.IsReadOnly(), "a well-formed file is not read-only");
		Check(logs.lines.empty(), "and logs nothing");

		char names[kMaxListed][kMaxNameLen];
		int n = store.ListNames("Speed", names, kMaxListed);
		Check(n == 7, "Speed lists all seven entries");
		Check(n > 0 && strcmp(names[0], "Fast Mode") == 0, "listed in byte order (Fast Mode first)");
		const std::string long64 = std::string(31, 'N') + " " + std::string(32, 'm');
		bool haveLong = false, haveHigh = false, haveBracket = false, haveEq = false, haveColon = false;
		for (int i = 0; i < n; ++i)
		{
			haveLong |= long64 == names[i];
			haveHigh |= strcmp(names[i], "caf\xE9 \xFC\xDF \x80\xFF") == 0;
			haveBracket |= strcmp(names[i], "odd]name") == 0;
			haveEq |= strcmp(names[i], "key=value") == 0;
			haveColon |= strcmp(names[i], "time: 10:30") == 0;
		}
		Check(haveLong, "a 64-character name lists whole");
		Check(haveHigh, "high-ANSI bytes survive");
		Check(haveBracket, "a name with ] survives");
		Check(haveEq && haveColon, "names with = and : survive");
		Check(store.ListNames("Range", names, kMaxListed) == 2, "Range lists two");
		Check(store.ListNames("Nope", names, kMaxListed) == 0, "an unknown group lists none");
		Check(store.Exists("Lonely", "No Fields"), "an entry with no fields exists");

		Field f[2] = { { "maxRadius", -1 }, { "missing", 7 } };
		Check(store.Get("Range", "Wide", f, 2) && f[0].value == 5000.0f && f[1].value == 7.0f, "Get fills matching keys, leaves others");
		Check(!store.Get("Range", "Gone", f, 2), "Get of a missing preset is false");
	}

	// More than 16 in a group: 16 listed, all kept in the file.
	{
		std::string text;
		for (int i = 0; i < 20; ++i)
		{
			char h[64];
			snprintf(h, sizeof(h), "[G:p%02d]\r\nk=1.000000\r\n\r\n", i);
			text += h;
		}
		Sandbox sb(text);
		Store store;
		store.LoadFile(sb.Path().c_str());
		char names[kMaxListed][kMaxNameLen];
		Check(store.ListNames("G", names, kMaxListed) == 16, "at most 16 are listed per group");
		Field k[1] = { { "k", 2.0f } };
		Check(store.Save("G", "p00", k, 1), "saving over one still works");
		const std::string after = ReadBytes(sb.file);
		Check(after.find("[G:p19]") != std::string::npos && after.find("[G:p16]") != std::string::npos, "and the unlisted entries stay in the file");
	}

	// Loading never writes.
	{
		Sandbox sb(kFixture);
		const auto before = fs::last_write_time(sb.file);
		Store store;
		store.LoadFile(sb.Path().c_str());
		char names[kMaxListed][kMaxNameLen];
		store.ListNames("Speed", names, kMaxListed);
		Check(ReadBytes(sb.file) == kFixture && fs::last_write_time(sb.file) == before, "load leaves the file untouched");
		Check(!fs::exists(sb.bak) && !fs::exists(sb.tmp), "and creates no .bak or .tmp");

		Sandbox missing("", false);
		Store s2;
		Check(s2.LoadFile(missing.Path().c_str()) && !fs::exists(missing.file), "loading a missing file does not create it");
		Check(!fs::exists(missing.bak), "and no .bak either");
	}

	// Load then save with no change is byte-identical; the .bak holds the original.
	{
		Sandbox sb(kFixture);
		Store store;
		store.LoadFile(sb.Path().c_str());
		Check(SaveAllUnchanged(store, kFixture), "saving every entry with its own values succeeds");
		Check(ReadBytes(sb.file) == kFixture, "load then save, unchanged, is byte-identical");
		Check(fs::exists(sb.bak) && ReadBytes(sb.bak) == kFixture, "a .bak of the original was made");
		Check(!fs::exists(sb.tmp), "no .tmp is left behind");
	}

	// Save over changes only the selected section.
	{
		Sandbox sb(kFixture);
		Store store;
		store.LoadFile(sb.Path().c_str());
		Field f[2] = { { "speedPerSec", 321.5f }, { "boostMultiplier", 1.25f } };
		Check(store.Save("Speed", "Fast Mode", f, 2), "save over an existing entry succeeds");
		const std::string expected = ReplaceSection(kFixture, "[Speed:Fast Mode]\r\n",
			"speedPerSec=321.500000\r\nboostMultiplier=1.250000\r\n");
		Check(ReadBytes(sb.file) == expected, "only that section changed, the rest is byte-identical");
		Check(ReadBytes(sb.bak) == kFixture, "the .bak is the original");
	}

	// A new save appends; rename and delete.
	{
		Sandbox sb(kFixture);
		Store store;
		store.LoadFile(sb.Path().c_str());
		Field f[1] = { { "speedPerSec", 5.0f } };
		Check(store.Save("Speed", "Custom", f, 1), "a new name saves");
		Check(ReadBytes(sb.file) == kFixture + "[Speed:Custom]\r\nspeedPerSec=5.000000\r\n\r\n", "and is appended at the end");

		Check(store.Rename("Speed", "Custom", "Mine"), "rename works");
		Check(ReadBytes(sb.file) == kFixture + "[Speed:Mine]\r\nspeedPerSec=5.000000\r\n\r\n", "and keeps the entry's place");
		Check(!store.Rename("Speed", "Mine", "Slow"), "rename onto a used name is refused");
		Check(!store.Rename("Speed", "Mine", ""), "rename to empty is refused");
		Check(!store.Rename("Speed", "Nope", "X"), "rename of a missing entry is refused");
		Check(store.Delete("Speed", "Mine"), "delete works");
		Check(ReadBytes(sb.file) == kFixture, "and the file is back to the original bytes");
		Check(!store.Delete("Speed", "Mine"), "delete of a missing entry is false");

		char out[kMaxNameLen];
		store.SuggestName("Speed", "Custom", out, sizeof(out));
		Check(strcmp(out, "Custom") == 0, "SuggestName gives the base when free");
		store.Save("Speed", "Custom", f, 1);
		store.SuggestName("Speed", "Custom", out, sizeof(out));
		Check(strcmp(out, "Custom 2") == 0, "and the next number when taken");
		store.Save("Speed", "Custom 2", f, 1);
		store.SuggestName("Speed", "Custom", out, sizeof(out));
		Check(strcmp(out, "Custom 3") == 0, "and keeps counting");
	}

	// .bak is made once and never overwritten.
	{
		Sandbox sb(kFixture);
		Store store;
		store.LoadFile(sb.Path().c_str());
		Field f[1] = { { "speedPerSec", 1.0f } };
		store.Save("Speed", "A", f, 1);
		store.Save("Speed", "B", f, 1);
		Check(ReadBytes(sb.bak) == kFixture, "a second rewrite leaves the first .bak alone");

		Sandbox sb2(kFixture);
		WriteBytes(sb2.bak, "older backup");
		Store s2;
		s2.LoadFile(sb2.Path().c_str());
		s2.Save("Speed", "A", f, 1);
		Check(ReadBytes(sb2.bak) == "older backup", "an existing .bak is never replaced");

		Sandbox sb3(kFixture);
		Store s3;
		s3.LoadFile(sb3.Path().c_str());
		s3.Save("Speed", "A", f, 1);
		Store s4; // a later session: the .bak from the first stays
		s4.LoadFile(sb3.Path().c_str());
		s4.Save("Speed", "B", f, 1);
		Check(ReadBytes(sb3.bak) == kFixture, "nor across sessions");

		Sandbox fresh("", false);
		Store s5;
		s5.LoadFile(fresh.Path().c_str());
		s5.Save("Speed", "A", f, 1);
		Check(fs::exists(fresh.file) && !fs::exists(fresh.bak), "a new file has nothing to back up");
		Check(ReadBytes(fresh.file) == "[Speed:A]\r\nspeedPerSec=1.000000\r\n\r\n", "and is written in the old layout");
	}

	// An unreadable file is never written.
	{
		const char* bad[] = {
			"[Speed:Slow]\r\nspeedPerSec=600.000000\r\nthis line is nonsense\r\n\r\n[Speed:Fast]\r\nspeedPerSec=9.000000\r\n\r\n",
			"[Speed:Slow]\r\nspeedPerSec=abc\r\n\r\n[Speed:Fast]\r\nspeedPerSec=9.000000\r\n\r\n",
			"speedPerSec=1.000000\r\n[Speed:Fast]\r\nspeedPerSec=9.000000\r\n\r\n",
			"[Speed:Slow\r\nspeedPerSec=1.000000\r\n\r\n[Speed:Fast]\r\nspeedPerSec=9.000000\r\n\r\n",
			"[NoColon]\r\nspeedPerSec=1.000000\r\n\r\n[Speed:Fast]\r\nspeedPerSec=9.000000\r\n\r\n",
			"[Speed:Fast]\r\nspeedPerSec=9.000000\r\n\r\n[Speed:Fast]\r\nspeedPerSec=1.000000\r\n\r\n",
			"\xEF\xBB\xBF[Speed:Fast]\r\nspeedPerSec=9.000000\r\n\r\n",
		};
		for (const char* text : bad)
		{
			Sandbox sb(text);
			Logs logs;
			Store store(logs.Sink());
			store.LoadFile(sb.Path().c_str());
			Check(store.IsReadOnly(), "an unreadable line makes the store read-only");
			Check(logs.lines.size() == 1, "and it is logged once");
			Field f[1] = { { "speedPerSec", 1.0f } };
			Check(!store.Save("Speed", "Fast", f, 1), "Save is refused");
			Check(!store.Save("Speed", "New", f, 1), "a new Save is refused");
			Check(!store.Rename("Speed", "Fast", "Quick"), "Rename is refused");
			Check(!store.Delete("Speed", "Fast"), "Delete is refused");
			Check(ReadBytes(sb.file) == text, "the file bytes are unchanged");
			Check(!fs::exists(sb.bak) && !fs::exists(sb.tmp), "and no .bak or .tmp was made");
		}

		Sandbox sb(bad[0]);
		Store store;
		store.LoadFile(sb.Path().c_str());
		char names[kMaxListed][kMaxNameLen];
		Check(store.ListNames("Speed", names, kMaxListed) == 2, "readable entries still list");
		Field f[1] = { { "speedPerSec", 0 } };
		Check(store.Get("Speed", "Fast", f, 1) && f[0].value == 9.0f, "and still load");

		// Comments and blank lines are fine.
		Sandbox ok("; note\r\n# note\r\n\r\n[A:b]\r\nk=1.000000\r\n");
		Store s2;
		s2.LoadFile(ok.Path().c_str());
		Check(!s2.IsReadOnly(), "comments and blank lines are understood");
	}

	// The row: overwrite confirmation, new entry, built-ins, read-only message.
	{
		Sandbox sb(kFixture);
		Store store;
		store.LoadFile(sb.Path().c_str());
		RowState state;

		// Pick a saved preset from the dropdown: it applies.
		Fake::Reset();
		Fake::comboPick = "Fast Mode";
		Frame(store, state);
		Check(strcmp(state.selected, "Fast Mode") == 0 && g_applied[0] == 4000.0f && g_applied[1] == 3.5f, "picking a preset selects and applies it");

		// Save with a user preset selected: a confirmation, nothing written yet.
		Fake::Reset();
		Fake::clicks.insert("Save");
		Frame(store, state);
		Check(Fake::openPopups.count("Overwrite preset?##Speed") == 1, "Save with a saved preset selected opens the overwrite popup");
		Frame(store, state);
		Check(Fake::Said("You're saving over Fast Mode. Continue?"), "which asks the question with the name");
		Check(ReadBytes(sb.file) == kFixture, "and writes nothing yet");

		Fake::clicks.insert("Cancel##overwrite");
		Frame(store, state);
		Frame(store, state);
		Check(Fake::openPopups.empty() && ReadBytes(sb.file) == kFixture, "Cancel closes it and writes nothing");

		// Continue rewrites only that entry.
		Fake::clicks.insert("Save");
		Frame(store, state);
		Fake::clicks.insert("Continue##overwrite");
		Frame(store, state);
		const std::string expected = ReplaceSection(kFixture, "[Speed:Fast Mode]\r\n",
			"speedPerSec=111.000000\r\nboostMultiplier=2.500000\r\nacceleration=9000.000000\r\ndeceleration=0.250000\r\n");
		Check(ReadBytes(sb.file) == expected, "Continue rewrites only the selected entry and keeps the stored keys the live set lacks");
		Check(strcmp(state.selected, "Fast Mode") == 0, "and keeps it selected");

		// Nothing selected: a new auto-named entry. (A boost no saved preset has, so the row
		// does not pick one by itself.)
		g_live[1] = 9.0f;
		RowState fresh;
		g_suggest = "Custom";
		Fake::Reset();
		Fake::clicks.insert("Save");
		Frame(store, fresh);
		Check(Fake::openPopups.empty(), "Save with nothing selected asks nothing");
		Check(strcmp(fresh.selected, "Custom") == 0 && store.Exists("Speed", "Custom"), "and adds Custom");

		// The same row state again: Save now asks to overwrite Custom, and
		// choosing "(none)" first gets a second new entry.
		Fake::clicks.insert("Save");
		Frame(store, fresh);
		Check(Fake::openPopups.count("Overwrite preset?##Speed") == 1, "a second Save on the same row asks to overwrite");
		Fake::Reset();
		Fake::clicks.insert("Cancel##overwrite");
		Frame(store, fresh);
		Frame(store, fresh);
		Fake::Reset();
		Fake::comboPick = "(none)##nosel";
		Frame(store, fresh);
		Check(fresh.selected[0] == '\0', "picking (none) clears the selection");
		Fake::clicks.insert("Save");
		Frame(store, fresh);
		Check(Fake::openPopups.empty() && strcmp(fresh.selected, "Custom 2") == 0 && store.Exists("Speed", "Custom 2"),
			"and the next Save on the same row state adds Custom 2");

		// The overwrite question is about the name it was opened for, even if the
		// selection moves, or the entry vanishes, before Continue.
		{
			RowState st;
			snprintf(st.selected, sizeof(st.selected), "%s", "Custom");
			Fake::Reset();
			Fake::clicks.insert("Save");
			Frame(store, st);
			Frame(store, st);
			Check(Fake::Said("You're saving over Custom. Continue?"), "the question names the entry it was opened for");
			snprintf(st.selected, sizeof(st.selected), "%s", "Custom 2"); // a hotkey moved the selection
			g_live[0] = 222.0f;
			Fake::clicks.insert("Continue##overwrite");
			Frame(store, st);
			g_live[0] = 111.0f;
			const std::string after = ReadBytes(sb.file);
			Check(after.find("[Speed:Custom]\r\nspeedPerSec=222.000000\r\n") != std::string::npos
				&& after.find("[Speed:Custom 2]\r\nspeedPerSec=111.000000\r\n") != std::string::npos,
				"Continue saves over the entry the question named, not the moved selection");

			RowState gone;
			snprintf(gone.selected, sizeof(gone.selected), "%s", "Custom 2");
			Fake::Reset();
			Fake::clicks.insert("Save");
			Frame(store, gone);
			store.Delete("Speed", "Custom 2");
			const std::string beforeGone = ReadBytes(sb.file);
			Fake::clicks.insert("Continue##overwrite");
			Frame(store, gone);
			Check(Fake::Said("That preset no longer exists, so nothing was saved.") && ReadBytes(sb.file) == beforeGone
				&& !store.Exists("Speed", "Custom 2"), "a vanished target writes nothing and says so");
		}

		// The Delete question likewise keeps the name it was opened for.
		{
			RowState st;
			snprintf(st.selected, sizeof(st.selected), "%s", "Custom");
			Fake::Reset();
			Fake::clicks.insert("Delete");
			Frame(store, st);
			Frame(store, st);
			snprintf(st.selected, sizeof(st.selected), "%s", "Slow"); // the selection moved
			Fake::clicks.insert("Delete##confirm");
			Frame(store, st);
			Check(!store.Exists("Speed", "Custom") && store.Exists("Speed", "Slow"),
				"Delete removes the entry the question named, not the moved selection");
			Check(strcmp(st.selected, "Slow") == 0, "and leaves the new selection alone");
		}

		// A built-in preset in play: the suggested name carries its label; the built-in is never written.
		RowState fresh3;
		g_suggest = "Normal Custom";
		Fake::clicks.insert("Save");
		Frame(store, fresh3);
		Check(store.Exists("Speed", "Normal Custom") && !store.Exists("Speed", "Normal"), "a built-in selection makes a new '<built-in> Custom' entry");

		// A saved entry that happens to share a built-in's name is not overwritten either.
		RowState legacy;
		Fake::comboPick = "Slow";
		Frame(store, legacy);
		g_suggest = "Slow Custom";
		Fake::clicks.insert("Save");
		const std::string beforeSlow = ReadBytes(sb.file);
		Frame(store, legacy);
		Check(Fake::openPopups.empty() && store.Exists("Speed", "Slow Custom") && strcmp(legacy.selected, "Slow Custom") == 0,
			"a selected name that is a built-in goes the new-entry way");
		Check(ReadBytes(sb.file).find("[Speed:Slow]\r\nspeedPerSec=600.000000\r\nboostMultiplier=2.000000\r\nacceleration=1500.000000\r\ndeceleration=1500.000000\r\n\r\n") != std::string::npos
			&& beforeSlow.size() < ReadBytes(sb.file).size(), "and the built-in-named entry is left as it was");

		// Rename and Delete through the row.
		Fake::Reset();
		Fake::clicks.insert("Delete");
		Frame(store, state);
		Check(Fake::openPopups.count("Delete preset?##Speed") == 1, "Delete asks first");
		Frame(store, state);
		Fake::clicks.insert("Delete##confirm");
		Frame(store, state);
		Check(!store.Exists("Speed", "Fast Mode") && state.selected[0] == '\0', "and then deletes");
		g_live[1] = 2.5f;
	}

	// The row on a read-only store: messages, no popups, no writes.
	{
		const std::string bad = "[Speed:Slow]\r\nspeedPerSec=600.000000\r\nnonsense\r\n\r\n";
		Sandbox sb(bad);
		Store store;
		store.LoadFile(sb.Path().c_str());
		RowState state;
		Fake::Reset();
		Fake::comboPick = "Slow";
		Frame(store, state);
		const char* msg = "Presets file has lines this plugin can't read; it was left unchanged.";
		const char* buttons[] = { "Save", "Rename", "Delete" };
		for (const char* b : buttons)
		{
			Fake::Reset();
			Fake::clicks.insert(b);
			Frame(store, state);
			Frame(store, state);
			Check(Fake::Said(msg), "each button shows the read-only message");
			Check(Fake::openPopups.empty() && !state.renaming, "and opens nothing");
		}
		Check(ReadBytes(sb.file) == bad && !fs::exists(sb.bak), "the file is untouched");
	}

	// A file that exists but cannot be read at load: read-only for the session.
	{
		const std::string original = "[Speed:Mine]\r\nspeedPerSec=7.000000\r\n\r\n[Speed:Other]\r\nspeedPerSec=8.000000\r\n\r\n";
		Sandbox sb(original);
		WriteBytes(sb.bak, "older backup");
		Logs logs;
		Store store(logs.Sink());
		HANDLE lock = CreateFileW(sb.file.wstring().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		Check(lock != INVALID_HANDLE_VALUE, "the test can lock the file");
		store.LoadFile(sb.Path().c_str());
		CloseHandle(lock); // the lock is gone before Save, as with a scan that has finished

		Check(store.IsReadOnly(), "an existing file that could not be read makes the store read-only");
		Field f[1] = { { "speedPerSec", 1.0f } };
		Check(!store.Save("Speed", "Custom", f, 1), "Save is refused");
		Check(!store.Save("Speed", "Mine", f, 1), "Save over is refused");
		Check(!store.Rename("Speed", "Mine", "Yours"), "Rename is refused");
		Check(!store.Delete("Speed", "Mine"), "Delete is refused");
		Check(ReadBytes(sb.file) == original, "the file bytes are unchanged");
		Check(ReadBytes(sb.bak) == "older backup", "and so is the .bak");
		Check(!fs::exists(sb.tmp), "and no .tmp was made");

		// The row says so instead of saving.
		RowState state;
		Fake::Reset();
		Fake::clicks.insert("Save");
		Frame(store, state);
		Check(Fake::openPopups.empty()
			&& Fake::Said("Presets file could not be read at start. Restart the game to use saved presets."),
			"the row says the file could not be read instead of saving");
		Check(ReadBytes(sb.file) == original, "and writes nothing");
	}

	// Concurrent use of one store (the panel and a hotkey).
	{
		Sandbox sb(kFixture);
		Store store;
		store.LoadFile(sb.Path().c_str());
		bool ok = true;
		std::thread reader([&] {
			for (int i = 0; i < 2000; ++i)
			{
				char names[kMaxListed][kMaxNameLen];
				Field f[1] = { { "speedPerSec", 0 } };
				store.ListNames("Speed", names, kMaxListed);
				store.Get("Speed", "Slow", f, 1);
			}
		});
		for (int i = 0; i < 50; ++i)
		{
			Field f[1] = { { "speedPerSec", static_cast<float>(i) } };
			ok &= store.Save("Speed", "Slow", f, 1);
		}
		reader.join();
		Check(ok, "a reader thread and a writer thread share one store");
	}

	// Select on load: a fresh row selects the first saved preset equal to the live values.
	{
		const float savedLive0 = g_live[0], savedLive1 = g_live[1];
		auto setLive = [](float a, float b) { g_live[0] = a; g_live[1] = b; };
		const std::string head = "[Speed:A]\r\nspeedPerSec=10.000000\r\nboostMultiplier=1.000000\r\n\r\n";
		const std::string b = "[Speed:B]\r\nspeedPerSec=20.000000\r\nboostMultiplier=2.000000\r\n\r\n";
		const std::string c = "[Speed:C]\r\nspeedPerSec=20.000000\r\nboostMultiplier=2.000000\r\n\r\n";
		auto selectedOnFirstFrame = [&](const std::string& text, float l0, float l1) {
			Sandbox sb(text);
			Store store;
			store.LoadFile(sb.Path().c_str());
			setLive(l0, l1);
			RowState fresh;
			Frame(store, fresh);
			const bool untouched = ReadBytes(sb.file) == text && !fs::exists(sb.bak) && !fs::exists(sb.tmp);
			Check(untouched, "select on load writes no file");
			return std::string(fresh.selected);
		};

		Check(selectedOnFirstFrame(head + b, 20.0f, 2.0f) == "B", "a fresh row whose live values equal preset B (not first) selects B");
		Check(selectedOnFirstFrame(head + b + c, 20.0f, 2.0f) == "B", "equal to two presets selects the first in list order");
		Check(selectedOnFirstFrame(head + b, 99.0f, 99.0f).empty(), "no match stays (none)");
		Check(selectedOnFirstFrame(head + b, 20.0f, 3.0f).empty(), "one differing key is no match");

		const std::string shared = "[Speed:K]\r\nspeedPerSec=30.000000\r\nextra=5.000000\r\n\r\n";
		Check(selectedOnFirstFrame(shared, 30.0f, 7.0f) == "K", "matching only on the keys both share");
		Check(selectedOnFirstFrame("[Speed:M]\r\nspeedPerSec=31.000000\r\nextra=5.000000\r\n\r\n", 30.0f, 7.0f).empty(), "a shared key that differs is no match");
		Check(selectedOnFirstFrame("[Speed:Z]\r\nextra=5.000000\r\n\r\n", 30.0f, 7.0f).empty(), "no shared key is no match");
		Check(selectedOnFirstFrame("[Speed:Slow]\r\nspeedPerSec=20.000000\r\nboostMultiplier=2.000000\r\n\r\n", 20.0f, 2.0f).empty(), "a built-in name is never auto-selected");
		Check(selectedOnFirstFrame("[Speed:T]\r\nspeedPerSec=1.2345674\r\nboostMultiplier=2.000000\r\n\r\n", 1.2345671f, 2.0f) == "T", "values are compared as the text the store writes");

		// NaN and Inf compare as the store writes them.
		const std::string special = "[Speed:N]\r\nspeedPerSec=nan\r\nboostMultiplier=inf\r\n\r\n";
		Check(selectedOnFirstFrame(special, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()) == "N", "NaN and Inf live values match a preset that stores them");
		Check(selectedOnFirstFrame(special, std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::infinity()).empty(), "+Inf and -Inf differ");

		// "(none)" picked on purpose stays until an apply; a tweak after auto-select keeps the selection.
		{
			Sandbox sb(head + b + c);
			Store store;
			store.LoadFile(sb.Path().c_str());
			setLive(20.0f, 2.0f);
			RowState row;
			Frame(store, row);
			Check(strcmp(row.selected, "B") == 0, "the row starts on B");

			setLive(25.0f, 2.0f); // tweak: B stays selected
			Frame(store, row);
			Check(strcmp(row.selected, "B") == 0, "tweaking a value keeps the auto-selection");
			Fake::Reset();
			Fake::clicks.insert("Save");
			Frame(store, row);
			Check(Fake::openPopups.count("Overwrite preset?##Speed") == 1, "Save after auto-select goes to the overwrite path");
			Frame(store, row);
			Check(Fake::Said("You're saving over B. Continue?"), "and names the auto-selected preset");
			Fake::Reset();

			Fake::comboPick = "(none)##nosel";
			Frame(store, row);
			Check(row.selected[0] == '\0', "picking (none) clears the selection");
			setLive(20.0f, 2.0f); // live now equals B again
			Frame(store, row);
			Frame(store, row);
			Check(row.selected[0] == '\0', "(none) picked explicitly is not re-selected");
			row.ClearSelection(); // a built-in applied
			Frame(store, row);
			Check(strcmp(row.selected, "B") == 0, "after an apply the row looks again");

			Fake::Reset();
			Fake::comboPick = "(none)##nosel";
			Frame(store, row);
			Fake::Reset();
			Fake::comboPick = "C";
			Frame(store, row);
			Check(strcmp(row.selected, "C") == 0, "picking a saved preset applies it and selects it");
			Fake::Reset();
			Check(ReadBytes(sb.file) == head + b + c && !fs::exists(sb.bak), "the store file is unchanged after all of it");
		}

		// The look for a match is gated on the live values changing: a preset that
		// appears while they stay put is found only after they change.
		{
			Sandbox sb(head);
			Store store;
			store.LoadFile(sb.Path().c_str());
			setLive(20.0f, 2.0f);
			RowState row;
			Frame(store, row);
			Check(row.selected[0] == '\0', "no match yet");
			Field f[2] = { { "speedPerSec", 20.0f }, { "boostMultiplier", 2.0f } };
			store.Save("Speed", "B", f, 2);
			Frame(store, row);
			Check(row.selected[0] == '\0', "unchanged live values are not scanned again");
			setLive(20.0f, 3.0f);
			Frame(store, row);
			setLive(20.0f, 2.0f);
			Frame(store, row);
			Check(strcmp(row.selected, "B") == 0, "a change in the live values triggers the next scan");
		}
		setLive(savedLive0, savedLive1);
	}

	// Optional: round trip on a real file.
	{
		const char* real = argc > 1 ? argv[1] : getenv("PLUGIN_PRESETS_REAL_FILE");
		if (!real || !real[0])
		{
			printf("  real-file round trip: skipped (no path given)\n");
		}
		else
		{
			const std::string original = ReadBytes(real);
			Sandbox sb(original);
			Logs logs;
			Store store(logs.Sink());
			store.LoadFile(sb.Path().c_str());
			const bool before = ReadBytes(sb.file) == original;
			const bool saved = SaveAllUnchanged(store, original);
			const bool same = ReadBytes(sb.file) == original;
			printf("  real-file round trip on %s (%zu bytes, %zu sections): readOnly=%d, saved=%d, identical=%d\n",
				real, original.size(), ScanSections(original).size(), store.IsReadOnly() ? 1 : 0, saved ? 1 : 0, same ? 1 : 0);
			Check(!original.empty(), "real file is readable");
			Check(before, "real file: load did not write");
			Check(!store.IsReadOnly() && logs.lines.empty(), "real file: the reader understands every line");
			Check(saved && same, "real file: load then save is byte-identical");
			Check(ReadBytes(sb.bak) == original, "real file: the .bak holds the original");
		}
	}

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
