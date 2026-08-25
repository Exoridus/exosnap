#pragma once

// The runtime renderer behind every shell icon whose appearance depends on
// something the build cannot know.
//
// WHY THIS IS NOT A SET OF FILES
// ------------------------------
// The marks themselves ARE files -- `app/assets/brand/marks` is the designer cut
// -- but they are authored in one reference palette. What ships depends on the
// user's accent, on the appearance, and on the pixel size the shell asked for.
// Rasterizing that ahead of time means one .ico per (state x accent x appearance
// x frame x size), which is a file count that grows every time the palette does.
//
// So a mark is loaded as SVG, recoloured (ui/brand/BrandMarkSvg.h) and
// rasterized at the exact pixel size the shell asked for -- which is also what
// makes the optical profiles reachable at all: a single asset downsampled to
// 16 px cannot get thicker strokes there.
//
// COST
// ----
// A raster is produced once per distinct key and cached. The recording heartbeat
// is a finite transition and therefore a fixed handful of keys per session, not
// one per tick; nothing here parses an asset on a timer.
//
// The static multi-resolution application icon is NOT rendered here. It is the
// executable's build-time identity, it carries no state and no accent, and
// Explorer wants it out of the PE resource table.

#include <QColor>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QString>

#include "models/ShellPresence.h"
#include "ui/brand/BrandMarkSvg.h"

namespace exosnap::ui::brand {

// A transport action as a shape. The same shapes the thumbnail toolbar uses,
// from the same geometry, so a tray menu entry and the taskbar button beside it
// cannot draw two different pause glyphs.
enum class ShellGlyph {
    Record,
    Pause,
    Resume,
    Stop,
    // The menu's other entries. Not transport, and drawn as outlines rather than
    // as filled shapes for that reason.
    Window,
    Folder,
    Notifications,
    Quit,
};

// Everything a mark's appearance depends on. Note what is absent: the physical
// size of the caller's widget, the device pixel ratio as a separate number, and
// anything about the window. `px` is the raster the shell will use, already
// multiplied out, because that is the only form Shell_NotifyIcon accepts.
struct ShellMarkRequest {
    BrandMarkKind kind = BrandMarkKind::Idle;
    int px = 16;
    // Indexes an animated mark's frames. Meaningful only for a kind that has
    // them; every other mark renders from its single asset.
    int frame = 0;
    // Whether the mark stands alone in shell chrome, and therefore reserves the
    // margin a tray or taskbar icon needs. False for a mark placed inside the
    // application's own layout, which has margin of its own.
    bool standalone = true;
    // Ids from ui/theme/ExoSnapThemes.h, not resolved colours: the renderer
    // reads the same tables the application does, so a palette change cannot
    // reach the UI and miss the tray.
    QString appearance_id;
    QString accent_id;
};

struct ShellGlyphRequest {
    ShellGlyph glyph = ShellGlyph::Record;
    int px = 16;
    QString appearance_id;
    QString accent_id;
};

// The Qt Quick image provider these ids are served through, and the scheme half
// of every URL that carries one. Declared here rather than beside the provider so
// a caller can build a URL without depending on Qt Quick -- the tray's model does
// exactly that.
inline constexpr char kShellIconProviderId[] = "exosnap-shell";

// `image://exosnap-shell/<image id>`.
[[nodiscard]] QString ShellIconImageUrl(const QString& image_id);

// The mark a shell state shows. The inventory in BrandMarkSvg.h is wider than
// this: a drawing exists for states the product does not currently reach, and
// deciding which of them it does is the projection's job, not the renderer's.
[[nodiscard]] BrandMarkKind BrandMarkKindFor(ShellIconState state) noexcept;

// The image-provider id these requests travel as, and back.
//
// The provider is stateless: a URL carries every input, which is what lets Qt
// Quick's own pixmap cache key on it correctly. A URL that renders differently
// on two days is the defect this shape prevents.
[[nodiscard]] QString MarkImageId(const ShellMarkRequest& request);
[[nodiscard]] QString GlyphImageId(const ShellGlyphRequest& request);
[[nodiscard]] bool ParseMarkImageId(const QString& id, ShellMarkRequest& out);
[[nodiscard]] bool ParseGlyphImageId(const QString& id, ShellGlyphRequest& out);

// The colours a request resolves to. Exposed because the tray menu's transport
// glyphs and the application's own surfaces must resolve them exactly once.
[[nodiscard]] QColor ResolveAccent(const QString& appearance_id, const QString& accent_id);
[[nodiscard]] BrandMarkPalette ResolvePalette(const QString& appearance_id, const QString& accent_id);

// Paints one mark. Transparent background, premultiplied ARGB, exactly `px`
// square. Callable with no QGuiApplication, which is what makes it testable.
[[nodiscard]] QImage RenderMark(const ShellMarkRequest& request);
[[nodiscard]] QImage RenderGlyph(const ShellGlyphRequest& request);

// The cache. Separate from the painting functions so a test can render without
// one, and so the provider owns exactly one.
//
// Thread-safe: Qt Quick calls an image provider from its own loader thread.
class ShellIconCache {
  public:
    [[nodiscard]] QImage mark(const ShellMarkRequest& request);
    [[nodiscard]] QImage glyph(const ShellGlyphRequest& request);

    // Drops everything. Not needed for a palette change -- the ids are part of
    // the key -- but a session that has cycled through several accents has no
    // reason to keep the rasters of the ones it left.
    void clear();
    [[nodiscard]] int sizeForTest();

  private:
    QMutex mutex_;
    QHash<QString, QImage> images_;
};

} // namespace exosnap::ui::brand
