// test_update_handoff.cpp -- the handoff document's schema, its round trip, its
// atomic write and the install-context rule.
//
// The theme throughout: this document is UNTRUSTED input. Every test that
// mutates a field asserts a refusal, not a repair.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <update_handoff/handoff.h>

using namespace exosnap::update_handoff;
using exosnap::update::InstallMode;

namespace {

UpdateHandoff Sample() {
    UpdateHandoff handoff;
    handoff.update_transaction_id = QStringLiteral("u-0123456789abcdef");
    handoff.target_version = QStringLiteral("0.9.0-rc9");
    handoff.current_version = QStringLiteral("0.9.0-rc1");
    handoff.manifest_path = QStringLiteral("C:/scratch/u-1/update-manifest.json");
    handoff.manifest_signature_path = QStringLiteral("C:/scratch/u-1/update-manifest.json.sig");
    handoff.install_mode = InstallMode::Portable;
    handoff.install_dir = QStringLiteral("C:/scratch/install");
    handoff.app_pid = 4242;
    handoff.verify_reinstall = false;
    return handoff;
}

// The serialised document with ONE field replaced -- the shape every tampering
// test needs.
QByteArray SerializedWith(const QString& field, const QJsonValue& value) {
    QJsonObject object = QJsonDocument::fromJson(SerializeUpdateHandoff(Sample())).object();
    if (value.isUndefined())
        object.remove(field);
    else
        object.insert(field, value);
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

InstallContextFacts GoodFacts() {
    InstallContextFacts facts;
    facts.path_is_absolute = true;
    facts.directory_exists = true;
    facts.executable_exists = true;
    facts.executable_product_version = QStringLiteral("0.9.0-rc1");
    facts.claimed_current_version = QStringLiteral("0.9.0-rc1");
    facts.install_mode = InstallMode::Portable;
    facts.install_dir = QStringLiteral("C:/scratch/install");
    return facts;
}

} // namespace

// ---------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------

TEST(UpdateHandoffSchema, RoundTripsEveryField) {
    const UpdateHandoff original = Sample();
    const HandoffLoadResult parsed = ParseUpdateHandoff(SerializeUpdateHandoff(original));
    ASSERT_TRUE(parsed.ok()) << HandoffRejectionName(parsed.rejection) << ": " << parsed.detail.toStdString();
    EXPECT_EQ(*parsed.handoff, original);
}

TEST(UpdateHandoffSchema, RoundTripsInstalledModeAndVerifyReinstall) {
    UpdateHandoff original = Sample();
    original.install_mode = InstallMode::Installed;
    original.verify_reinstall = true;
    original.target_version = original.current_version; // ADR 0055: the identical version
    const HandoffLoadResult parsed = ParseUpdateHandoff(SerializeUpdateHandoff(original));
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.handoff->install_mode, InstallMode::Installed);
    EXPECT_TRUE(parsed.handoff->verify_reinstall);
    EXPECT_EQ(parsed.handoff->target_version, parsed.handoff->current_version);
}

// Nothing large or trust-bearing is embedded: the document REFERENCES the
// manifest, it does not carry it, and it carries no signature of its own.
TEST(UpdateHandoffSchema, CarriesNoManifestBodyAndNoSignature) {
    const QJsonObject object = QJsonDocument::fromJson(SerializeUpdateHandoff(Sample())).object();
    EXPECT_FALSE(object.contains(QStringLiteral("manifest")));
    EXPECT_FALSE(object.contains(QStringLiteral("signature")));
    EXPECT_FALSE(object.contains(QStringLiteral("packages")));
    EXPECT_FALSE(object.contains(QStringLiteral("sha256")));
    EXPECT_LT(SerializeUpdateHandoff(Sample()).size(), 1024);
}

// ---------------------------------------------------------------------------
// Version gate
// ---------------------------------------------------------------------------

