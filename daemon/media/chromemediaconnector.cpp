#include "chromemediaconnector.h"
#include "BluetoothMonitor.h"
#include "logger.h"
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>

namespace {
constexpr int pollIntervalMs = 1000;
constexpr int queryTimeoutMs = 1500;
// A D-Bus timeout does not cancel BlueZ's underlying connection attempt.
constexpr int connectTimeoutMs = 60000;
const QString playerPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString playerInterface = QStringLiteral("org.mpris.MediaPlayer2.Player");

bool isChromePlayer(const QString &service)
{
    return service == "org.mpris.MediaPlayer2.chromium"
        || service.startsWith("org.mpris.MediaPlayer2.chromium.")
        || service == "org.mpris.MediaPlayer2.google-chrome"
        || service.startsWith("org.mpris.MediaPlayer2.google-chrome.");
}
}

ChromeMediaConnector::ChromeMediaConnector(QSettings *settings, QObject *parent)
    : QObject(parent), m_settings(settings)
{
    qDBusRegisterMetaType<ManagedObjectList>();
    m_address = settings->value("chromeConnect/address").toString();
    m_enabled = settings->value("chromeConnect/enabled", true).toBool();
    m_paused = settings->value("chromeConnect/paused", false).toBool();
    m_clock.start();
    m_timer.setInterval(pollIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &ChromeMediaConnector::poll);
    if (!QDBusConnection::systemBus().connect("org.bluez", QString(), "org.bluez.Device1",
                                             "Disconnected", this, SLOT(onDisconnected(QDBusMessage)))) {
        LOG_ERROR("Chrome media connect: cannot subscribe to BlueZ disconnect reasons");
    }
    restartMonitoring();
}

void ChromeMediaConnector::saveSettings()
{
    m_settings->setValue("chromeConnect/enabled", m_enabled);
    m_settings->setValue("chromeConnect/address", m_address);
    m_settings->setValue("chromeConnect/paused", m_paused);
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        LOG_ERROR("Chrome media connect: cannot save preferences to " << m_settings->fileName());
    }
}

void ChromeMediaConnector::restartMonitoring()
{
    ++m_generation;
    m_policy = ChromeConnectPolicy();
    if (active()) m_timer.start();
    else m_timer.stop();
    LOG_INFO("Chrome media connect: " << (!m_enabled ? "disabled" : m_paused ? "paused after local disconnect"
                                         : m_address.isEmpty() ? "waiting for first AirPods connection" : "monitoring"));
}

void ChromeMediaConnector::setEnabled(bool enabled)
{
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    saveSettings();
    restartMonitoring();
}

void ChromeMediaConnector::deviceConnected(const QString &address)
{
    m_connected = true;
    if (m_address != address || m_paused) {
        m_address = address;
        m_paused = false;
        saveSettings();
        restartMonitoring();
    }
    m_policy.satisfy();
}

void ChromeMediaConnector::onDisconnected(const QDBusMessage &message)
{
    const QString suffix = "/dev_" + QString(m_address).replace(':', '_');
    if (m_address.isEmpty() || !message.path().endsWith(suffix)) return;
    // Disconnected("org.bluez.Reason.Local", "Connection terminated by local host")
    const auto arguments = message.arguments();
    if (arguments.size() != 2 || arguments.at(0).toString() != "org.bluez.Reason.Local") return;
    m_connected = false;
    m_paused = true;
    saveSettings();
    restartMonitoring();
}

void ChromeMediaConnector::deviceDisconnected(const QString &address)
{
    if (address != m_address) return;
    m_connected = false;
    // A switch away during playback must not be followed by another takeover.
    m_policy.satisfy();
}

void ChromeMediaConnector::poll()
{
    if (m_pollBusy || !active()) return;
    m_pollBusy = true;
    const quint64 generation = m_generation;
    auto message = QDBusMessage::createMethodCall("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                 "org.freedesktop.DBus", "ListNames");
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, queryTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, generation](QDBusPendingCallWatcher *finished) {
        QDBusPendingReply<QStringList> reply = *finished;
        finished->deleteLater();
        if (reply.isError()) {
            LOG_WARN("Chrome media connect: ListNames failed: " << reply.error().message());
            playbackObserved(std::nullopt, generation);
            return;
        }
        QStringList players;
        for (const auto &name : reply.value()) {
            if (isChromePlayer(name)) players.append(name);
        }
        queryPlayers(players, generation);
    });
}

