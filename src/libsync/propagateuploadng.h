/*
 * Copyright (C) by Olivier Goffart <ogoffart@owncloud.com>
 *
 * Modified by BW-Tech GmbH for owncloud.online: re-introduced the ownCloud 10
 * "new" (NG) chunked upload algorithm that upstream dropped in DC-54, so large
 * files can be uploaded in resumable chunks against owncloud.online (11.0.x)
 * servers, which advertise dav.chunking 1.0 but no TUS support.
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

#include "propagateuploadfile.h"
#include "uploaddevice.h"

#include <QHash>
#include <QMap>
#include <QVector>

namespace OCC {
Q_DECLARE_LOGGING_CATEGORY(lcPropagateUploadNG)

class PUTFileJob;

/**
 * @ingroup libsync
 *
 * Propagation job, implementing the ownCloud 10 "new" chunking algorithm
 * (dav.chunking 1.0): chunks are PUT to /remote.php/dav/uploads/<user>/<id>/<offset>
 * and assembled with a final MOVE of the virtual ".file". Supports resume via a
 * PROPFIND of already-uploaded chunks and optional dynamic chunk sizing.
 */
class PropagateUploadFileNG : public PropagateUploadCommon
{
    Q_OBJECT
private:
    /** Amount of data that was already sent in bytes.
     *
     * If this job is resuming an upload, this number includes bytes that were
     * sent in previous jobs.
     */
    qint64 _sent = 0;

    /** Amount of data that needs to be sent to the server in bytes.
     *
     * For normal uploads this will be the file size.
     *
     * This value is intended to be comparable to _sent: it's always the total
     * amount of data that needs to be present at the server to finish the upload -
     * regardless of whether previous jobs have already sent something.
     */
    qint64 _bytesToUpload;

    uint _transferId = 0; /// transfer id (part of the url)
    qint64 _currentChunkOffset = 0; /// byte offset of the next chunk data that will be sent
    qint64 _currentChunkSize = 0; /// current chunk size
    bool _removeJobError = false; /// if not null, there was an error removing the job

    // Map chunk number with its size  from the PROPFIND on resume.
    // (Only used from slotPropfindIterate/slotPropfindFinished because the PropfindJob use signals to report data.)
    struct ServerChunkInfo
    {
        qint64 size;
        QString originalName;
    };
    QMap<qint64, ServerChunkInfo> _serverChunks;

    // Vector with expected PUT ranges.
    struct UploadRangeInfo
    {
        qint64 start;
        qint64 size;
        qint64 end() const { return start + size; }
    };
    QVector<UploadRangeInfo> _rangesToUpload;

    /** Maximum number of chunk PUTs of the SAME file in flight at once.
     * The server accepts independent chunk uploads to different offsets, so
     * uploading them in parallel speeds up single large files on high-latency
     * links. Kept modest to avoid opening too many connections per file. */
    int _maxParallelChunks = 3;

    /** Chunk PUTs currently in flight, mapping the job to the byte size of its
     * chunk (used for progress/sent accounting once the chunk completes). */
    QHash<PUTFileJob *, qint64> _inFlightChunks;

    /**
     * Return the path of a chunk.
     * If chunkOffset == -1, returns the URL of the parent folder containing the chunks
     */
    QString chunkPath(qint64 chunkOffset = -1);

    /**
     * Finds the range starting at 'start' in _rangesToUpload and removes the first
     * 'size' bytes from it. If it becomes empty, remove the range.
     *
     * Returns false if no matching range was found.
     */
    bool markRangeAsDone(qint64 start, qint64 size);

public:
    PropagateUploadFileNG(OwncloudPropagator *propagator, const SyncFileItemPtr &item);
    void doStartUpload() override;

private:
    void doStartUploadNext();
    void startNewUpload();
    void scheduleChunks();
    void doFinalMove();
public Q_SLOTS:
    void abort(PropagatorJob::AbortType abortType) override;
private Q_SLOTS:
    void slotPropfindFinished();
    void slotPropfindFinishedWithError();
    void slotPropfindIterate(const QString &name, const QMap<QString, QString> &properties);
    void slotDeleteJobFinished();
    void slotMkColFinished();
    void slotPutFinished();
    void slotMoveJobFinished();
    void slotUploadProgress(qint64, qint64);
};
}
