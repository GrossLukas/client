/*
 * Copyright (C) by BW-Tech GmbH for owncloud.online.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "quotainfo.h"

#include "account.h"
#include "accountstate.h"
#include "application.h"
#include "common/utility.h"
#include "networkjobs.h"
#include "owncloudgui.h"

#include <QLoggingCategory>

#include <limits>

using namespace std::chrono_literals;

namespace OCC {

Q_LOGGING_CATEGORY(lcQuotaInfo, "gui.quotainfo", QtInfoMsg)

namespace {
    // usage ratio that triggers the one-shot "nearly full" tray warning, with
    // a lower re-arm bound so the warning does not repeat on every poll
    constexpr double warnThreshold = 0.90;
    constexpr double rearmThreshold = 0.85;
}

QuotaInfo::QuotaInfo(AccountState *accountState)
    : QObject(accountState)
    , _accountState(accountState)
{
    connect(accountState, &AccountState::stateChanged, this, [this](AccountState::State state) {
        if (state == AccountState::Connected)
            refresh();
    });

    _refreshTimer.setInterval(5min);
    connect(&_refreshTimer, &QTimer::timeout, this, &QuotaInfo::refresh);
    _refreshTimer.start();

    refresh();
}

void QuotaInfo::refresh()
{
    if (_jobRunning || !_accountState->isConnected())
        return;

    Account *account = _accountState->account();
    auto *job = new PropfindJob(account, account->davUrl(), QString(), PropfindJob::Depth::Zero, this);
    job->setProperties({QByteArrayLiteral("quota-available-bytes"), QByteArrayLiteral("quota-used-bytes")});
    _jobRunning = true;
    connect(job, &QObject::destroyed, this, [this] { _jobRunning = false; });
    connect(job, &PropfindJob::directoryListingIterated, this, [this](const QString &, const QMap<QString, QString> &properties) {
        bool okUsed = false;
        bool okAvailable = false;
        const qint64 used = properties.value(QStringLiteral("quota-used-bytes")).toLongLong(&okUsed);
        const qint64 available = properties.value(QStringLiteral("quota-available-bytes")).toLongLong(&okAvailable);
        if (!okUsed)
            return;
        // negative "available" values are DAV specials (-1 unknown, -2 unlimited,
        // -3 infinite): treat them all as "no limit"
        _lastQuotaUsedBytes = qMax<qint64>(0, used);
        // saturate: both values come from the server and must not overflow the sum
        _lastQuotaTotalBytes = (okAvailable && available >= 0)
            ? (available > std::numeric_limits<qint64>::max() - _lastQuotaUsedBytes
                      ? std::numeric_limits<qint64>::max()
                      : _lastQuotaUsedBytes + available)
            : 0;
        qCDebug(lcQuotaInfo) << "Quota for" << _accountState->account()->displayNameWithHost()
                             << "used:" << _lastQuotaUsedBytes << "total:" << _lastQuotaTotalBytes;
        maybeWarnNearlyFull();
        emit quotaUpdated(_lastQuotaUsedBytes, _lastQuotaTotalBytes);
    });
    job->start();
}

void QuotaInfo::maybeWarnNearlyFull()
{
    if (_lastQuotaTotalBytes <= 0)
        return;
    const double ratio = double(_lastQuotaUsedBytes) / double(_lastQuotaTotalBytes);
    if (ratio < rearmThreshold) {
        _warnedNearlyFull = false;
        return;
    }
    if (ratio < warnThreshold || _warnedNearlyFull)
        return;

    _warnedNearlyFull = true;
    qCWarning(lcQuotaInfo) << "Account storage nearly full:" << _lastQuotaUsedBytes << "of" << _lastQuotaTotalBytes;
    Application *app = ocAppOrNull(); // tests run account states without an Application
    if (ownCloudGui *gui = app ? app->gui() : nullptr) {
        gui->slotShowTrayMessage(tr("Storage space almost full"),
            tr("The cloud storage of %1 is almost full: %2 of %3 are used.")
                .arg(_accountState->account()->displayNameWithHost(),
                    Utility::octetsToString(_lastQuotaUsedBytes),
                    Utility::octetsToString(_lastQuotaTotalBytes)));
    }
}

} // namespace OCC
