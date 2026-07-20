/*
 * Copyright (C) by Olivier Goffart <ogoffart@owncloud.com>
 *
 * Modified by BW-Tech GmbH for owncloud.online: re-introduced the ownCloud 10
 * "new" (NG) chunked upload algorithm dropped upstream in DC-54 (#12176), so
 * large files upload in resumable chunks against owncloud.online servers that
 * advertise dav.chunking 1.0 but no TUS support.
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

#include "propagateuploadng.h"
#include "account.h"
#include "bandwidthmanager.h"
#include "common/asserts.h"
#include "common/syncjournaldb.h"
#include "filesystem.h"
#include "networkjobs.h"
#include "owncloudpropagator_p.h"
#include "propagateremotedelete.h"
#include "propagateremotemove.h"
#include "propagatorjobs.h"
#include "putfilejob.h"
#include "syncengine.h"
#include "uploaddevice.h"

#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTimer>

#include <memory>

namespace OCC {

Q_LOGGING_CATEGORY(lcPropagateUploadNG, "sync.propagator.upload.ng", QtInfoMsg)

namespace {

    // Maximum number of in-run retries for transient server errors per transfer.
    // Enough to ride out a busy database without ever spinning forever.
    constexpr int maxTransientRetries = 10;

    /* oc10 on MySQL/MariaDB occasionally rejects an operation on the uploads
     * directory with a rolled-back transaction when requests race each other:
     *   "SQLSTATE[40001]: Serialization failure: 1213 Deadlock found when
     *    trying to get lock; try restarting transaction"
     * The server explicitly asks for a retry, so treat this as transient
     * instead of failing the whole transfer. */
    bool isTransientServerError(int httpCode, const QByteArray &errorBody)
    {
        if (httpCode < 500) {
            return false;
        }
        return errorBody.contains("SQLSTATE[40001]") || errorBody.contains("1213 Deadlock")
            || errorBody.contains("Deadlock found") || errorBody.contains("try restarting transaction");
    }

    std::chrono::milliseconds transientRetryDelay(int attempt)
    {
        // 500ms, 1s, 2s, 4s, 8s, then capped, plus jitter so parallel clients spread out
        const int exponent = qMin(attempt - 1, 4);
        const int base = qMin(500 * (1 << exponent), 8000);
        return std::chrono::milliseconds(base + int(QRandomGenerator::global()->bounded(250)));
    }
}

QString PropagateUploadFileNG::chunkPath(qint64 chunkOffset)
{
    QString path = QLatin1String("remote.php/dav/uploads/")
        + propagator()->account()->davUser()
        + QLatin1Char('/') + QString::number(_transferId);
    if (chunkOffset != -1) {
        // We need to do add leading 0 because the server orders the chunk alphabetically
        path += QLatin1Char('/') + QString::number(chunkOffset).rightJustified(16, QLatin1Char('0')); // 1e16 is 10 petabyte
    }
    return path;
}


/*
State machine:

  +---> doStartUpload()
        doStartUploadNext()
        Check the db: is there an entry?
           +                           +
           |no                         |yes
           |                           v
           v                        PROPFIND
           startNewUpload() <-+        +-------------------------------------+
              +               |        +                                     |
             MKCOL            + slotPropfindFinishedWithError()     slotPropfindFinished()
              +                                                       Is there stale files to remove?
          slotMkColFinished()                                         +                      +
              +                                                       no                    yes
              |                                                       +                      +
              |                                                       |                  DeleteJob
              |                                                       |                      +
        +-----+^------------------------------------------------------+^--+  slotDeleteJobFinished()
        |
        |
        |
        +---->  startNextChunk() +-> finished?  +-
                      ^               +          |
                      +---------------+          |
                                                 |
        +----------------------------------------+
        |
        +-> MOVE +-----> moveJobFinished() +--> finalize()
 */

