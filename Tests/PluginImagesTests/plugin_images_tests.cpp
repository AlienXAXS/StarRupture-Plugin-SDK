// Plugin images tests. The helper in include/plugin_images.h takes the hooks
// as a template parameter, so a fake image table and fake draw calls stand in
// for the loader.
#include <plugin_images.h>

#include <cstdio>
#include <cstring>
#include <limits>

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

namespace
{
	// What the fake draw calls saw.
	struct Seen
	{
		int   dummies = 0;
		int   images  = 0;
		float w = 0.0f, h = 0.0f;
		int   gets    = 0;
		int   askedSize = 0;
		int   copyW = 0, copyH = 0; // what the fake loader answers for a copy
		int   states  = 0;
		bool  onScreen = true;
		PluginTextureHandle handle = nullptr;
		int   state   = 0; // PLUGIN_IMAGE_NONE
	} g_seen;

	void Reset() { g_seen = Seen{}; }

	void FakeDummy(float w, float h) { ++g_seen.dummies; g_seen.w = w; g_seen.h = h; }
	bool FakeIsRectVisible(float, float) { return g_seen.onScreen; }
	void FakeImage(PluginTextureHandle, float w, float h) { ++g_seen.images; g_seen.w = w; g_seen.h = h; }
	PluginTextureHandle FakeGetSized(uint32_t, int size, int* w, int* h)
	{
		++g_seen.gets;
		g_seen.askedSize = size;
		if (w) *w = g_seen.copyW;
		if (h) *h = g_seen.copyH;
		return g_seen.handle;
	}
	int FakeState(uint32_t) { ++g_seen.states; return g_seen.state; }

	struct FakeImages
	{
		PluginTextureHandle (*GetSized)(uint32_t, int, int*, int*);
		int (*State)(uint32_t);
	};

	struct HooksWithTable
	{
		FakeImages*            Images;
		IPluginImGuiTextures*  ImGuiTextures;
	};

	struct HooksWithoutTable
	{
		IPluginImGuiTextures* ImGuiTextures;
	};

	constexpr int kWaiting = 1, kNoImage = 3; // PLUGIN_IMAGE_WAITING, PLUGIN_IMAGE_NO_IMAGE
	const float kSize = PluginImages::kMinSize;
}

namespace
{
	// A fake of the cursor and group maths IconRow relies on, with a call log.
	struct Row
	{
		char  log[256] = "";
		int   pushes = 0, pops = 0, begins = 0, ends = 0;
		float y = 0.0f, lineTop = 0.0f, lineH = 0.0f, spacing = 6.0f, textH = 15.0f;
		float groupStart[4] = {}, groupBottom[4] = {}, savedLineH[4] = {};
		int   depth = 0;
		float imageY = -1.0f;
	} g_row;

	void Log(char c) { size_t n = strlen(g_row.log); g_row.log[n] = c; g_row.log[n + 1] = 0; }
	void NoteItem(float h)
	{
		g_row.lineTop = g_row.y;
		if (h > g_row.lineH) g_row.lineH = h;
		if (g_row.depth > 0 && g_row.y + h > g_row.groupBottom[g_row.depth - 1])
			g_row.groupBottom[g_row.depth - 1] = g_row.y + h;
		g_row.y += g_row.lineH + g_row.spacing;
		g_row.lineH = 0.0f;
	}
	void RowPush(const char*) { ++g_row.pushes; Log('P'); }
	void RowPop() { ++g_row.pops; Log('p'); }
	void RowBegin()
	{
		++g_row.begins; Log('B');
		g_row.groupStart[g_row.depth] = g_row.y;
		g_row.groupBottom[g_row.depth] = g_row.y;
		g_row.savedLineH[g_row.depth] = g_row.lineH; // a group opens a fresh line
		g_row.lineH = 0.0f;
		++g_row.depth;
	}
	void RowEnd()
	{
		++g_row.ends; Log('E');
		--g_row.depth;
		const float top = g_row.groupStart[g_row.depth], h = g_row.groupBottom[g_row.depth] - top;
		g_row.y = top;
		g_row.lineH = g_row.savedLineH[g_row.depth];
		NoteItem(h);
	}
	void RowSameLine(float, float) { Log('S'); g_row.y = g_row.lineTop; }
	float RowGetY() { return g_row.y; }
	void RowSetY(float y) { Log('Y'); g_row.y = y; }
	float RowTextH() { return g_row.textH; }
	float RowTextHS() { return g_row.textH + g_row.spacing; }
	bool RowVisible(float, float) { return true; }
	void RowImage(PluginTextureHandle, float, float h)
	{
		Log('I');
		g_row.imageY = g_row.y;
		NoteItem(h);
		g_row.lineH = h; // SameLine restores the picture's height as the line height
	}
	void RowText() { Log('T'); NoteItem(g_row.textH); }
}

