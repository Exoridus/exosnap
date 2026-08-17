#pragma once

// SettingsAutomationKeys.h -- the stable product keys `settings.get` and
// `settings.set` speak, and the only way either of them reaches a setting.
//
// Three properties make this a contract rather than a convenience:
//
//  1. The keys are PRODUCT names ("video.container", "audio.sampleRate"), not
//     QML property names. A protocol field named after a frontend property turns
//     that property into a compatibility promise, and the promise is then kept
//     by never renaming it.
//  2. Enum values are the product's own canonical spellings ("MKV", "AV1", "P6"),
//     resolved through ui::CodecLabels where one exists. A wire value is never a
//     raw enumerator ordinal: that would make the enum's DECLARATION ORDER part
//     of the protocol.
//  3. Every write goes through the SettingsAdapter setter the QML control writes
//     to. Validation, container/codec reconciliation, persistence and the
//     propagation into the recording side therefore all happen exactly as they
//     do for a user edit. Nothing here mutates a model directly, and nothing
//     here re-implements a rule.
//
// This is a fixed table, not reflection: a key that is not listed cannot be read
// or written, and adding one is a code change with a review.

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace exosnap::quick {

class SettingsAdapter;

namespace settings_automation {

enum class ValueType {
    Bool,
    Int,
    Number,
    Enum, // one of `allowed`
    Text, // free-form string constrained by the product, not by this table
};

[[nodiscard]] QString ValueTypeName(ValueType type);

struct KeyDescriptor {
    QString key;
    ValueType type = ValueType::Bool;
    // For Enum: the accepted wire values, in the product's own spelling.
    QStringList allowed;
    QString description;

    std::function<QJsonValue(const SettingsAdapter&)> read;
    // Returns false with `error` filled for a value this key cannot take. A
    // write that the PRODUCT then reconciles to something else is still a
    // success -- reconciliation is the product's answer, not a rejection.
    std::function<bool(SettingsAdapter&, const QJsonValue&, QString*)> write;
};

[[nodiscard]] const QVector<KeyDescriptor>& AllKeys();
[[nodiscard]] const KeyDescriptor* FindKey(const QString& key);

// `settings.describe`: every key with its type and accepted values. The payload
// a client reads before it writes anything, so a rejected value is a client bug
// rather than a discovery mechanism.
[[nodiscard]] QJsonObject DescribeKeys();

// `settings.get`: one key, or every key when `key` is empty.
[[nodiscard]] QJsonObject ReadKeys(const SettingsAdapter& adapter, const QString& key, QString* error);

// `settings.set`: writes through the product edge and reports what the setting
// reads back as afterwards -- which is the reconciled value, not the requested
// one. A caller diffs the two to see that the product changed its mind.
[[nodiscard]] bool WriteKey(SettingsAdapter& adapter, const QString& key, const QJsonValue& value, QString* error);

} // namespace settings_automation
} // namespace exosnap::quick