PropagateUploadFileNG::PropagateUploadFileNG(OwncloudPropagator *propagator, const SyncFileItemPtr &item)
    : PropagateUploadCommon(propagator, item)
    , _bytesToUpload(item->_size)
{
    // On HTTP/2 extra concurrent chunk streams are cheap (one connection, many
    // multiplexed streams), so push more chunks in parallel there.
    if (propagator->account() && propagator->account()->isHttp2Supported()) {
        _maxParallelChunks = 6;
    }
    // Admin override, e.g. for servers whose database cannot cope with
    // concurrent chunk PUTs into the same upload directory (set to 1).
    const int envMaxParallelChunks = qEnvironmentVariableIntValue("OWNCLOUD_MAX_PARALLEL_CHUNKS");
    if (envMaxParallelChunks > 0) {
        _maxParallelChunks = qBound(1, envMaxParallelChunks, 16);
    }
    // A transfer of this sync run already hit a server-side transaction
    // deadlock: don't keep provoking it with concurrent chunk PUTs.
    if (propagator->_serializeChunkUploads) {
        _maxParallelChunks = 1;
    }
}
void PropagateUploadFileNG::doStartUpload()
{
    // All upload jobs are constructed up front when the propagator builds its job
    // tree, so the run-wide serial fallback has to be picked up here, at the time
    // the transfer actually starts (a deadlock may have happened since).
    if (propagator()->_serializeChunkUploads) {
        _maxParallelChunks = 1;
    }

    const QString fileName = propagator()->fullLocalPath(_item->_file);
    // If the file is currently locked, we want to retry the sync
    // when it becomes available again.
    if (FileSystem::isFileLocked(fileName, FileSystem::LockMode::SharedRead)) {
        Q_EMIT propagator()->seenLockedFile(fileName, FileSystem::LockMode::SharedRead);
        abortWithError(SyncFileItem::SoftError, tr("%1 the file is currently in use").arg(QDir::toNativeSeparators(fileName)));
        return;
    }

    propagator()->_activeJobList.append(this);

    UploadRangeInfo rangeinfo = { 0, _item->_size };
    _rangesToUpload.append(rangeinfo);
    _bytesToUpload = _item->_size;
    doStartUploadNext();
}


void PropagateUploadFileNG::doStartUploadNext()
{
    const SyncJournalDb::UploadInfo progressInfo = propagator()->_journal->getUploadInfo(_item->_file);
    if (progressInfo.isChunked() && progressInfo.validate(_item->_size, _item->_modtime, _item->_checksumHeader)) {
        _transferId = progressInfo._transferid;
        auto job = new PropfindJob(propagator()->account(), propagator()->account()->url(), chunkPath(), PropfindJob::Depth::One, this);
        addChildJob(job);
        job->setProperties({ QByteArrayLiteral("resourcetype"), QByteArrayLiteral("getcontentlength") });
        connect(job, &PropfindJob::finishedWithoutError, this, &PropagateUploadFileNG::slotPropfindFinished);
        connect(job, &PropfindJob::finishedWithError,
            this, &PropagateUploadFileNG::slotPropfindFinishedWithError);
        connect(job, &PropfindJob::directoryListingIterated,
            this, &PropagateUploadFileNG::slotPropfindIterate);
        job->start();
        return;
    } else if (progressInfo._valid && progressInfo.isChunked()) {
        // The upload info is stale. remove the stale chunks on the server
        _transferId = progressInfo._transferid;
        // Fire and forget. Any error will be ignored.
        (new DeleteJob(propagator()->account(), propagator()->account()->url(), chunkPath(), this))->start();
        // startNewUpload will reset the _transferId and the UploadInfo in the db.
    }

    startNewUpload();
}

void PropagateUploadFileNG::slotPropfindIterate(const QString &name, const QMap<QString, QString> &properties)
{
    if (name.endsWith(chunkPath())) {
        return; // skip the info about the path itself
    }
    bool ok = false;
    QString chunkName = name.mid(name.lastIndexOf(QLatin1Char('/')) + 1);
    qint64 chunkOffset = chunkName.toLongLong(&ok);
    if (ok) {
        ServerChunkInfo chunkinfo = { properties[QStringLiteral("getcontentlength")].toLongLong(), chunkName };
        _serverChunks[chunkOffset] = chunkinfo;
    }
}


