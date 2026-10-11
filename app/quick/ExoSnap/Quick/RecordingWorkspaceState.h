#pragma once

#include <QObject>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

namespace exosnap::quick {

class RecordingWorkspaceState : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString sourceState READ sourceState NOTIFY changed FINAL)
    Q_PROPERTY(QString profileState READ profileState NOTIFY changed FINAL)
    Q_PROPERTY(double sourceWidth READ sourceWidth NOTIFY changed FINAL)
    Q_PROPERTY(double profileWidth READ profileWidth NOTIFY changed FINAL)
    Q_PROPERTY(double workspaceWidth READ workspaceWidth WRITE setWorkspaceWidth NOTIFY changed FINAL)
    Q_PROPERTY(double workspaceHeight READ workspaceHeight WRITE setWorkspaceHeight NOTIFY changed FINAL)
    Q_PROPERTY(bool canPinSources READ canPinSources NOTIFY changed FINAL)
    Q_PROPERTY(bool canPinProfile READ canPinProfile NOTIFY changed FINAL)
    Q_PROPERTY(QString notice READ notice NOTIFY changed FINAL)
    Q_PROPERTY(QString activePanel READ activePanel NOTIFY changed FINAL)

  public:
    explicit RecordingWorkspaceState(QObject* parent = nullptr);
    QString sourceState() const;
    QString profileState() const;
    double sourceWidth() const;
    double profileWidth() const;
    double workspaceWidth() const;
    double workspaceHeight() const;
    void setWorkspaceWidth(double value);
    void setWorkspaceHeight(double value);
    bool canPinSources() const;
    bool canPinProfile() const;
    const QString& notice() const;
    QString activePanel() const;
    Q_INVOKABLE void openSources();
    Q_INVOKABLE void openProfile();
    Q_INVOKABLE void closeSources();
    Q_INVOKABLE void closeProfile();
    Q_INVOKABLE void toggleSources();
    Q_INVOKABLE void toggleProfile();
    Q_INVOKABLE void pinSources();
    Q_INVOKABLE void pinProfile();
    Q_INVOKABLE void toggleSourcePin();
    Q_INVOKABLE void toggleProfilePin();
    Q_INVOKABLE bool closeTopPanel();

  signals:
    void changed();
    void panelClosed(QString panel);

  private:
    enum class State { Closed, Overlay, Pinned };
    struct Panel {
        State state = State::Closed;
        bool pinned_preference = false;
        unsigned long long order = 0;
    };
    static QString stateText(State state);
    void open(Panel& panel, bool source);
    void close(Panel& panel, bool source);
    void pin(Panel& panel, bool source);
    void togglePin(Panel& panel, bool source);
    void reconcileSize();
    bool safePinned(bool source, bool profile) const;
    Panel sources_;
    Panel profile_;
    unsigned long long next_order_ = 0;
    double width_ = 0;
    double height_ = 0;
    QString notice_;
};

} // namespace exosnap::quick
