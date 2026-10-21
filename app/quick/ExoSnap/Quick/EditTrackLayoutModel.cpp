#include "EditTrackLayoutModel.h"

#include <QColor>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <optional>

namespace exosnap::quick {

EditTrackLayoutModel::EditTrackLayoutModel(QObject* parent) : QObject(parent) {
}

bool EditTrackLayoutModel::validTracks(const QVariantList& tracks) {
    QSet<qulonglong> ids;
    for (const auto& value : tracks) {
        bool ok = false;
        const auto id = value.toMap().value(QStringLiteral("id")).toULongLong(&ok);
        if (!ok || !id || ids.contains(id))
            return false;
        ids.insert(id);
    }
    return true;
}

void EditTrackLayoutModel::setTracks(const QVariantList& tracks) {
    if (tracks == tracks_ || !validTracks(tracks))
        return;
    tracks_ = tracks;
    rebuild();
}

void EditTrackLayoutModel::setPreviewTracks(const QVariantList& tracks) {
    if (tracks == preview_tracks_ || !validTracks(tracks))
        return;
    preview_tracks_ = tracks;
    rebuild();
}

void EditTrackLayoutModel::reset() {
    tracks_.clear();
    preview_tracks_.clear();
    heights_.clear();
    expanded_heights_.clear();
    colors_.clear();
    rebuild();
}

QVariantList EditTrackLayoutModel::layoutRows(const QVariantList& tracks) const {
    QVariantList result;
    qreal top = 0;
    for (const auto& value : tracks) {
        auto row = value.toMap();
        const auto id = row.value(QStringLiteral("id")).toULongLong();
        const auto height = heights_.value(id, row.value(QStringLiteral("video")).toBool() ? 64 : 44);
        const auto header = height < 44 ? height : height < 64 ? 16 : 18;
        row.insert(QStringLiteral("top"), top);
        row.insert(QStringLiteral("height"), height);
        row.insert(QStringLiteral("headerHeight"), header);
        row.insert(QStringLiteral("contentTop"), top + header);
        row.insert(QStringLiteral("contentHeight"), height - header);
        row.insert(QStringLiteral("collapsed"), height < 44);
        row.insert(QStringLiteral("color"), colors_.value(id));
        result.push_back(row);
        top += height;
    }
    return result;
}

QVariantMap EditTrackLayoutModel::geometryFor(const QVariantList& rows) {
    QVariantMap result;
    for (const auto& value : rows) {
        const auto row = value.toMap();
        result.insert(QString::number(row.value(QStringLiteral("id")).toULongLong()), row);
    }
    return result;
}

void EditTrackLayoutModel::rebuild() {
    rows_ = layoutRows(tracks_);
    const auto preview_rows = layoutRows(preview_tracks_);
    geometry_ = geometryFor(rows_);
    preview_geometry_ = geometryFor(preview_rows);
    const auto extent = [](const QVariantList& rows) -> qreal {
        if (rows.empty())
            return 0;
        const auto row = rows.back().toMap();
        return row.value(QStringLiteral("top")).toReal() + row.value(QStringLiteral("height")).toReal();
    };
    base_height_ = extent(rows_);
    total_height_ = std::max(base_height_, extent(preview_rows));
    emit layoutChanged();
}

void EditTrackLayoutModel::setTrackHeight(qulonglong id, qreal height) {
    const auto key = QString::number(id);
    if (!std::isfinite(height) || (!geometry_.contains(key) && !preview_geometry_.contains(key)))
        return;
    height = std::clamp(height, minimumHeight(), maximumHeight());
    if (heights_.value(id, defaultHeight(id)) == height)
        return;
    heights_.insert(id, height);
    if (height >= 44)
        expanded_heights_.insert(id, height);
    rebuild();
}

qreal EditTrackLayoutModel::defaultHeight(qulonglong id) const {
    const auto find = [id](const QVariantList& rows) -> std::optional<qreal> {
        for (const auto& value : rows) {
            const auto row = value.toMap();
            if (row.value(QStringLiteral("id")).toULongLong() == id)
                return row.value(QStringLiteral("video")).toBool() ? 64 : 44;
        }
        return {};
    };
    return find(tracks_).value_or(find(preview_tracks_).value_or(64));
}

void EditTrackLayoutModel::toggleCollapsed(qulonglong id) {
    const auto key = QString::number(id);
    if (!geometry_.contains(key))
        return;
    const auto height = heightForId(id);
    setTrackHeight(id, height < 44 ? expanded_heights_.value(id, defaultHeight(id)) : 22);
}

void EditTrackLayoutModel::setTrackColor(qulonglong id, const QString& color) {
    if (!geometry_.contains(QString::number(id)) || (!color.isEmpty() && !QColor::isValidColorName(color)))
        return;
    if (colors_.value(id) == color)
        return;
    colors_.insert(id, color);
    rebuild();
}

qreal EditTrackLayoutModel::topForId(qulonglong id, bool preview) const {
    const auto rows = preview ? previewGeometry() : geometry_;
    const auto row = rows.value(QString::number(id)).toMap();
    return row.empty() ? -1 : row.value(QStringLiteral("top")).toReal();
}

qreal EditTrackLayoutModel::heightForId(qulonglong id, bool preview) const {
    const auto rows = preview ? previewGeometry() : geometry_;
    return rows.value(QString::number(id)).toMap().value(QStringLiteral("height")).toReal();
}

qulonglong EditTrackLayoutModel::trackAtY(qreal y) const {
    if (!std::isfinite(y) || y < 0 || y >= base_height_)
        return 0;
    for (const auto& value : rows_) {
        const auto row = value.toMap();
        if (y < row.value(QStringLiteral("top")).toReal() + row.value(QStringLiteral("height")).toReal())
            return row.value(QStringLiteral("id")).toULongLong();
    }
    return 0;
}

QVariantList EditTrackLayoutModel::trackIdsIntersecting(qreal from_y, qreal to_y) const {
    QVariantList ids;
    if (!std::isfinite(from_y) || !std::isfinite(to_y) || from_y >= to_y)
        return ids;
    for (const auto& value : rows_) {
        const auto row = value.toMap();
        const auto top = row.value(QStringLiteral("top")).toReal();
        const auto bottom = top + row.value(QStringLiteral("height")).toReal();
        if (top < to_y && bottom > from_y)
            ids.push_back(row.value(QStringLiteral("id")));
    }
    return ids;
}

QVariantMap EditTrackLayoutModel::dropTargetAtY(qreal y) const {
    if (!std::isfinite(y))
        return {};
    if (y < 0)
        return {{QStringLiteral("newTrack"), 0}};
    if (y >= base_height_)
        return {{QStringLiteral("newTrack"), 2}};
    for (const auto& value : rows_) {
        const auto row = value.toMap();
        if (!row.value(QStringLiteral("video")).toBool()) {
            if (std::abs(y - row.value(QStringLiteral("top")).toReal()) <= 8)
                return {{QStringLiteral("newTrack"), 1}};
            break;
        }
    }
    return {{QStringLiteral("trackId"), QVariant::fromValue(trackAtY(y))}, {QStringLiteral("newTrack"), -1}};
}

} // namespace exosnap::quick
