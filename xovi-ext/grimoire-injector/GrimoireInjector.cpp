#include "GrimoireInjector.hpp"
#include "rm_SceneLineItem.hpp"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlApplicationEngine>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QQmlEngine>
#include <QGuiApplication>
#include <QWindow>
#include <QTabletEvent>
#include <QPointingDevice>
#include <QInputDevice>
#include <QPointF>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <functional>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <time.h>
#include <cstdarg>

static void grimoire_log(const char *fmt, ...) {
    FILE *fp = fopen("/tmp/grimoire_ext.log", "a");
    if (fp) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(fp, fmt, ap);
        va_end(ap);
        fprintf(fp, "\n");
        fclose(fp);
    }
}

static GrimoireInjector *g_instance = nullptr;
static PenIdleWatcher *g_idleWatcher = nullptr;

/* Provided by entry.c hooks */
extern "C" {
    void *grimoire_getQmlEngine(void);
    void *grimoire_getSceneController(void);
    int grimoire_checkReload(void);
    void *grimoire_getFramebuffer(int *w, int *h, int *bpl, int *fmt);
}

static void *watchThreadFunc(void *) {
    grimoire_log("Watch thread started");
    sleep(5);  // Wait for xochitl to fully initialize

    /* Start the pen idle watcher now that QGuiApplication should exist.
     * Must invoke on main thread since installEventFilter is not thread-safe.
     * Retry a few times in case startup is slow. */
    bool idleWatcherStarted = false;
    for (int attempt = 0; attempt < 10 && !idleWatcherStarted; attempt++) {
        if (g_idleWatcher && qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
            QMetaObject::invokeMethod(g_idleWatcher, "activate", Qt::QueuedConnection);
            idleWatcherStarted = true;
            grimoire_log("PenIdleWatcher activate queued (attempt %d)", attempt + 1);
        } else {
            grimoire_log("Waiting for QGuiApplication (attempt %d, g_idleWatcher=%p, app=%p)...",
                         attempt + 1, (void*)g_idleWatcher, (void*)QGuiApplication::instance());
            sleep(2);
        }
    }
    if (!idleWatcherStarted) {
        grimoire_log("WARNING: PenIdleWatcher failed to start!");
    }

    const char *path = "/tmp/grimoire_strokes.json";
    const char *armedPath = "/tmp/grimoire_armed";
    long long lastMod = 0;
    int lastArmed = -1;

    while (true) {
        /* Check armed state file */
        FILE *afp = fopen(armedPath, "r");
        if (afp) {
            char abuf[16] = {0};
            if (fgets(abuf, sizeof(abuf), afp)) {
                char *ap = abuf;
                while (*ap == '"' || *ap == ' ' || *ap == '\t') ap++;
                int val = atoi(ap);
                if (val != lastArmed && g_instance) {
                    lastArmed = val;
                    int capturedVal = val;
                    QMetaObject::invokeMethod(g_instance, [capturedVal]() {
                        g_instance->setArmed(capturedVal != 0);
                    }, Qt::QueuedConnection);
                    grimoire_log("Armed state changed: %d", val);
                }
            }
            fclose(afp);
        }

        /* Check for hot-reload signal */
        if (grimoire_checkReload()) {
            fprintf(stderr, "[grimoire] Hot-reload signal received! Re-scanning...\n");
            lastMod = 0;  // Force re-read of stroke file
        }

        /* Check for screenshot trigger */
        if (access("/tmp/grimoire_screenshot", F_OK) == 0) {
            unlink("/tmp/grimoire_screenshot");
            int sw, sh, sbpl, sfmt;
            void *fb = grimoire_getFramebuffer(&sw, &sh, &sbpl, &sfmt);
            if (fb && sw > 0 && sh > 0) {
                const char *outpath = "/tmp/grimoire_fb.raw";
                FILE *fp = fopen(outpath, "wb");
                if (fp) {
                    size_t size = (size_t)sbpl * sh;
                    fwrite(fb, 1, size, fp);
                    fclose(fp);
                    fprintf(stderr, "[grimoire] Screenshot saved: %dx%d bpl=%d (%zu bytes)\n",
                            sw, sh, sbpl, size);
                }
            } else {
                fprintf(stderr, "[grimoire] Screenshot requested but no framebuffer available\n");
            }
        }

        struct stat st;
        if (stat(path, &st) == 0 && st.st_mtime != lastMod) {
            lastMod = st.st_mtime;
            fprintf(stderr, "[grimoire] File changed (mtime=%lld), triggering load\n",
                    (long long)lastMod);
            if (g_instance) {
                // Call back into main thread
                QMetaObject::invokeMethod(g_instance, "loadAndInject", Qt::QueuedConnection);
            }
        }
        sleep(1);
    }
    return nullptr;
}