bool PropagateUploadFileNG::markRangeAsDone(qint64 start, qint64 size)
{
    bool found = false;
    for (auto iter = _rangesToUpload.begin(); iter != _rangesToUpload.end(); ++iter) {
        /* Only remove if they start at exactly the same chunk */
        if (iter->start == start && iter->size >= size) {
            found = true;
            iter->start += size;
            iter->size -= size;
            if (iter->size <= 0) {
                _rangesToUpload.erase(iter);
                break;
            }
        }
    }

    return found;
}

void PropagateUploadFileNG::slotPropfindFinished()
{
    propagator()->_activeJobList.removeOne(this);

    _currentChunkOffset = 0;
    _sent = 0;

    // here is a copy because we might need to remove item(s) during iteration
    const auto serverChunks = _serverChunks;
    for (auto it = serverChunks.cbegin(); it != serverChunks.cend(); ++it) {
        const auto &chunkOffset = it.key();
        const auto &chunkSize = it.value().size;
        if (markRangeAsDone(chunkOffset, chunkSize)) {
            qCDebug(lcPropagateUploadNG) << "Reusing existing data:" << chunkOffset << chunkSize;
            _sent += chunkSize;
            _serverChunks.remove(chunkOffset);
        } else {
            qCDebug(lcPropagateUploadNG) << "Discarding existing data:" << chunkOffset << chunkSize;
        }
    }

    if (_sent > _bytesToUpload) {
        // Normally this can't happen because the size is xor'ed with the transfer id, and it is
        // therefore impossible that there is more data on the server than on the file.
        qCCritical(lcPropagateUploadNG) << "Inconsistency while resuming " << _item->_file
                                      << ": the size on the server (" << _sent << ") is bigger than the size of the file ("
                                      << _item->_size << ")";

        // Wipe the old chunking data.
        // Fire and forget. Any error will be ignored.
        (new DeleteJob(propagator()->account(), propagator()->account()->url(), chunkPath(), this))->start();

        propagator()->_activeJobList.append(this);
        startNewUpload();
        return;
    }

    qCInfo(lcPropagateUploadNG) << "Resuming " << _item->_file << "; sent =" << _sent << "; total=" << _bytesToUpload;

    if (!_serverChunks.isEmpty()) {
        qCInfo(lcPropagateUploadNG) << "To Delete" << _serverChunks.keys();
        propagator()->_activeJobList.append(this);
        _removeJobError = false;

        // Make sure that if there is a "hole" and then a few more chunks, on the server
        // we should remove the later chunks. Otherwise, when we do dynamic chunk sizing, we may end up
        // with corruptions if there are too many chunks, or if we abort and there are still stale chunks.
        for (auto it = _serverChunks.begin(); it != _serverChunks.end(); ++it) {
            auto job = new DeleteJob(propagator()->account(), propagator()->account()->url(), chunkPath() + QLatin1Char('/') + it->originalName, this);
            addChildJob(job);
            connect(job, &DeleteJob::finishedSignal, this, &PropagateUploadFileNG::slotDeleteJobFinished);
            job->start();
        }
        _serverChunks.clear();
        return;
    }

    scheduleChunks();
}

void PropagateUploadFileNG::slotPropfindFinishedWithError()
{
    auto job = qobject_cast<PropfindJob *>(sender());
    _item->_httpErrorCode = job->reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    _item->_responseTimeStamp = job->responseTimestamp();
    _item->_requestId = job->requestId();

    QNetworkReply::NetworkError err = job->reply()->error();
    auto status = classifyError(err, _item->_httpErrorCode, &propagator()->_anotherSyncNeeded);
    if (status == SyncFileItem::FatalError) {
        propagator()->_activeJobList.removeOne(this);
        abortWithError(status, job->errorStringParsingBody());
        return;
    }
    startNewUpload();
}

