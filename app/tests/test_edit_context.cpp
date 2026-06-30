// test_edit_context.cpp — tests for the marker sidecar JSON round-trip.
//
// Replicates the write/read logic from EditExportPage::saveMarkers() /
// loadMarkers() without pulling in a Qt widget (no QApplication needed).
//
// Format: JSON object with "version", "timebase", "markers" array.
// Each marker: { "timeMs": <uint64>, "type": "general|cut|highlight", "label": "" }

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "../models/RecordingMarker.h"

namespace {

// ---- Minimal sidecar writer/reader (mirrors EditExportPage logic) ----

struct SidecarFixture {
    QTemporaryDir tmp;
    QString path;

    SidecarFixture() : path(tmp.path() + QStringLiteral("/test.markers.json")) {
    }

    bool write(const std::vector<exosnap::RecordingMarker>& markers) {
        QJsonArray arr;
        for (const auto& m : markers) {
            QJsonObject obj;
            obj[QStringLiteral("timeMs")] = static_cast<qint64>(m.time_ms);
            obj[QStringLiteral("type")] = QString::fromLatin1(exosnap::RecordingMarkerTypeToString(m.type));
            obj[QStringLiteral("label")] = QString::fromStdString(m.label);
            arr.append(obj);
        }
        QJsonObject root;
        root[QStringLiteral("version")] = 1;
        root[QStringLiteral("timebase")] = QStringLiteral("milliseconds");
        root[QStringLiteral("markers")] = arr;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(QJsonDocument(root).toJson());
        return true;
    }

    std::vector<exosnap::RecordingMarker> read() {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        const QJsonArray arr = doc.object().value(QStringLiteral("markers")).toArray();
        std::vector<exosnap::RecordingMarker> out;
        for (const auto& v : arr) {
            const QJsonObject obj = v.toObject();
            exosnap::RecordingMarker m;
            m.time_ms = static_cast<uint64_t>(obj.value(QStringLiteral("timeMs")).toDouble());
            const QString t = obj.value(QStringLiteral("type")).toString();
            if (t == QStringLiteral("cut"))
                m.type = exosnap::RecordingMarkerType::Cut;
            else if (t == QStringLiteral("highlight"))
                m.type = exosnap::RecordingMarkerType::Highlight;
            else
                m.type = exosnap::RecordingMarkerType::General;
            m.label = obj.value(QStringLiteral("label")).toString().toStdString();
            out.push_back(m);
        }
        return out;
    }
};

} // namespace

// ---- Tests ----

TEST(MarkerSidecarTest, RoundTrip) {
    SidecarFixture fx;
    std::vector<exosnap::RecordingMarker> markers = {
        {1000, exosnap::RecordingMarkerType::General, "Start"},
        {5000, exosnap::RecordingMarkerType::Cut, "Cut here"},
        {9999, exosnap::RecordingMarkerType::Highlight, "Clip"},
    };
    ASSERT_TRUE(fx.write(markers));
    const auto loaded = fx.read();
    ASSERT_EQ(loaded.size(), markers.size());
    for (size_t i = 0; i < markers.size(); ++i) {
        EXPECT_EQ(loaded[i].time_ms, markers[i].time_ms);
        EXPECT_EQ(loaded[i].type, markers[i].type);
        EXPECT_EQ(loaded[i].label, markers[i].label);
    }
}

TEST(MarkerSidecarTest, EmptyMarkersWriteReadEmpty) {
    SidecarFixture fx;
    ASSERT_TRUE(fx.write({}));
    EXPECT_TRUE(fx.read().empty());
}

TEST(MarkerSidecarTest, MissingFileReturnsEmpty) {
    SidecarFixture fx;
    // Never wrote — read must return empty, not crash.
    EXPECT_TRUE(fx.read().empty());
}

TEST(MarkerSidecarTest, TypeStringsRoundTrip) {
    using T = exosnap::RecordingMarkerType;
    SidecarFixture fx;
    std::vector<exosnap::RecordingMarker> markers = {
        {0, T::General, "g"},
        {100, T::Cut, "c"},
        {200, T::Highlight, "h"},
    };
    ASSERT_TRUE(fx.write(markers));
    const auto loaded = fx.read();
    ASSERT_EQ(loaded.size(), 3u);
    EXPECT_EQ(loaded[0].type, T::General);
    EXPECT_EQ(loaded[1].type, T::Cut);
    EXPECT_EQ(loaded[2].type, T::Highlight);
}

TEST(MarkerSidecarTest, LargeTimestamp) {
    SidecarFixture fx;
    // Ensure uint64 timestamps survive the qint64/double JSON round-trip.
    // Largest safe integer for JSON double: 2^53 - 1 = 9007199254740991 ms
    // (~285 000 years), well above any realistic recording duration.
    const uint64_t big = 9007199254740991ULL;
    std::vector<exosnap::RecordingMarker> markers = {{big, exosnap::RecordingMarkerType::General, "end"}};
    ASSERT_TRUE(fx.write(markers));
    const auto loaded = fx.read();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].time_ms, big);
}