GrimoireInjector::GrimoireInjector(QObject *parent)
    : QObject(parent)
{
    g_instance = this;

    /* Start pen idle watcher — detects when user stops writing.
     * QGuiApplication may not exist yet during _xovi_construct, so
     * we defer event filter installation to the watch thread which
     * retries until the app is ready. */
    g_idleWatcher = new PenIdleWatcher(this);

    pthread_t tid;
    pthread_create(&tid, nullptr, watchThreadFunc, nullptr);
    pthread_detach(tid);
    fprintf(stderr, "[grimoire] Constructor done, watch thread spawned\n");
}

void GrimoireInjector::setArmed(bool armed) {
    if (m_armed == armed) return;
    m_armed = armed;

    /* Write state file so grimoired can see it */
    FILE *fp = fopen("/tmp/grimoire_armed", "w");
    if (fp) {
        fprintf(fp, "%d\n", armed ? 1 : 0);
        fclose(fp);
    }
    grimoire_log("setArmed(%d)", armed);
    emit armedChanged(armed);
}

void GrimoireInjector::loadAndInject() {
    QFileInfo fi(m_watchPath);
    if (!fi.exists()) return;

    qint64 modTime = fi.lastModified().toMSecsSinceEpoch();
    if (modTime == m_lastModTime) return;

    m_lastModTime = modTime;
    fprintf(stderr, "[grimoire] File changed, injecting via synthetic tablet events...\n");

    int count = loadStrokes(m_watchPath);
    if (count == 0) return;

    /* Find the focused window to post events to */
    QWindow *targetWin = nullptr;
    QObject *inputTarget = nullptr;
    auto *guiApp = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (guiApp) {
        targetWin = guiApp->focusWindow();
        inputTarget = guiApp->focusObject();
        if (inputTarget) {
            fprintf(stderr, "[grimoire] Focus object: %s @ %p\n",
                    inputTarget->metaObject()->className(), (void*)inputTarget);
        }
        
        if (!targetWin) {
            for (QWindow *win : guiApp->topLevelWindows()) {
                if (win->isVisible()) {
                    targetWin = win;
                    break;
                }
            }
        }
        
        /* Check QQuickWindow properties for contentItem/activeFocusItem */
        if (targetWin) {
            const QMetaObject *mo = targetWin->metaObject();
            for (int i = mo->propertyOffset(); i < mo->propertyCount(); i++) {
                QMetaProperty prop = mo->property(i);
                if (strcmp(prop.name(), "contentItem") == 0 || 
                    strcmp(prop.name(), "activeFocusItem") == 0) {
                    QVariant val = prop.read(targetWin);
                    QObject *obj = val.value<QObject*>();
                    if (obj) {
                        fprintf(stderr, "[grimoire]   %s: %s @ %p\n",
                                prop.name(), obj->metaObject()->className(), (void*)obj);
                        if (!inputTarget) inputTarget = obj;
                    }
                }
            }
        }
    }

    if (!inputTarget && targetWin) {
        inputTarget = targetWin;
    }
    
    if (!inputTarget) {
        fprintf(stderr, "[grimoire] No input target found\n");
        return;
    }

    fprintf(stderr, "[grimoire] Posting %d strokes as tablet events to %s @ %p\n",
            count, inputTarget->metaObject()->className(), (void*)inputTarget);

    /* Find the pen/stylus pointing device */
    const QPointingDevice *penDevice = nullptr;
    QList<const QInputDevice*> allDevices = QInputDevice::devices();
    for (const QInputDevice *dev : allDevices) {
        const auto *pDev = dynamic_cast<const QPointingDevice*>(dev);
        if (!pDev) continue;
        fprintf(stderr, "[grimoire]   device: %s type=%d pointer=%d\n",
                pDev->name().toUtf8().constData(),
                (int)pDev->type(), (int)pDev->pointerType());
        if (pDev->pointerType() == QPointingDevice::PointerType::Pen) {
            penDevice = pDev;
            break;
        }
    }
    if (!penDevice) {
        /* Fallback: use first pointing device */
        for (const QInputDevice *dev : allDevices) {
            const auto *pDev = dynamic_cast<const QPointingDevice*>(dev);
            if (pDev) {
                penDevice = pDev;
                fprintf(stderr, "[grimoire] No pen device, using fallback: %s\n",
                        pDev->name().toUtf8().constData());
                break;
            }
        }
    }
    if (!penDevice) {
        fprintf(stderr, "[grimoire] No pointing devices available\n");
        return;
    }

    /* Post each stroke as a sequence of tablet events: press → move... → release */
    /* Transform from .rm v6 coords (origin top-center) to screen pixels (origin top-left) */
    /* Screen: 1404x1872. RM v6: x[-702,701] y[0,1871] → screen x = rm_x + 702, y = rm_y */
    const float xOff = 702.0f;
    const float yOff = 0.0f;

    for (int s = 0; s < m_items.size(); s++) {
        auto *lineItem = reinterpret_cast<SceneLineItem*>(m_items[s].get());
        const Line &line = lineItem->line;

        if (line.points.isEmpty()) continue;

        /* Tablet press at first point */
        const auto &first = line.points.first();
        QPointF pos(first.x + xOff, first.y + yOff);

        QTabletEvent press(
            QEvent::TabletPress, penDevice,
            pos, pos,
            first.pressure / 255.0,
            0, 0, 0, 0, 0,
            Qt::NoModifier,
            Qt::LeftButton,
            Qt::LeftButton
        );
        QCoreApplication::sendEvent(inputTarget, &press);

        /* Tablet move for intermediate points */
        for (int i = 1; i < line.points.size() - 1; i++) {
            const auto &pt = line.points[i];
            QPointF ppos(pt.x + xOff, pt.y + yOff);

            QTabletEvent move(
                QEvent::TabletMove, penDevice,
                ppos, ppos,
                pt.pressure / 255.0,
                0, 0, 0, 0, 0,
                Qt::NoModifier,
                Qt::NoButton,
                Qt::LeftButton
            );
            QCoreApplication::sendEvent(inputTarget, &move);
        }

        /* Tablet release at last point */
        const auto &last = line.points.last();
        QPointF lpos(last.x + xOff, last.y + yOff);

        QTabletEvent release(
            QEvent::TabletRelease, penDevice,
            lpos, lpos,
            last.pressure / 255.0,
            0, 0, 0, 0, 0,
            Qt::NoModifier,
            Qt::LeftButton,
            Qt::NoButton
        );
        QCoreApplication::sendEvent(inputTarget, &release);
    }

    fprintf(stderr, "[grimoire] Posted all tablet events\n");
}