int main()
{
	printf("plugin images\n");

	IModLoaderImGui imgui{};
	imgui.Dummy         = &FakeDummy;
	imgui.IsRectVisible = &FakeIsRectVisible;

	IPluginImGuiTextures textures{};
	textures.Image = &FakeImage;

	FakeImages images{ &FakeGetSized, &FakeState };
	HooksWithTable hooks{ &images, &textures };
	PluginTextureHandle handle = reinterpret_cast<PluginTextureHandle>(static_cast<size_t>(1));

	{
		Reset();
		g_seen.handle = handle;
		PluginImages::Image(&imgui, &hooks, 7, kSize, kSize);
		Check(g_seen.images == 1 && g_seen.dummies == 0, "a ready image is drawn");
		Check(g_seen.w == kSize && g_seen.h == kSize, "the image is drawn at the size asked for");
	}

	{
		Reset();
		g_seen.handle = handle;
		PluginImages::Image(&imgui, &hooks, 7, 40.4f, 40.4f);
		Check(g_seen.askedSize == 40, "the library is asked for the size to be drawn, in whole pixels");
		Check(g_seen.w == 40.0f && g_seen.h == 40.0f, "a stored picture is drawn scaled into the whole-pixel box");
	}

	{
		Reset();
		g_seen.handle = handle;
		g_seen.copyW  = 40;
		g_seen.copyH  = 40;
		PluginImages::Image(&imgui, &hooks, 7, 40.0f, 40.0f);
		Check(g_seen.images == 1 && g_seen.w == 40.0f && g_seen.h == 40.0f, "a copy of exactly the size asked for is drawn at that size");
	}

	{
		Reset();
		g_seen.state = kWaiting;
		PluginImages::Image(&imgui, &hooks, 7, 28.0f, 40.0f);
		Check(g_seen.dummies == 1 && g_seen.w == 28.0f && g_seen.h == 40.0f, "a non-square box reserves its own shape");
		float reservedW = g_seen.w, reservedH = g_seen.h;

		Reset();
		g_seen.handle = handle;
		g_seen.copyW  = 40;
		g_seen.copyH  = 40;
		PluginImages::Image(&imgui, &hooks, 7, 28.0f, 40.0f);
		Check(g_seen.askedSize == 40, "a non-square box asks for its longer side");
		Check(g_seen.images == 1 && g_seen.w == reservedW && g_seen.h == reservedH, "a copy is drawn in the box that was reserved, not at the copy's size");
	}

	{
		Reset();
		g_seen.handle = handle;
		g_seen.copyW  = 56;
		g_seen.copyH  = 56;
		PluginImages::Image(&imgui, &hooks, 7, 32.0f, 32.0f);
		Check(g_seen.images == 1 && g_seen.w == 32.0f && g_seen.h == 32.0f, "a copy of another size than asked is drawn in the box");
	}

	{
		Reset();
		g_seen.handle = handle;
		PluginImages::Image(&imgui, &hooks, 7, 10.0f, 10.0f);
		Check(g_seen.askedSize == 28, "a size under the minimum is asked for at the minimum");
		Check(g_seen.w == 28.0f && g_seen.h == 28.0f, "a size under the minimum is drawn at the minimum");
	}

	{
		Reset();
		g_seen.state = kWaiting;
		PluginImages::Image(&imgui, &hooks, 7, 10.0f, 10.0f);
		Check(g_seen.dummies == 1 && g_seen.w == 28.0f && g_seen.h == 28.0f, "the space reserved is the minimum too");
	}

	{
		Reset();
		g_seen.onScreen = false;
		PluginImages::Image(&imgui, &hooks, 7, 10.0f, 10.0f);
		Check(g_seen.dummies == 1 && g_seen.w == 28.0f && g_seen.h == 28.0f, "an off-screen rectangle reserves the minimum too");
	}

	{
		Reset();
		g_seen.state = kWaiting;
		PluginImages::Image(&imgui, &hooks, 7, kSize, kSize);
		Check(g_seen.images == 0 && g_seen.dummies == 1, "an image still loading reserves its space");
		Check(g_seen.w == kSize && g_seen.h == kSize, "the reserved space is the size asked for");
	}

	{
		Reset();
		g_seen.onScreen = false;
		g_seen.handle   = handle;
		PluginImages::Image(&imgui, &hooks, 7, kSize, kSize);
		Check(g_seen.gets == 0 && g_seen.states == 0, "an off-screen rectangle does not ask the library");
		Check(g_seen.images == 0 && g_seen.dummies == 1, "an off-screen rectangle reserves its space");
	}

	{
		Reset();
		g_seen.state = kNoImage;
		PluginImages::Image(&imgui, &hooks, 7, kSize, kSize);
		Check(g_seen.dummies == 1 && g_seen.images == 0, "no image reserves its space without CollapseNoImage");
	}

	{
		Reset();
		g_seen.state = kNoImage;
		PluginImages::Image(&imgui, &hooks, 7, kSize, kSize, PluginImages::CollapseNoImage);
		Check(g_seen.dummies == 0 && g_seen.images == 0, "no image takes no space with CollapseNoImage");
	}

	{
		Reset();
		g_seen.state = kWaiting;
		PluginImages::Image(&imgui, &hooks, 7, kSize, kSize, PluginImages::CollapseNoImage);
		Check(g_seen.dummies == 1, "CollapseNoImage keeps the space of an image still loading");
	}

	{
		Reset();
		g_seen.onScreen = false;
		g_seen.state    = kNoImage;
		PluginImages::Image(&imgui, &hooks, 7, kSize, kSize, PluginImages::CollapseNoImage);
		Check(g_seen.dummies == 1, "an off-screen rectangle keeps its space even with CollapseNoImage");
	}

	{
		Reset();
		g_seen.handle = handle;
		HooksWithTable noTextures{ &images, nullptr };
		PluginImages::Image(&imgui, &noTextures, 7, kSize, kSize);
		Check(g_seen.images == 0 && g_seen.dummies == 1, "a missing texture interface reserves the space");
	}

	{
		Reset();
		HooksWithTable nullTable{ nullptr, &textures };
		PluginImages::Image(&imgui, &nullTable, 7, kSize, kSize);
		Check(g_seen.images == 0 && g_seen.dummies == 1, "a null table (server or generic build) reserves the space");
	}

	{
		Reset();
		HooksWithoutTable older{ &textures };
		PluginImages::Image(&imgui, &older, 7, kSize, kSize, PluginImages::CollapseNoImage);
		Check(g_seen.images == 0 && g_seen.dummies == 1, "hooks without the table member reserve the space");
	}

	{
		Reset();
		IPluginHooks real{};
		PluginImages::Image(&imgui, &real, 7, kSize, kSize);
		Check(g_seen.images == 0 && g_seen.dummies == 1, "the SDK hooks type compiles and reserves the space when empty");
	}

	{
		Reset();
		HooksWithTable* none = nullptr;
		PluginImages::Image(&imgui, none, 7, kSize, kSize);
		Check(g_seen.dummies == 1, "null hooks reserve the space");
		PluginImages::Image(static_cast<IModLoaderImGui*>(nullptr), &hooks, 7, kSize, kSize);
		Check(g_seen.dummies == 1 && g_seen.images == 0, "null imgui does nothing");
	}

	{
		using PluginImages::SizeForFont;
		const float d = PluginImages::kDefaultFontSize;
		Check(SizeForFont(28.0f, d, d) == 28.0f && SizeForFont(40.0f, d, d) == 40.0f, "the default font gives the same size");
		Check(SizeForFont(28.0f, 18.0f, d) == 34.0f, "an 18 px font gives 34 for 28");
		Check(SizeForFont(40.0f, 18.0f, d) == 48.0f, "an 18 px font gives 48 for 40");
		Check(SizeForFont(28.0f, 7.0f, d) == PluginImages::kMinSize, "a small font is raised to kMinSize");
		Check(SizeForFont(40.0f, 150.0f, d) == PluginImages::kMaxSize, "a large font is capped at kMaxSize");
		Check(SizeForFont(45.0f, 20.0f, 15.0f) == 60.0f, "an exact result is kept");
		Check(SizeForFont(31.5f, 20.0f, 20.0f) == 32.0f, "31.5 rounds up to 32");
		Check(SizeForFont(31.4f, 20.0f, 20.0f) == 31.0f, "31.4 rounds down to 31");
		IModLoaderImGui big{};
		big.GetFontSize = []() { return 18.0f; };
		Check(SizeForFont(&big, 40.0f) == 48.0f, "the wrapper reads the loader's font size");
		Check(SizeForFont(28.0f, 0.0f, d) == 28.0f, "font 0 gives 28");
		Check(SizeForFont(28.0f, -5.0f, d) == 28.0f, "a negative font gives 28");
		Check(SizeForFont(static_cast<IModLoaderImGui*>(nullptr), 40.0f) == 40.0f, "null imgui uses the default font");
		Check(SizeForFont(static_cast<IModLoaderImGui*>(nullptr), 5.0f) == PluginImages::kMinSize, "null imgui clamps sizeAtDefault");
		const float nan = std::numeric_limits<float>::quiet_NaN();
		const float inf = std::numeric_limits<float>::infinity();
		const float rn = SizeForFont(nan, 18.0f, d), ri = SizeForFont(28.0f, inf, d);
		Check(rn >= PluginImages::kMinSize && rn <= PluginImages::kMaxSize, "NaN stays in range");
		Check(ri >= PluginImages::kMinSize && ri <= PluginImages::kMaxSize, "inf stays in range");
	}

	{
		using PluginImages::SizeForLines;
		using PluginImages::LinesHeight;
		const float nan = std::numeric_limits<float>::quiet_NaN();
		Check(SizeForLines(1, 40.0f, 6.0f) == 40.0f, "1 line is one line height");
		Check(SizeForLines(2, 20.0f, 6.0f) == 46.0f, "2 lines add one spacing");
		Check(SizeForLines(3, 20.0f, 4.0f) == 68.0f, "3 lines add two spacings");
		Check(LinesHeight(2, 15.0f, 6.0f) == 36.0f && LinesHeight(1, 15.0f, 6.0f) == 15.0f, "default theme: 15 and 36 before the floor");
		Check(SizeForLines(1, 15.0f, 6.0f) == PluginImages::kMinSize, "default theme, 1 line: the 28 floor");
		Check(SizeForLines(2, 15.0f, 6.0f) == 36.0f, "default theme, 2 lines: 36");
		Check(SizeForLines(0, 40.0f, 6.0f) == 40.0f && SizeForLines(-3, 40.0f, 6.0f) == 40.0f, "a count under 1 is 1");
		Check(SizeForLines(2, 20.4f, 6.0f) == 47.0f, "rounds to a whole pixel");
		Check(SizeForLines(1, 5.0f, 6.0f) == PluginImages::kMinSize, "a small size is raised to kMinSize");
		Check(SizeForLines(4, 100.0f, 6.0f) == PluginImages::kMaxSize, "a large size is capped at kMaxSize");
		Check(SizeForLines(2, nan, 6.0f) == PluginImages::kMinSize && SizeForLines(2, 15.0f, nan) == PluginImages::kMinSize, "NaN lands on kMinSize");
		IModLoaderImGui theme{};
		theme.GetTextLineHeight = []() { return 15.0f; };
		theme.GetTextLineHeightWithSpacing = []() { return 21.0f; };
		Check(SizeForLines(&theme, 1) == 28.0f && SizeForLines(&theme, 2) == 36.0f, "the wrapper reads the loader's theme");
		Check(SizeForLines(static_cast<IModLoaderImGui*>(nullptr), 2) == 36.0f, "null imgui uses the default theme");
	}

	{
		IModLoaderImGui g{};
		g.PushIDStr = &RowPush; g.PopID = &RowPop; g.BeginGroup = &RowBegin; g.EndGroup = &RowEnd;
		g.SameLine = &RowSameLine; g.GetCursorPosY = &RowGetY; g.SetCursorPosY = &RowSetY;
		g.GetTextLineHeight = &RowTextH; g.GetTextLineHeightWithSpacing = &RowTextHS;
		g.IsRectVisible = &RowVisible;
		IPluginImGuiTextures t{};
		t.Image = &RowImage;
		FakeImages imgs{ &FakeGetSized, &FakeState };
		HooksWithTable h{ &imgs, &t };

		// Picture taller than the text: 1 line, 15 high text, 28 picture.
		Reset();
		g_seen.handle = handle;
		g_row = Row{};
		g_row.y = 100.0f;
		PluginImages::IconRow(&g, &h, "row", 7, 1, []() { RowText(); });
		Check(g_row.pushes == 1 && g_row.pops == 1 && g_row.begins == 1 && g_row.ends == 1, "IconRow balances Push/Pop and Begin/EndGroup");
		Check(strcmp(g_row.log, "PISBYTEp") == 0, "IconRow call order: push, image, same line, group, shift, text, end, pop");
		Check(g_row.y == 100.0f + 28.0f + g_row.spacing, "picture taller: the next line is at startY + size + spacing");

		// Text taller than the picture: 300 high text, picture capped at kMaxSize.
		g_row = Row{};
		g_row.y = 100.0f;
		g_row.textH = 300.0f;
		PluginImages::IconRow(&g, &h, "row", 7, 1, []() { RowText(); });
		Check(g_row.pushes == 1 && g_row.pops == 1 && g_row.begins == 1 && g_row.ends == 1, "IconRow balances pairs, text taller");
		Check(g_row.y == 100.0f + 300.0f + g_row.spacing, "text taller: the next line is at startY + text height + spacing");
		Check(strcmp(g_row.log, "PYISYBTEp") == 0 && g_row.imageY == 122.0f, "text taller: the picture is shifted by whole pixels before it is drawn");

		PluginImages::IconRow(&g, &h, nullptr, 7, 1, []() {});
		Check(g_row.pushes == 2 && g_row.pops == 2, "a null id does not crash and still balances");
	}

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
