#include <exosnap/engine/cursor_sprite.h>

#include <cstring>
#include <vector>

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

        // DrawIconEx writes alpha only for cursors that carry an alpha channel.
        // A mask-based cursor -- monochrome ones such as the default I-beam, or a
        // 24-bit colour cursor with an AND mask -- leaves every alpha byte at the
        // zero the memset put there, and the compositor's cursor pass honours
        // sprite alpha, so such a pointer is composited fully transparent and
        // vanishes from the recording. Rebuild alpha from the mask instead.
        bool any_alpha = false;
        for (size_t i = 3; i < out.bgra.size(); i += 4) {
            if (out.bgra[i] != 0) {
                any_alpha = true;
                break;
            }
        }
        if (!any_alpha && icon.hbmMask != nullptr) {
            // The whole mask bitmap, not just its first plane. A cursor with no
            // colour bitmap stores the AND plane and the XOR plane stacked in it,
            // and the XOR plane is what separates a pixel that leaves the
            // destination alone from one that inverts it. Reading only the AND
            // plane collapses those two into "transparent", which erases a cursor
            // built entirely from inverting pixels -- the default I-beam is one.
            const bool has_xor_plane = icon.hbmColor == nullptr && bitmap.bmHeight == height * 2;
            const int mask_rows = has_xor_plane ? height * 2 : height;
            BITMAPINFO mask_info{};
            mask_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            mask_info.bmiHeader.biWidth = width;
            mask_info.bmiHeader.biHeight = -mask_rows; // top-down; the AND plane comes first
            mask_info.bmiHeader.biPlanes = 1;
            mask_info.bmiHeader.biBitCount = 1;
            mask_info.bmiHeader.biCompression = BI_RGB;
            const size_t mask_stride = (static_cast<size_t>(width) + 31u) / 32u * 4u;
            std::vector<uint8_t> mask_bits(mask_stride * static_cast<size_t>(mask_rows) + 8u);
            // Two-colour table follows the header; reserve room for it.
            std::vector<uint8_t> info_storage(sizeof(BITMAPINFOHEADER) + 2u * sizeof(RGBQUAD));
            std::memcpy(info_storage.data(), &mask_info, sizeof(BITMAPINFOHEADER));
            if (GetDIBits(dc, icon.hbmMask, 0, static_cast<UINT>(mask_rows), mask_bits.data(),
                          reinterpret_cast<BITMAPINFO*>(info_storage.data()), DIB_RGB_COLORS) == mask_rows) {
                std::vector<uint8_t> invert;
                for (int y = 0; y < height; ++y) {
                    const uint8_t* and_row = mask_bits.data() + static_cast<size_t>(y) * mask_stride;
                    const uint8_t* xor_row =
                        has_xor_plane ? mask_bits.data() + static_cast<size_t>(y + height) * mask_stride : nullptr;
                    for (int x = 0; x < width; ++x) {
                        const bool and_bit = ((and_row[x / 8] >> (7 - (x % 8))) & 1u) != 0u;
                        const bool xor_bit = xor_row != nullptr && ((xor_row[x / 8] >> (7 - (x % 8))) & 1u) != 0u;
                        const size_t pixel = (static_cast<size_t>(y) * width + x) * 4u;
                        const Win32CursorMaskState state = Win32CursorMaskStateOf(and_bit, xor_bit);
                        if (state == Win32CursorMaskState::Invert) {
                            if (invert.empty()) {
                                invert.assign(static_cast<size_t>(width) * height * 4u, 0u);
                            }
                            invert[pixel] = 255u;
                            invert[pixel + 1u] = 255u;
                            invert[pixel + 2u] = 255u;
                            invert[pixel + 3u] = 255u;
                        }
                        // The colour DrawIconEx produced already distinguishes the two
                        // opaque states; only whether the pixel survives is rebuilt.
                        out.bgra[pixel + 3u] = and_bit ? 0u : 255u;
                    }
                }
                out.invert = std::move(invert);
            }
        }
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