int GrimoireInjector::loadStrokes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fprintf(stderr, "[grimoire] Cannot open %s\n", path.toUtf8().constData());
        return 0;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    QJsonArray jsonArray = doc.array();

    fprintf(stderr, "[grimoire] Loading %d strokes from %s\n",
           jsonArray.size(), path.toUtf8().constData());

    m_items.clear();

    for (int i = 0; i < jsonArray.size(); i++) {
        QJsonObject obj = jsonArray[i].toObject();
        QJsonArray pointsArray = obj["points"].toArray();
        QList<LinePoint> linePoints;

        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;

        for (int j = 0; j < pointsArray.size(); j++) {
            QJsonArray ptArr = pointsArray[j].toArray();
            LinePoint pt;
            pt.x = static_cast<float>(ptArr[0].toDouble());
            pt.y = static_cast<float>(ptArr[1].toDouble());
            pt.speed = static_cast<unsigned short>(ptArr[2].toInt());
            pt.width = static_cast<unsigned short>(ptArr[3].toInt());
            pt.direction = static_cast<unsigned char>(ptArr[4].toInt());
            pt.pressure = static_cast<unsigned char>(ptArr[5].toInt());
            linePoints.append(pt);

            minX = std::min(minX, pt.x);
            minY = std::min(minY, pt.y);
            maxX = std::max(maxX, pt.x);
            maxY = std::max(maxY, pt.y);
        }

        QRectF bounds(minX, minY, maxX - minX, maxY - minY);

        Line line;
        line.points = linePoints;
        line.bounds = bounds;
        line.rgba = static_cast<unsigned int>(obj["rgba"].toDouble(0xFF000000));
        line.color = obj["color"].toInt(0);
        line.tool = obj["tool"].toInt(0x0F); // BALLPOINT_2
        line.maskScale = obj["maskScale"].toDouble(1.0);
        line.thickness = static_cast<float>(obj["thickness"].toDouble(2.0));

        auto item = std::make_shared<SceneLineItem>(
            SceneLineItem::fromLine(std::move(line)));
        m_items.push_back(item);
    }

    fprintf(stderr, "[grimoire] Loaded %d scene items\n", m_items.size());
    return m_items.size();
}

