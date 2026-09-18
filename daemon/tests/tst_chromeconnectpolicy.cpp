#include <QtTest>
#include "media/chromeconnectpolicy.hpp"

class ChromeConnectPolicyTest : public QObject
{
    Q_OBJECT
private slots:
    void unknownPlaybackDoesNotRearmEpisode()
    {
        ChromeConnectPolicy policy;
        policy.observe(0, true, true);
        policy.observe(1000, std::nullopt, false);
        policy.observe(12000, std::nullopt, false);
        QVERIFY(!policy.observe(13000, true, false));
        QVERIFY(!policy.observe(15000, true, false));
    }

    void unknownPlaybackCancelsPendingLookup()
    {
        ChromeConnectPolicy policy;
        policy.observe(0, true, false);
        QVERIFY(policy.observe(2000, true, false));
        policy.observe(2001, std::nullopt, false);
        QVERIFY(!policy.connectionStillWanted(2001));
    }

    void slowFailureWaitsBeforeRetry()
    {
        ChromeConnectPolicy policy;
        policy.observe(0, true, false);
        QVERIFY(policy.observe(2000, true, false));
        QVERIFY(!policy.observe(17000, true, false, true));
        policy.connectionFinished(20000);
        QVERIFY(!policy.observe(20001, true, false));
        QVERIFY(!policy.observe(34999, true, false));
        QVERIFY(policy.observe(35000, true, false));
    }

    void idleConnectionEventsDoNotSuppressNextPlayback()
    {
        ChromeConnectPolicy policy;
        policy.satisfy();
        QVERIFY(!policy.observe(0, true, false));
        QVERIFY(policy.observe(2000, true, false));
    }

    void disconnectDuringLookupCancelsRequest()
    {
        ChromeConnectPolicy policy;
        policy.observe(0, true, false);
        QVERIFY(policy.observe(2000, true, false));
        QVERIFY(policy.connectionStillWanted(2000));
        policy.satisfy();
        QVERIFY(!policy.connectionStillWanted(2001));
    }

    void debounceAndBoundedRetries()
    {
        ChromeConnectPolicy policy;
        QVERIFY(!policy.observe(0, true, false));
        QVERIFY(!policy.observe(1999, true, false));
        QVERIFY(policy.observe(2000, true, false));
        QVERIFY(!policy.observe(16999, true, false));
        QVERIFY(policy.observe(17000, true, false));
        QVERIFY(!policy.observe(120000, true, false));
    }

    void shortPlaybackNeverConnects()
    {
        ChromeConnectPolicy policy;
        QVERIFY(!policy.observe(0, true, false));
        QVERIFY(!policy.observe(1000, false, false));
        QVERIFY(!policy.observe(2000, true, false));
        QVERIFY(!policy.observe(3000, false, false));
    }

    void manualDisconnectAndBriefPauseDoNotReclaim()
    {
        ChromeConnectPolicy policy;
        QVERIFY(!policy.observe(0, true, true));
        QVERIFY(!policy.observe(2000, true, false));
        QVERIFY(!policy.observe(3000, false, false));
        QVERIFY(!policy.observe(5000, true, false));
        QVERIFY(!policy.observe(7000, true, false));
        QVERIFY(!policy.observe(8000, false, false));
        QVERIFY(!policy.observe(18000, true, false));
        QVERIFY(policy.observe(20000, true, false));
    }

    void cooldownSurvivesNewEpisode()
    {
        ChromeConnectPolicy policy;
        policy.observe(0, true, false);
        QVERIFY(policy.observe(2000, true, false));
        policy.observe(3000, false, false);
        policy.observe(14000, true, false);
        QVERIFY(!policy.observe(16000, true, false));
        QVERIFY(!policy.observe(61999, true, false));
        QVERIFY(policy.observe(62000, true, false));
    }

    void pendingRequestDoesNotConsumeRetry()
    {
        ChromeConnectPolicy policy;
        policy.observe(0, true, false);
        QVERIFY(policy.observe(2000, true, false));
        QVERIFY(!policy.observe(17000, true, false, true));
        QVERIFY(policy.observe(18000, true, false));
        policy.satisfy();
        QVERIFY(!policy.observe(90000, true, false));
    }
};

QTEST_GUILESS_MAIN(ChromeConnectPolicyTest)
#include "tst_chromeconnectpolicy.moc"
