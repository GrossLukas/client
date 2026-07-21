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

class FolderMan;

/**
 * @brief Pins classic (non-VFS) sync folders to the Windows Explorer
 * navigation pane, the way OneDrive appears there.
 *
 * Virtual-file (Cloud Files API) folders already get their navigation-pane
 * entry from the StorageProviderSyncRootManager registration and are
 * therefore skipped here. All registry entries live under HKEY_CURRENT_USER,
 * so no elevation is needed and every Windows user manages their own pins.
 * On other platforms this class compiles to a no-op.
 *
 * @ingroup gui
 */
class NavigationPaneHelper : public QObject
{
    Q_OBJECT
public:
    explicit NavigationPaneHelper(FolderMan *folderMan);

    bool showInExplorerNavigationPane() const { return _showInExplorerNavigationPane; }
    void setShowInExplorerNavigationPane(bool show);

    /** Coalesces bursts of folder changes into one registry update. */
    void scheduleUpdateCloudStorageRegistry();

private:
    void updateCloudStorageRegistry();

    FolderMan *_folderMan;
    bool _showInExplorerNavigationPane;
    QTimer _updateCloudStorageRegistryTimer;
};

} // namespace OCC