void PropagateUploadFileNG::slotDeleteJobFinished()
{
    auto job = qobject_cast<DeleteJob *>(sender());
    _item->_httpErrorCode = job->reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    _item->_responseTimeStamp = job->responseTimestamp();
    _item->_requestId = job->requestId();

    QNetworkReply::NetworkError err = job->reply()->error();
    if (err != QNetworkReply::NoError && err != QNetworkReply::ContentNotFoundError) {
        const int httpStatus = job->reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        SyncFileItem::Status status = classifyError(err, httpStatus);
        if (status == SyncFileItem::FatalError) {
            abortWithError(status, job->errorString());
            return;
        } else {
            qCWarning(lcPropagateUploadNG) << "DeleteJob errored out" << job->errorString() << job->reply()->url();
            _removeJobError = true;
            // Let the other jobs finish
        }
    }

    // If no more Delete jobs are running, we can continue
    bool runningDeleteJobs = false;
    for (auto *otherJob : childJobs()) {
        if (qobject_cast<DeleteJob *>(otherJob))
            runningDeleteJobs = true;
    }
    if (!runningDeleteJobs) {
        propagator()->_activeJobList.removeOne(this);
        if (_removeJobError) {
            // There was an error removing some files, just start over
            startNewUpload();
        } else {
            scheduleChunks();
        }
    }
}

void PropagateUploadFileNG::startNewUpload()
{
    OC_ASSERT(propagator()->_activeJobList.count(this) == 1);
    _transferId = QRandomGenerator::global()->generate();
    _sent = 0;

    propagator()->reportProgress(*_item, 0);

    auto pi = _item->toUploadInfo();
    pi._transferid = _transferId;
    propagator()->_journal->setUploadInfo(_item->_file, pi);
    propagator()->_journal->commit(QStringLiteral("Upload info"));
    QMap<QByteArray, QByteArray> headers;
    headers["OC-Total-Length"] = QByteArray::number(_item->_size);
    auto job = new MkColJob(propagator()->account(), propagator()->account()->url(), chunkPath(), headers, this);
    connect(job, &MkColJob::finishedWithError,
        this, &PropagateUploadFileNG::slotMkColFinished);
    connect(job, &MkColJob::finishedWithoutError,
        this, &PropagateUploadFileNG::slotMkColFinished);
    job->start();
}

void PropagateUploadFileNG::slotMkColFinished()
{
    propagator()->_activeJobList.removeOne(this);
    auto job = qobject_cast<MkColJob *>(sender());
    _item->_httpErrorCode = job->reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    _item->_responseTimeStamp = job->responseTimestamp();
    _item->_requestId = job->requestId();

    QNetworkReply::NetworkError err = job->reply()->error();
    if (err != QNetworkReply::NoError || _item->_httpErrorCode != 201) {
        // With several transfers running, even the MKCOL of the upload directory
        // can hit the server-side transaction deadlock; it is safe to repeat.
        const QByteArray errorBody = job->reply()->peek(128 * 1024);
        if (err != QNetworkReply::NoError && isTransientServerError(_item->_httpErrorCode, errorBody)
            && _transientRetryCount < maxTransientRetries) {
            ++_transientRetryCount;
            propagator()->_serializeChunkUploads = true;
            _maxParallelChunks = 1;
            qCWarning(lcPropagateUploadNG) << "Transient server error (transaction deadlock) on MKCOL for" << _item->_file
                                           << "- retry" << _transientRetryCount << "of" << maxTransientRetries;
            const auto delay = transientRetryDelay(_transientRetryCount);
            QTimer::singleShot(delay, this, [this] {
                if (!_finished && !_aborting && !propagator()->_abortRequested) {
                    propagator()->_activeJobList.append(this);
                    startNewUpload();
                }
            });
            return;
        }
        _item->_requestId = job->requestId();
        SyncFileItem::Status status = classifyError(err, _item->_httpErrorCode,
            &propagator()->_anotherSyncNeeded);
        abortWithError(status, job->errorStringParsingBody());
        return;
    }

    scheduleChunks();
}

