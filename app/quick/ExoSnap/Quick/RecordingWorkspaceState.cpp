#include "RecordingWorkspaceState.h"

#include <algorithm>
#include <cmath>

namespace exosnap::quick {

RecordingWorkspaceState::RecordingWorkspaceState(QObject* parent) : QObject(parent) {
}

QString RecordingWorkspaceState::stateText(State state) {
    switch (state) {
    case State::Closed:
        return QStringLiteral("Closed");
    case State::Overlay:
        return QStringLiteral("Overlay");
    case State::Pinned:
        return QStringLiteral("Pinned");
    }
    return {};
}
QString RecordingWorkspaceState::sourceState() const {
    return stateText(sources_.state);
}
QString RecordingWorkspaceState::profileState() const {
    return stateText(profile_.state);
}
double RecordingWorkspaceState::sourceWidth() const {
    return std::min(320.0, width_);
}
double RecordingWorkspaceState::profileWidth() const {
    return std::min(360.0, width_);
}
double RecordingWorkspaceState::workspaceWidth() const {
    return width_;
}
double RecordingWorkspaceState::workspaceHeight() const {
    return height_;
}
const QString& RecordingWorkspaceState::notice() const {
    return notice_;
}
QString RecordingWorkspaceState::activePanel() const {
    if (sources_.state == State::Closed && profile_.state == State::Closed)
        return {};
    if (sources_.state != State::Closed && (profile_.state == State::Closed || sources_.order > profile_.order))
        return QStringLiteral("sources");
    return QStringLiteral("profile");
}
bool RecordingWorkspaceState::safePinned(bool source, bool profile) const {
    return height_ >= 280 && width_ - (source ? sourceWidth() : 0) - (profile ? profileWidth() : 0) >= 480;
}
bool RecordingWorkspaceState::canPinSources() const {
    return safePinned(true, profile_.state == State::Pinned);
}
bool RecordingWorkspaceState::canPinProfile() const {
    return safePinned(sources_.state == State::Pinned, true);
}

void RecordingWorkspaceState::setWorkspaceWidth(double value) {
    value = std::isfinite(value) ? std::max(0.0, value) : 0;
    if (width_ == value)
        return;
    width_ = value;
    reconcileSize();
    emit changed();
}
void RecordingWorkspaceState::setWorkspaceHeight(double value) {
    value = std::isfinite(value) ? std::max(0.0, value) : 0;
    if (height_ == value)
        return;
    height_ = value;
    reconcileSize();
    emit changed();
}
void RecordingWorkspaceState::close(Panel& panel, bool source) {
    if (panel.state == State::Closed)
        return;
    panel.state = State::Closed;
    emit panelClosed(source ? QStringLiteral("sources") : QStringLiteral("profile"));
}
void RecordingWorkspaceState::open(Panel& panel, bool source) {
    notice_.clear();
    panel.order = ++next_order_;
    if (panel.state == State::Closed)
        panel.state =
            panel.pinned_preference && (source ? canPinSources() : canPinProfile()) ? State::Pinned : State::Overlay;
    if (width_ < 1200 && panel.state == State::Overlay) {
        auto& other = source ? profile_ : sources_;
        if (other.state == State::Overlay)
            close(other, !source);
    }
    emit changed();
}
void RecordingWorkspaceState::pin(Panel& panel, bool source) {
    if (!(source ? canPinSources() : canPinProfile())) {
        notice_ = tr("Pinning requires at least 480 by 280 pixels for the preview.");
        emit changed();
        return;
    }
    panel.state = State::Pinned;
    panel.pinned_preference = true;
    panel.order = ++next_order_;
    notice_.clear();
    emit changed();
}
void RecordingWorkspaceState::togglePin(Panel& panel, bool source) {
    if (panel.state != State::Pinned) {
        pin(panel, source);
        return;
    }
    panel.state = State::Overlay;
    panel.pinned_preference = false;
    open(panel, source);
}
void RecordingWorkspaceState::reconcileSize() {
    if (width_ < 1200 && sources_.state == State::Overlay && profile_.state == State::Overlay) {
        const bool source = sources_.order < profile_.order;
        close(source ? sources_ : profile_, source);
    }
    while ((sources_.state == State::Pinned || profile_.state == State::Pinned) &&
           !safePinned(sources_.state == State::Pinned, profile_.state == State::Pinned)) {
        const bool source =
            sources_.state != State::Closed && (profile_.state == State::Closed || sources_.order > profile_.order);
        close(source ? sources_ : profile_, source);
        notice_ = tr("A panel was closed to keep the preview usable. Reopen it when you have enough space.");
    }
}
void RecordingWorkspaceState::openSources() {
    open(sources_, true);
}
void RecordingWorkspaceState::openProfile() {
    open(profile_, false);
}
void RecordingWorkspaceState::closeSources() {
    close(sources_, true);
    emit changed();
}
void RecordingWorkspaceState::closeProfile() {
    close(profile_, false);
    emit changed();
}
void RecordingWorkspaceState::toggleSources() {
    if (sources_.state == State::Closed)
        openSources();
    else
        closeSources();
}
void RecordingWorkspaceState::toggleProfile() {
    if (profile_.state == State::Closed)
        openProfile();
    else
        closeProfile();
}
void RecordingWorkspaceState::pinSources() {
    pin(sources_, true);
}
void RecordingWorkspaceState::pinProfile() {
    pin(profile_, false);
}
void RecordingWorkspaceState::toggleSourcePin() {
    togglePin(sources_, true);
}
void RecordingWorkspaceState::toggleProfilePin() {
    togglePin(profile_, false);
}
bool RecordingWorkspaceState::closeTopPanel() {
    const auto active = activePanel();
    if (active.isEmpty())
        return false;
    if (active == QStringLiteral("sources"))
        closeSources();
    else
        closeProfile();
    return true;
}

} // namespace exosnap::quick
