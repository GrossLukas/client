// SPDX-License-Identifier: GPL-2.0-or-later

#include <QtTest>

#include "libsync/owncloudtheme.h"
#include "libsync/config/appconfig.h"

class TestAppConfig : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testConfigPath()
    {
        auto t = OCC::ownCloudTheme();
        QCOMPARE(OCC::AppConfig::configPath(QOperatingSystemVersion::Windows, t), QString("HKEY_LOCAL_MACHINE\\Software\\Policies\\BW-Tech GmbH\\owncloud.online"));
        QCOMPARE(OCC::AppConfig::configPath(QOperatingSystemVersion::MacOS, t), QString("/Library/Preferences/online.owncloud.desktopclient/owncloud.online.ini"));
        QCOMPARE(OCC::AppConfig::configPath(QOperatingSystemVersion::Unknown, t), QString("/etc/owncloud.online/owncloud.online.ini"));
    }
};

QTEST_GUILESS_MAIN(TestAppConfig)
#include "testappconfig.moc"
