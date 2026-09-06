/* Minimal stub: provides mColorConvert without pulling in libpng via image.c.
 * pocket-cam.c uses mColorConvert to convert pixel colours for the GB Camera
 * accessory. This implementation is extracted verbatim from mgba/src/util/image.c. */
#include <mgba-util/image.h>

uint32_t mColorConvert(uint32_t color, enum mColorFormat from, enum mColorFormat to) {
	if (from == to) {
		return color;
	}

	uint32_t r = 0;
	uint32_t g = 0;
	uint32_t b = 0;
	uint32_t a = 0xFF;

	switch (from) {
	case mCOLOR_ARGB8:
		a = color >> 24;
		// Fall through
	case mCOLOR_XRGB8:
	case mCOLOR_RGB8:
		r = (color >> 16) & 0xFF;
		g = (color >> 8) & 0xFF;
		b = color & 0xFF;
		break;

	case mCOLOR_ABGR8:
		a = color >> 24;
		// Fall through
	case mCOLOR_XBGR8:
	case mCOLOR_BGR8:
		b = (color >> 16) & 0xFF;
		g = (color >> 8) & 0xFF;
		r = color & 0xFF;
		break;

	case mCOLOR_RGBA8:
		a = color & 0xFF;
		// Fall through
	case mCOLOR_RGBX8:
		r = (color >> 24) & 0xFF;
		g = (color >> 16) & 0xFF;
		b = (color >> 8) & 0xFF;
		break;

	case mCOLOR_BGRA8:
		a = color & 0xFF;
		// Fall through
	case mCOLOR_BGRX8:
		b = (color >> 24) & 0xFF;
		g = (color >> 16) & 0xFF;
		r = (color >> 8) & 0xFF;
		break;

	case mCOLOR_ARGB5:
		a = (color >> 15) * 0xFF;
		// Fall through
	case mCOLOR_RGB5:
		r = (((color >> 10) & 0x1F) * 0x21) >> 2;
		g = (((color >> 5) & 0x1F) * 0x21) >> 2;
		b = ((color & 0x1F) * 0x21) >> 2;
		break;

	case mCOLOR_ABGR5:
		a = (color >> 15) * 0xFF;
		// Fall through
	case mCOLOR_BGR5:
		b = (((color >> 10) & 0x1F) * 0x21) >> 2;
		g = (((color >> 5) & 0x1F) * 0x21) >> 2;
		r = ((color & 0x1F) * 0x21) >> 2;
		break;

	case mCOLOR_RGBA5:
		a = (color & 1) * 0xFF;
		r = (((color >> 11) & 0x1F) * 0x21) >> 2;
		g = (((color >> 6) & 0x1F) * 0x21) >> 2;
		b = (((color >> 1) & 0x1F) * 0x21) >> 2;
		break;
	case mCOLOR_BGRA5:
		a = (color & 1) * 0xFF;
		b = (((color >> 11) & 0x1F) * 0x21) >> 2;
		g = (((color >> 6) & 0x1F) * 0x21) >> 2;
		r = (((color >> 1) & 0x1F) * 0x21) >> 2;
		break;

	case mCOLOR_RGB565:
		r = (((color >> 11) & 0x1F) * 0x21) >> 2;
		g = (((color >> 5) & 0x3F) * 0x41) >> 4;
		b = ((color & 0x1F) * 0x21) >> 2;
		break;
	case mCOLOR_BGR565:
		b = (((color >> 11) & 0x1F) * 0x21) >> 2;
		g = (((color >> 5) & 0x3F) * 0x41) >> 4;
		r = ((color & 0x1F) * 0x21) >> 2;
		break;

	case mCOLOR_L8:
		r = color;
		g = color;
		b = color;
		break;

	case mCOLOR_PAL8:
	case mCOLOR_ANY:
		return 0;
	}

	color = 0;
	switch (to) {
	case mCOLOR_XRGB8:
		a = 0xFF;
		// Fall through
	case mCOLOR_ARGB8:
		color |= a << 24;
		// Fall through
	case mCOLOR_RGB8:
		color |= r << 16;
		color |= g << 8;
		color |= b;
		break;
	case mCOLOR_XBGR8:
		a = 0xFF;
		// Fall through
	case mCOLOR_ABGR8:
		color |= a << 24;
		// Fall through
	case mCOLOR_BGR8:
		color |= b << 16;
		color |= g << 8;
		color |= r;
		break;
	case mCOLOR_RGBX8:
		a = 0xFF;
		// Fall through
	case mCOLOR_RGBA8:
		color |= a;
		color |= r << 24;
		color |= g << 16;
		color |= b << 8;
		break;
	case mCOLOR_BGRX8:
		a = 0xFF;
		// Fall through
	case mCOLOR_BGRA8:
		color |= a;
		color |= b << 24;
		color |= g << 16;
		color |= r << 8;
		break;
	case mCOLOR_ARGB5:
		color |= (!!a << 15);
		// Fall through
	case mCOLOR_RGB5:
		color |= (r >> 3) << 10;
		color |= (g >> 3) << 5;
		color |= b >> 3;
		break;
	case mCOLOR_ABGR5:
		color |= (!!a << 15);
		// Fall through
	case mCOLOR_BGR5:
		color |= (b >> 3) << 10;
		color |= (g >> 3) << 5;
		color |= r >> 3;
		break;
	case mCOLOR_RGBA5:
		color |= !!a;
		color |= (r >> 3) << 11;
		color |= (g >> 3) << 6;
		color |= (b >> 3) << 1;
		break;
	case mCOLOR_BGRA5:
		color |= !!a;
		color |= (b >> 3) << 11;
		color |= (g >> 3) << 6;
		color |= (r >> 3) << 1;
		break;
	case mCOLOR_RGB565:
		color |= (r >> 3) << 11;
		color |= (g >> 2) << 5;
		color |= b >> 3;
		break;
	case mCOLOR_BGR565:
		color |= (b >> 3) << 11;
		color |= (g >> 2) << 5;
		color |= r >> 3;
		break;
	case mCOLOR_L8:
		color = (55 * r + 184 * g + 18 * b) >> 8;
		break;
	case mCOLOR_PAL8:
	case mCOLOR_ANY:
		return 0;
	}

	return color;
}
