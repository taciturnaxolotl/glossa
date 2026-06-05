#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <memory>
#include <atomic>
#include "rm_SceneItem.hpp"
#include "rm_Line.hpp"

class GrimoireInjector : public QObject {
    Q_OBJECT
public:
    explicit GrimoireInjector(QObject *parent = nullptr);

    Q_INVOKABLE int loadStrokes(const QString& path);
    Q_INVOKABLE bool setupVtable();
    Q_INVOKABLE int itemCount() const { return m_items.size(); }
    Q_INVOKABLE bool isReady() const { return m_vtableReady; }

public slots:
    void loadAndInject();

private:
    QList<std::shared_ptr<SceneItem>> m_items;
    bool m_vtableReady = false;
    QString m_watchPath = "/tmp/grimoire_strokes.json";
    qint64 m_lastModTime = 0;
};

/**
 * Event filter that detects pen-lift (TabletRelease) and writes an idle
 * signal file after a debounce period. Installed on QGuiApplication.
 */
class PenIdleWatcher : public QObject {
    Q_OBJECT
public:
    explicit PenIdleWatcher(QObject *parent = nullptr);

public slots:
    /** Must be called on the main thread. Installs event filter on QGuiApplication. */
    void activate();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    std::atomic<bool> m_penDown{false};
    pthread_t m_debounceThread;
    std::atomic<bool> m_running{false};
    std::atomic<long long> m_lastLiftMs{0};
    // Updated on EVERY pen event (down or up). The idle countdown is
    // measured from this, so any new stroke slides the deadline forward
    // — a tap-refresh that survives normal mid-drawing pauses.
    std::atomic<long long> m_lastActivityMs{0};

    // 2.5s of total pen silence before we consider the page "settled".
    // Combined with the tap-refresh (any stroke resets the window),
    // normal mid-drawing pauses won't trigger it.
    static constexpr int DEBOUNCE_MS = 2500;
    static constexpr const char *IDLE_PATH = "/tmp/grimoire_idle";

    static void *debounceThreadFunc(void *arg);
    void writeIdleSignal();
};
