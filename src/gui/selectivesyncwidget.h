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

#include <QPointer>
#include <QUrl>
#include <QWidget>

#include "common/pinstate.h"
#include "csync_exclude.h"

class QTreeWidgetItem;
class QTreeWidget;
class QNetworkReply;
class QLabel;
namespace OCC {

class Folder;
class Account;

/**
 * @brief The SelectiveSyncWidget contains a folder tree with labels
 * @ingroup gui
 */
class SelectiveSyncWidget : public QWidget
{
    Q_OBJECT
public:
    explicit SelectiveSyncWidget(Account *account, QWidget *parent);

    /// Returns a list of blacklisted paths, each including the trailing /
    QSet<QString> createBlackList(QTreeWidgetItem *root = nullptr) const;

    // Estimates the total size of checked items (recursively)
    qint64 estimatedSize(QTreeWidgetItem *root = nullptr);

    // oldBlackList is a list of excluded paths, each including a trailing /
    void setFolderInfo(const QString &folderPath, const QString &rootName, const QSet<QString> &oldBlackList = {});

    QSize sizeHint() const override;

    void setDavUrl(const QUrl &davUrl);

    /** Switch the widget into VFS pin-state mode for the given folder.
     *
     * Instead of selective-sync checkboxes, the tree stays fully browsable and
     * right-clicking a row lets the user change the availability of that folder
     * (keep always on this device / free up space, i.e. online-only). This is the
     * in-client equivalent of the Explorer/Finder "Always keep on this device" /
     * "Free up space" context menu and works with any VFS backend.
     */
    void setPinStateFolder(Folder *folder);

private Q_SLOTS:
    void slotUpdateDirectories(QStringList);
    void slotItemExpanded(QTreeWidgetItem *);
    void slotItemChanged(QTreeWidgetItem *, int);
    void slotContextMenu(const QPoint &pos);

private:
    void refreshFolders();
    void recursiveInsert(QTreeWidgetItem *parent, QStringList pathTrail, QString path, qint64 size, bool showChildIndicator);
    QUrl davUrl() const;

    bool pinStateMode() const { return _pinStateFolder; }
    void applyPinState(QTreeWidgetItem *item, PinState state);
    void refreshAvailability(QTreeWidgetItem *item);

private:
    QPointer<Account> _account;
    QPointer<Folder> _pinStateFolder;

    QString _folderPath;
    QString _rootName;
    QSet<QString> _oldBlackList;

    QUrl _davUrl;

    bool _inserting; // set to true when we are inserting new items on the list
    QLabel *_loading;
    QLabel *_header = nullptr;

    QTreeWidget *_folderTree;

    // During account setup we want to filter out excluded folders from the
    // view without having a Folder.SyncEngine.ExcludedFiles instance.
    ExcludedFiles _excludedFiles;
};

}
