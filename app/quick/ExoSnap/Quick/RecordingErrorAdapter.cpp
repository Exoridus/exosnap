#include "RecordingErrorAdapter.h"

#include "models/RecordingErrorDetailText.h"

#include <QStringList>
#include <QVariantMap>

namespace exosnap::quick {
namespace {

QVariantMap Row(const QString& label, const QString& value) {
    QVariantMap row;
    row.insert(QStringLiteral("label"), label);
    row.insert(QStringLiteral("value"), value);
    return row;
}

} // namespace

RecordingErrorAdapter::RecordingErrorAdapter(QObject* parent) : QObject(parent) {
}

bool RecordingErrorAdapter::active() const noexcept {
    return active_;
}

const QString& RecordingErrorAdapter::title() const noexcept {
    return report_.title;
}

const QString& RecordingErrorAdapter::summary() const noexcept {
    return report_.summary;
}

const QVariantList& RecordingErrorAdapter::detailRows() const noexcept {
    return detail_rows_;
}

bool RecordingErrorAdapter::canSendReport() const noexcept {
    return can_send_report_;
}

const models::RecordingFailureReport& RecordingErrorAdapter::report() const noexcept {
    return report_;
}

void RecordingErrorAdapter::present(const models::RecordingFailureReport& report, bool can_send_report) {
    report_ = report;
    can_send_report_ = can_send_report;
    active_ = true;
    rebuildDetailRows();
    emit changed();
}

void RecordingErrorAdapter::rebuildDetailRows() {
    detail_rows_.clear();
    // Empty fields are omitted rather than shown blank: a "CODE —" row tells the
    // user nothing and costs a line in a card that has to fit at 860x700.
    if (!report_.phase.isEmpty())
        detail_rows_.append(Row(QStringLiteral("PHASE"), report_.phase));
    if (!report_.code.isEmpty())
        detail_rows_.append(Row(QStringLiteral("CODE"), report_.code));

    QStringList format_bits;
    if (!report_.container.isEmpty())
        format_bits << report_.container;
    if (!report_.video_codec.isEmpty())
        format_bits << report_.video_codec;
    if (!report_.audio_codec.isEmpty())
        format_bits << report_.audio_codec;
    if (!format_bits.isEmpty())
        detail_rows_.append(Row(QStringLiteral("FORMAT"), format_bits.join(QStringLiteral(" \xc2\xb7 "))));

    if (!report_.detail.isEmpty()) {
        // The engine reports C++ enum tokens ("Container::Matroska requires
        // VideoCodec::Av1"). Humanized to the shared codec-label canon so the
        // panel never leaks internal spelling.
        detail_rows_.append(Row(QStringLiteral("DETAIL"), ui::dialogs::HumanizeEngineDetail(report_.detail)));
    }
}

void RecordingErrorAdapter::dismiss() {
    if (!active_)
        return;
    active_ = false;
    emit changed();
}

void RecordingErrorAdapter::sendReport() {
    if (!active_ || !can_send_report_)
        return;
    emit sendReportRequested();
    dismiss();
}

void RecordingErrorAdapter::openLogs() {
    if (!active_)
        return;
    emit openLogsRequested();
    dismiss();
}

} // namespace exosnap::quick
