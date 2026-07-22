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
#pragma once

#include <QObject>
#include <QTimer>

namespace OCC {

class AccountState;

/**
 * @brief Polls the server for the account's storage quota.
 *
 * Uses an RFC 4331 PROPFIND (quota-used-bytes / quota-available-bytes) on the
 * account's WebDAV root - refreshed when the account connects and every five
 * minutes while it stays connected. Warns once via the tray when usage crosses
 * 90% (re-armed when it drops below 85% again).
 *
 * @ingroup gui
 */
class QuotaInfo : public QObject
{
    Q_OBJECT
public:
    explicit QuotaInfo(AccountState *accountState);

    qint64 lastQuotaUsedBytes() const { return _lastQuotaUsedBytes; }
    /// 0 while unknown, or when the server reports no limit
    qint64 lastQuotaTotalBytes() const { return _lastQuotaTotalBytes; }

public slots:
    void refresh();

signals:
    void quotaUpdated(qint64 usedBytes, qint64 totalBytes);

private:
    void maybeWarnNearlyFull();

    AccountState *_accountState;
    QTimer _refreshTimer;
    qint64 _lastQuotaUsedBytes = 0;
    qint64 _lastQuotaTotalBytes = 0;
    bool _warnedNearlyFull = false;
    bool _jobRunning = false;
};

} // namespace OCC
