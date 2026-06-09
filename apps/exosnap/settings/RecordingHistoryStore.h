#pragma once

#include <QString>
#include <QVector>

#include "models/CompletedRecording.h"

namespace exosnap {

class RecordingHistoryStore {
  public:
    RecordingHistoryStore();
    explicit RecordingHistoryStore(QString file_path);

    [[nodiscard]] QVector<CompletedRecording> Load() const;
    bool Save(const QVector<CompletedRecording>& recordings) const;

    [[nodiscard]] const QString& StorePath() const;

  private:
    QString file_path_;

    static constexpr int kSchemaVersion = 1;
};

} // namespace exosnap
