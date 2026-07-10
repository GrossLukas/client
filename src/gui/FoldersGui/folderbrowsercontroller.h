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

#pragma once

#include "csync_exclude.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUuid>

class QModelIndex;
class QPoint;
class QStandardItem;
class QStandardItemModel;
class QWidget;

namespace OCC {

class Folder;
class FolderItem;

/**
 * @brief Lets the user browse the remote directory structure directly in the folder list.
 *
 * Every folder row in the AccountFoldersView gets an expander; expanding a row runs a
 * Depth::One PROPFIND and inserts the subfolders as child rows (name + size), which are
 * themselves expandable, so the whole remote tree can be explored inside the client -
 * like the folder tree the classic 2.x client showed inline in its settings page.
 *
 * For folders synced without virtual files the rows carry selective-sync checkboxes.
 * Changes are collected as a pending blacklist per folder and only written to the sync
 * journal (and a sync forced) when the user confirms via the apply bar in the view;
 * discarding restores the journal state. For virtual-files folders the rows show the
 * current availability instead, and a context menu sets the pin state (always keep on
 * this device / free up space), mirroring the Explorer integration.
 */
class FolderBrowserController : public QObject
{
    Q_OBJECT
public:
    explicit FolderBrowserController(QStandardItemModel *model, QObject *parent);

public slots:
    void onItemExpanded(const QModelIndex &index);
    void applyPendingChanges();
    void discardPendingChanges();
    void popAvailabilityMenu(const QModelIndex &index, const QPoint &globalPos, QWidget *menuParent);

signals:
    void selectiveSyncPendingChanged(bool pending);

private slots:
    void onRowsInserted(const QModelIndex &parent, int first, int last);
    void onItemChanged(QStandardItem *item);
    void onFolderRemoved(const QUuid &accountId, OCC::Folder *folder);

private:
    struct FolderSyncState
    {
        QSet<QString> journalBlackList;
        QSet<QString> pendingBlackList;
        bool vfsAtLoad = false;
    };

    void ensureSyncState(Folder *folder);
    void maybeRefreshFolderState(Folder *folder, FolderItem *rootItem);
    void attachPlaceholder(QStandardItem *parentItem);
    void requestListing(Folder *folder, const QString &relPath);
    void populateListing(Folder *folder, const QString &parentRelPath, const QStringList &subfolders, const QHash<QString, qint64> &sizes);
    void onListingError(Folder *folder, const QString &parentRelPath, bool notFound);

    FolderItem *rootItemForFolder(Folder *folder) const;
    FolderItem *rootItemForIndex(const QModelIndex &index) const;
    QStandardItem *itemForPath(FolderItem *rootItem, const QString &relPath) const;
    QStandardItem *placeholderChild(const QStandardItem *parentItem) const;

    static Qt::CheckState stateForPath(const QSet<QString> &blackList, const QString &pathWithSlash);
    void propagateCheckChange(QStandardItem *item);
    QSet<QString> computeBlackList(const QStandardItem *parentItem, const QSet<QString> &journalBlackList) const;
    void recomputePending();
    void refreshCheckStates(QStandardItem *parentItem, const QSet<QString> &blackList);
    void refreshAvailability(Folder *folder, QStandardItem *item, bool recursive);
    QString availabilityText(Folder *folder, const QString &relPath) const;
    void setBrowserRowDetails(Folder *folder, QStandardItem *item, const QString &relPath, qint64 size, bool vfsMode);

    static int itemKind(const QStandardItem *item);
    static QList<QStandardItem *> browserChildren(const QStandardItem *parentItem);
    static QString folderKey(const Folder *folder);

    QStandardItemModel *_model = nullptr;
    // keyed by folderKey(); tracks the journal blacklist and the (possibly dirty) pending edits
    QHash<QString, FolderSyncState> _syncStates;
    // folderKey() + '\x1f' + relPath of PROPFINDs in flight, to avoid duplicate jobs on re-expand
    QSet<QString> _pendingJobs;
    // true while this controller mutates the model, so onItemChanged only reacts to the user
    bool _updatingModel = false;
    ExcludedFiles _excludedFiles;
};
}