void PropagateUploadFileNG::doFinalMove()
{
    // Still not finished all ranges.
    if (!_rangesToUpload.isEmpty())
        return;
    Q_ASSERT_X(childJobs().empty(), Q_FUNC_INFO, "MOVE for upload even though jobs are still running");

    _finished = true;

    // Finish with a MOVE
    QString destination = QDir::cleanPath(propagator()->webDavUrl().path()
        + propagator()->fullRemotePath(_item->_file));
    auto headers = PropagateUploadCommon::headers();

    // "If-Match" applies to the source, but we are interested in comparing the etag of the destination
    auto ifMatch = headers.take(QByteArrayLiteral("If-Match"));
    if (!ifMatch.isEmpty()) {
        headers[QByteArrayLiteral("If")] = "<" + QUrl::toPercentEncoding(destination, "/") + "> ([" + ifMatch + "])";
    }
    if (!_transmissionChecksumHeader.isEmpty()) {
        headers[checkSumHeaderC] = _transmissionChecksumHeader;
    }
    headers[QByteArrayLiteral("OC-Total-Length")] = QByteArray::number(_bytesToUpload);
    headers[QByteArrayLiteral("OC-Total-File-Length")] = QByteArray::number(_item->_size);

    const QString source = chunkPath() + QStringLiteral("/.file");

    auto job = new MoveJob(propagator()->account(), propagator()->account()->url(), source, destination, headers, this);
    addChildJob(job);
    connect(job, &MoveJob::finishedSignal, this, &PropagateUploadFileNG::slotMoveJobFinished);
    propagator()->_activeJobList.append(this);
    adjustLastJobTimeout(job, _item->_size);
    job->start();
    return;
}

void PropagateUploadFileNG::scheduleChunks()
{
    if (propagator()->_abortRequested)
        return;

    OC_ENFORCE_X(_bytesToUpload >= _sent, "Sent data exceeds file size");

    // Top up the pipeline: keep up to _maxParallelChunks chunk PUTs in flight.
    // Each chunk is an independent PUT to its own offset path, so the server
    // accepts them concurrently; the final MOVE assembles them once all are done.
    while (_inFlightChunks.size() < _maxParallelChunks && !_rangesToUpload.isEmpty()) {
        auto &range = _rangesToUpload.first();
        const qint64 offset = range.start;
        const qint64 size = qMin(propagator()->_chunkSize, range.size);

        // Carve the chunk off the front of the range now (at dispatch time) so the
        // same bytes are never dispatched twice while this PUT is still in flight.
        range.start += size;
        range.size -= size;
        if (range.size <= 0) {
            _rangesToUpload.removeFirst();
        }

        const QString fileName = propagator()->fullLocalPath(_item->_file);
        auto device = std::make_unique<UploadDevice>(fileName, offset, size, propagator()->_bandwidthManager);
        if (!device->open(QIODevice::ReadOnly)) {
            qCWarning(lcPropagateUploadNG) << "Could not prepare upload device: " << device->errorString();
            // Soft error because this is likely caused by the user modifying his files while syncing
            abortWithError(SyncFileItem::SoftError, device->errorString());
            return;
        }

        // job takes ownership of device via a std::unique_ptr. Job deletes itself when finishing
        PUTFileJob *job = new PUTFileJob(propagator()->account(), propagator()->account()->url(), chunkPath(offset), std::move(device), {}, this);
        addChildJob(job);
        _inFlightChunks.insert(job, {offset, size});
        connect(job, &PUTFileJob::finishedSignal, this, &PropagateUploadFileNG::slotPutFinished);
        connect(job, &PUTFileJob::uploadProgress, this, &PropagateUploadFileNG::slotUploadProgress);
        job->start();
        // Each in-flight chunk counts as one active transfer (a single job can be
        // on the active list several times when uploading chunks in parallel).
        propagator()->_activeJobList.append(this);
    }

    // Everything uploaded? Finish with the assembling MOVE.
    if (_inFlightChunks.isEmpty() && _rangesToUpload.isEmpty()) {
        doFinalMove();
    }
}

