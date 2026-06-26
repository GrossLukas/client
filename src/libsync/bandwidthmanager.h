/*
 * Copyright (C) by Markus Goetz <markus@woboq.com>
 *
 * Re-introduced for owncloud.online: upstream removed bandwidth limiting in
 * DC-45 (#12171); this restores the proven BandwidthManager so users can cap
 * upload/download speed again. Recovered from before the removal.
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
#include <QIODevice>

#include <list>

namespace OCC {

class UploadDevice;
class GETFileJob;
class OwncloudPropagator;

/**
 * @brief The BandwidthManager class
 * @ingroup libsync
 */
class BandwidthManager : public QObject
{
    Q_OBJECT
public:
    BandwidthManager(OwncloudPropagator *p);
    ~BandwidthManager() override;

    bool usingAbsoluteUploadLimit() { return _currentUploadLimit > 0; }
    bool usingRelativeUploadLimit() { return _currentUploadLimit < 0; }
    bool usingAbsoluteDownloadLimit() { return _currentDownloadLimit > 0; }
    bool usingRelativeDownloadLimit() { return _currentDownloadLimit < 0; }


    qint64 currentDownloadLimit() const;
    void setCurrentDownloadLimit(qint64 newCurrentDownloadLimit);

    qint64 currentUploadLimit() const;
    void setCurrentUploadLimit(qint64 newCurrentUploadLimit);

public Q_SLOTS:
    void registerUploadDevice(UploadDevice *);
    void unregisterUploadDevice(QObject *);

    void registerDownloadJob(GETFileJob *);
    void unregisterDownloadJob(GETFileJob *);

    void absoluteLimitTimerExpired();

    void relativeUploadMeasuringTimerExpired();
    void relativeUploadDelayTimerExpired();

    void relativeDownloadMeasuringTimerExpired();
    void relativeDownloadDelayTimerExpired();

private:
    // FIXME this timer and this variable should be replaced
    // by the propagator emitting the changed limit values to us as signal
    OwncloudPropagator *_propagator;

    // for absolute up/down bw limiting
    QTimer _absoluteLimitTimer;

    // FIXME merge these two lists
    std::list<UploadDevice *> _absoluteUploadDeviceList;
    std::list<UploadDevice *> _relativeUploadDeviceList;

    QTimer _relativeUploadMeasuringTimer;

    // for relative bw limiting, we need to wait this amount before measuring again
    QTimer _relativeUploadDelayTimer;

    // the device measured
    UploadDevice *_relativeLimitCurrentMeasuredDevice;

    // for measuring how much progress we made at start
    qint64 _relativeUploadLimitProgressAtMeasuringRestart;
    qint64 _currentUploadLimit;

    std::list<GETFileJob *> _downloadJobList;
    QTimer _relativeDownloadMeasuringTimer;

    // for relative bw limiting, we need to wait this amount before measuring again
    QTimer _relativeDownloadDelayTimer;

    // the device measured
    GETFileJob *_relativeLimitCurrentMeasuredJob;

    // for measuring how much progress we made at start
    qint64 _relativeDownloadLimitProgressAtMeasuringRestart;

    qint64 _currentDownloadLimit;
};
}
