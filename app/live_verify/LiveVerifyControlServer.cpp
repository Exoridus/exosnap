#include "LiveVerifyControlServer.h"

#include "LiveVerifyOptions.h"

namespace exosnap::live_verify {

LiveVerifyControlServer::LiveVerifyControlServer(LiveVerifySource* source, QString run_id, QObject* parent)
    : QObject(parent), dispatcher_(source, run_id),
      server_(&dispatcher_, QString::fromLatin1(kControlRole), std::move(run_id), QStringLiteral("live-verify"), this) {
}

LiveVerifyControlServer::~LiveVerifyControlServer() {
    // Explicit, and before the dispatcher goes: ControlServer::Stop() joins the
    // pipe thread, and that thread hands requests to the dispatcher. Letting
    // member destruction order decide would be relying on a declaration order to
    // keep a live pointer valid.
    server_.Stop();
}

bool LiveVerifyControlServer::Start(QString* error) {
    return server_.Start(error);
}

void LiveVerifyControlServer::Stop() {
    server_.Stop();
}

void LiveVerifyControlServer::EmitEvent(const QString& name, QJsonObject data) {
    server_.EmitEvent(name, std::move(data));
}

} // namespace exosnap::live_verify