TEST(UpdateHandoffSchema, UnknownVersionIsRejectedOutright) {
    for (const int version : {0, 2, 99}) {
        const HandoffLoadResult parsed = ParseUpdateHandoff(SerializedWith(QStringLiteral("handoffVersion"), version));
        EXPECT_FALSE(parsed.ok()) << "handoffVersion " << version << " must not be interpreted";
        EXPECT_EQ(parsed.rejection, HandoffRejection::UnsupportedVersion);
    }
}

TEST(UpdateHandoffSchema, MissingVersionIsRejected) {
    const HandoffLoadResult parsed = ParseUpdateHandoff(SerializedWith(QStringLiteral("handoffVersion"), QJsonValue()));
    EXPECT_EQ(parsed.rejection, HandoffRejection::UnsupportedVersion);
}

// The forward-compatible half of the rule: an unknown ADDITIONAL key is ignored,
// so a future writer may add fields without breaking this reader -- but it may
// not redefine the ones here without bumping the version.
TEST(UpdateHandoffSchema, UnknownAdditionalFieldsAreIgnored) {
    const HandoffLoadResult parsed =
        ParseUpdateHandoff(SerializedWith(QStringLiteral("somethingFromTheFuture"), QStringLiteral("value")));
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(*parsed.handoff, Sample());
}

// ---------------------------------------------------------------------------
// Malformed / missing / invalid
// ---------------------------------------------------------------------------

TEST(UpdateHandoffSchema, MalformedJsonIsRejected) {
    EXPECT_EQ(ParseUpdateHandoff(QByteArray("{ not json")).rejection, HandoffRejection::MalformedJson);
    EXPECT_EQ(ParseUpdateHandoff(QByteArray("[]")).rejection, HandoffRejection::MalformedJson);
    EXPECT_EQ(ParseUpdateHandoff(QByteArray()).rejection, HandoffRejection::MalformedJson);
}

TEST(UpdateHandoffSchema, EveryRequiredStringFieldIsRequired) {
    for (const char* field : {"updateTransactionId", "targetVersion", "currentVersion", "manifestPath",
                              "manifestSignaturePath", "installMode", "installDir"}) {
        const QString name = QString::fromLatin1(field);
        EXPECT_EQ(ParseUpdateHandoff(SerializedWith(name, QJsonValue())).rejection, HandoffRejection::MissingField)
            << field << " absent";
        EXPECT_EQ(ParseUpdateHandoff(SerializedWith(name, QString())).rejection, HandoffRejection::MissingField)
            << field << " empty";
    }
}

TEST(UpdateHandoffSchema, AppPidAndVerifyReinstallAreRequiredAndTyped) {
    EXPECT_EQ(ParseUpdateHandoff(SerializedWith(QStringLiteral("appPid"), QJsonValue())).rejection,
              HandoffRejection::MissingField);
    // A handoff without a parent to wait for cannot sequence the swap.
    EXPECT_EQ(ParseUpdateHandoff(SerializedWith(QStringLiteral("appPid"), 0)).rejection,
              HandoffRejection::InvalidField);
    EXPECT_EQ(ParseUpdateHandoff(SerializedWith(QStringLiteral("verifyReinstall"), QJsonValue())).rejection,
              HandoffRejection::MissingField);
    EXPECT_EQ(ParseUpdateHandoff(SerializedWith(QStringLiteral("verifyReinstall"), QStringLiteral("true"))).rejection,
              HandoffRejection::MissingField);
}

TEST(UpdateHandoffSchema, UnsupportedInstallModeIsRejected) {
    EXPECT_EQ(ParseUpdateHandoff(SerializedWith(QStringLiteral("installMode"), QStringLiteral("msix"))).rejection,
              HandoffRejection::InvalidField);
    EXPECT_EQ(ParseUpdateHandoff(SerializedWith(QStringLiteral("installMode"), QStringLiteral("Portable"))).rejection,
              HandoffRejection::InvalidField)
        << "the vocabulary is exact, not case-folded";
}

