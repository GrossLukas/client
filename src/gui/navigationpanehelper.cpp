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

#include "navigationpanehelper.h"

#include "configfile.h"
#include "folder.h"
#include "folderman.h"
#include "theme.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSettings>
#include <QStringList>
#include <QUuid>
#include <QVector>

namespace OCC {

Q_LOGGING_CATEGORY(lcNavPane, "gui.navigationpane", QtInfoMsg)

#ifdef Q_OS_WIN
namespace {
    // Fixed namespace for deriving one stable CLSID per local folder path, so
    // the same folder keeps the same navigation-pane entry across restarts
    // without having to persist anything.
    const QUuid navPaneNamespaceUuid("{d5e56b58-4b3a-45ac-9c1e-7a0b1c6e9f42}");

    // The pane entry delegates to the standard folder shortcut implementation.
    const QString delegateFolderClsid = QStringLiteral("{0E5AAE11-A475-4C5B-AB00-C66DE400274E}");
}
#endif

NavigationPaneHelper::NavigationPaneHelper(FolderMan *folderMan)
    : QObject(nullptr) // owned via unique_ptr in FolderMan, not via QObject parenting
    , _folderMan(folderMan)
{
    ConfigFile cfg;
    _showInExplorerNavigationPane = cfg.showInExplorerNavigationPane();

    _updateCloudStorageRegistryTimer.setSingleShot(true);
    _updateCloudStorageRegistryTimer.setInterval(500);
    connect(&_updateCloudStorageRegistryTimer, &QTimer::timeout,
        this, &NavigationPaneHelper::updateCloudStorageRegistry);

    connect(_folderMan, &FolderMan::folderAdded, this,
        [this](const QUuid &, Folder *) { scheduleUpdateCloudStorageRegistry(); });
    connect(_folderMan, &FolderMan::folderRemoved, this,
        [this](const QUuid &, Folder *) { scheduleUpdateCloudStorageRegistry(); });
    connect(_folderMan, &FolderMan::folderListChanged, this,
        [this](const QUuid &, const QList<Folder *> &) { scheduleUpdateCloudStorageRegistry(); });

    // Reconcile at startup: folders may have been removed, or the setting
    // toggled, while the client was not running.
    scheduleUpdateCloudStorageRegistry();
}

void NavigationPaneHelper::setShowInExplorerNavigationPane(bool show)
{
    if (_showInExplorerNavigationPane == show)
        return;

    _showInExplorerNavigationPane = show;
    ConfigFile cfg;
    cfg.setShowInExplorerNavigationPane(show);
    scheduleUpdateCloudStorageRegistry();
}

void NavigationPaneHelper::scheduleUpdateCloudStorageRegistry()
{
    _updateCloudStorageRegistryTimer.start();
}

void NavigationPaneHelper::updateCloudStorageRegistry()
{
#ifdef Q_OS_WIN
    const QString appName = Theme::instance()->appNameGUI();

    // Desired state: one entry per classic sync folder. Virtual-file folders
    // are pinned by their Cloud Files API sync-root registration already.
    struct Entry {
        QString clsid;
        QString title;
        QString path;
    };
    QVector<Entry> desired;
    if (_showInExplorerNavigationPane) {
        const auto folders = _folderMan->folders();
        for (Folder *folder : folders) {
            if (folder->virtualFilesEnabled())
                continue;
            const QString cleanedPath = QDir::cleanPath(folder->path());
            Entry entry;
            entry.clsid = QUuid::createUuidV5(navPaneNamespaceUuid, cleanedPath)
                              .toString(QUuid::WithBraces);
            entry.title = folders.size() == 1
                ? appName
                : appName + QStringLiteral(" - ") + folder->shortGuiLocalPath();
            entry.path = QDir::toNativeSeparators(cleanedPath);
            desired.append(entry);
        }
    }

    // Icon: the branded .ico shipped next to the install root if available,
    // otherwise the icon embedded in the client binary.
    const QString exePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString brandedIco =
        QFileInfo(QCoreApplication::applicationDirPath() + QStringLiteral("/../owncloud.ico"))
            .canonicalFilePath();
    const QString iconPath =
        brandedIco.isEmpty() ? exePath + QStringLiteral(",0") : QDir::toNativeSeparators(brandedIco);

    const QString clsidRootKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\CLSID");
    const QString namespaceRootKey = QStringLiteral(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Desktop\\NameSpace");
    const QString newStartPanelKey = QStringLiteral(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\NewStartPanel");

    // Remove entries we created earlier (recognized by the ApplicationName
    // marker) that are no longer wanted.
    QStringList desiredClsids;
    for (const Entry &entry : desired)
        desiredClsids.append(entry.clsid);

    QSettings clsidRoot(clsidRootKey, QSettings::NativeFormat);
    const QStringList existing = clsidRoot.childGroups();
    for (const QString &group : existing) {
        if (clsidRoot.value(group + QStringLiteral("/ApplicationName")).toString() != appName)
            continue;
        if (desiredClsids.contains(group, Qt::CaseInsensitive))
            continue;
        qCInfo(lcNavPane) << "Removing stale navigation pane entry" << group;
        clsidRoot.remove(group);
        QSettings namespaceRoot(namespaceRootKey, QSettings::NativeFormat);
        namespaceRoot.remove(group);
        QSettings newStartPanel(newStartPanelKey, QSettings::NativeFormat);
        newStartPanel.remove(group);
    }

    // (Re-)create the wanted entries. Overwriting existing values is fine and
    // keeps titles/paths current after folder moves or renames.
    for (const Entry &entry : desired) {
        qCInfo(lcNavPane) << "Pinning" << entry.path << "to the navigation pane as" << entry.title;

        QSettings clsid(clsidRootKey + QLatin1Char('\\') + entry.clsid, QSettings::NativeFormat);
        clsid.setValue(QStringLiteral("."), entry.title);
        clsid.setValue(QStringLiteral("ApplicationName"), appName);
        clsid.setValue(QStringLiteral("DefaultIcon/."), iconPath);
        clsid.setValue(QStringLiteral("InProcServer32/."),
            qEnvironmentVariable("systemroot") + QStringLiteral("\\system32\\shell32.dll"));
        clsid.setValue(QStringLiteral("Instance/CLSID"), delegateFolderClsid);
        clsid.setValue(QStringLiteral("Instance/InitPropertyBag/Attributes"), quint32(0x11));
        clsid.setValue(QStringLiteral("Instance/InitPropertyBag/TargetFolderPath"), entry.path);
        clsid.setValue(QStringLiteral("ShellFolder/FolderValueFlags"), quint32(0x28));
        clsid.setValue(QStringLiteral("ShellFolder/Attributes"), quint32(0xF080004D));
        clsid.setValue(QStringLiteral("System.IsPinnedToNameSpaceTree"), quint32(0x1));
        clsid.setValue(QStringLiteral("SortOrderIndex"), quint32(0x41));

        QSettings namespaceEntry(
            namespaceRootKey + QLatin1Char('\\') + entry.clsid, QSettings::NativeFormat);
        namespaceEntry.setValue(QStringLiteral("."), entry.title);

        QSettings newStartPanel(newStartPanelKey, QSettings::NativeFormat);
        newStartPanel.setValue(entry.clsid, quint32(1));
    }
#endif // Q_OS_WIN
}

} // namespace OCC
