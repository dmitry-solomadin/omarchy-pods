#pragma once

#include <QString>

inline bool isAutoConnectPlayer(const QString &service)
{
    for (const auto *player : {"chromium", "google-chrome", "spotify"}) {
        const QString name = QStringLiteral("org.mpris.MediaPlayer2.") + QLatin1String(player);
        if (service == name || service.startsWith(name + '.')) return true;
    }
    return false;
}