bool GrimoireInjector::setupVtable() {
    if (m_items.empty()) {
        fprintf(stderr, "[grimoire] setupVtable: no items loaded\n");
        return false;
    }
    auto* item = reinterpret_cast<SceneLineItem*>(m_items.first().get());
    SceneLineItem::setupVtable(item->vtable);
    m_vtableReady = true;
    fprintf(stderr, "[grimoire] Vtable ready\n");
    return true;
}

/* ─── PenIdleWatcher ─────────────────────────────────────────────── */

static long long nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

PenIdleWatcher::PenIdleWatcher(QObject *parent)
    : QObject(parent), m_debounceThread(0), m_running(false), m_lastLiftMs(0)
{
}

void PenIdleWatcher::activate() {
    if (m_running.exchange(true)) return;

    auto *app = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (!app) {
        grimoire_log("PenIdleWatcher::activate: no QGuiApplication!");
        m_running = false;
        return;
    }

    app->installEventFilter(this);
    grimoire_log("PenIdleWatcher: event filter installed");

    pthread_create(&m_debounceThread, nullptr, debounceThreadFunc, this);
    pthread_detach(m_debounceThread);
    grimoire_log("PenIdleWatcher: debounce thread started (debounce=%dms)", DEBOUNCE_MS);
}

bool PenIdleWatcher::eventFilter(QObject * /*obj*/, QEvent *event) {
    // reMarkable delivers pen input as touch + mouse events
    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::TouchBegin ||
        event->type() == QEvent::TouchUpdate) {
        m_penDown = true;
        m_lastActivityMs = nowMs();  // tap-refresh: slide the deadline
    } else if (event->type() == QEvent::MouseButtonRelease ||
               event->type() == QEvent::TouchEnd) {
        m_penDown = false;
        m_lastLiftMs = nowMs();
        m_lastActivityMs = nowMs();  // lift also counts as activity
        grimoire_log("Pen up (lift)");
    }
    return false;
}

void *PenIdleWatcher::debounceThreadFunc(void *arg) {
    auto *self = static_cast<PenIdleWatcher*>(arg);
    fprintf(stderr, "[grimoire] PenIdleWatcher debounce thread running\n");

    while (self->m_running) {
        long long activity = self->m_lastActivityMs.load();
        // Safety: if m_penDown has been stuck true for 30s+ with no new
        // activity, force it false. A missed release event would otherwise
        // permanently stall the debounce.
        if (self->m_penDown.load() && activity > 0) {
            long long stuck = nowMs() - activity;
            if (stuck > 30000) {
                self->m_penDown = false;
                grimoire_log("Pen down stuck for %lldms, forcing lift", stuck);
            }
        }
        // Only fire when the pen is currently up AND the page has been
        // completely quiet (no down or up events) for the full window.
        // Any new stroke updates m_lastActivityMs and resets the count.
        if (activity > 0 && !self->m_penDown.load()) {
            long long elapsed = nowMs() - activity;
            if (elapsed >= DEBOUNCE_MS) {
                self->writeIdleSignal();
                self->m_lastActivityMs = 0;  // reset so we don't re-fire
            }
        }
        usleep(250000);  // check every 250ms
    }
    return nullptr;
}

void PenIdleWatcher::writeIdleSignal() {
    FILE *fp = fopen(IDLE_PATH, "w");
    if (fp) {
        long long ts = nowMs();
        fprintf(fp, "%lld\n", ts);
        fclose(fp);
        grimoire_log("Idle signal written (ts=%lld)", ts);
    } else {
        grimoire_log("ERROR: failed to write %s", IDLE_PATH);
    }
}
