/*
 * Copyright (C) by Olivier Goffart <ogoffart@woboq.com>
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

#include "owncloudlib.h"
#include "common/filesystembase.h"
#include "common/vfs.h"

#include <QRegularExpression>
#include <QSharedPointer>
#include <QString>

#include <chrono>


namespace OCC {

/**
 * Value class containing the options given to the sync engine
 */
class OWNCLOUDSYNC_EXPORT SyncOptions
{
public:
    explicit SyncOptions(QSharedPointer<Vfs> vfs);
    ~SyncOptions();

    /** If remotely deleted files are needed to move to trash */
    bool _moveFilesToTrash = false;

    /** Size limit in bytes above which a NEW remote folder needs user confirmation
     * before it is synced. Negative = feature disabled. Only used while virtual
     * files are off. */
    qint64 _newBigFolderSizeLimit = -1;

    /** Ask for confirmation before syncing folders from external storages. Only
     * used while virtual files are off. */
    bool _confirmExternalStorage = false;

    /** Create a virtual file for new files instead of downloading. May not be null */
    QSharedPointer<Vfs> _vfs;

    /** The initial un-adjusted chunk size in bytes for chunked uploads, which also
     * classifies an item as "to be chunked" (item size above this value).
     *
     * In chunking-NG, when dynamic chunk size adjustments are done, this is the
     * starting value and is then gradually adjusted within the
     * _minChunkSize / _maxChunkSize bounds.
     */
    qint64 _initialChunkSize = 10 * 1000 * 1000; // 10MB

    /** Size threshold (bytes) above which an upload uses the resumable chunked-NG
     * path instead of a single PUT. Decoupled from _initialChunkSize (the chunk
     * *size*): a file only needs one chunk yet still benefits from resume and from
     * bypassing the server's single-request body limit. Kept equal to the client
     * bulk-upload per-file ceiling (BulkPropagatorJob::maxBulkFileSize = 1 MiB) so
     * there is no gap: new files <= 1 MiB batch via bulk, everything larger (and any
     * overwrite > 1 MiB) chunks resumably, and nothing falls back to a bare PUT. */
    qint64 _chunkUploadThreshold = 1 * 1024 * 1024; // 1 MiB, matches maxBulkFileSize

    /** The minimum chunk size in bytes for chunked uploads */
    qint64 _minChunkSize = 1 * 1000 * 1000; // 1MB

    /** The maximum chunk size in bytes for chunked uploads */
    qint64 _maxChunkSize = 100 * 1000 * 1000; // 100MB

    /** The target duration of chunk uploads for dynamic chunk sizing.
     *
     * Set to 0 it will disable dynamic chunk sizing.
     */
    std::chrono::milliseconds _targetChunkUploadDuration = std::chrono::minutes(1);

    /** The maximum number of active jobs in parallel  */
    int _parallelNetworkJobs = 6;

    /** Reads settings from env vars where available. */
    void fillFromEnvironmentVariables();

    /** Ensure min <= initial <= max.
     *
     * Adjusts _minChunkSize / _maxChunkSize to always include _initialChunkSize,
     * so env overrides can't produce an inconsistent configuration.
     */
    void verifyChunkSizes();

    /** A regular expression to match file names
     * If no pattern is provided the default is an invalid regular expression.
     */
    QRegularExpression fileRegex() const;

    /**
     * A pattern like *.txt, matching only file names
     */
    void setFilePattern(const QString &pattern);

    /**
     * A pattern like /own.*\/.*txt matching the full path
     */
    void setPathPattern(const QString &pattern);


private:
    /**
     * Only sync files that mathc the expression
     * Invalid pattern by default.
     */
    QRegularExpression _fileRegex = QRegularExpression(QStringLiteral("("));
};

}