TEST(UpdateHandoffSchema, RelativePathsAreRejected) {
    EXPECT_EQ(ParseUpdateHandoff(SerializedWith(QStringLiteral("installDir"), QStringLiteral("install"))).rejection,
              HandoffRejection::InvalidField);
    EXPECT_EQ(ParseUpdateHandoff(SerializedWith(QStringLiteral("manifestPath"), QStringLiteral("m.json"))).rejection,
              HandoffRejection::InvalidField);
}

// ---------------------------------------------------------------------------
// File IO
// ---------------------------------------------------------------------------

TEST(UpdateHandoffIo, WritesAtomicallyAndReadsBack) {
    QTemporaryDir scratch;
    ASSERT_TRUE(scratch.isValid());
    const QString path = QDir(scratch.path()).filePath(QString::fromLatin1(kHandoffFileName));

    QString error;
    ASSERT_TRUE(WriteUpdateHandoffAtomically(path, Sample(), &error)) << error.toStdString();
    // The atomic write leaves exactly the destination behind -- no temporary
    // sibling a reader could pick up or a cleanup would have to know about.
    EXPECT_EQ(QDir(scratch.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);

    const HandoffLoadResult loaded = LoadUpdateHandoff(path);
    ASSERT_TRUE(loaded.ok()) << loaded.detail.toStdString();
    EXPECT_EQ(*loaded.handoff, Sample());
}

TEST(UpdateHandoffIo, OverwriteIsAlsoAtomicAndCompleteAfterwards) {
    QTemporaryDir scratch;
    ASSERT_TRUE(scratch.isValid());
    const QString path = QDir(scratch.path()).filePath(QString::fromLatin1(kHandoffFileName));

    UpdateHandoff first = Sample();
    first.update_transaction_id = QStringLiteral("u-1111111111111111");
    ASSERT_TRUE(WriteUpdateHandoffAtomically(path, first, nullptr));

    UpdateHandoff second = Sample();
    second.update_transaction_id = QStringLiteral("u-2222222222222222");
    ASSERT_TRUE(WriteUpdateHandoffAtomically(path, second, nullptr));

    const HandoffLoadResult loaded = LoadUpdateHandoff(path);
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded.handoff->update_transaction_id, second.update_transaction_id);
}

TEST(UpdateHandoffIo, MissingFileIsUnreadableRatherThanMalformed) {
    QTemporaryDir scratch;
    ASSERT_TRUE(scratch.isValid());
    const HandoffLoadResult loaded = LoadUpdateHandoff(QDir(scratch.path()).filePath(QStringLiteral("absent.json")));
    EXPECT_EQ(loaded.rejection, HandoffRejection::FileUnreadable);
}

TEST(UpdateHandoffIo, AssetPresenceIsCheckedSeparatelyFromTheSchema) {
    QTemporaryDir scratch;
    ASSERT_TRUE(scratch.isValid());
    UpdateHandoff handoff = Sample();
    handoff.manifest_path = QDir(scratch.path()).filePath(QString::fromLatin1(kManifestFileName));
    handoff.manifest_signature_path = QDir(scratch.path()).filePath(QString::fromLatin1(kManifestSignatureFileName));

    QString detail;
    EXPECT_FALSE(HandoffAssetsPresent(handoff, &detail));
    EXPECT_TRUE(detail.contains(QString::fromLatin1(kManifestFileName)));

    for (const QString& path : {handoff.manifest_path, handoff.manifest_signature_path}) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("x");
        file.close();
    }
    EXPECT_TRUE(HandoffAssetsPresent(handoff, &detail));
}

// ---------------------------------------------------------------------------
// Transaction identity
// ---------------------------------------------------------------------------

