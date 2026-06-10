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

    // v1: single-file recordings only.
    // v2: adds per-recording "segments" array (SPLIT-RECORDING-R1). v1 files load
    //     forward-compatibly (no segments => single-file recording).
    static constexpr int kSchemaVersion = 2;
};

} // namespace exosnap
