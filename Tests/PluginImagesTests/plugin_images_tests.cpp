// Plugin images tests. The helper in include/plugin_images.h takes the hooks
// as a template parameter, so a fake image table and fake draw calls stand in
// for the loader.
#include <plugin_images.h>

#include <cstdio>
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

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