void ChromeMediaConnector::queryPlayers(QStringList services, quint64 generation, bool unknown)
{
    if (!active() || generation != m_generation || services.isEmpty()) {
        playbackObserved(unknown ? std::nullopt : std::optional<bool>(false), generation);
        return;
    }
    const QString service = services.takeFirst();
    // Chromium's empty introspection requires an explicit Properties.Get call.
    auto message = QDBusMessage::createMethodCall(service, playerPath, "org.freedesktop.DBus.Properties", "Get");
    message << playerInterface << QStringLiteral("PlaybackStatus");
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, queryTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, services, service, generation, unknown](QDBusPendingCallWatcher *finished) {
        QDBusPendingReply<QDBusVariant> reply = *finished;
        finished->deleteLater();
        if (reply.isError()) {
            LOG_DEBUG("Chrome media connect: PlaybackStatus unavailable for " << service << ": " << reply.error().message());
        } else if (reply.value().variant().toString() == "Playing") {
            playbackObserved(true, generation);
            return;
        }
        queryPlayers(services, generation, unknown || reply.isError());
    });
}

void ChromeMediaConnector::playbackObserved(std::optional<bool> playing, quint64 generation)
{
    m_pollBusy = false;
    if (!active() || generation != m_generation) return;
    if (m_policy.observe(m_clock.elapsed(), playing, m_connected, m_connectBusy)) {
        requestConnection(generation);
    }
}

bool ChromeMediaConnector::canConnect(quint64 generation) const
{
    return active() && generation == m_generation && !m_connected && m_policy.connectionStillWanted();
}

void ChromeMediaConnector::requestConnection(quint64 generation)
{
    m_connectBusy = true;
    auto message = QDBusMessage::createMethodCall("org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, queryTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, generation](QDBusPendingCallWatcher *finished) {
        QDBusPendingReply<ManagedObjectList> reply = *finished;
        finished->deleteLater();
        m_connectBusy = false;
        if (!canConnect(generation)) return;
        if (reply.isError()) {
            LOG_WARN("Chrome media connect: BlueZ device lookup failed: " << reply.error().message());
            return;
        }
        const auto objects = reply.value();
        for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
            const QVariantMap device = it.value().value("org.bluez.Device1");
            if (device.value("Address").toString() != m_address) continue;
            if (device.value("Connected").toBool()) {
                m_connected = true;
                m_policy.satisfy();
                return;
            }
            if (!device.value("Paired").toBool() || !device.value("Trusted").toBool()) {
                LOG_WARN("Chrome media connect: remembered AirPods must be paired and trusted");
                m_policy.satisfy();
                return;
            }
            auto request = QDBusMessage::createMethodCall("org.bluez", it.key().path(), "org.bluez.Device1", "Connect");
            m_connectBusy = true;
            LOG_INFO("Chrome media connect: requesting AirPods connection for browser playback");
            auto *connection = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(request, connectTimeoutMs), this);
            connect(connection, &QDBusPendingCallWatcher::finished, this, [this, generation](QDBusPendingCallWatcher *done) {
                QDBusPendingReply<> result = *done;
                done->deleteLater();
                m_connectBusy = false;
                if (generation == m_generation) m_policy.connectionFinished(m_clock.elapsed());
                if (result.isError()) {
                    LOG_WARN("Chrome media connect: BlueZ Connect failed: " << result.error().name() << ": " << result.error().message());
                } else if (generation == m_generation) {
                    m_policy.satisfy();
                    LOG_INFO("Chrome media connect: BlueZ Connect succeeded");
                }
            });
            return;
        }
        LOG_WARN("Chrome media connect: remembered AirPods not found in BlueZ; connect them manually once");
        m_policy.satisfy();
    });
}