TEST(UpdateTransactionId, IsOpaqueAndFreshPerOperation) {
    const QString first = MakeUpdateTransactionId();
    const QString second = MakeUpdateTransactionId();
    EXPECT_NE(first, second);
    EXPECT_TRUE(first.startsWith(QStringLiteral("u-")));
    EXPECT_EQ(first.size(), 18);
    // No path, no version, no host name: it correlates, it does not describe.
    EXPECT_FALSE(first.contains(QLatin1Char('\\')));
    EXPECT_FALSE(first.contains(QLatin1Char('/')));
}

// ---------------------------------------------------------------------------
// Install-context rule
// ---------------------------------------------------------------------------

TEST(InstallContext, AcceptsAnExoSnapTreeRunningTheClaimedVersion) {
    EXPECT_EQ(ValidateInstallContext(GoodFacts()), InstallContextRejection::None);
}

TEST(InstallContext, RejectsInOrderOfEvidence) {
    InstallContextFacts facts = GoodFacts();
    facts.path_is_absolute = false;
    EXPECT_EQ(ValidateInstallContext(facts), InstallContextRejection::PathNotAbsolute);

    facts = GoodFacts();
    facts.directory_exists = false;
    EXPECT_EQ(ValidateInstallContext(facts), InstallContextRejection::DirectoryMissing);

    // The check that stops the document from naming an arbitrary directory: any
    // folder can exist, only an ExoSnap installation carries exosnap.exe.
    facts = GoodFacts();
    facts.executable_exists = false;
    EXPECT_EQ(ValidateInstallContext(facts), InstallContextRejection::ExecutableMissing);

    facts = GoodFacts();
    facts.executable_product_version.clear();
    EXPECT_EQ(ValidateInstallContext(facts), InstallContextRejection::VersionUnreadable);
}

// The binding that makes installDir more than "a directory that exists": the
// executable there has to be the version the document says is running.
TEST(InstallContext, RejectsADirectoryRunningAnotherVersion) {
    InstallContextFacts facts = GoodFacts();
    facts.executable_product_version = QStringLiteral("0.8.1");
    EXPECT_EQ(ValidateInstallContext(facts), InstallContextRejection::VersionMismatch);
}

// Exact string equality, deliberately: SemVer normalisation collapses foreign
// prerelease labels and would make two different builds interchangeable here.
TEST(InstallContext, VersionComparisonIsExactStringEquality) {
    InstallContextFacts facts = GoodFacts();
    facts.claimed_current_version = QStringLiteral("0.9.0-rc1");
    facts.executable_product_version = QStringLiteral("0.9.0-beta1");
    EXPECT_EQ(ValidateInstallContext(facts), InstallContextRejection::VersionMismatch);
}

TEST(InstallContext, InstalledModeMustAgreeWithTheRecordedLocation) {
    InstallContextFacts facts = GoodFacts();
    facts.install_mode = InstallMode::Installed;
    facts.registry_install_dir = QStringLiteral("C:/Program Files/ExoSnap");
    EXPECT_EQ(ValidateInstallContext(facts), InstallContextRejection::RegistryMismatch);

    // Same directory, different spelling: separators and case are not a
    // mismatch on Windows.
    facts.registry_install_dir = QStringLiteral("C:\\SCRATCH\\install\\");
    EXPECT_EQ(ValidateInstallContext(facts), InstallContextRejection::None);

    // No record kept: the check has nothing to compare and does not invent one.
    facts.registry_install_dir.clear();
    EXPECT_EQ(ValidateInstallContext(facts), InstallContextRejection::None);
}

// Portable installations are not registered, so a stray registry stamp from an
// unrelated installed copy must not decide anything about them.
TEST(InstallContext, PortableModeIgnoresTheRegistryRecord) {
    InstallContextFacts facts = GoodFacts();
    facts.install_mode = InstallMode::Portable;
    facts.registry_install_dir = QStringLiteral("C:/Program Files/ExoSnap");
    EXPECT_EQ(ValidateInstallContext(facts), InstallContextRejection::None);
}
