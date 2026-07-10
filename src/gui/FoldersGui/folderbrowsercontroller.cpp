/*
 * Copyright (C) BW-Tech GmbH
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

#include "folderbrowsercontroller.h"

#include "common/pinstate.h"
#include "common/utility.h"
#include "common/vfs.h"
#include "configfile.h"
#include "folder.h"
#include "folderitem.h"
#include "folderman.h"
#include "networkjobs.h"

#include "resources/resources.h"

#include <QCollator>
#include <QLoggingCategory>
#include <QMenu>
#include <QNetworkReply>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QScopedValueRollback>
#include <QStandardItemModel>

#include <functional>

namespace OCC {

Q_LOGGING_CATEGORY(lcFolderBrowser, "gui.accountfolders.browser", QtInfoMsg)

FolderBrowserController::FolderBrowserController(QStandardItemModel *model, QObject *parent)
    : QObject(parent)
    , _model(model)
{
    connect(_model, &QStandardItemModel::rowsInserted, this, &FolderBrowserController::onRowsInserted);
    connect(_model, &QStandardItemModel::itemChanged, this, &FolderBrowserController::onItemChanged);
    connect(FolderMan::instance(), &FolderMan::folderRemoved, this, &FolderBrowserController::onFolderRemoved);

    ConfigFile::setupDefaultExcludeFilePaths(_excludedFiles);
    _excludedFiles.reloadExcludeFiles();
}

int FolderBrowserController::itemKind(const QStandardItem *item)
{
    return item ? item->data(FolderItemRoles::ItemKindRole).toInt() : 0;
}

QList<QStandardItem *> FolderBrowserController::browserChildren(const QStandardItem *parentItem)
{
    QList<QStandardItem *> result;
    for (int row = 0; row < parentItem->rowCount(); ++row) {
        QStandardItem *child = parentItem->child(row, 0);
        if (child && itemKind(child) == static_cast<int>(FolderTreeItemKind::BrowserFolder))
            result.append(child);
    }
    return result;
}

QString FolderBrowserController::folderKey(const Folder *folder)
{
    return QString::fromUtf8(folder->id());
}

void FolderBrowserController::onRowsInserted(const QModelIndex &parent, int first, int last)
{
    // only the top-level folder rows need bootstrapping; the child rows are our own
    if (parent.isValid())
        return;

    for (int row = first; row <= last; ++row) {
        FolderItem *folderItem = dynamic_cast<FolderItem *>(_model->item(row, 0));
        if (!folderItem || !folderItem->folder())
            continue;
        ensureSyncState(folderItem->folder());
        // the placeholder gives the row its expander; the actual listing is fetched lazily on expand
        if (!placeholderChild(folderItem) && browserChildren(folderItem).isEmpty())
            attachPlaceholder(folderItem);
    }

    // a folder list rebuild destroys the browser rows and with them any unapplied checkbox
    // edits; recompute so stored pending state and the apply bar stay in agreement
    recomputePending();
}

void FolderBrowserController::ensureSyncState(Folder *folder)
{
    const QString key = folderKey(folder);
    if (_syncStates.contains(key))
        return;

    FolderSyncState state;
    bool ok = false;
    state.journalBlackList = folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, ok);
    if (!ok) {
        qCWarning(lcFolderBrowser) << "Could not read selective sync list for" << folder->path();
        state.journalBlackList.clear();
    }
    state.pendingBlackList = state.journalBlackList;
    state.vfsAtLoad = folder->virtualFilesEnabled();
    _syncStates.insert(key, state);
}

void FolderBrowserController::onFolderRemoved(const QUuid &accountId, Folder *folder)
{
    Q_UNUSED(accountId);
    if (!folder)
        return;
    _syncStates.remove(folderKey(folder));
    recomputePending();
}

void FolderBrowserController::attachPlaceholder(QStandardItem *parentItem)
{
    QStandardItem *placeholder = new QStandardItem(tr("Loading …"));
    placeholder->setFlags(Qt::ItemIsEnabled);
    placeholder->setData(static_cast<int>(FolderTreeItemKind::BrowserPlaceholder), FolderItemRoles::ItemKindRole);
    // keep placeholders below folder rows and error rows when the model gets (re)sorted
    placeholder->setData(-1, FolderItemRoles::SortPriorityRole);

    QStandardItem *filler = new QStandardItem();
    // enabled (but nothing else) so keyboard navigation in column 1 can step onto the row
    filler->setFlags(Qt::ItemIsEnabled);

    QScopedValueRollback<bool> guard(_updatingModel, true);
    parentItem->appendRow({placeholder, filler});
}

QStandardItem *FolderBrowserController::placeholderChild(const QStandardItem *parentItem) const
{
    for (int row = 0; row < parentItem->rowCount(); ++row) {
        QStandardItem *child = parentItem->child(row, 0);
        if (child && itemKind(child) == static_cast<int>(FolderTreeItemKind::BrowserPlaceholder))
            return child;
    }
    return nullptr;
}

FolderItem *FolderBrowserController::rootItemForFolder(Folder *folder) const
{
    for (int row = 0; row < _model->rowCount(); ++row) {
        FolderItem *folderItem = dynamic_cast<FolderItem *>(_model->item(row, 0));
        if (folderItem && folderItem->folder() == folder)
            return folderItem;
    }
    return nullptr;
}

FolderItem *FolderBrowserController::rootItemForIndex(const QModelIndex &index) const
{
    QModelIndex top = index;
    while (top.parent().isValid())
        top = top.parent();
    return dynamic_cast<FolderItem *>(_model->item(top.row(), 0));
}

QStandardItem *FolderBrowserController::itemForPath(FolderItem *rootItem, const QString &relPath) const
{
    if (relPath.isEmpty())
        return rootItem;

    QStandardItem *current = rootItem;
    const QStringList segments = relPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString accumulated;
    for (const QString &segment : segments) {
        accumulated = accumulated.isEmpty() ? segment : accumulated + QLatin1Char('/') + segment;
        QStandardItem *next = nullptr;
        for (QStandardItem *child : browserChildren(current)) {
            if (child->data(FolderItemRoles::RemotePathRole).toString() == accumulated) {
                next = child;
                break;
            }
        }
        if (!next)
            return nullptr;
        current = next;
    }
    return current;
}

void FolderBrowserController::onItemExpanded(const QModelIndex &index)
{
    if (!index.isValid())
        return;

    FolderItem *rootItem = rootItemForIndex(index);
    if (!rootItem || !rootItem->folder())
        return;
    Folder *folder = rootItem->folder();

    const QModelIndex contentIndex = index.siblingAtColumn(0);
    QStandardItem *item = _model->itemFromIndex(contentIndex);
    if (!item)
        return;

    // run the staleness checks on every expansion in the subtree, not only the root:
    // toggling virtual files invalidates all rows of the folder. When the subtree was
    // rebuilt, the expanded child row is gone - relist the (still expanded) root instead.
    if (maybeRefreshFolderState(folder, rootItem)) {
        requestListing(folder, QString());
        return;
    }

    QString relPath;
    if (item != rootItem) {
        if (itemKind(item) != static_cast<int>(FolderTreeItemKind::BrowserFolder))
            return;
        relPath = item->data(FolderItemRoles::RemotePathRole).toString();
        if (relPath.isEmpty())
            return;
    }

    requestListing(folder, relPath);
}

bool FolderBrowserController::maybeRefreshFolderState(Folder *folder, FolderItem *rootItem)
{
    const QString key = folderKey(folder);
    auto it = _syncStates.find(key);
    if (it == _syncStates.end())
        return false;

    // virtual files may have been toggled since the rows were built; the row mode
    // (checkboxes vs. availability) is then stale, so rebuild the subtree from scratch
    // and reload the journal state
    bool rebuilt = false;
    if (it->vfsAtLoad != folder->virtualFilesEnabled()) {
        {
            QScopedValueRollback<bool> guard(_updatingModel, true);
            for (int row = rootItem->rowCount() - 1; row >= 0; --row) {
                const int kind = itemKind(rootItem->child(row, 0));
                if (kind == static_cast<int>(FolderTreeItemKind::BrowserFolder) || kind == static_cast<int>(FolderTreeItemKind::BrowserPlaceholder))
                    rootItem->removeRow(row);
            }
        }
        attachPlaceholder(rootItem);
        _syncStates.remove(key);
        ensureSyncState(folder);
        // the rebuild dropped any pending edits, make the apply bar reflect that
        recomputePending();
        it = _syncStates.find(key);
        if (it == _syncStates.end())
            return true;
        rebuilt = true;
    }

    // pick up blacklist changes made elsewhere (e.g. the "Manage subfolder sync" modal),
    // but never while the user has unapplied edits here
    if (it->pendingBlackList == it->journalBlackList) {
        bool ok = false;
        const QSet<QString> journal = folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, ok);
        if (ok && journal != it->journalBlackList) {
            it->journalBlackList = journal;
            it->pendingBlackList = journal;
            refreshCheckStates(rootItem, journal);
        }
    }
    return rebuilt;
}

void FolderBrowserController::requestListing(Folder *folder, const QString &relPath)
{
    if (!folder->accountState() || !folder->accountState()->account())
        return;

    const QString jobKey = folderKey(folder) + QLatin1Char('\x1f') + relPath;
    if (_pendingJobs.contains(jobKey))
        return;

    const QString folderPath = Utility::stripTrailingSlash(folder->remotePath());
    const QString propfindPath = relPath.isEmpty() ? folderPath : Utility::concatUrlPathItems({folderPath, relPath});

    auto *job = new PropfindJob(folder->accountState()->account(), folder->webDavUrl(), propfindPath, PropfindJob::Depth::One, this);
    job->setProperties({QByteArrayLiteral("resourcetype"), QByteArrayLiteral("http://owncloud.org/ns:size")});

    _pendingJobs.insert(jobKey);
    // catch-all cleanup; the result handlers below additionally remove the key up front
    // because the job is only destroyed via deleteLater - a relist triggered from inside
    // a result handler (VFS toggled mid-flight) must not be dropped by the dedup check
    connect(job, &QObject::destroyed, this, [this, jobKey] { _pendingJobs.remove(jobKey); });

    QPointer<Folder> folderGuard(folder);
    connect(job, &PropfindJob::directoryListingSubfolders, this, [this, job, jobKey, folderGuard, relPath](const QStringList &subfolders) {
        _pendingJobs.remove(jobKey);
        if (!folderGuard)
            return;
        populateListing(folderGuard, relPath, subfolders, job->sizes());
    });
    connect(job, &PropfindJob::finishedWithError, this, [this, job, jobKey, folderGuard, relPath] {
        _pendingJobs.remove(jobKey);
        if (!folderGuard)
            return;
        const bool notFound = job->reply() && job->reply()->error() == QNetworkReply::ContentNotFoundError;
        onListingError(folderGuard, relPath, notFound);
    });
    job->start();
}

void FolderBrowserController::onListingError(Folder *folder, const QString &parentRelPath, bool notFound)
{
    FolderItem *rootItem = rootItemForFolder(folder);
    if (!rootItem)
        return;
    QStandardItem *parentItem = itemForPath(rootItem, parentRelPath);
    if (!parentItem)
        return;
    QStandardItem *placeholder = placeholderChild(parentItem);
    if (!placeholder)
        return;

    QScopedValueRollback<bool> guard(_updatingModel, true);
    // keep the placeholder so collapsing and re-expanding retries the listing
    placeholder->setText(notFound ? tr("Currently there are no subfolders on the server.")
                                  : tr("An error occurred while loading the list of subfolders."));
}

void FolderBrowserController::populateListing(Folder *folder, const QString &parentRelPath, const QStringList &subfolders, const QHash<QString, qint64> &sizes)
{
    FolderItem *rootItem = rootItemForFolder(folder);
    if (!rootItem)
        return;

    // the listing is async: virtual files may have been toggled while it was in flight,
    // in which case these results describe rows of the wrong mode - rebuild and relist
    if (maybeRefreshFolderState(folder, rootItem)) {
        requestListing(folder, QString());
        return;
    }

    QStandardItem *parentItem = itemForPath(rootItem, parentRelPath);
    if (!parentItem)
        return;

    const QString key = folderKey(folder);
    ensureSyncState(folder);
    FolderSyncState &syncState = _syncStates[key];

    const QString folderPath = Utility::stripTrailingSlash(folder->remotePath());
    const QString rootPath = Utility::ensureTrailingSlash(Utility::concatUrlPath(folder->webDavUrl(), folderPath).path());
    const bool ignoreHiddenFiles = FolderMan::instance()->ignoreHiddenFiles();
    const bool vfsMode = folder->virtualFilesEnabled();

    // collect the direct children of parentRelPath from the PROPFIND result
    const QString parentPrefix = parentRelPath.isEmpty() ? QString() : Utility::ensureTrailingSlash(parentRelPath);
    struct Entry
    {
        QString relPath;
        qint64 size;
    };
    QHash<QString, Entry> entriesByName;
    QStringList names;
    for (const QString &absPath : subfolders) {
        if (!absPath.startsWith(rootPath))
            continue;
        if (_excludedFiles.isExcludedRemote(absPath, rootPath, ignoreHiddenFiles, ItemTypeDirectory))
            continue;
        const QString relPath = Utility::stripTrailingSlash(absPath.mid(rootPath.size()));
        if (relPath.isEmpty() || !relPath.startsWith(parentPrefix))
            continue;
        const QString name = relPath.mid(parentPrefix.size());
        if (name.isEmpty() || name.contains(QLatin1Char('/')))
            continue;
        entriesByName.insert(name, {relPath, sizes.value(Utility::ensureTrailingSlash(absPath), -1)});
        names.append(name);
    }
    Utility::sortFilenames(names);

    // a blacklist of just "/" means "everything deselected"; concretize it to the actual
    // top-level folders as soon as they are known, exactly like the selective sync dialog
    if (parentRelPath.isEmpty() && syncState.journalBlackList.contains(QStringLiteral("/"))) {
        const bool wasClean = syncState.pendingBlackList == syncState.journalBlackList;
        syncState.journalBlackList.clear();
        for (const QString &name : std::as_const(names))
            syncState.journalBlackList.insert(Utility::ensureTrailingSlash(entriesByName.value(name).relPath));
        if (wasClean)
            syncState.pendingBlackList = syncState.journalBlackList;
    }

    QScopedValueRollback<bool> guard(_updatingModel, true);

    // same collation as Utility::sortFilenames, for ordered insertion among existing rows
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);

    // index what is already in the tree below this parent
    QHash<QString, QStandardItem *> existing;
    for (QStandardItem *child : browserChildren(parentItem))
        existing.insert(child->data(FolderItemRoles::RemotePathRole).toString(), child);

    for (const QString &name : std::as_const(names)) {
        const Entry entry = entriesByName.value(name);
        QStandardItem *child = existing.take(entry.relPath);
        if (!child) {
            child = new QStandardItem(name);
            child->setIcon(Resources::getCoreIcon(QStringLiteral("folder-sync")));
            child->setToolTip(entry.relPath);
            child->setData(static_cast<int>(FolderTreeItemKind::BrowserFolder), FolderItemRoles::ItemKindRole);
            child->setData(entry.relPath, FolderItemRoles::RemotePathRole);
            child->setData(0, FolderItemRoles::SortPriorityRole);

            Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
            if (!vfsMode)
                flags |= Qt::ItemIsUserCheckable;
            child->setFlags(flags);
            if (!vfsMode)
                child->setCheckState(stateForPath(syncState.pendingBlackList, Utility::ensureTrailingSlash(entry.relPath)));

            QStandardItem *filler = new QStandardItem();
            // enabled (but nothing else) so keyboard navigation in column 1 can step onto the row
            filler->setFlags(Qt::ItemIsEnabled);

            // keep the rows in filename order (same collation as Utility::sortFilenames):
            // insert before the first browser row that should come later (error rows at
            // the top and the placeholder stay put)
            int insertRow = parentItem->rowCount();
            for (int row = 0; row < parentItem->rowCount(); ++row) {
                QStandardItem *sibling = parentItem->child(row, 0);
                const int kind = itemKind(sibling);
                if (kind == static_cast<int>(FolderTreeItemKind::BrowserPlaceholder)
                    || (kind == static_cast<int>(FolderTreeItemKind::BrowserFolder) && collator.compare(sibling->text(), name) > 0)) {
                    insertRow = row;
                    break;
                }
            }
            parentItem->insertRow(insertRow, {child, filler});
            // give the new row its own expander; a Depth::One listing cannot tell
            // whether the subfolder has children, so assume it does until expanded
            attachPlaceholder(child);
        } else if (!placeholderChild(child) && browserChildren(child).isEmpty()) {
            // the row was previously listed as empty and lost its placeholder; restore the
            // expander so subfolders created on the server later remain discoverable
            attachPlaceholder(child);
        }
        setBrowserRowDetails(folder, child, entry.relPath, entry.size, vfsMode);
    }

    // prune rows for folders that no longer exist on the server
    for (QStandardItem *stale : std::as_const(existing))
        parentItem->removeRow(stale->row());

    if (QStandardItem *placeholder = placeholderChild(parentItem)) {
        if (names.isEmpty() && parentItem == rootItem) {
            // keep the top-level placeholder as an informational row, otherwise the
            // folder row would permanently lose its expander
            placeholder->setText(tr("Currently there are no subfolders on the server."));
        } else {
            parentItem->removeRow(placeholder->row());
        }
    }
}

void FolderBrowserController::setBrowserRowDetails(Folder *folder, QStandardItem *item, const QString &relPath, qint64 size, bool vfsMode)
{
    QString detail;
    if (vfsMode)
        detail = availabilityText(folder, relPath);
    else if (size >= 0)
        detail = Utility::octetsToString(size);
    item->setData(detail, FolderItemRoles::DetailStringRole);

    //: Accessible text for a remote folder row, %1 is the folder name, %2 its size or availability
    const QString accessible = detail.isEmpty() ? item->text() : tr("Folder %1, %2").arg(item->text(), detail);
    item->setData(accessible, Qt::AccessibleTextRole);
}

QString FolderBrowserController::availabilityText(Folder *folder, const QString &relPath) const
{
    const auto availability = folder->vfs().availability(relPath);
    return availability ? Utility::enumToDisplayName(*availability) : QString();
}

Qt::CheckState FolderBrowserController::stateForPath(const QSet<QString> &blackList, const QString &pathWithSlash)
{
    for (const QString &entry : blackList) {
        // the entry blacklists this folder itself or one of its ancestors
        if (entry == QLatin1String("/") || pathWithSlash == entry || pathWithSlash.startsWith(entry))
            return Qt::Unchecked;
    }
    for (const QString &entry : blackList) {
        if (entry.startsWith(pathWithSlash))
            return Qt::PartiallyChecked;
    }
    return Qt::Checked;
}

void FolderBrowserController::onItemChanged(QStandardItem *item)
{
    if (_updatingModel || !item)
        return;
    if (itemKind(item) != static_cast<int>(FolderTreeItemKind::BrowserFolder) || !(item->flags() & Qt::ItemIsUserCheckable))
        return;

    propagateCheckChange(item);

    // only the edited folder's pending list is recomputed from its rows; the unloaded-subtree
    // fallback uses the previous pending list because that is what seeded the check states
    FolderItem *rootItem = rootItemForIndex(item->index());
    if (rootItem && rootItem->folder()) {
        auto it = _syncStates.find(folderKey(rootItem->folder()));
        if (it != _syncStates.end())
            it->pendingBlackList = computeBlackList(rootItem, it->pendingBlackList);
    }
    recomputePending();
}

void FolderBrowserController::propagateCheckChange(QStandardItem *item)
{
    QScopedValueRollback<bool> guard(_updatingModel, true);

    // downwards: a decisive state wins over whatever the loaded descendants had
    const Qt::CheckState state = item->checkState();
    if (state == Qt::Checked || state == Qt::Unchecked) {
        std::function<void(QStandardItem *)> setDescendants = [&](QStandardItem *parent) {
            for (QStandardItem *child : browserChildren(parent)) {
                if (child->checkState() != state)
                    child->setCheckState(state);
                setDescendants(child);
            }
        };
        setDescendants(item);
    }

    // upwards: a parent is fully checked only if all children are; it never becomes
    // unchecked automatically because the parent folder itself stays synced
    QStandardItem *parent = item->parent();
    while (parent && itemKind(parent) == static_cast<int>(FolderTreeItemKind::BrowserFolder) && (parent->flags() & Qt::ItemIsUserCheckable)) {
        bool allChecked = true;
        for (QStandardItem *child : browserChildren(parent)) {
            if (child->checkState() != Qt::Checked) {
                allChecked = false;
                break;
            }
        }
        const Qt::CheckState parentState = allChecked ? Qt::Checked : Qt::PartiallyChecked;
        if (parent->checkState() != parentState)
            parent->setCheckState(parentState);
        parent = parent->parent();
    }
}

QSet<QString> FolderBrowserController::computeBlackList(const QStandardItem *parentItem, const QSet<QString> &previousBlackList) const
{
    QSet<QString> result;
    for (QStandardItem *child : browserChildren(parentItem)) {
        const QString path = Utility::ensureTrailingSlash(child->data(FolderItemRoles::RemotePathRole).toString());
        switch (child->checkState()) {
        case Qt::Unchecked:
            result.insert(path);
            break;
        case Qt::Checked:
            break;
        case Qt::PartiallyChecked: {
            if (!browserChildren(child).isEmpty()) {
                result += computeBlackList(child, previousBlackList);
            } else {
                // subtree not loaded from the server; the previous list (which seeded
                // the check states) stays authoritative below this folder
                for (const QString &entry : previousBlackList) {
                    if (entry.startsWith(path))
                        result.insert(entry);
                }
            }
            break;
        }
        }
    }
    return result;
}

void FolderBrowserController::recomputePending()
{
    // the pending lists themselves are maintained in onItemChanged; this only decides
    // whether the apply bar shows, and drops edits the user can no longer see or change
    bool anyDirty = false;
    for (int row = 0; row < _model->rowCount(); ++row) {
        FolderItem *rootItem = dynamic_cast<FolderItem *>(_model->item(row, 0));
        if (!rootItem || !rootItem->folder())
            continue;
        Folder *folder = rootItem->folder();
        auto it = _syncStates.find(folderKey(folder));
        if (it == _syncStates.end())
            continue;
        if (folder->virtualFilesEnabled() || browserChildren(rootItem).isEmpty()) {
            // the rows are gone (folder list rebuild) or the folder switched to virtual
            // files: the edits have no visible representation anymore, discard them
            // instead of silently committing them on the next apply
            it->pendingBlackList = it->journalBlackList;
            continue;
        }
        if (it->pendingBlackList != it->journalBlackList)
            anyDirty = true;
    }
    emit selectiveSyncPendingChanged(anyDirty);
}

void FolderBrowserController::applyPendingChanges()
{
    for (int row = 0; row < _model->rowCount(); ++row) {
        FolderItem *rootItem = dynamic_cast<FolderItem *>(_model->item(row, 0));
        if (!rootItem || !rootItem->folder())
            continue;
        Folder *folder = rootItem->folder();
        auto it = _syncStates.find(folderKey(folder));
        if (it == _syncStates.end() || it->pendingBlackList == it->journalBlackList)
            continue;
        if (folder->virtualFilesEnabled()) {
            // selective sync does not apply under virtual files; stale edits from before
            // a mode switch must never reach the journal
            it->pendingBlackList = it->journalBlackList;
            continue;
        }

        // the journal can have changed underneath us (the "Manage subfolder sync" modal,
        // remote renames of blacklisted folders, ...). Never write our snapshot wholesale:
        // re-read the journal and apply only the user's delta on top of it.
        bool ok = false;
        QSet<QString> merged = folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, ok);
        if (!ok) {
            qCWarning(lcFolderBrowser) << "Could not read selective sync list for" << folder->path() << "- not applying changes";
            continue;
        }
        const QSet<QString> added = it->pendingBlackList - it->journalBlackList;
        const QSet<QString> removed = it->journalBlackList - it->pendingBlackList;
        for (const QString &entry : removed)
            merged.remove(entry);
        for (const QString &entry : added) {
            // entries below a newly blacklisted folder are redundant
            for (auto m = merged.begin(); m != merged.end();) {
                if (m->startsWith(entry))
                    m = merged.erase(m);
                else
                    ++m;
            }
        }
        for (const QString &entry : added) {
            bool coveredByAncestor = false;
            for (const QString &m : std::as_const(merged)) {
                if (entry != m && entry.startsWith(m)) {
                    coveredByAncestor = true;
                    break;
                }
            }
            if (!coveredByAncestor)
                merged.insert(entry);
        }

        qCInfo(lcFolderBrowser) << "Applying selective sync changes for" << folder->path() << "- excluding" << added << "- re-including" << removed;
        folder->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, merged);
        it->journalBlackList = merged;
        it->pendingBlackList = merged;
        // reflect entries that were changed externally in the loaded rows too
        refreshCheckStates(rootItem, merged);
        FolderMan::instance()->forceFolderSync(folder);
    }
    // not a plain 'false': a folder whose journal read failed above keeps its pending
    // edits, and the apply bar must keep showing them
    recomputePending();
}

void FolderBrowserController::discardPendingChanges()
{
    for (int row = 0; row < _model->rowCount(); ++row) {
        FolderItem *rootItem = dynamic_cast<FolderItem *>(_model->item(row, 0));
        if (!rootItem || !rootItem->folder())
            continue;
        auto it = _syncStates.find(folderKey(rootItem->folder()));
        if (it == _syncStates.end())
            continue;
        if (it->pendingBlackList != it->journalBlackList) {
            it->pendingBlackList = it->journalBlackList;
            refreshCheckStates(rootItem, it->journalBlackList);
        }
    }
    emit selectiveSyncPendingChanged(false);
}

void FolderBrowserController::refreshCheckStates(QStandardItem *parentItem, const QSet<QString> &blackList)
{
    QScopedValueRollback<bool> guard(_updatingModel, true);
    std::function<void(QStandardItem *)> refresh = [&](QStandardItem *parent) {
        for (QStandardItem *child : browserChildren(parent)) {
            if (child->flags() & Qt::ItemIsUserCheckable) {
                const Qt::CheckState state = stateForPath(blackList, Utility::ensureTrailingSlash(child->data(FolderItemRoles::RemotePathRole).toString()));
                if (child->checkState() != state)
                    child->setCheckState(state);
            }
            refresh(child);
        }
    };
    refresh(parentItem);
}

void FolderBrowserController::refreshAvailability(Folder *folder, QStandardItem *item, bool recursive)
{
    QScopedValueRollback<bool> guard(_updatingModel, true);
    const QString relPath = item->data(FolderItemRoles::RemotePathRole).toString();
    setBrowserRowDetails(folder, item, relPath, -1, true);
    if (recursive) {
        for (QStandardItem *child : browserChildren(item))
            refreshAvailability(folder, child, true);
    }
}

void FolderBrowserController::popAvailabilityMenu(const QModelIndex &index, const QPoint &globalPos, QWidget *menuParent)
{
    FolderItem *rootItem = rootItemForIndex(index);
    if (!rootItem || !rootItem->folder())
        return;
    if (!rootItem->folder()->virtualFilesEnabled())
        return;

    QStandardItem *item = _model->itemFromIndex(index.siblingAtColumn(0));
    if (!item || itemKind(item) != static_cast<int>(FolderTreeItemKind::BrowserFolder))
        return;

    // menu.exec() spins a nested event loop: the folder can be removed and the row can be
    // pruned by an in-flight listing while the menu is open, so guard and re-resolve after
    QPointer<Folder> folder(rootItem->folder());
    const QPersistentModelIndex persistentIndex(index.siblingAtColumn(0));

    QMenu menu(menuParent);
    QAction *keepLocal = menu.addAction(tr("Always keep on this device"));
    QAction *freeUp = menu.addAction(tr("Free up space (online only)"));
    menu.addSeparator();
    QAction *reset = menu.addAction(tr("Reset to default"));

    QAction *chosen = menu.exec(globalPos);
    PinState state;
    if (chosen == keepLocal)
        state = PinState::AlwaysLocal;
    else if (chosen == freeUp)
        state = PinState::OnlineOnly;
    else if (chosen == reset)
        state = PinState::Inherited;
    else
        return;

    if (!folder || !folder->virtualFilesEnabled() || !persistentIndex.isValid())
        return;
    item = _model->itemFromIndex(QModelIndex(persistentIndex));
    if (!item || itemKind(item) != static_cast<int>(FolderTreeItemKind::BrowserFolder))
        return;

    const QString relPath = item->data(FolderItemRoles::RemotePathRole).toString();
    if (!folder->vfs().setPinState(relPath, state)) {
        qCWarning(lcFolderBrowser) << "Failed to set pin state" << static_cast<int>(state) << "for" << relPath;
        return;
    }
    refreshAvailability(folder, item, true);
}
}