void PropagateUploadFileNG::slotPutFinished()
{
    PUTFileJob *job = qobject_cast<PUTFileJob *>(sender());
    OC_ASSERT(job);

    _item->_httpErrorCode = job->reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    _item->_responseTimeStamp = job->responseTimestamp();
    _item->_requestId = job->requestId();

    propagator()->_activeJobList.removeOne(this);
    const UploadRangeInfo chunkRange = _inFlightChunks.take(job);
    const qint64 chunkSize = chunkRange.size;

    if (_finished) {
        // The upload already finished (the assembling MOVE was started or an error
        // ended the transfer). Ignore the results of any other in-flight chunks.
        return;
    }

    QNetworkReply::NetworkError err = job->reply()->error();

    if (err != QNetworkReply::NoError) {
        // peek so a later commonErrorHandling() can still read the full body
        const QByteArray errorBody = job->reply()->peek(128 * 1024);
        if (isTransientServerError(_item->_httpErrorCode, errorBody) && _transientRetryCount < maxTransientRetries) {
            ++_transientRetryCount;
            // Concurrent chunk PUTs into the same upload directory are what
            // provokes the database deadlock: finish this transfer serially,
            // and let the rest of the sync run start serial right away.
            _maxParallelChunks = 1;
            propagator()->_serializeChunkUploads = true;
            // re-queue the failed range; it will be re-sent by scheduleChunks()
            _rangesToUpload.append(chunkRange);
            qCWarning(lcPropagateUploadNG) << "Transient server error (transaction deadlock) on chunk at offset" << chunkRange.start << "of"
                                           << _item->_file << "- retry" << _transientRetryCount << "of" << maxTransientRetries << "(serialized)";
            if (!_inFlightChunks.isEmpty()) {
                // the remaining in-flight chunks drain first; their completion
                // re-enters scheduleChunks(), now limited to one chunk at a time
                return;
            }
            const auto delay = transientRetryDelay(_transientRetryCount);
            QTimer::singleShot(delay, this, [this] {
                if (!_finished && !_aborting && !propagator()->_abortRequested) {
                    scheduleChunks();
                }
            });
            return;
        }
        commonErrorHandling(job);
        return;
    }

    _sent += chunkSize;
    OC_ENFORCE_X(_sent <= _bytesToUpload, "can't send more than size");
    propagator()->reportProgress(*_item, _sent);

    // Adjust the chunk size for the time taken. Only meaningful when chunks are
    // uploaded one at a time; with parallel chunks the per-chunk wall-clock time
    // includes contention from the other chunks and would skew the heuristic.
    auto targetDuration = propagator()->syncOptions()._targetChunkUploadDuration;
    if (_maxParallelChunks <= 1 && targetDuration.count() > 0) {
        auto uploadTime = ++job->msSinceStart(); // add one to avoid div-by-zero
        qint64 predictedGoodSize = (chunkSize * targetDuration) / uploadTime;

        // Exponential moving average to smooth the chunk sizes a bit.
        qint64 targetSize = propagator()->_chunkSize / 2 + predictedGoodSize / 2;
        propagator()->_chunkSize = qBound(
            propagator()->syncOptions()._minChunkSize,
            targetSize,
            propagator()->syncOptions()._maxChunkSize);

        qCInfo(lcPropagateUploadNG) << "Chunked upload of" << chunkSize << "bytes took" << uploadTime.count()
                                  << "ms, desired is" << targetDuration.count() << "ms, expected good chunk size is"
                                  << predictedGoodSize << "bytes and nudged next chunk size to "
                                  << propagator()->_chunkSize << "bytes";
    }

    const bool allChunksDone = _rangesToUpload.isEmpty() && _inFlightChunks.isEmpty();

    // Check if the file still exists
    const QString fullFilePath(propagator()->fullLocalPath(_item->_file));
    if (!FileSystem::fileExists(fullFilePath)) {
        if (!allChunksDone) {
            abortWithError(SyncFileItem::SoftError, tr("The local file was removed during sync."));
            return;
        } else {
            propagator()->_anotherSyncNeeded = true;
        }
    }

    // Check whether the file changed since discovery.
    if (FileSystem::fileChanged(QFileInfo{fullFilePath}, _item->_size, _item->_modtime)) {
        propagator()->_anotherSyncNeeded = true;
        if (!allChunksDone) {
            abortWithError(SyncFileItem::Message, fileChangedMessage());
            return;
        }
    }

    if (!allChunksDone) {
        // Deletes an existing blacklist entry on successful chunk upload
        if (_item->_hasBlacklistEntry) {
            propagator()->_journal->wipeErrorBlacklistEntry(_item->_file);
            _item->_hasBlacklistEntry = false;
        }

        // Reset the error count on successful chunk upload
        auto uploadInfo = propagator()->_journal->getUploadInfo(_item->_file);
        uploadInfo._errorCount = 0;
        propagator()->_journal->setUploadInfo(_item->_file, uploadInfo);
        propagator()->_journal->commit(QStringLiteral("Upload info"));
    }

    // Dispatch the next chunk(s), or start the assembling MOVE when all are done.
    scheduleChunks();
}

