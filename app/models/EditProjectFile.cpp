#include "EditProjectFile.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <QStringConverter>
#include <QStringView>
#include <QTemporaryFile>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace exosnap::edit {
namespace {

constexpr QByteArrayView kMagic("EXOSNAP PROJECT\r\n");
constexpr quint32 kVersion = 1;
constexpr qsizetype kChecksumBytes = 32;
constexpr qint64 kFileLimit = 16 * 1024 * 1024;
constexpr size_t kAllocationLimit = 32 * 1024 * 1024;
constexpr size_t kCollectionLimit = 65'536;
constexpr size_t kBlobLimit = 1024 * 1024;
constexpr qsizetype kPathLimit = 32'767;

QByteArray Digest(QByteArrayView bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

bool SafeComponents(QStringView components) {
    for (const auto& part : components.split(u'/')) {
        if (part.empty() || part.endsWith(u'.') || part.endsWith(u' '))
            return false;
        for (const auto& character : part)
            if (character.unicode() < 32 || QStringView(u"\\:*?\"<>|").contains(character))
                return false;
#ifdef Q_OS_WIN
        // Win32 device names still dispatch to devices when a file extension is appended.
        const auto separator = part.indexOf(u'.');
        const auto base = (separator == -1 ? part : part.first(separator)).trimmed().toString().toUpper();
        if (base == u"CON" || base == u"PRN" || base == u"AUX" || base == u"NUL" || base == u"CONIN$" ||
            base == u"CONOUT$" ||
            (base.size() == 4 && (base.startsWith(u"COM") || base.startsWith(u"LPT")) &&
             QStringView(u"0123456789\u00b9\u00b2\u00b3").contains(base.back())))
            return false;
#endif
    }
    return true;
}

bool SafeAbsolutePath(QStringView path) {
    if (path.size() > kPathLimit || !path.isValidUtf16())
        return false;
#ifdef Q_OS_WIN
    if (path.startsWith(u"//"))
        return path.count(u'/') >= 4 && SafeComponents(path.sliced(2));
    return path.size() > 3 && QStringView(u"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz").contains(path[0]) &&
           path[1] == u':' && path[2] == u'/' && SafeComponents(path.sliced(3));
#else
    return path.startsWith(u'/') && SafeComponents(path.sliced(1));
#endif
}

template <bool Read> class ProjectArchive {
  public:
    static constexpr bool reading = Read;

    ProjectArchive(QDataStream& stream, QDir directory) : stream_(stream), directory_(std::move(directory)) {
        stream_.setVersion(QDataStream::Qt_6_0);
        stream_.setByteOrder(QDataStream::BigEndian);
        stream_.setFloatingPointPrecision(QDataStream::DoublePrecision);
    }

    [[nodiscard]] bool valid() const {
        return stream_.status() == QDataStream::Ok;
    }

    void invalidate() {
        stream_.setStatus(QDataStream::ReadCorruptData);
    }

    template <typename... Values> void operator()(Values&... values) {
        (field(values), ...);
    }

    template <typename Value, typename Enum> void enumeration(Value& value, Enum maximum) {
        quint8 wire = static_cast<quint8>(value);
        number(wire);
        if (wire > static_cast<quint8>(maximum))
            invalidate();
        if constexpr (reading)
            value = static_cast<Enum>(wire);
    }

    template <typename Value> void mediaPath(Value& media) {
        QString path;
        bool relative = false;
        if constexpr (!reading) {
            path = QString::fromStdWString(media);
            path.replace(u'\\', u'/');
            if (!SafeAbsolutePath(path)) {
                invalidate();
                return;
            }
            auto local = directory_.relativeFilePath(path);
            if (SafeComponents(local) && !QDir::isAbsolutePath(local)) {
                path = std::move(local);
                relative = true;
            }
        }
        field(relative);
        auto encoded = path.toStdString();
        bytes(encoded, kPathLimit * 4);
        if constexpr (reading) {
            QStringDecoder decoder(QStringDecoder::Utf8,
                                   QStringConverter::Flag::Stateless | QStringConverter::Flag::ConvertInitialBom);
            path = decoder(QByteArrayView(encoded.data(), static_cast<qsizetype>(encoded.size())));
            if (decoder.hasError() || path.size() > kPathLimit ||
                (relative ? !SafeComponents(path) : !SafeAbsolutePath(path))) {
                invalidate();
                return;
            }
            const auto resolved = relative ? directory_.absoluteFilePath(path) : path;
            if (!SafeAbsolutePath(resolved)) {
                invalidate();
                return;
            }
            media = resolved.toStdWString();
        }
    }

  private:
    bool charge(size_t count, size_t stride, size_t limit) {
        if (!valid() || count > limit || count > allocation_left_ / stride) {
            invalidate();
            return false;
        }
        allocation_left_ -= count * stride;
        return true;
    }

    template <typename Value> void field(Value& value) {
        if (!valid())
            return;
        using Type = std::remove_cv_t<Value>;
        if constexpr (std::is_same_v<Type, bool>) {
            quint8 wire = value;
            number(wire);
            if (wire > 1)
                invalidate();
            if constexpr (reading)
                value = wire == 1;
        } else if constexpr (std::is_integral_v<Type>) {
            using Wire =
                std::conditional_t<std::is_signed_v<Type>, std::conditional_t<sizeof(Type) == 4, qint32, qint64>,
                                   std::conditional_t<sizeof(Type) == 4, quint32, quint64>>;
            Wire wire = static_cast<Wire>(value);
            number(wire);
            if constexpr (reading)
                value = static_cast<Type>(wire);
        } else if constexpr (std::is_same_v<Type, double>) {
            double wire = value;
            number(wire);
            if (!std::isfinite(wire))
                invalidate();
            if constexpr (reading)
                value = wire;
        } else {
            Fields(*this, value);
        }
    }

    template <typename Value> void number(Value& wire) {
        if constexpr (reading)
            stream_ >> wire;
        else
            stream_ << wire;
    }

    template <typename T>
    void field(std::vector<T>& value)
        requires(reading)
    {
        const auto count = length(0, sizeof(T), kCollectionLimit);
        if (!valid())
            return;
        value.resize(count);
        for (auto& item : value) {
            if (!valid())
                break;
            field(item);
        }
    }

    template <typename T>
    void field(const std::vector<T>& value)
        requires(!reading)
    {
        length(value.size(), sizeof(T), kCollectionLimit);
        for (const auto& item : value) {
            if (!valid())
                break;
            field(item);
        }
    }

    template <typename T, size_t Length>
    void field(std::array<T, Length>& value)
        requires(reading)
    {
        for (auto& item : value)
            field(item);
    }

    template <typename T, size_t Length>
    void field(const std::array<T, Length>& value)
        requires(!reading)
    {
        for (const auto& item : value)
            field(item);
    }

    template <size_t Length>
    void field(std::array<uint8_t, Length>& value)
        requires(reading)
    {
        raw(value.data(), Length);
    }

    template <size_t Length>
    void field(const std::array<uint8_t, Length>& value)
        requires(!reading)
    {
        raw(value.data(), Length);
    }

    template <typename T>
    void field(std::optional<T>& value)
        requires(reading)
    {
        bool present = false;
        field(present);
        if (present)
            field(value.emplace());
    }

    template <typename T>
    void field(const std::optional<T>& value)
        requires(!reading)
    {
        const bool present = value.has_value();
        field(present);
        if (present)
            field(*value);
    }

    template <typename T>
    void field(std::shared_ptr<const T>& value)
        requires(reading)
    {
        bool present = false;
        field(present);
        if (present && charge(1, sizeof(T), 1)) {
            auto entry = std::make_shared<T>();
            field(*entry);
            value = std::move(entry);
        }
    }

    template <typename T>
    void field(const std::shared_ptr<const T>& value)
        requires(!reading)
    {
        const bool present = static_cast<bool>(value);
        field(present);
        if (present && charge(1, sizeof(T), 1))
            field(*value);
    }

    void field(std::string& value)
        requires(reading)
    {
        bytes(value, kBlobLimit);
    }

    void field(const std::string& value)
        requires(!reading)
    {
        bytes(value, kBlobLimit);
    }

    void field(std::vector<uint8_t>& value)
        requires(reading)
    {
        bytes(value, kBlobLimit);
    }

    void field(const std::vector<uint8_t>& value)
        requires(!reading)
    {
        bytes(value, kBlobLimit);
    }

    quint32 length(size_t size, size_t stride, size_t limit) {
        if constexpr (!reading)
            if (size > limit) {
                invalidate();
                return 0;
            }
        auto wire = static_cast<quint32>(size);
        number(wire);
        if (!charge(wire, stride, limit))
            return 0;
        if constexpr (reading)
            if (wire > stream_.device()->bytesAvailable()) {
                invalidate();
                return 0;
            }
        return wire;
    }

    template <typename Value> void bytes(Value& value, size_t limit) {
        const auto size = length(value.size(), 1, limit);
        if (!valid())
            return;
        if constexpr (reading)
            value.resize(size);
        raw(value.data(), size);
    }

    template <typename Byte> void raw(Byte* bytes, qint64 size) {
        if (!size || !valid())
            return;
        if constexpr (reading) {
            if (stream_.readRawData(reinterpret_cast<char*>(bytes), size) != size)
                invalidate();
        } else if (stream_.writeRawData(reinterpret_cast<const char*>(bytes), size) != size) {
            invalidate();
        }
    }

    QDataStream& stream_;
    QDir directory_;
    size_t allocation_left_ = kAllocationLimit;
};

template <typename Archive, typename T> using Field = std::conditional_t<Archive::reading, T, const T>;

template <typename Archive> void Fields(Archive& archive, Field<Archive, Span>& span) {
    archive(span.in_us, span.out_us);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, RationalFps>& fps) {
    archive(fps.num, fps.den);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, engine::AudioTrackId>& id) {
    archive(id.bytes);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, engine::AudioSourceId>& id) {
    archive(id.bytes);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, StreamRef>& stream) {
    archive(stream.asset_id, stream.stream_identity, stream.container_index);
    archive.enumeration(stream.kind, engine::TimelineStreamKind::Audio);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, SourceStreamDescriptor>& stream) {
    auto& codec = stream.codec;
    archive(stream.ref, stream.source_span, stream.timeline_offset_us, codec.codec_id, codec.codec_private,
            codec.sample_rate, codec.channels, codec.bit_depth, codec.codec_delay_samples, codec.channel_layout,
            stream.fps, stream.name, stream.role, stream.source_ids);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, ValidatedSourceDescriptor>& descriptor) {
    archive(descriptor.asset_identity_fingerprint, descriptor.audio_identities_verified, descriptor.streams);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, InputMixSelection>& mix) {
    archive(mix.asset_identity_fingerprint, mix.selected_stream_identities, mix.provenance_fingerprint,
            mix.original_recipe_group_id);
    archive.enumeration(mix.authority, InputMixAuthority::UserConfirmed);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, OriginalRecipeAnchor>& recipe) {
    archive(recipe.group_id, recipe.streams, recipe.source_span);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, RecordingMarker>& marker) {
    archive(marker.time_ms, marker.label);
    archive.enumeration(marker.type, RecordingMarkerType::Highlight);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, Asset>& asset) {
    archive.mediaPath(asset.path);
    archive(asset.id, asset.name, asset.duration, asset.width, asset.height, asset.fps, asset.fps_num, asset.fps_den,
            asset.audio, asset.markers, asset.source_descriptor, asset.input_mix_selection, asset.mix_decision_required,
            asset.original_recipe, asset.source_version);
    if constexpr (Archive::reading)
        asset.state = AssetState::Missing;
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, CurveSpec>& curve) {
    archive.enumeration(curve.kind, engine::CurveKind::Bias);
    archive(curve.bias);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, EdgeFade>& fade) {
    archive(fade.duration_us, fade.curve);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, engine::PropertyKey>& key) {
    archive(key.source_us, key.value);
    archive.enumeration(key.interpolation, engine::KeyInterpolation::Hold);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, engine::PropertyChannel>& channel) {
    archive(channel.keys);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, engine::VideoTransform>& transform) {
    archive(transform.x, transform.y, transform.scale, transform.rotation);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, engine::RangeGainRamp>& ramp) {
    archive(ramp.in_sample, ramp.out_sample, ramp.ramp_samples, ramp.delta_db);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, engine::GainEnvelope>& envelope) {
    archive(envelope.channel, envelope.ranges);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, Clip>& clip) {
    archive(clip.id, clip.asset, clip.track, clip.group, clip.start, clip.source_in, clip.source_out, clip.stream,
            clip.enabled, clip.gain_db, clip.opacity, clip.fade_in, clip.fade_out, clip.original_enabled,
            clip.opacity_channel, clip.transform, clip.transform_channels, clip.gain_envelope);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, Track>& track) {
    archive(track.id, track.name, track.edit_enabled, track.gain_db);
    archive.enumeration(track.type, TrackType::Audio);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, Spacer>& spacer) {
    archive(spacer.id, spacer.track, spacer.start_us, spacer.duration_us);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, Transition>& junction) {
    archive(junction.outgoing, junction.incoming, junction.duration, junction.id, junction.track, junction.in_us,
            junction.out_us, junction.cut_us, junction.curve, junction.incoming_anchor_us);
    archive.enumeration(junction.kind, TransitionKind::HardCut);
    archive.enumeration(junction.audio_law, AudioGainLaw::EqualPower);
    archive.enumeration(junction.anchor, JunctionAnchor::Custom);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, TimelineEnd>& end) {
    archive(end.end_us);
    archive.enumeration(end.mode, TimelineEndMode::FixedEnd);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, ResultRange>& range) {
    archive(range.in_us, range.out_us);
    archive.enumeration(range.out_mode, ResultOutMode::FollowTimelineEnd);
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, EditSnapshot>& timeline) {
    archive(timeline.assets, timeline.tracks, timeline.clips, timeline.junctions, timeline.result_range,
            timeline.output_fps, timeline.output_width, timeline.output_height, timeline.id_high_watermark,
            timeline.spacers, timeline.locked_track_ids, timeline.timeline_end, timeline.source_version_high_watermark);
    if (!ProjectEntityCountFits(timeline) ||
        std::any_of(timeline.assets.begin(), timeline.assets.end(), [](const auto& asset) { return !asset; }))
        archive.invalidate();
}

template <typename Archive> void Fields(Archive& archive, Field<Archive, ProjectDocument>& document) {
    archive(document.timeline, document.snapping, document.ripple, document.auto_crossfade, document.linked_selection);
}

ProjectFileError ReadBytes(const QString& path, QByteArray& bytes, bool* absent = nullptr) {
    QFile file(path);
    const QFileInfo info(file);
    if (info.isSymLink() || info.isJunction())
        return ProjectFileError::Io;
    if (absent && !info.exists()) {
        *absent = true;
        return ProjectFileError::None;
    }
    if (!info.isFile() || !file.open(QIODevice::ReadOnly) || file.isSequential())
        return ProjectFileError::Io;
    bytes = file.read(kFileLimit + 1);
    if (file.error() != QFileDevice::NoError)
        return ProjectFileError::Io;
    return bytes.size() <= kFileLimit ? ProjectFileError::None : ProjectFileError::Invalid;
}

ProjectFileError ObserveDestination(const QString& path, QByteArray& observed) {
    QByteArray bytes;
    bool absent = false;
    const auto error = ReadBytes(path, bytes, &absent);
    if (error == ProjectFileError::None)
        observed = absent ? QByteArray{} : Digest(bytes);
    return error;
}

ProjectFileError CommitBytes(const QString& destination, const QByteArray& before, const QByteArray& bytes) {
    if (before.isEmpty()) {
        QTemporaryFile staged(destination + QStringLiteral(".XXXXXX"));
        if (!staged.open() || staged.write(bytes) != bytes.size() || !staged.flush())
            return ProjectFileError::Io;
        if (!staged.rename(destination))
            return QFileInfo::exists(destination) ? ProjectFileError::Changed : ProjectFileError::Io;
        staged.setAutoRemove(false);
        return ProjectFileError::None;
    }

    QSaveFile file(destination);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        return ProjectFileError::Io;
    QByteArray after;
    if (const auto error = ObserveDestination(destination, after); error != ProjectFileError::None)
        return error;
    if (before != after)
        return ProjectFileError::Changed;
    return file.commit() ? ProjectFileError::None : ProjectFileError::Io;
}

} // namespace

ProjectReadResult ReadProjectFile(const QString& path) {
    QByteArray bytes;
    if (const auto error = ReadBytes(path, bytes); error != ProjectFileError::None)
        return {.error = error};
    if (bytes.size() < kMagic.size() + static_cast<qsizetype>(sizeof(kVersion)) + kChecksumBytes ||
        !QByteArrayView(bytes).startsWith(kMagic))
        return {.error = ProjectFileError::Invalid};

    const auto body = QByteArrayView(bytes).chopped(kChecksumBytes);
    if (Digest(body) != QByteArrayView(bytes).last(kChecksumBytes))
        return {.error = ProjectFileError::Invalid};
    const auto payload = body.sliced(kMagic.size());
    const auto data = QByteArray::fromRawData(payload.data(), payload.size());
    QDataStream stream(data);
    ProjectArchive<true> reader(stream, QFileInfo(path).absoluteDir());
    quint32 version = 0;
    stream >> version;
    if (version != kVersion)
        return {.error = ProjectFileError::UnsupportedVersion};

    ProjectReadResult result;
    reader(result.document);
    if (!reader.valid() || !stream.atEnd())
        return {.error = ProjectFileError::Invalid};
    result.digest = Digest(bytes);
    return result;
}

ProjectFileError WriteProjectFile(const QString& path, const ProjectDocument& document,
                                  const std::optional<QByteArray>& expected_digest, QByteArray& written_digest) {
    const QFileInfo requested(path);
    const auto parent = requested.absoluteDir().canonicalPath();
    if (parent.isEmpty())
        return ProjectFileError::Io;
    const auto destination = QDir(parent).absoluteFilePath(requested.fileName());
    QByteArray bytes(kMagic.data(), kMagic.size());
    QDataStream stream(&bytes, QIODevice::Append);
    ProjectArchive<false> writer(stream, requested.absoluteDir());
    stream << kVersion;
    writer(document);
    if (!writer.valid() || bytes.size() > kFileLimit - kChecksumBytes)
        return ProjectFileError::Invalid;
    bytes += Digest(bytes);

    QLockFile lock(destination + QStringLiteral(".exosnap-write.lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock())
        return lock.error() == QLockFile::LockFailedError ? ProjectFileError::Changed : ProjectFileError::Io;
    QByteArray before;
    if (const auto error = ObserveDestination(destination, before); error != ProjectFileError::None)
        return error;
    if (expected_digest && before != *expected_digest)
        return ProjectFileError::Changed;

    if (const auto error = CommitBytes(destination, before, bytes); error != ProjectFileError::None)
        return error;
    written_digest = Digest(bytes);
    return ProjectFileError::None;
}

} // namespace exosnap::edit
