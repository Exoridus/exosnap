#include "QuickLiveVerifySource.h"

#include "AboutViewModelAdapter.h"
#include "DiagnosticsAdapter.h"
#include "EditPlayerAdapter.h"
#include "EditSessionAdapter.h"
#include "QuickApplication.h"
#include "RecordPreviewAdapter.h"
#include "RecordViewModelAdapter.h"
#include "SettingsAdapter.h"

#include "diagnostics/NativeWindowFacts.h"
#include "models/AboutInfo.h"
#include "services/RecordingCoordinator.h"
#include "ui/CodecLabels.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QQuickWindow>
#include <QRect>
#include <QScreen>

#include <windows.h>

namespace exosnap::quick {
namespace {

QString ToQString(const std::wstring& value) {
    return QString::fromStdWString(value);
}

QJsonObject ScreenJson(const QScreen* screen) {
    QJsonObject json;
    if (screen == nullptr)
        return json;
    const QRect geometry = screen->geometry();
    json.insert(QStringLiteral("name"), screen->name());
    json.insert(QStringLiteral("x"), geometry.x());
    json.insert(QStringLiteral("y"), geometry.y());
    json.insert(QStringLiteral("width"), geometry.width());
    json.insert(QStringLiteral("height"), geometry.height());
    json.insert(QStringLiteral("devicePixelRatio"), screen->devicePixelRatio());
    json.insert(QStringLiteral("refreshHz"), screen->refreshRate());
    return json;
}

QJsonObject NativeFactsJson(const diagnostics::NativeWindowFacts& facts) {
    QJsonObject json;
    json.insert(QStringLiteral("valid"), facts.valid);
    json.insert(QStringLiteral("style"), QStringLiteral("0x%1").arg(facts.style, 8, 16, QLatin1Char('0')));
    json.insert(QStringLiteral("exStyle"), QStringLiteral("0x%1").arg(facts.ex_style, 8, 16, QLatin1Char('0')));
    json.insert(QStringLiteral("layered"), facts.layered);
    json.insert(QStringLiteral("transparentForInput"), facts.transparent_for_input);
    json.insert(QStringLiteral("childHwnds"), facts.child_hwnds);
    json.insert(QStringLiteral("nonClientInsetLeft"), facts.inset.left);
    json.insert(QStringLiteral("nonClientInsetTop"), facts.inset.top);
    json.insert(QStringLiteral("nonClientInsetRight"), facts.inset.right);
    json.insert(QStringLiteral("nonClientInsetBottom"), facts.inset.bottom);
    json.insert(QStringLiteral("nativeTitlebar"), facts.inset.top > 0);
    json.insert(QStringLiteral("displayAffinityKnown"), facts.affinity_known);
    json.insert(QStringLiteral("displayAffinity"), static_cast<int>(facts.display_affinity));
    json.insert(QStringLiteral("captureExcluded"),
                facts.affinity_known && facts.display_affinity == 0x00000011 /* WDA_EXCLUDEFROMCAPTURE */);
    json.insert(QStringLiteral("x"), facts.x);
    json.insert(QStringLiteral("y"), facts.y);
    json.insert(QStringLiteral("width"), facts.width);
    json.insert(QStringLiteral("height"), facts.height);
    return json;
}

} // namespace

QuickLiveVerifySource::QuickLiveVerifySource(QuickApplication& application, QQuickWindow* root_window)
    : application_(application), root_window_(root_window) {
}

QJsonObject QuickLiveVerifySource::Identity() const {
    const models::AboutInfo& about = application_.aboutViewModel()->info();
    const QString executable = QCoreApplication::applicationFilePath();
    if (executable_sha256_.isEmpty())
        executable_sha256_ = models::ComputeFileSha256(executable);

    QJsonObject json;
    json.insert(QStringLiteral("productVersion"), about.version);
    json.insert(QStringLiteral("commit"), about.commit_full);
    json.insert(QStringLiteral("buildId"), about.build_id);
    json.insert(QStringLiteral("configuration"), about.configuration);
    json.insert(QStringLiteral("officialBuild"), about.official_build);
    json.insert(QStringLiteral("dirtySourceTree"), about.dirty_source_tree);
    json.insert(QStringLiteral("installMode"), about.install_mode_label);
    json.insert(QStringLiteral("channel"), about.channel);
    json.insert(QStringLiteral("executablePath"), executable);
    json.insert(QStringLiteral("executableSha256"), executable_sha256_);
    json.insert(QStringLiteral("pid"), static_cast<int>(QCoreApplication::applicationPid()));
    json.insert(QStringLiteral("qtVersion"), QString::fromLatin1(qVersion()));
    return json;
}

QJsonObject QuickLiveVerifySource::SystemSnapshot() const {
    QJsonObject json;
    QJsonArray screens;
    for (const QScreen* screen : QGuiApplication::screens())
        screens.append(ScreenJson(screen));
    json.insert(QStringLiteral("screens"), screens);
    json.insert(QStringLiteral("screenCount"), screens.size());
    json.insert(QStringLiteral("primaryScreen"),
                QGuiApplication::primaryScreen() != nullptr ? QGuiApplication::primaryScreen()->name() : QString{});
    json.insert(QStringLiteral("pid"), static_cast<int>(QCoreApplication::applicationPid()));
    return json;
}

QJsonObject QuickLiveVerifySource::AppSnapshot() const {
    QJsonObject json;
    if (auto* settings = application_.settingsAdapter()) {
        json.insert(QStringLiteral("appearanceId"), settings->appearanceId());
        json.insert(QStringLiteral("accentId"), settings->accentId());
    }
    // The shell's own navigation index, read from the object the QML declares by
    // name. Read-only: navigation itself stays a UI action, because a control
    // channel that could navigate would let a check "prove" a tab works without
    // ever touching the tab.
    if (root_window_ != nullptr) {
        if (QObject* shell = root_window_->findChild<QObject*>(QStringLiteral("quickAppShell")))
            json.insert(QStringLiteral("currentPage"), shell->property("currentPage").toInt());
    }
    if (auto* diagnostics = application_.diagnosticsAdapter())
        json.insert(QStringLiteral("expertMode"), diagnostics->expertMode());
    json.insert(QStringLiteral("windowVisible"), root_window_ != nullptr && root_window_->isVisible());
    return json;
}

QJsonObject QuickLiveVerifySource::WindowSnapshot() const {
    QJsonObject json;
    if (root_window_ == nullptr) {
        json.insert(QStringLiteral("present"), false);
        return json;
    }
    json.insert(QStringLiteral("present"), true);
    json.insert(QStringLiteral("visible"), root_window_->isVisible());
    json.insert(QStringLiteral("exposed"), root_window_->isExposed());
    json.insert(QStringLiteral("screen"), ScreenJson(root_window_->screen()));

    const QRect geometry = root_window_->geometry();
    json.insert(QStringLiteral("x"), geometry.x());
    json.insert(QStringLiteral("y"), geometry.y());
    json.insert(QStringLiteral("width"), geometry.width());
    json.insert(QStringLiteral("height"), geometry.height());
    json.insert(QStringLiteral("devicePixelRatio"), root_window_->devicePixelRatio());

    json.insert(QStringLiteral("native"),
                NativeFactsJson(diagnostics::QueryNativeWindowFacts(reinterpret_cast<void*>(root_window_->winId()))));
    return json;
}

QJsonObject QuickLiveVerifySource::PreviewSnapshot() const {
    QJsonObject json;
    auto* preview = application_.recordPreviewAdapter();
    if (preview == nullptr) {
        json.insert(QStringLiteral("available"), false);
        return json;
    }
    json.insert(QStringLiteral("available"), true);
    json.insert(QStringLiteral("active"), preview->active());
    json.insert(QStringLiteral("sourceAvailable"), preview->sourceAvailable());
    json.insert(QStringLiteral("frameReady"), preview->frameReady());
    json.insert(QStringLiteral("sourceName"), preview->sourceName());
    json.insert(QStringLiteral("statusText"), preview->statusText());
    json.insert(QStringLiteral("errorText"), preview->errorText());
    json.insert(QStringLiteral("presentationRate"), preview->presentationRate());
    json.insert(QStringLiteral("sourceDeliveryRate"), preview->sourceDeliveryRate());
    json.insert(QStringLiteral("consumedFrames"), static_cast<double>(preview->consumedFrames()));

    // The redraw gate's own counters. `owed` is the whole cross-monitor
    // question: a publish that no render pass has followed means the newest
    // frame is in the transport and the screen has not shown it. Reported as
    // structured state rather than left to preview-trace log parsing, because a
    // runner that greps a text log is one log-format change away from silently
    // passing everything.
    if (const auto& scheduler = preview->updateScheduler()) {
        QJsonObject gate;
        gate.insert(QStringLiteral("publishSignals"), static_cast<double>(scheduler->PublishSignals()));
        gate.insert(QStringLiteral("coalescedSignals"), static_cast<double>(scheduler->CoalescedSignals()));
        gate.insert(QStringLiteral("wakeups"), static_cast<double>(scheduler->Wakeups()));
        gate.insert(QStringLiteral("sceneUpdateRequests"), static_cast<double>(scheduler->SceneUpdateRequests()));
        gate.insert(QStringLiteral("renderPasses"), static_cast<double>(scheduler->RenderPasses()));
        gate.insert(QStringLiteral("owed"), scheduler->HasUnrenderedPublish());
        json.insert(QStringLiteral("updateGate"), gate);
    }
    return json;
}

QJsonObject QuickLiveVerifySource::RecordSnapshot() const {
    QJsonObject json;
    auto* record = application_.recordViewModelAdapter();
    if (record == nullptr) {
        json.insert(QStringLiteral("available"), false);
        return json;
    }
    json.insert(QStringLiteral("available"), true);
    json.insert(QStringLiteral("state"), record->state());
    json.insert(QStringLiteral("stateText"), record->stateText());
    json.insert(QStringLiteral("recording"), record->recording());
    json.insert(QStringLiteral("paused"), record->paused());
    json.insert(QStringLiteral("preparing"), record->preparing());
    json.insert(QStringLiteral("finalizing"), record->finalizing());
    json.insert(QStringLiteral("blocked"), record->blocked());
    json.insert(QStringLiteral("failed"), record->failed());
    json.insert(QStringLiteral("countdownActive"), record->countdownActive());
    json.insert(QStringLiteral("canStart"), record->canStart());
    json.insert(QStringLiteral("canStop"), record->canStop());
    json.insert(QStringLiteral("canPause"), record->canPause());
    json.insert(QStringLiteral("canResume"), record->canResume());
    json.insert(QStringLiteral("sourceName"), record->sourceName());
    json.insert(QStringLiteral("sourceKindText"), record->sourceKindText());
    json.insert(QStringLiteral("formatText"), record->formatText());
    json.insert(QStringLiteral("elapsedText"), record->elapsedText());
    json.insert(QStringLiteral("systemAudioEnabled"), record->systemAudioEnabled());
    json.insert(QStringLiteral("appAudioEnabled"), record->appAudioEnabled());
    json.insert(QStringLiteral("microphoneEnabled"), record->microphoneEnabled());
    if (auto* coordinator = application_.recordingCoordinator()) {
        json.insert(QStringLiteral("currentOutputPath"),
                    QString::fromStdWString(coordinator->CurrentOutputPath().wstring()));
    }
    return json;
}

QJsonObject QuickLiveVerifySource::RecordResult() const {
    const RecordViewModel& view_model = application_.recordViewModel();
    QJsonObject json;
    json.insert(QStringLiteral("hasResult"), view_model.HasResult());
    json.insert(QStringLiteral("succeeded"), view_model.last_succeeded);
    json.insert(QStringLiteral("statusText"), ToQString(view_model.result_status_text));
    json.insert(QStringLiteral("outputPath"), ToQString(view_model.result_output_path));
    json.insert(QStringLiteral("editMasterPath"), ToQString(view_model.result_mkv_master_path));
    json.insert(QStringLiteral("markerSidecarPath"), ToQString(view_model.result_marker_sidecar_path));
    json.insert(QStringLiteral("errorPhase"), ToQString(view_model.result_error_phase));
    json.insert(QStringLiteral("errorDetail"), ToQString(view_model.result_error_detail));
    json.insert(QStringLiteral("hresult"), ToQString(view_model.result_hresult_text));
    json.insert(QStringLiteral("outputFileBytes"), static_cast<double>(view_model.result_output_file_bytes));
    json.insert(QStringLiteral("durationSeconds"), view_model.result_duration_seconds);
    json.insert(QStringLiteral("outputWidth"), static_cast<int>(view_model.result_output_width));
    json.insert(QStringLiteral("outputHeight"), static_cast<int>(view_model.result_output_height));
    json.insert(QStringLiteral("frameRateNum"), static_cast<int>(view_model.result_frame_rate_num));
    json.insert(QStringLiteral("frameRateDen"), static_cast<int>(view_model.result_frame_rate_den));
    json.insert(QStringLiteral("cfr"), view_model.result_cfr);
    json.insert(QStringLiteral("container"), ui::containerLabel(view_model.result_container));
    json.insert(QStringLiteral("videoCodec"), ui::videoCodecLabel(view_model.result_video_codec));
    json.insert(QStringLiteral("audioCodec"), ui::audioCodecLabel(view_model.result_audio_codec));
    json.insert(QStringLiteral("markerCount"), static_cast<int>(view_model.result_markers.size()));
    return json;
}

QJsonObject QuickLiveVerifySource::OverlaySnapshot() const {
    // The five capture-excluded overlays are structurally unobservable to every
    // pixel instrument this project has: WDA_EXCLUDEFROMCAPTURE defeats
    // screenshots, screen recording and PrintWindow, and grabWindow() renders the
    // scene graph, which shows correct alpha even when the window composes
    // wrongly on the desktop. What CAN be proven is the native state that decides
    // composition -- which is exactly what this reports. The desktop appearance
    // stays a human gate; this makes the human gate the only remaining one.
    QJsonArray overlays;
    for (QWindow* window : QGuiApplication::topLevelWindows()) {
        if (window == nullptr || !window->objectName().startsWith(QLatin1String("quickOverlay")))
            continue;
        QJsonObject json;
        json.insert(QStringLiteral("objectName"), window->objectName());
        json.insert(QStringLiteral("visible"), window->isVisible());
        json.insert(QStringLiteral("exposed"), window->isExposed());
        if (window->screen() != nullptr)
            json.insert(QStringLiteral("screen"), window->screen()->name());
        // winId() on a hidden window would force the platform window into
        // existence; a snapshot must observe, never create.
        const bool has_handle = window->handle() != nullptr;
        json.insert(QStringLiteral("nativeWindowCreated"), has_handle);
        if (has_handle) {
            json.insert(QStringLiteral("native"),
                        NativeFactsJson(diagnostics::QueryNativeWindowFacts(reinterpret_cast<void*>(window->winId()))));
        }
        overlays.append(json);
    }

    QJsonObject json;
    json.insert(QStringLiteral("overlays"), overlays);
    json.insert(QStringLiteral("count"), overlays.size());
    return json;
}

QJsonObject QuickLiveVerifySource::EditorSnapshot() const {
    QJsonObject json;
    auto* session = application_.editSessionAdapter();
    auto* player = application_.editPlayerAdapter();
    if (session == nullptr) {
        json.insert(QStringLiteral("available"), false);
        return json;
    }
    json.insert(QStringLiteral("available"), true);
    json.insert(QStringLiteral("open"), session->open());
    json.insert(QStringLiteral("clipPath"), session->clipPath());
    json.insert(QStringLiteral("durationMs"), static_cast<double>(session->durationMs()));
    json.insert(QStringLiteral("positionMs"), static_cast<double>(session->positionMs()));
    json.insert(QStringLiteral("trimStartMs"), static_cast<double>(session->trimStartMs()));
    json.insert(QStringLiteral("trimEndMs"), static_cast<double>(session->trimEndMs()));
    json.insert(QStringLiteral("trimmed"), session->trimmed());
    json.insert(QStringLiteral("exportRunning"), session->exportRunning());
    json.insert(QStringLiteral("hasUnsavedEdits"), session->hasUnsavedEdits());
    if (player != nullptr) {
        json.insert(QStringLiteral("playing"), player->playing());
        json.insert(QStringLiteral("clipOpen"), player->clipOpen());
        json.insert(QStringLiteral("placeholderText"), player->placeholderText());
    }
    return json;
}

QJsonObject QuickLiveVerifySource::DiagnosticsSnapshot() const {
    QJsonObject json;
    auto* diagnostics = application_.diagnosticsAdapter();
    if (diagnostics == nullptr) {
        json.insert(QStringLiteral("available"), false);
        return json;
    }
    json.insert(QStringLiteral("available"), true);
    json.insert(QStringLiteral("dataReady"), diagnostics->dataReady());
    json.insert(QStringLiteral("checking"), diagnostics->checking());
    json.insert(QStringLiteral("verdictState"), diagnostics->verdictState());
    json.insert(QStringLiteral("verdictHeadline"), diagnostics->verdictHeadline());
    json.insert(QStringLiteral("blockerCount"), diagnostics->blockerCount());
    json.insert(QStringLiteral("noticeCount"), diagnostics->noticeCount());
    json.insert(QStringLiteral("elevated"), diagnostics->elevated());
    return json;
}

bool QuickLiveVerifySource::MoveWindowToScreen(const QString& screen_name, QString* error) {
    if (root_window_ == nullptr) {
        *error = QStringLiteral("There is no main window");
        return false;
    }
    QScreen* target = nullptr;
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen != nullptr && screen->name() == screen_name) {
            target = screen;
            break;
        }
    }
    if (target == nullptr) {
        *error = QStringLiteral("No screen named %1").arg(screen_name);
        return false;
    }
    if (root_window_->screen() == target)
        return true;

    // Move by geometry, not by setScreen(): the window must actually cross the
    // boundary the way a drag does, so the platform emits the same
    // screenChanged/expose sequence the defect under test rides on. setScreen()
    // alone can retarget without a geometry change on some paths.
    const QRect current = root_window_->geometry();
    const QRect available = target->availableGeometry();
    const int width = qMin(current.width(), available.width());
    const int height = qMin(current.height(), available.height());
    const int x = available.x() + ((available.width() - width) / 2);
    const int y = available.y() + ((available.height() - height) / 2);
    root_window_->setGeometry(QRect(x, y, width, height));
    return true;
}

