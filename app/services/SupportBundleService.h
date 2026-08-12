#pragma once

#include "../diagnostics/SupportBundle.h"

#include <capability/adapter_enum.h>
#include <capability/capability_set.h>

#include <QObject>
#include <QString>
#include <QThreadPool>

#include <string>
#include <vector>

// Support-bundle assembly + write, extracted from MainWindow::createSupportBundle.
//
// The collector itself (diagnostics::CollectBundleEntries / WriteBundleZip) is
// already UI-agnostic. What lived in MainWindow was the mapping from the app's
// capability + settings state onto BundleInputs, plus the fact that the whole
// thing — up to six log files, two global regex scrubs, adapter enumeration and a
// miniz deflate — ran synchronously on the GUI thread.
//
// This service keeps the mapping pure and static, and moves the collect+write off
// the GUI thread. Choosing the destination path and revealing the finished file
// stay with the frontend: both are UI decisions.
namespace exosnap {

// Everything the bundle needs that is NOT already on the CapabilitySet.
struct SupportBundleContext {
    QString log_dir;
    QString launch_session_id;
    QString created_at; // ISO 8601
    QString settings_summary;
    bool verify_update_reinstall = false;
    int max_reports = 10;
};

// Pure: maps app state onto the collector's plain-data inputs. Adapter enumeration
// is passed in rather than performed here so the mapping stays testable.
[[nodiscard]] diagnostics::BundleInputs BuildSupportBundleInputs(const SupportBundleContext& context,
                                                                 const capability::CapabilitySet& caps,
                                                                 const std::vector<capability::AdapterInfo>& adapters);

class SupportBundleService : public QObject {
    Q_OBJECT

  public:
    explicit SupportBundleService(QObject* parent = nullptr);

    // Collects and writes on a worker thread; `finished` arrives on the GUI thread.
    // Adapter enumeration runs on that worker too — it is a DXGI call, and the point
    // of this service is that none of the bundle work touches the GUI thread.
    // A second request while one is in flight is rejected with a busy result rather
    // than racing two writers onto the same path.
    void createAsync(const QString& zip_path, SupportBundleContext context, capability::CapabilitySet caps);

    [[nodiscard]] bool busy() const noexcept;

  signals:
    void busyChanged();
    // ok == false carries the failure reason; ok == true carries the written path.
    void finished(bool ok, const QString& message);

  private:
    bool busy_ = false;

    // Declared last so it is destroyed FIRST: its destructor waits for the
    // in-flight bundle write. Without that wait the worker outlives the process
    // — it reads log files, enumerates DXGI adapters and deflates a ZIP, none of
    // which survives Qt's static teardown. The QPointer on the result callback
    // guards only the callback, never the worker body.
    QThreadPool write_pool_;
};

} // namespace exosnap
