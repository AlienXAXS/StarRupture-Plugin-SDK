#pragma once

// Draws a game image from the loader's image library, or reserves its space.
//
// Include after plugin_interface.h. Call from a render callback.
//
//   PluginImages::Image(imgui, hooks, id, PluginImages::kMinSize, PluginImages::kMinSize);
//
// id comes from hooks->Images->Item, Path or FromTexture. Each call does the
// same four steps:
//
//   1. If the rectangle is not on screen, it reserves the space and does not
//      ask the library, so rows scrolled out of view never queue a read.
//   2. If the image is ready, it draws it.
//   3. Otherwise it reserves the space, so a row keeps its height while the
//      image loads.
//   4. With CollapseNoImage, an item that has no image takes no space.
//
// The size is in screen pixels. A size under kMinSize is raised to it, and the
// space the call takes is the raised size, so size your row from kMinSize. The
// size is rounded to whole pixels. The loader keeps a copy of the image made
// for the longer side of the box, so draw at the size you want and do not scale
// the result yourself. The item is always the box you asked for, whatever size
// the loader's copy has: a copy of another size is scaled into the box.
//
// Where the loader has no image table (a server or generic build, or a plugin
// built against a header without hooks->Images), this reserves the space and
// draws nothing. The call reads hooks->Images->GetSized, which only a loader
// with interface 77 has, and a plugin cannot ask the loader which number it
// has. Use this helper only in a plugin that declares 77, which is the default
// (PLUGIN_INTERFACE_VERSION): a loader with a lower maximum then refuses the
// plugin instead of running it on a shorter table.
//
// Size icons from the loader's font, so they grow with it. SizeForFont(imgui, 28.0f)
// is 28 pixels at the default font and larger when the player raises the font
// scale. Work the size out once per frame and use it for every row. Use as few
// distinct sizes per picture as you can: the loader keeps at most 3 sharp
// copies of each picture, and 256 copies in all (a count, not a size). The result is whole pixels, so it stays steady
// while the font does not change.
//
// The image is valid for this frame only. Nothing is kept: ask again on the
// next frame.

#include "plugin_interface.h"

#include <cmath>
#include <cstdint>

namespace PluginImages
{
	enum Flags : unsigned
	{
		None            = 0,
		// An item the library says has no image takes no space.
		CollapseNoImage = 1u << 0,
	};

	// The smallest size an image is drawn at, in screen pixels. Below this an
	// icon cannot be read. Image raises a smaller size to this.
	constexpr float kMinSize = 28.0f;

	// The largest size the library stores a picture at, in screen pixels.
	constexpr float kMaxSize = 256.0f;

	// The loader's default overlay font size, in pixels.
	constexpr float kDefaultFontSize = 15.0f;

	// The size for a font of fontSize, given the size wanted at defaultFontSize:
	// sizeAtDefault * fontSize / defaultFontSize, in whole pixels, between
	// kMinSize and kMaxSize.
	inline float SizeForFont(float sizeAtDefault, float fontSize, float defaultFontSize)
	{
		const float size = std::floor(sizeAtDefault * fontSize / defaultFontSize + 0.5f);
		// A NaN fails the first test, so it lands on kMinSize.
		return !(size >= kMinSize) ? kMinSize : (size > kMaxSize ? kMaxSize : size);
	}

	// The same for the loader's current font. imgui is the render callback's.
	inline float SizeForFont(IModLoaderImGui* imgui, float sizeAtDefault)
	{
		return SizeForFont(sizeAtDefault, imgui ? imgui->GetFontSize() : kDefaultFontSize, kDefaultFontSize);
	}

	namespace detail
	{
		// The value of PLUGIN_IMAGE_NO_IMAGE.
		constexpr int kNoImage = 3;

		// A size as a whole number of pixels, at least kMinSize.
		inline float WholePixels(float size)
		{
			return size < kMinSize ? kMinSize : static_cast<float>(static_cast<int>(size + 0.5f));
		}

		template <class Hooks>
		concept HasImageTable = requires(Hooks* hooks) { hooks->Images; };

		template <class Hooks>
		bool TableIsThere(Hooks* hooks)
		{
			if constexpr (HasImageTable<Hooks>)
				return hooks && hooks->Images;
			else
				return false;
		}
	}

	// Draws image id at the cursor, w by h, or reserves that much space. A size
	// under kMinSize is raised to it. hooks is the plugin's IPluginHooks; imgui
	// is the render callback's.
	template <class Hooks>
	void Image(IModLoaderImGui* imgui, Hooks* hooks, uint32_t id, float w, float h, unsigned flags = None)
	{
		if (!imgui)
			return;

		w = detail::WholePixels(w);
		h = detail::WholePixels(h);

		if (!detail::TableIsThere(hooks) || !imgui->IsRectVisible(w, h))
		{
			imgui->Dummy(w, h);
			return;
		}

		if constexpr (detail::HasImageTable<Hooks>)
		{
			// The loader answers with a copy made for the longer side, or with the
			// stored picture. Either is drawn into the box, so the item is the
			// box on every path. A copy of the box's own size is drawn 1:1.
			if (PluginTextureHandle image = hooks->Images->GetSized(id, static_cast<int>(w > h ? w : h), nullptr, nullptr))
			{
				if (hooks->ImGuiTextures)
				{
					hooks->ImGuiTextures->Image(image, w, h);
					return;
				}
			}
			else if ((flags & CollapseNoImage) && hooks->Images->State(id) == detail::kNoImage)
				return;

			imgui->Dummy(w, h);
		}
	}
}
