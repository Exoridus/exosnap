#include "CapabilityCacheStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "diagnostics/AppLog.h"
#include "settings/ConfigPaths.h"

namespace exosnap {
namespace {

using capability::CapabilityCacheKey;
using capability::DisplayHdrFacts;
using capability::MfAacRuntimeFacts;
using capability::MfWebcamRuntimeFacts;
using capability::NvidiaRuntimeFacts;
using capability::OsRuntimeFacts;
using capability::RuntimeCapabilitySnapshot;

// int64_t (the packed adapter LUID) is round-tripped as a decimal string —
// QJsonValue's numeric type is a double, which cannot represent every int64_t
// exactly.
QJsonObject KeyToJson(const CapabilityCacheKey& key) {
    QJsonObject obj;
    obj[QStringLiteral("adapter_luid")] = QString::number(key.adapter_luid);
    obj[QStringLiteral("driver_version")] = QString::fromStdString(key.driver_version);
    obj[QStringLiteral("app_version")] = QString::fromStdString(key.app_version);
    obj[QStringLiteral("schema_version")] = key.schema_version;
    return obj;
}

bool KeyFromJson(const QJsonObject& obj, CapabilityCacheKey& out) {
    bool ok = false;
    const qint64 luid = obj.value(QStringLiteral("adapter_luid")).toString().toLongLong(&ok);
    if (!ok)
        return false;
    out.adapter_luid = luid;
    out.driver_version = obj.value(QStringLiteral("driver_version")).toString().toStdString();
    out.app_version = obj.value(QStringLiteral("app_version")).toString().toStdString();
    out.schema_version = obj.value(QStringLiteral("schema_version")).toInt(-1);
    return true;
}

QJsonObject DisplayToJson(const DisplayHdrFacts& d) {
    QJsonObject obj;
    obj[QStringLiteral("name")] = QString::fromStdString(d.name);
    obj[QStringLiteral("hdr_active")] = d.hdr_active;
    obj[QStringLiteral("bits_per_color")] = static_cast<int>(d.bits_per_color);
    obj[QStringLiteral("red_primary_x")] = d.red_primary_x;
    obj[QStringLiteral("red_primary_y")] = d.red_primary_y;
    obj[QStringLiteral("green_primary_x")] = d.green_primary_x;
    obj[QStringLiteral("green_primary_y")] = d.green_primary_y;
    obj[QStringLiteral("blue_primary_x")] = d.blue_primary_x;
    obj[QStringLiteral("blue_primary_y")] = d.blue_primary_y;
    obj[QStringLiteral("white_point_x")] = d.white_point_x;
    obj[QStringLiteral("white_point_y")] = d.white_point_y;
    obj[QStringLiteral("max_luminance_nits")] = d.max_luminance_nits;
    obj[QStringLiteral("min_luminance_nits")] = d.min_luminance_nits;
    obj[QStringLiteral("max_full_frame_nits")] = d.max_full_frame_nits;
    return obj;
}

DisplayHdrFacts DisplayFromJson(const QJsonObject& obj) {
    DisplayHdrFacts d;
    d.name = obj.value(QStringLiteral("name")).toString().toStdString();
    d.hdr_active = obj.value(QStringLiteral("hdr_active")).toBool(false);
    d.bits_per_color = static_cast<uint32_t>(obj.value(QStringLiteral("bits_per_color")).toInt(0));
    d.red_primary_x = static_cast<float>(obj.value(QStringLiteral("red_primary_x")).toDouble(0.0));
    d.red_primary_y = static_cast<float>(obj.value(QStringLiteral("red_primary_y")).toDouble(0.0));
    d.green_primary_x = static_cast<float>(obj.value(QStringLiteral("green_primary_x")).toDouble(0.0));
    d.green_primary_y = static_cast<float>(obj.value(QStringLiteral("green_primary_y")).toDouble(0.0));
    d.blue_primary_x = static_cast<float>(obj.value(QStringLiteral("blue_primary_x")).toDouble(0.0));
    d.blue_primary_y = static_cast<float>(obj.value(QStringLiteral("blue_primary_y")).toDouble(0.0));
    d.white_point_x = static_cast<float>(obj.value(QStringLiteral("white_point_x")).toDouble(0.0));
    d.white_point_y = static_cast<float>(obj.value(QStringLiteral("white_point_y")).toDouble(0.0));
    d.max_luminance_nits = static_cast<float>(obj.value(QStringLiteral("max_luminance_nits")).toDouble(0.0));
    d.min_luminance_nits = static_cast<float>(obj.value(QStringLiteral("min_luminance_nits")).toDouble(0.0));
    d.max_full_frame_nits = static_cast<float>(obj.value(QStringLiteral("max_full_frame_nits")).toDouble(0.0));
    return d;
}

QJsonObject SnapshotToJson(const RuntimeCapabilitySnapshot& s) {
    QJsonObject nvidia;
    nvidia[QStringLiteral("nvenc_dll_present")] = s.nvidia.nvenc_dll_present;
    nvidia[QStringLiteral("nvenc_api_version_valid")] = s.nvidia.nvenc_api_version_valid;
    nvidia[QStringLiteral("nvenc_api_version")] = static_cast<qint64>(s.nvidia.nvenc_api_version);
    nvidia[QStringLiteral("adapter_name")] = QString::fromStdString(s.nvidia.adapter_name);
    nvidia[QStringLiteral("failure_detail")] = QString::fromStdString(s.nvidia.failure_detail);
    nvidia[QStringLiteral("nvenc_codec_probed")] = s.nvidia.nvenc_codec_probed;
    nvidia[QStringLiteral("nvenc_av1")] = s.nvidia.nvenc_av1;
    nvidia[QStringLiteral("nvenc_hevc")] = s.nvidia.nvenc_hevc;
    nvidia[QStringLiteral("nvenc_h264")] = s.nvidia.nvenc_h264;
    nvidia[QStringLiteral("nvenc_yuv444_h264")] = s.nvidia.nvenc_yuv444_h264;
    nvidia[QStringLiteral("nvenc_yuv444_hevc")] = s.nvidia.nvenc_yuv444_hevc;

    auto adv_to_json = [](const capability::NvencAdvancedEncodeFacts& adv) {
        QJsonObject obj;
        obj[QStringLiteral("max_bframes")] = adv.max_bframes;
        obj[QStringLiteral("bframe_ref_mode")] = adv.bframe_ref_mode;
        obj[QStringLiteral("lookahead")] = adv.lookahead;
        obj[QStringLiteral("temporal_aq")] = adv.temporal_aq;
        return obj;
    };
    nvidia[QStringLiteral("nvenc_adv_h264")] = adv_to_json(s.nvidia.nvenc_adv_h264);
    nvidia[QStringLiteral("nvenc_adv_hevc")] = adv_to_json(s.nvidia.nvenc_adv_hevc);
    nvidia[QStringLiteral("nvenc_adv_av1")] = adv_to_json(s.nvidia.nvenc_adv_av1);

    QJsonObject mf_aac;
    mf_aac[QStringLiteral("mftenum_found")] = s.mf_aac.mftenum_found;
    mf_aac[QStringLiteral("clsid_instantiable")] = s.mf_aac.clsid_instantiable;
    mf_aac[QStringLiteral("failure_detail")] = QString::fromStdString(s.mf_aac.failure_detail);

    QJsonObject mf_webcam;
    mf_webcam[QStringLiteral("available")] = s.mf_webcam.available;
    mf_webcam[QStringLiteral("failure_detail")] = QString::fromStdString(s.mf_webcam.failure_detail);

    QJsonObject os;
    os[QStringLiteral("build_number")] = static_cast<qint64>(s.os.build_number);
    os[QStringLiteral("version_string")] = QString::fromStdString(s.os.version_string);
    os[QStringLiteral("failure_detail")] = QString::fromStdString(s.os.failure_detail);

    QJsonArray displays;
    for (const auto& d : s.displays)
        displays.append(DisplayToJson(d));

    QJsonObject snapshot;
    snapshot[QStringLiteral("nvidia")] = nvidia;
    snapshot[QStringLiteral("mf_aac")] = mf_aac;
    snapshot[QStringLiteral("mf_webcam")] = mf_webcam;
    snapshot[QStringLiteral("os")] = os;
    snapshot[QStringLiteral("displays")] = displays;
    return snapshot;
}

// Returns false only on a structural problem (missing/wrong-typed required
// sub-objects) — the caller treats that identically to a key mismatch: no
// crash, cache discarded silently.
bool SnapshotFromJson(const QJsonObject& obj, RuntimeCapabilitySnapshot& out) {
    if (!obj.value(QStringLiteral("nvidia")).isObject() || !obj.value(QStringLiteral("mf_aac")).isObject() ||
        !obj.value(QStringLiteral("mf_webcam")).isObject() || !obj.value(QStringLiteral("os")).isObject())
        return false;

    const QJsonObject nvidia = obj.value(QStringLiteral("nvidia")).toObject();
    out.nvidia.nvenc_dll_present = nvidia.value(QStringLiteral("nvenc_dll_present")).toBool(false);
    out.nvidia.nvenc_api_version_valid = nvidia.value(QStringLiteral("nvenc_api_version_valid")).toBool(false);
    out.nvidia.nvenc_api_version = static_cast<uint32_t>(nvidia.value(QStringLiteral("nvenc_api_version")).toDouble(0));
    out.nvidia.adapter_name = nvidia.value(QStringLiteral("adapter_name")).toString().toStdString();
    out.nvidia.failure_detail = nvidia.value(QStringLiteral("failure_detail")).toString().toStdString();
    out.nvidia.nvenc_codec_probed = nvidia.value(QStringLiteral("nvenc_codec_probed")).toBool(false);
    out.nvidia.nvenc_av1 = nvidia.value(QStringLiteral("nvenc_av1")).toBool(false);
    out.nvidia.nvenc_hevc = nvidia.value(QStringLiteral("nvenc_hevc")).toBool(false);
    out.nvidia.nvenc_h264 = nvidia.value(QStringLiteral("nvenc_h264")).toBool(false);
    out.nvidia.nvenc_yuv444_h264 = nvidia.value(QStringLiteral("nvenc_yuv444_h264")).toBool(false);
    out.nvidia.nvenc_yuv444_hevc = nvidia.value(QStringLiteral("nvenc_yuv444_hevc")).toBool(false);

    auto adv_from_json = [](const QJsonObject& parent, const char* key) {
        capability::NvencAdvancedEncodeFacts adv;
        const QJsonObject obj = parent.value(QLatin1String(key)).toObject();
        adv.max_bframes = obj.value(QStringLiteral("max_bframes")).toInt(0);
        adv.bframe_ref_mode = obj.value(QStringLiteral("bframe_ref_mode")).toInt(0);
        adv.lookahead = obj.value(QStringLiteral("lookahead")).toBool(false);
        adv.temporal_aq = obj.value(QStringLiteral("temporal_aq")).toBool(false);
        return adv;
    };
    out.nvidia.nvenc_adv_h264 = adv_from_json(nvidia, "nvenc_adv_h264");
    out.nvidia.nvenc_adv_hevc = adv_from_json(nvidia, "nvenc_adv_hevc");
    out.nvidia.nvenc_adv_av1 = adv_from_json(nvidia, "nvenc_adv_av1");

    const QJsonObject mf_aac = obj.value(QStringLiteral("mf_aac")).toObject();
    out.mf_aac.mftenum_found = mf_aac.value(QStringLiteral("mftenum_found")).toBool(false);
    out.mf_aac.clsid_instantiable = mf_aac.value(QStringLiteral("clsid_instantiable")).toBool(false);
    out.mf_aac.failure_detail = mf_aac.value(QStringLiteral("failure_detail")).toString().toStdString();

    const QJsonObject mf_webcam = obj.value(QStringLiteral("mf_webcam")).toObject();
    out.mf_webcam.available = mf_webcam.value(QStringLiteral("available")).toBool(false);
    out.mf_webcam.failure_detail = mf_webcam.value(QStringLiteral("failure_detail")).toString().toStdString();

    const QJsonObject os = obj.value(QStringLiteral("os")).toObject();
    out.os.build_number = static_cast<uint32_t>(os.value(QStringLiteral("build_number")).toDouble(0));
    out.os.version_string = os.value(QStringLiteral("version_string")).toString().toStdString();
    out.os.failure_detail = os.value(QStringLiteral("failure_detail")).toString().toStdString();

    out.displays.clear();
    const QJsonArray displays = obj.value(QStringLiteral("displays")).toArray();
    out.displays.reserve(static_cast<size_t>(displays.size()));
    for (const QJsonValue& v : displays) {
        if (v.isObject())
            out.displays.push_back(DisplayFromJson(v.toObject()));
    }

    return true;
}

} // namespace

CapabilityCacheStore::CapabilityCacheStore() {
    const QString config_dir = settings::ResolveAppConfigDir();
    if (!config_dir.isEmpty()) {
        QDir().mkpath(config_dir);
        file_path_ = QDir(config_dir).filePath(QStringLiteral("capability-cache.json"));
    } else {
        file_path_ = QStringLiteral("capability-cache.json");
    }
}

CapabilityCacheStore::CapabilityCacheStore(QString file_path) : file_path_(std::move(file_path)) {
}

std::optional<RuntimeCapabilitySnapshot>
CapabilityCacheStore::LoadMatching(const CapabilityCacheKey& expected_key) const {
    if (file_path_.isEmpty())
        return std::nullopt;

    QFileInfo info(file_path_);
    if (!info.exists())
        return std::nullopt; // no cache yet — normal on first run

    QFile file(file_path_);
    if (!file.open(QIODevice::ReadOnly)) {
        diagnostics::AppLog::info(QStringLiteral("capability.cache"),
                                  QStringLiteral("Cannot open cache for reading: %1").arg(file_path_));
        return std::nullopt;
    }
    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        // Corrupt cache — discard silently (pre-v1 policy: no migration). The next
        // real probe overwrites this file with a fresh, valid one.
        diagnostics::AppLog::info(QStringLiteral("capability.cache"),
                                  QStringLiteral("Corrupt cache — discarding: %1").arg(parse_error.errorString()));
        return std::nullopt;
    }