bool QuickLiveVerifySource::SelectRecordTarget(const QString& kind, const QString& title_filter, QString* error) {
    const auto target_kind = kind == QStringLiteral("window") ? recorder_core::CaptureTarget::Kind::Window
                                                              : recorder_core::CaptureTarget::Kind::Monitor;
    if (!application_.selectCaptureTargetForAutomation(target_kind, title_filter)) {
        *error = QStringLiteral("No %1 target matched").arg(kind);
        return false;
    }
    return true;
}

namespace {

// Every intent below is refused when the application itself says the control is
// unavailable. That is deliberate: the acceptance value of `record.pause` is
// that it goes through the same gate the button does, so a check that pressed it
// in a state the UI forbids would be testing a path users cannot reach.
bool RefuseUnless(bool allowed, const char* what, QString* error) {
    if (allowed)
        return true;
    *error = QStringLiteral("%1 is not available in the current state").arg(QString::fromLatin1(what));
    return false;
}

} // namespace

bool QuickLiveVerifySource::RecordStart(QString* error) {
    auto* record = application_.recordViewModelAdapter();
    if (record == nullptr || !RefuseUnless(record->canStart(), "record.start", error))
        return false;
    record->requestStart();
    return true;
}

bool QuickLiveVerifySource::RecordPause(QString* error) {
    auto* record = application_.recordViewModelAdapter();
    if (record == nullptr || !RefuseUnless(record->canPause(), "record.pause", error))
        return false;
    record->requestPause();
    return true;
}

bool QuickLiveVerifySource::RecordResume(QString* error) {
    auto* record = application_.recordViewModelAdapter();
    if (record == nullptr || !RefuseUnless(record->canResume(), "record.resume", error))
        return false;
    record->requestResume();
    return true;
}

bool QuickLiveVerifySource::RecordStop(QString* error) {
    auto* record = application_.recordViewModelAdapter();
    if (record == nullptr || !RefuseUnless(record->canStop(), "record.stop", error))
        return false;
    record->requestStop();
    return true;
}

bool QuickLiveVerifySource::RecordSplit(QString* error) {
    auto* record = application_.recordViewModelAdapter();
    if (record == nullptr || !RefuseUnless(record->splitEnabled(), "record.split", error))
        return false;
    record->requestSplit();
    return true;
}

bool QuickLiveVerifySource::RecordCaptureFrame(QString* error) {
    auto* record = application_.recordViewModelAdapter();
    if (record == nullptr || !RefuseUnless(record->captureFrameEnabled(), "record.captureFrame", error))
        return false;
    record->requestCaptureFrame();
    return true;
}

} // namespace exosnap::quick
