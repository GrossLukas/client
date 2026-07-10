/*
 * Copyright (C) Lisa Reese <lisa.reese@kiteworks.com>
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

#include <QWidget>

class QStandardItemModel;
class QItemSelectionModel;
class QModelIndex;
class QTreeView;
class QLabel;
class QPushButton;
class QMenu;

namespace OCC {

class AccountFoldersView : public QWidget
{
    Q_OBJECT
public:
    explicit AccountFoldersView(QWidget *parent = nullptr);

    void setItemModels(QStandardItemModel *model, QItemSelectionModel *selectionModel);
    void setFolderActions(QList<QAction *> actions);
    void setSyncedFolderCount(int synced, int total);
    void enableAddFolder(bool enableAdd);
    void setMenuActions(QList<QAction *> actions);

    /// shows/hides the apply bar for pending selective sync changes made in the folder browser rows
    void setSelectiveSyncPending(bool pending);

signals:
    void addFolderTriggered();
    void requestActionsUpdate();
    /// a row of the folder tree was expanded (folder rows and remote file browser rows)
    void itemExpanded(const QModelIndex &index);
    void applySelectiveSyncRequested();
    void discardSelectiveSyncRequested();
    /// context menu requested on a remote file browser row
    void browserMenuRequested(const QModelIndex &index, const QPoint &globalPos);

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    void buildView();
    void popItemMenu(const QPoint &pos);
    void refreshMenu();
    bool performBizarreSetupOnTreeView();

    QTreeView *_treeView = nullptr;
    QLabel *_syncedFolderCountLabel = nullptr;
    QPushButton *_addFolderButton = nullptr;
    QMenu *_itemMenu = nullptr;
    QWidget *_selectiveSyncApplyBar = nullptr;
    bool _firstShowAfterCreation = true;
};
}
