#include <exosnap/engine/cursor_sprite.h>

#include <cstring>

namespace exosnap::engine {

bool CaptureWin32CursorBitmap(HCURSOR cursor, Win32CursorBitmap& out) {
    if (cursor == nullptr) {
        return false;
    }

    ICONINFO icon{};
    if (GetIconInfo(cursor, &icon) == FALSE) {
        return false;
    }

    auto cleanup = [&]() {
        if (icon.hbmColor != nullptr) {
            DeleteObject(icon.hbmColor);
        }
        if (icon.hbmMask != nullptr) {
            DeleteObject(icon.hbmMask);
        }
    };

    BITMAP bitmap{};
    int width = 0;
    int height = 0;
    if (icon.hbmColor != nullptr && GetObjectW(icon.hbmColor, sizeof(bitmap), &bitmap) != 0) {
        width = bitmap.bmWidth;
        height = bitmap.bmHeight;
    } else if (icon.hbmMask != nullptr && GetObjectW(icon.hbmMask, sizeof(bitmap), &bitmap) != 0) {
        width = bitmap.bmWidth;
        height = bitmap.bmHeight / 2;
    }

    if (width <= 0 || height <= 0 || width > 256 || height > 256) {
        cleanup();
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr) {
        cleanup();
        return false;
    }
    HBITMAP dib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        if (dib != nullptr) {
            DeleteObject(dib);
        }
        DeleteDC(dc);
        cleanup();
        return false;
    }

    HGDIOBJ old = SelectObject(dc, dib);
    std::memset(bits, 0, static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    const BOOL drawn = DrawIconEx(dc, 0, 0, cursor, width, height, 0, nullptr, DI_NORMAL);
    if (old != nullptr) {
        SelectObject(dc, old);
    }

    if (drawn != FALSE) {
        out.width = width;
        out.height = height;
        out.hotspot_x = static_cast<int>(icon.xHotspot);
        out.hotspot_y = static_cast<int>(icon.yHotspot);
        out.bgra.assign(static_cast<const uint8_t*>(bits),
                        static_cast<const uint8_t*>(bits) + static_cast<size_t>(width) * height * 4u);
    }

    DeleteObject(dib);
    DeleteDC(dc);
    cleanup();
    return drawn != FALSE;
}

int32_t ScaleCoordinateToSource(int32_t screen_delta, int32_t source_pixels, int32_t bounds_pixels) noexcept {
    if (bounds_pixels <= 0 || source_pixels <= 0) {
        return screen_delta;
    }
    const int64_t numerator = static_cast<int64_t>(screen_delta) * source_pixels;
    const int64_t rounded = numerator >= 0 ? numerator + bounds_pixels / 2 : numerator - bounds_pixels / 2;
    return static_cast<int32_t>(rounded / bounds_pixels);
}

CursorSpriteClip ClipCursorSprite(int32_t x, int32_t y, int32_t bmp_w, int32_t bmp_h, int32_t target_w,
                                  int32_t target_h) noexcept {
    CursorSpriteClip clip;
    if (bmp_w <= 0 || bmp_h <= 0 || bmp_w > 256 || bmp_h > 256 || target_w <= 0 || target_h <= 0) {
        return clip;
    }

    int32_t cx = x;
    int32_t cy = y;
    int32_t cw = bmp_w;
    int32_t ch = bmp_h;
    int32_t off_x = 0;
    int32_t off_y = 0;
    if (cx < 0) {
        off_x = -cx;
        cw += cx;
        cx = 0;
    }
    if (cy < 0) {
        off_y = -cy;
        ch += cy;
        cy = 0;
    }
    if (cw > target_w - cx) {
        cw = target_w - cx;
    }
    if (ch > target_h - cy) {
        ch = target_h - cy;
    }
    if (cw <= 0 || ch <= 0) {
        return clip;
    }

    clip.visible = true;
    clip.x = cx;
    clip.y = cy;
    clip.w = cw;
    clip.h = ch;
    clip.bitmap_off_x = off_x;
    clip.bitmap_off_y = off_y;
    return clip;
}

CursorSpriteDraw PlaceCursorSprite(int32_t src_x, int32_t src_y, int32_t bmp_w, int32_t bmp_h, int32_t src_w,
                                   int32_t src_h, float content_x, float content_y, float content_w,
                                   float content_h) noexcept {
    CursorSpriteDraw draw;
    if (content_w <= 0.0f || content_h <= 0.0f) {
        return draw;
    }
    draw.clip = ClipCursorSprite(src_x, src_y, bmp_w, bmp_h, src_w, src_h);
    if (!draw.clip.visible) {
        return draw;
    }
    const float scale_x = content_w / static_cast<float>(src_w);
    const float scale_y = content_h / static_cast<float>(src_h);
    draw.visible = true;
    draw.dst_x = content_x + static_cast<float>(draw.clip.x) * scale_x;
    draw.dst_y = content_y + static_cast<float>(draw.clip.y) * scale_y;
    draw.dst_w = static_cast<float>(draw.clip.w) * scale_x;
    draw.dst_h = static_cast<float>(draw.clip.h) * scale_y;
    return draw;
}

} // namespace exosnap::engine