    const QJsonObject root = doc.object();
    if (!root.value(QStringLiteral("cache_key")).isObject() || !root.value(QStringLiteral("snapshot")).isObject())
        return std::nullopt;

    CapabilityCacheKey stored_key;
    if (!KeyFromJson(root.value(QStringLiteral("cache_key")).toObject(), stored_key))
        return std::nullopt;

    if (!(stored_key == expected_key)) {
        // Adapter/driver/app-version/schema changed since this cache was written —
        // normal (GPU swap, driver update, app upgrade), not a fault.
        diagnostics::AppLog::info(QStringLiteral("capability.cache"),
                                  QStringLiteral("Cache key mismatch — discarding warm-start snapshot"));
        return std::nullopt;
    }

    RuntimeCapabilitySnapshot snapshot;
    if (!SnapshotFromJson(root.value(QStringLiteral("snapshot")).toObject(), snapshot))
        return std::nullopt;

    return snapshot;
}

bool CapabilityCacheStore::Save(const RuntimeCapabilitySnapshot& snapshot, const CapabilityCacheKey& key) const {
    if (file_path_.isEmpty())
        return false;

    QDir().mkpath(QFileInfo(file_path_).absolutePath());

    QJsonObject root;
    root[QStringLiteral("cache_key")] = KeyToJson(key);
    root[QStringLiteral("snapshot")] = SnapshotToJson(snapshot);

    QSaveFile file(file_path_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        diagnostics::AppLog::info(QStringLiteral("capability.cache"),
                                  QStringLiteral("Cannot open cache for atomic write: %1").arg(file_path_));
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        diagnostics::AppLog::info(QStringLiteral("capability.cache"),
                                  QStringLiteral("Atomic cache save commit failed: %1").arg(file_path_));
        return false;
    }
    return true;
}

const QString& CapabilityCacheStore::StorePath() const {
    return file_path_;
}

} // namespace exosnap