void PropagateUploadFileNG::slotMoveJobFinished()
{
    propagator()->_activeJobList.removeOne(this);
    auto job = qobject_cast<MoveJob *>(sender());
    QNetworkReply::NetworkError err = job->reply()->error();
    _item->_httpErrorCode = job->reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    _item->_responseTimeStamp = job->responseTimestamp();
    _item->_requestId = job->requestId();

    if (err != QNetworkReply::NoError) {
        // The assembling MOVE races the chunk bookkeeping in the server database
        // as well; a deadlocked (rolled back) MOVE is safe to repeat.
        const QByteArray errorBody = job->reply()->peek(128 * 1024);
        if (isTransientServerError(_item->_httpErrorCode, errorBody) && _transientRetryCount < maxTransientRetries) {
            ++_transientRetryCount;
            propagator()->_serializeChunkUploads = true;
            qCWarning(lcPropagateUploadNG) << "Transient server error (transaction deadlock) on final MOVE of" << _item->_file
                                           << "- retry" << _transientRetryCount << "of" << maxTransientRetries;
            _finished = false;
            const auto delay = transientRetryDelay(_transientRetryCount);
            QTimer::singleShot(delay, this, [this] {
                if (!_finished && !_aborting && !propagator()->_abortRequested) {
                    doFinalMove();
                }
            });
            return;
        }
        commonErrorHandling(job);
        return;
    }

    if (_item->_httpErrorCode == 202) {
        done(SyncFileItem::NormalError, tr("The server did ask for a removed legacy feature(polling)"));
        return;
    }

    if (_item->_httpErrorCode != 201 && _item->_httpErrorCode != 204) {
        abortWithError(SyncFileItem::NormalError, tr("Unexpected return code from server (%1)").arg(_item->_httpErrorCode));
        return;
    }

    QByteArray fid = job->reply()->rawHeader("OC-FileID");
    if (fid.isEmpty()) {
        qCWarning(lcPropagateUploadNG) << "Server did not return a OC-FileID" << _item->_file;
        abortWithError(SyncFileItem::NormalError, tr("Missing File ID from server"));
        return;
    } else {
        // the old file id should only be empty for new files uploaded
        if (!_item->_fileId.isEmpty() && _item->_fileId != fid) {
            qCWarning(lcPropagateUploadNG) << "File ID changed!" << _item->_fileId << fid;
        }
        _item->_fileId = fid;
    }

    _item->_etag = getEtagFromReply(job->reply());
    if (_item->_etag.isEmpty()) {
        qCWarning(lcPropagateUploadNG) << "Server did not return an ETAG" << _item->_file;
        abortWithError(SyncFileItem::NormalError, tr("Missing ETag from server"));
        return;
    }
    finalize();
}

void PropagateUploadFileNG::slotUploadProgress(qint64 sent, qint64 total)
{
    // Completion is signaled with sent=0, total=0; avoid accidentally
    // resetting progress due to sent bytes being zero by ignoring it.
    // finishedSignal() is bound to be emitted soon anyway.
    // See https://bugreports.qt.io/browse/QTBUG-44782.
    if (sent == 0 && total == 0) {
        return;
    }
    // With several chunks in flight the partial bytes of one chunk can't be
    // attributed to a single offset cleanly, so only report smooth intra-chunk
    // progress when a single chunk is uploading; otherwise progress advances in
    // chunk-sized steps from slotPutFinished (reportProgress(_sent)).
    if (_inFlightChunks.size() <= 1) {
        propagator()->reportProgress(*_item, _sent + sent);
    }
}

void PropagateUploadFileNG::abort(PropagatorJob::AbortType abortType)
{
    abortNetworkJobs(
        abortType,
        [abortType](AbstractNetworkJob *job) {
            return abortType != AbortType::Asynchronous || !qobject_cast<MoveJob *>(job);
        });
}

}
