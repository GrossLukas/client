/*
 * Copyright (C) 2026 by BW-Tech GmbH (owncloud.online)
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

#include "bulkpropagatorjob.h"

#include "account.h"
#include "common/syncjournaldb.h"
#include "common/utility.h"
#include "filesystem.h"
#include "networkjobs.h"
#include "owncloudpropagator_p.h"
#include "syncengine.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <algorithm>
#include <utility>

namespace OCC {

Q_LOGGING_CATEGORY(lcBulkPropagatorJob, "sync.propagator.bulkupload", QtInfoMsg)

namespace {
    // X-File-Path is the path of the file relative to the user's files root, which
    // is exactly what OwncloudPropagator::fullRemotePath() returns (relative to the
    // WebDAV files root). Make sure it has a single leading slash.
    QByteArray remoteFilePath(OwncloudPropagator *propagator, const SyncFileItemPtr &item)
    {
        QString path = propagator->fullRemotePath(item->_file);
        if (!path.startsWith(QLatin1Char('/'))) {
            path.prepend(QLatin1Char('/'));
        }
        return path.toUtf8();
    }
}

BulkPropagatorJob::BulkPropagatorJob(OwncloudPropagator *propagator, QVector<SyncFileItemPtr> &&items)
    : PropagatorJob(propagator, QStringLiteral("bulk"))
    , _items(std::move(items))
{
}

bool BulkPropagatorJob::scheduleSelfOrChild()
{
    if (state() == Finished || _started) {
        return false;
    }
    _started = true;
    setState(Running);
    // ins Transfer-Budget des Schedulers einbuchen (Gegenstueck in markDone())
    propagator()->bulkJobStarted();

    const QByteArray boundary = QByteArrayLiteral("owncloud_online_bulk_boundary_marker");
    QByteArray body;
    body.reserve(64 * 1024);

    for (const auto &item : std::as_const(_items)) {
        const QString localPath = propagator()->fullLocalPath(item->_file);

        if (FileSystem::isFileLocked(localPath, FileSystem::LockMode::SharedRead)) {
            Q_EMIT propagator()->seenLockedFile(localPath, FileSystem::LockMode::SharedRead);
            completeItem(item, SyncFileItem::SoftError, {}, {}, tr("%1 the file is currently in use").arg(localPath));
            continue;
        }

        QFile file(localPath);
        if (!file.open(QIODevice::ReadOnly)) {
            completeItem(item, SyncFileItem::SoftError, {}, {}, tr("Could not read %1").arg(localPath));
            continue;
        }
        const QByteArray data = file.readAll();
        file.close();

        // Skip files that changed since discovery; they will be re-picked next sync.
        if (FileSystem::fileChanged(QFileInfo(localPath), item->_size, item->_modtime)) {
            propagator()->_anotherSyncNeeded = true;
            completeItem(item, SyncFileItem::SoftError, {}, {}, tr("Local file changed during sync."));
            continue;
        }

        const QByteArray md5 = QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex();
        body += "--" + boundary + "\r\n";
        body += "X-File-Path: " + remoteFilePath(propagator(), item) + "\r\n";
        body += "X-File-Md5: " + md5 + "\r\n";
        body += "X-OC-Mtime: " + QByteArray::number(static_cast<qint64>(item->_modtime)) + "\r\n";
        body += "Content-Length: " + QByteArray::number(static_cast<qint64>(data.size())) + "\r\n\r\n";
        body += data;
        body += "\r\n";
        _sentItems.append(item);
    }
    body += "--" + boundary + "--\r\n";

    if (_sentItems.isEmpty()) {
        // Every item failed to read; they were already completed with an error.
        finishJob(SyncFileItem::SoftError);
        return true;
    }

    const QByteArray contentType = QByteArrayLiteral("multipart/related; boundary=") + boundary;
    QNetworkRequest request;
    request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);

    // POST to the DAV root endpoint /remote.php/dav/bulk (NOT the files WebDAV url).
    _job = new SimpleNetworkJob(propagator()->account(), propagator()->account()->url(),
        QStringLiteral("remote.php/dav/bulk"), "POST", std::move(body), request, this);
    connect(_job, &SimpleNetworkJob::finishedSignal, this, &BulkPropagatorJob::slotUploadFinished);
    qCInfo(lcBulkPropagatorJob) << "Bulk uploading" << _sentItems.size() << "files in one request";
    _job->start();
    return true;
}

void BulkPropagatorJob::slotUploadFinished()
{
    if (state() == Finished) {
        // Aborted in the meantime; sent items will be retried on the next sync.
        return;
    }

    const auto reply = _job ? _job->reply() : nullptr;
    const int httpCode = reply ? reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0;
    const auto netError = reply ? reply->error() : QNetworkReply::OperationCanceledError;

    if (netError != QNetworkReply::NoError || httpCode < 200 || httpCode >= 300) {
        const QString err = reply ? reply->errorString() : tr("Bulk upload failed");
        qCWarning(lcBulkPropagatorJob) << "Bulk upload failed, http" << httpCode << err;
        // Whole batch failed: soft-error every file so they retry individually.
        for (const auto &item : std::as_const(_sentItems)) {
            completeItem(item, SyncFileItem::SoftError, {}, {}, err);
        }
        propagator()->_anotherSyncNeeded = true;
        finishJob(SyncFileItem::SoftError);
        return;
    }

    const QJsonObject results = QJsonDocument::fromJson(reply->readAll()).object();
    for (const auto &item : std::as_const(_sentItems)) {
        const QString key = QString::fromUtf8(remoteFilePath(propagator(), item));
        const QJsonObject entry = results.value(key).toObject();
        if (entry.isEmpty() || entry.value(QStringLiteral("error")).toBool(true)) {
            const QString msg = entry.value(QStringLiteral("message")).toString(tr("Server did not confirm the upload"));
            propagator()->_anotherSyncNeeded = true;
            completeItem(item, SyncFileItem::SoftError, {}, {}, msg);
        } else {
            const QString etag = Utility::normalizeEtag(entry.value(QStringLiteral("etag")).toString());
            const QByteArray fileId = entry.value(QStringLiteral("OC-FileID")).toVariant().toByteArray();
            if (etag.isEmpty()) {
                // A success entry without an etag would be written to the journal with an
                // empty etag, making the next discovery re-download the just-uploaded file.
                // Retry it on the next sync instead of recording an incomplete record.
                propagator()->_anotherSyncNeeded = true;
                completeItem(item, SyncFileItem::SoftError, {}, {}, tr("Server confirmed the upload without an etag"));
            } else {
                completeItem(item, SyncFileItem::Success, etag, fileId, {});
            }
        }
    }

    // If completeItem() hit a FatalError (a journal write failure in updateMetadata),
    // mirror PropagateItemJob::done(): a failing journal must hard-stop the whole sync,
    // because items already marked done in memory are not persisted and could later be
    // seen as deletions. A SoftError on the other hand only schedules another sync.
    const bool anyFatal = std::any_of(_sentItems.cbegin(), _sentItems.cend(),
        [](const SyncFileItemPtr &i) { return i->_status == SyncFileItem::FatalError; });
    if (anyFatal) {
        // Emit finished BEFORE aborting (like PropagateItemJob::done): propagator()->abort()
        // cascades into this job's abort(), which sets the state to Finished and would make
        // a subsequent finishJob() a no-op, swallowing the finished signal and hanging the
        // parent composite job.
        finishJob(SyncFileItem::FatalError);
        propagator()->abort();
        return;
    }
    finishJob(SyncFileItem::Success);
}

void BulkPropagatorJob::completeItem(const SyncFileItemPtr &item, SyncFileItem::Status status,
    const QString &etag, const QByteArray &fileId, const QString &errorString)
{
    item->_status = status;
    if (status == SyncFileItem::Success) {
        if (!etag.isEmpty()) {
            item->_etag = etag;
        }
        if (!fileId.isEmpty()) {
            item->_fileId = fileId;
        }
        item->_errorString.clear();
        if (item->_hasBlacklistEntry) {
            propagator()->_journal->wipeErrorBlacklistEntry(item->_file);
            item->_hasBlacklistEntry = false;
        }

        // Persist the sync journal record, exactly like PropagateUploadCommon::finalize()
        // does via updateMetadata(). Without this the file is re-discovered as new on the
        // next sync, which breaks no-op detection and delete propagation. The bulk response
        // carries no permissions, so _remotePerm stays null and the next remote PROPFIND
        // fills it in via a cheap metadata-only update.
        const auto result = propagator()->updateMetadata(*item);
        if (!result) {
            item->_status = SyncFileItem::FatalError;
            item->_errorString = tr("Error updating metadata: %1").arg(result.error());
            propagator()->_anotherSyncNeeded = true;
        } else {
            propagator()->_journal->setUploadInfo(item->_file, SyncJournalDb::UploadInfo());
            propagator()->reportProgress(*item, item->_size);
        }
    } else {
        if (item->_errorString.isEmpty()) {
            item->_errorString = errorString;
        }
    }
    // Mirror PropagateItemJob::done(): tell the engine so the sync journal is updated.
    Q_EMIT propagator()->itemCompleted(item);
}

void BulkPropagatorJob::finishJob(SyncFileItem::Status status)
{
    if (state() == Finished) {
        return;
    }
    setState(Finished);
    markDone();
    Q_EMIT finished(status);
}

void BulkPropagatorJob::markDone()
{
    // exakt einmal pro Running->Finished-Uebergang das Budget freigeben —
    // sowohl finishJob() als auch abort() (das ohne finished()-Emission
    // beendet) laufen hier durch
    if (_countedDone || !_started) {
        return;
    }
    _countedDone = true;
    propagator()->bulkJobFinished();
}

void BulkPropagatorJob::abort(PropagatorJob::AbortType abortType)
{
    // Stop further handling first so a cancelled reply doesn't double-complete items.
    if (state() != Finished) {
        setState(Finished);
        markDone();
    }
    if (_job && _job->reply()) {
        _job->reply()->abort();
    }
    if (abortType == AbortType::Asynchronous) {
        Q_EMIT abortFinished();
    }
}

}
