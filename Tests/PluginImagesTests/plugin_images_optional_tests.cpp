// Plugin images tests for the optional interface path. Built by the Optional and
// Optional74 configurations of PluginImagesTests.vcxproj with
// PLUGIN_NEEDS_INTERFACE below 77 and PLUGIN_OPTIONAL_INTERFACE=77, which
// hides IPluginHooks::Images, so PluginImages::Image finds the table through
// PluginOptImages. The loader is faked: the hooks are a zeroed block the size of
// the v77 table, with the Images slot written at its frozen offset.
#include <plugin_images.h>

#include <cstdio>
#include <cstring>

PLUGIN_EXPORT_HOST_INFO()

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
	int   g_dummies = 0, g_images = 0, g_gets = 0;
	float g_w = 0.0f, g_h = 0.0f;

	void Reset() { g_dummies = g_images = g_gets = 0; g_w = g_h = 0.0f; }

	void FakeDummy(float w, float h) { ++g_dummies; g_w = w; g_h = h; }
	bool FakeIsRectVisible(float, float) { return true; }
	void FakeImage(PluginTextureHandle, float w, float h) { ++g_images; g_w = w; g_h = h; }
	PluginTextureHandle FakeGetSized(PluginImageId, int, int*, int*)
	{
		++g_gets;
		return reinterpret_cast<PluginTextureHandle>(static_cast<size_t>(1));
	}

	// titleBarFn is read only from a plugin that needs interface 75 or later.
	template <class Desc>
	concept HasTitleBar = requires(Desc* desc) { desc->titleBarFn; };

	static_assert(HasTitleBar<PluginPanelDesc> == (PLUGIN_NEEDS_INTERFACE >= 75),
		"titleBarFn is hidden below interface 75");
	static_assert(PLUGIN_OPTIONAL_INTERFACE == 77, "this file tests the optional 77 path");
}

int main()
{
	printf("plugin images, optional path (need %d)\n", PLUGIN_NEEDS_INTERFACE);

	IModLoaderImGui imgui{};
	imgui.Dummy         = &FakeDummy;
	imgui.IsRectVisible = &FakeIsRectVisible;

	IPluginImGuiTextures textures{};
	textures.Image = &FakeImage;

	IPluginImages images{};
	images.GetSized = &FakeGetSized;

	// The hooks the loader hands out: the Images slot is past the end of the
	// IPluginHooks this build sees, so write it by offset.
	alignas(IPluginHooks) unsigned char block[256] = {};
	IPluginHooks* hooks = reinterpret_cast<IPluginHooks*>(block);
	hooks->ImGuiTextures = &textures;
	IPluginImages* table = &images;
	memcpy(block + PLUGIN_OPT_OFFSET_IMAGES, &table, sizeof(table));

	Check(!PluginHostHas(77), "before the loader reports, level 77 is not known");
	Check(PluginOptImages(hooks) == nullptr, "before the loader reports, PluginOptImages is null");

	Reset();
	PluginImages::Image(&imgui, hooks, 7, 40.0f, 40.0f);
	Check(g_dummies == 1 && g_w == 40.0f && g_h == 40.0f, "before the report, the space is reserved");
	Check(g_images == 0 && g_gets == 0, "before the report, the image library is not asked");

	const PluginHostInfo older{ sizeof(PluginHostInfo), 76 };
	PluginSetHostInfo(&older);
	Check(!PluginHostHas(77) && PluginHostHas(75), "a loader reporting 76 does not have 77");
	Reset();
	PluginImages::Image(&imgui, hooks, 7, 40.0f, 40.0f);
	Check(g_dummies == 1 && g_images == 0, "a loader reporting 76 still reserves the space");

	const PluginHostInfo host{ sizeof(PluginHostInfo), 80 };
	PluginSetHostInfo(&host);
	Check(PluginHostHas(77) && PluginHostHas(80), "after the report, level 77 is known");
	Check(PluginOptImages(hooks) == table, "after the report, PluginOptImages finds the table");
	Check(PluginOptImages(nullptr) == nullptr, "null hooks give no table");

	Reset();
	PluginImages::Image(&imgui, hooks, 7, 40.0f, 40.0f);
	Check(g_images == 1 && g_dummies == 0, "after the report, the image is drawn");
	Check(g_gets == 1 && g_w == 40.0f && g_h == 40.0f, "after the report, it is drawn at the size asked for");

	// A server loader leaves the slot null.
	const IPluginImages* none = nullptr;
	memcpy(block + PLUGIN_OPT_OFFSET_IMAGES, &none, sizeof(none));
	Reset();
	PluginImages::Image(&imgui, hooks, 7, 40.0f, 40.0f);
	Check(g_dummies == 1 && g_images == 0, "a null Images slot reserves the space");

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
