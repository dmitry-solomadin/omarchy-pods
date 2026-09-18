#pragma once

#include <QtGlobal>
#include <optional>

class ChromeConnectPolicy
{
public:
    static constexpr qint64 settleMs = 2000;
    static constexpr qint64 pauseGraceMs = 10000;
    static constexpr qint64 retryMs = 15000;
    static constexpr qint64 cooldownMs = 60000;
    static constexpr int maxAttempts = 2;

    bool observe(qint64 now, std::optional<bool> playing, bool connected, bool busy = false)
    {
        // An unreadable player is not evidence that playback stopped.
        if (!playing.has_value()) {
            m_playingSince = -1;
            m_pausedSince = -1;
            return false;
        }
        if (m_pausedSince >= 0 && now - m_pausedSince >= pauseGraceMs) {
            m_attempts = 0;
            m_satisfied = false;
            m_episodeActive = false;
        }
        if (!*playing) {
            m_playingSince = -1;
            if (m_pausedSince < 0) m_pausedSince = now;
            return false;
        }
        m_pausedSince = -1;
        m_episodeActive = true;
        if (m_playingSince < 0) m_playingSince = now;
        if (connected) m_satisfied = true;
        if (busy || m_satisfied || m_attempts >= maxAttempts || !settled(now) || now < m_retryAfter) return false;
        const qint64 delay = m_attempts == 0 ? cooldownMs : retryMs;
        if (m_lastAttempt >= 0 && now - m_lastAttempt < delay) return false;
        m_lastAttempt = now;
        ++m_attempts;
        return true;
    }

    bool settled(qint64 now) const { return m_playingSince >= 0 && now - m_playingSince >= settleMs; }
    bool connectionStillWanted(qint64 now) const { return settled(now) && !m_satisfied; }
    void satisfy() { if (m_episodeActive) m_satisfied = true; }
    void connectionFinished(qint64 now) { m_retryAfter = now + retryMs; }

private:
    qint64 m_playingSince = -1;
    qint64 m_pausedSince = -1;
    qint64 m_lastAttempt = -1;
    qint64 m_retryAfter = 0;
    int m_attempts = 0;
    bool m_satisfied = false;
    bool m_episodeActive = false;
};
