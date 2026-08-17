// handoff.cpp -- serialisation, schema validation and install-context checks for
// the App -> Updater handoff document.

#include <update_handoff/handoff.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRandomGenerator>
#include <QSaveFile>

#include <update/install_mode_detector.h>
#include <update/swap_engine.h>

namespace exosnap::update_handoff {
namespace {

using exosnap::update::InstallMode;

constexpr const char* kExeName = "exosnap.exe";

// Field names, in one place. The writer, the reader and the tests all spell them
// from here, so a rename is a compile error rather than a silent contract break.
constexpr const char* kFieldVersion = "handoffVersion";
constexpr const char* kFieldTransactionId = "updateTransactionId";
constexpr const char* kFieldTargetVersion = "targetVersion";
constexpr const char* kFieldCurrentVersion = "currentVersion";
constexpr const char* kFieldManifestPath = "manifestPath";
constexpr const char* kFieldSignaturePath = "manifestSignaturePath";
constexpr const char* kFieldInstallMode = "installMode";
constexpr const char* kFieldInstallDir = "installDir";
constexpr const char* kFieldAppPid = "appPid";
constexpr const char* kFieldVerifyReinstall = "verifyReinstall";

constexpr const char* kInstallModeInstalled = "installed";
constexpr const char* kInstallModePortable = "portable";

HandoffLoadResult Reject(HandoffRejection rejection, const QString& detail) {
    HandoffLoadResult result;
    result.rejection = rejection;
    result.detail = detail;
    return result;
}

// A required string field: present, of string type and non-empty. Empty is
// rejected rather than accepted-and-defaulted, because every string in this
// document names something the run cannot proceed without.
bool TakeRequiredString(const QJsonObject& object, const char* field, QString* out, HandoffLoadResult* rejection) {
    const QJsonValue value = object.value(QLatin1String(field));
    if (value.isUndefined() || value.isNull()) {
        *rejection = Reject(HandoffRejection::MissingField, QLatin1String(field));
        return false;
    }
    if (!value.isString()) {
        *rejection =
            Reject(HandoffRejection::InvalidField, QStringLiteral("%1 is not a string").arg(QLatin1String(field)));
        return false;
    }
    const QString text = value.toString();
    if (text.isEmpty()) {
        *rejection = Reject(HandoffRejection::MissingField, QStringLiteral("%1 is empty").arg(QLatin1String(field)));
        return false;
    }
    *out = text;
    return true;
}

// Canonicalised for comparison: forward slashes collapsed to the platform form,
// no trailing separator, case-folded. Windows paths are case-insensitive, and a
// registry stamp that differs only in case is the same directory.
QString CanonicalDir(const QString& path) {
    if (path.isEmpty())
        return {};
    return QDir::toNativeSeparators(QDir::cleanPath(path)).toLower();
}

} // namespace

const char* HandoffRejectionName(HandoffRejection rejection) noexcept {
    switch (rejection) {
    case HandoffRejection::None:
        return "none";
    case HandoffRejection::FileUnreadable:
        return "fileUnreadable";
    case HandoffRejection::MalformedJson:
        return "malformedJson";
    case HandoffRejection::UnsupportedVersion:
        return "unsupportedVersion";
    case HandoffRejection::MissingField:
        return "missingField";
    case HandoffRejection::InvalidField:
        return "invalidField";
    }
    return "invalidField";
}

const char* InstallContextRejectionName(InstallContextRejection rejection) noexcept {
    switch (rejection) {
    case InstallContextRejection::None:
        return "none";
    case InstallContextRejection::PathNotAbsolute:
        return "pathNotAbsolute";
    case InstallContextRejection::DirectoryMissing:
        return "directoryMissing";
    case InstallContextRejection::ExecutableMissing:
        return "executableMissing";
    case InstallContextRejection::VersionUnreadable:
        return "versionUnreadable";
    case InstallContextRejection::VersionMismatch:
        return "versionMismatch";
    case InstallContextRejection::RegistryMismatch:
        return "registryMismatch";
    }
    return "directoryMissing";
}

QByteArray SerializeUpdateHandoff(const UpdateHandoff& handoff) {
    QJsonObject object;
    object.insert(QLatin1String(kFieldVersion), handoff.handoff_version);
    object.insert(QLatin1String(kFieldTransactionId), handoff.update_transaction_id);
    object.insert(QLatin1String(kFieldTargetVersion), handoff.target_version);
    object.insert(QLatin1String(kFieldCurrentVersion), handoff.current_version);
    object.insert(QLatin1String(kFieldManifestPath), handoff.manifest_path);
    object.insert(QLatin1String(kFieldSignaturePath), handoff.manifest_signature_path);
    object.insert(QLatin1String(kFieldInstallMode), handoff.install_mode == InstallMode::Installed
                                                        ? QLatin1String(kInstallModeInstalled)
                                                        : QLatin1String(kInstallModePortable));
    object.insert(QLatin1String(kFieldInstallDir), handoff.install_dir);
    object.insert(QLatin1String(kFieldAppPid), static_cast<double>(handoff.app_pid));
    object.insert(QLatin1String(kFieldVerifyReinstall), handoff.verify_reinstall);
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

HandoffLoadResult ParseUpdateHandoff(const QByteArray& bytes) {
    QJsonParseError parse_error{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError)
        return Reject(HandoffRejection::MalformedJson, parse_error.errorString());
    if (!document.isObject())
        return Reject(HandoffRejection::MalformedJson, QStringLiteral("the document is not a JSON object"));

    const QJsonObject object = document.object();

    // The version gate comes FIRST and is exact. Reading any other field before
    // knowing the schema is reading a field whose meaning is not yet
    // established.
    const QJsonValue version = object.value(QLatin1String(kFieldVersion));
    if (!version.isDouble())
        return Reject(HandoffRejection::UnsupportedVersion,
                      QStringLiteral("%1 is absent or not a number").arg(QLatin1String(kFieldVersion)));
    if (version.toInt(-1) != kHandoffVersion)
        return Reject(HandoffRejection::UnsupportedVersion,
                      QStringLiteral("handoffVersion %1 is not supported (this build reads %2)")
                          .arg(version.toInt(-1))
                          .arg(kHandoffVersion));

    UpdateHandoff handoff;
    handoff.handoff_version = kHandoffVersion;

    HandoffLoadResult rejection;
    if (!TakeRequiredString(object, kFieldTransactionId, &handoff.update_transaction_id, &rejection))
        return rejection;
    if (!TakeRequiredString(object, kFieldTargetVersion, &handoff.target_version, &rejection))
        return rejection;
    if (!TakeRequiredString(object, kFieldCurrentVersion, &handoff.current_version, &rejection))
        return rejection;
    if (!TakeRequiredString(object, kFieldManifestPath, &handoff.manifest_path, &rejection))
        return rejection;
    if (!TakeRequiredString(object, kFieldSignaturePath, &handoff.manifest_signature_path, &rejection))
        return rejection;
    if (!TakeRequiredString(object, kFieldInstallDir, &handoff.install_dir, &rejection))
        return rejection;

    QString install_mode;
    if (!TakeRequiredString(object, kFieldInstallMode, &install_mode, &rejection))
        return rejection;
    if (install_mode == QLatin1String(kInstallModeInstalled)) {
        handoff.install_mode = InstallMode::Installed;
    } else if (install_mode == QLatin1String(kInstallModePortable)) {
        handoff.install_mode = InstallMode::Portable;
    } else {
        return Reject(HandoffRejection::InvalidField,
                      QStringLiteral("installMode '%1' (expected installed|portable)").arg(install_mode));
    }

    const QJsonValue pid = object.value(QLatin1String(kFieldAppPid));
    if (!pid.isDouble())
        return Reject(HandoffRejection::MissingField, QLatin1String(kFieldAppPid));
    const double pid_number = pid.toDouble(-1.0);
    if (pid_number <= 0.0 || pid_number > 4294967295.0)
        return Reject(HandoffRejection::InvalidField,
                      QStringLiteral("appPid %1 is not a running process id").arg(pid_number));
    handoff.app_pid = static_cast<quint32>(pid_number);

    const QJsonValue verify = object.value(QLatin1String(kFieldVerifyReinstall));
    if (!verify.isBool())
        return Reject(HandoffRejection::MissingField, QLatin1String(kFieldVerifyReinstall));
    handoff.verify_reinstall = verify.toBool();

    // The paths are absolute by contract: a relative path would resolve against
    // whatever working directory the updater happens to have been given.
    if (!QDir::isAbsolutePath(handoff.manifest_path) || !QDir::isAbsolutePath(handoff.manifest_signature_path))
        return Reject(HandoffRejection::InvalidField, QStringLiteral("manifest paths must be absolute"));
    if (!QDir::isAbsolutePath(handoff.install_dir))
        return Reject(HandoffRejection::InvalidField, QStringLiteral("installDir must be absolute"));

    HandoffLoadResult result;
    result.handoff = handoff;
    return result;
}

HandoffLoadResult LoadUpdateHandoff(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return Reject(HandoffRejection::FileUnreadable, QStringLiteral("%1: %2").arg(path, file.errorString()));
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError)
        return Reject(HandoffRejection::FileUnreadable, QStringLiteral("%1: %2").arg(path, file.errorString()));
    return ParseUpdateHandoff(bytes);
}

bool WriteUpdateHandoffAtomically(const QString& path, const UpdateHandoff& handoff, QString* error) {
    const QByteArray bytes = SerializeUpdateHandoff(handoff);

    // QSaveFile is the atomic write: it buffers into a temporary sibling and
    // renames it over the destination in commit(). The updater can therefore
    // never observe a half-written document -- it either sees the previous file
    // (or none) or the complete new one.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr)
            *error = QStringLiteral("%1: %2").arg(path, file.errorString());
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        if (error != nullptr)
            *error = QStringLiteral("%1: %2").arg(path, file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error != nullptr)
            *error = QStringLiteral("%1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

QString MakeUpdateTransactionId() {
    const quint64 value = QRandomGenerator::global()->generate64();
    return QStringLiteral("u-%1").arg(value, 16, 16, QLatin1Char('0'));
}

InstallContextRejection ValidateInstallContext(const InstallContextFacts& facts) {
    if (!facts.path_is_absolute)
        return InstallContextRejection::PathNotAbsolute;
    if (!facts.directory_exists)
        return InstallContextRejection::DirectoryMissing;
    // The shape check that makes this an ExoSnap installation rather than an
    // arbitrary directory a tampered document pointed at.
    if (!facts.executable_exists)
        return InstallContextRejection::ExecutableMissing;
    if (facts.executable_product_version.isEmpty())
        return InstallContextRejection::VersionUnreadable;
    // The binding: the document claims version C is running in this directory,
    // and the executable in it has to say the same thing. Exact string equality,
    // the same rule the target-version gate uses -- SemVer normalisation would
    // make "0.9.0-rc1" and "0.9.0-beta1" interchangeable here.
    if (facts.executable_product_version != facts.claimed_current_version)
        return InstallContextRejection::VersionMismatch;
    // Installed mode has a second, independent record of where the installation
    // is. When Windows kept one, the document has to agree with it.
    if (facts.install_mode == InstallMode::Installed && !facts.registry_install_dir.isEmpty() &&
        CanonicalDir(facts.registry_install_dir) != CanonicalDir(facts.install_dir))
        return InstallContextRejection::RegistryMismatch;
    return InstallContextRejection::None;
}

InstallContextRejection ValidateInstallContextOnDisk(const UpdateHandoff& handoff, QString* detail) {
    InstallContextFacts facts;
    facts.install_mode = handoff.install_mode;
    facts.claimed_current_version = handoff.current_version;
    facts.install_dir = handoff.install_dir;
    facts.path_is_absolute = QDir::isAbsolutePath(handoff.install_dir);

    const QFileInfo directory(handoff.install_dir);
    facts.directory_exists = directory.exists() && directory.isDir();

    const QString exe = QDir(handoff.install_dir).filePath(QLatin1String(kExeName));
    facts.executable_exists = QFileInfo::exists(exe);
    if (facts.executable_exists) {
        const std::optional<std::string> product =
            exosnap::update::ReadProductVersionString(QDir::toNativeSeparators(exe).toStdWString());
        if (product.has_value())
            facts.executable_product_version = QString::fromStdString(*product);
    }

    if (handoff.install_mode == InstallMode::Installed) {
        if (const std::optional<std::wstring> recorded = exosnap::update::ReadInstallPath();
            recorded.has_value() && !recorded->empty())
            facts.registry_install_dir = QString::fromStdWString(*recorded);
    }

    const InstallContextRejection rejection = ValidateInstallContext(facts);
    if (rejection != InstallContextRejection::None && detail != nullptr) {
        *detail = QStringLiteral("%1 (installDir=\"%2\", claimed=\"%3\", measured=\"%4\")")
                      .arg(QString::fromLatin1(InstallContextRejectionName(rejection)), handoff.install_dir,
                           facts.claimed_current_version,
                           facts.executable_product_version.isEmpty() ? QStringLiteral("<unreadable>")
                                                                      : facts.executable_product_version);
    }
    return rejection;
}

bool HandoffAssetsPresent(const UpdateHandoff& handoff, QString* detail) {
    for (const QString& path : {handoff.manifest_path, handoff.manifest_signature_path}) {
        if (!QFileInfo::exists(path)) {
            if (detail != nullptr)
                *detail = QStringLiteral("the handoff names \"%1\", which does not exist").arg(path);
            return false;
        }
    }
    return true;
}

} // namespace exosnap::update_handoff
