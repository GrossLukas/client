/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
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

#include "owncloudgui.h"
#include "aboutdialog.h"
#include "account.h"
#include "accountmanager.h"
#include "accountstate.h"
#include "application.h"
#include "common/restartmanager.h"
#include "common/syncjournalfilerecord.h"
#include "configfile.h"
#include "folderman.h"
#include "gui/networkinformation.h"
#include "guiutility.h"
#include "libsync/theme.h"
#include "logbrowser.h"
#include "progressdispatcher.h"
#include "settingsdialog.h"
#include "sharedialog.h"

#include "newaccountwizard/newaccountbuilder.h"
#include "newaccountwizard/newaccountmodel.h"
#include "newaccountwizard/newaccountwizard.h"
#include "newaccountwizard/newaccountwizardcontroller.h"

#include "resources/resources.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QMessageBox>
#include <QTimer>

#include <chrono>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace std::chrono_literals;

namespace OCC {

// localized, human-readable label of the overall sync status for the tray
// icon accessible name (tooltip), read out by screen readers (NVDA/Narrator/Orca)
QString trayStatusToString(SyncResult::Status status)
{
    switch (status) {
    case SyncResult::Status::Undefined:
    case SyncResult::Status::Offline:
        return QCoreApplication::translate("ownCloudGui", "Offline");
    case SyncResult::Status::NotYetStarted:
    case SyncResult::Status::SyncPrepare:
    case SyncResult::Status::SyncRunning:
        return QCoreApplication::translate("ownCloudGui", "Syncing");
    case SyncResult::Status::SyncAbortRequested:
        return QCoreApplication::translate("ownCloudGui", "Aborting sync");
    case SyncResult::Status::Success:
        return QCoreApplication::translate("ownCloudGui", "Up to date");
    case SyncResult::Status::Problem:
        return QCoreApplication::translate("ownCloudGui", "Up to date, some files were ignored");
    case SyncResult::Status::Error:
    case SyncResult::Status::SetupError:
        return QCoreApplication::translate("ownCloudGui", "Error");
    case SyncResult::Status::Paused:
        return QCoreApplication::translate("ownCloudGui", "Sync paused");
    case SyncResult::Status::Unavailable:
        return QCoreApplication::translate("ownCloudGui", "Unavailable");
    }
    return QCoreApplication::translate("ownCloudGui", "Offline");
}

// simple helper to compute the overall status for the tray icon
SyncResult::Status trayOverallStatus()
{
    TrayOverallStatusResult result;
    for (auto *folder : FolderMan::instance()->folders()) {
        result.addResult(folder);
    }
    if (result.overallStatus().status() == SyncResult::Undefined) {
        return SyncResult::Offline;
    }
    return result.overallStatus().status();
}

ownCloudGui::ownCloudGui(Application *parent)
    : QObject(parent)
    , _tray(new QSystemTrayIcon(this))
    , _settingsDialog(new SettingsDialog(this))
    , _app(parent)
{
    connect(_tray, &QSystemTrayIcon::activated,
        this, &ownCloudGui::slotTrayClicked);

    // Timer that auto-resumes a timed "pause synchronization" request.
    _pauseSyncTimer = new QTimer(this);
    _pauseSyncTimer->setSingleShot(true);
    connect(_pauseSyncTimer, &QTimer::timeout, this, &ownCloudGui::resumeSync);

    setupTrayContextMenu();

    // init systray
    slotComputeOverallSyncStatus();
    _tray->show();

    // Refactoring todo: we have a very nasty habit of connecting to globally available managers, etc inside constructors or similar.
    // this should only happen when we are dealing with a formal dependency: eg a dep passed by injection and kept as member,
    // or a dep that is instantiated inside the class (also with a member kept)
    // in cases like this one, the external deps should be instantiated externally and connected externally. This is normally part
    // of an app building routine. The global singletons have to go and this is an important step to achieving that.
    FolderMan *folderMan = FolderMan::instance();
    connect(folderMan, &FolderMan::folderSyncStateChange, this, &ownCloudGui::slotSyncStateChange);
}

ownCloudGui::~ownCloudGui()
{
    delete _settingsDialog;
}

// This should rather be in application.... or rather in ConfigFile?
void ownCloudGui::slotOpenSettingsDialog()
{
    // if account is set up, start the configuration wizard.
    if (!AccountManager::instance()->accounts().isEmpty()) {
        if (QApplication::activeWindow() != _settingsDialog) {
            slotShowSettings();
        } else {
            // ????!!!!?????????
            _settingsDialog->close();
        }
    } else {
        qCInfo(lcApplication) << "No configured folders yet, starting setup wizard";
        runAccountWizard();
    }
}

void ownCloudGui::slotTrayClicked(QSystemTrayIcon::ActivationReason reason)
{
    // Left click
    if (reason == QSystemTrayIcon::Trigger) {
#ifdef Q_OS_MAC
        // on macOS, a left click always opens menu.
        // However if the settings dialog is already visible but hidden
        // by other applications, this will bring it to the front.
        if (_settingsDialog->isVisible()) {
            raise();
        }
#else
        slotOpenSettingsDialog();
#endif
    }
}

void ownCloudGui::slotSyncStateChange(Folder *folder)
{
    slotComputeOverallSyncStatus();

    if (!folder) {
        return; // Valid, just a general GUI redraw was needed.
    }

    auto result = folder->syncResult();

    qCInfo(lcApplication) << "Sync state changed for folder " << folder->remoteUrl().toString() << ": " << Utility::enumToDisplayName(result.status());
}

void ownCloudGui::slotTrayMessageIfServerUnsupported(Account *account)
{
    if (account->serverSupportLevel() != Account::ServerSupportLevel::Supported) {
        slotShowTrayMessage(tr("Unsupported Server Version"),
            tr("The server on account %1 runs an unsupported version %2. "
               "Using this client with unsupported server versions is untested and "
               "potentially dangerous. Proceed at your own risk.")
                .arg(account->displayNameWithHost(), account->capabilities().status().versionString()));
    }
}

QIcon ownCloudGui::getTrayStatusIcon(const SyncResult::Status &status) const
{
    auto contextMenuVisible = _tray->contextMenu() && _tray->contextMenu()->isVisible();
    return Theme::instance()->themeTrayIcon(SyncResult{status}, contextMenuVisible);
}

void ownCloudGui::slotComputeOverallSyncStatus()
{
    auto status = trayOverallStatus();
    const QIcon statusIcon = getTrayStatusIcon(status);
    _tray->setIcon(statusIcon);
    // Set a localized, descriptive tooltip so the tray icon exposes an
    // accessible name (status) to screen readers instead of icon-only graphics
    _tray->setToolTip(Theme::instance()->appNameGUI() + QStringLiteral(" - ") + trayStatusToString(status));
}

SettingsDialog *ownCloudGui::settingsDialog() const
{
    return _settingsDialog;
}

void ownCloudGui::setupTrayContextMenu()
{
    // using the main windows (_settingsDialog) as parent for memory management
    auto menu = new QMenu(_settingsDialog);
    menu->setTitle(Theme::instance()->appNameGUI());

    _tray->setContextMenu(menu);

    // Populate the context menu now.
    menu->addAction(Theme::instance()->applicationIcon(), tr("Show %1").arg(Theme::instance()->appNameGUI()), this, &ownCloudGui::slotShowSettings);
    menu->addSeparator();

    // owncloud.online: let the user temporarily pause syncing; it auto-resumes via
    // _pauseSyncTimer (or immediately via the "Resume" entry).
    _pauseSyncMenu = menu->addMenu(tr("Pause synchronization"));
    _pauseSyncMenu->addAction(tr("For 30 minutes"), this, [this] { pauseSyncFor(30); });
    _pauseSyncMenu->addAction(tr("For 1 hour"), this, [this] { pauseSyncFor(60); });
    _pauseSyncMenu->addAction(tr("Until I resume"), this, [this] { pauseSyncFor(0); });
    _resumeSyncAction = menu->addAction(tr("Resume synchronization"), this, [this] { resumeSync(); });
    updatePauseSyncMenuState();
    menu->addSeparator();

    if (_app->debugMode()) {
        auto *debugMenu = menu->addMenu(QStringLiteral("Debug actions"));
        debugMenu->addAction(QStringLiteral("Crash if asserts enabled - OC_ENSURE"), _app, [] {
            if (OC_ENSURE(false)) {
                Q_UNREACHABLE();
            }
        });
        debugMenu->addAction(QStringLiteral("Crash if asserts enabled - Q_ASSERT"), _app, [] { Q_ASSERT(false); });
        debugMenu->addAction(QStringLiteral("Crash now - Utility::crash()"), _app, [] { Utility::crash(); });
        debugMenu->addAction(QStringLiteral("Crash now - OC_ENFORCE()"), _app, [] { OC_ENFORCE(false); });
        debugMenu->addAction(QStringLiteral("Crash now - qFatal"), _app, [] { qFatal("la Qt fatale"); });
        debugMenu->addAction(QStringLiteral("Restart now"), _app, [] { RestartManager::requestRestart(); });
        debugMenu->addSeparator();
        auto captivePortalCheckbox = debugMenu->addAction(QStringLiteral("Behind Captive Portal"));
        captivePortalCheckbox->setCheckable(true);
        captivePortalCheckbox->setChecked(NetworkInformation::instance()->isForcedCaptivePortal());
        connect(captivePortalCheckbox, &QAction::triggered, [](bool checked) { NetworkInformation::instance()->setForcedCaptivePortal(checked); });

        menu->addSeparator();
    }

    if (!Theme::instance()->helpUrl().isEmpty()) {
        menu->addAction(tr("Help"), this, &ownCloudGui::slotHelp);
    }

    if (! Theme::instance()->about().isEmpty()) {
        menu->addAction(tr("About %1").arg(Theme::instance()->appNameGUI()), this, &ownCloudGui::slotAbout);
    }

    menu->addAction(tr("Quit %1").arg(Theme::instance()->appNameGUI()), _app, &QApplication::quit);
}

void ownCloudGui::pauseSyncFor(int minutes)
{
    // Globally stop the scheduler without touching the per-folder paused state, so
    // folders the user paused individually stay paused and are not silently resumed
    // later. (An internal setSyncEnabled(true) triggered by actively adding a folder
    // during the pause window would end the pause early, which is harmless.)
    FolderMan::instance()->setSyncEnabled(false);

    _pauseSyncTimer->stop();
    _syncPaused = true;
    if (minutes > 0) {
        _syncPausedUntil = QDateTime::currentDateTime().addSecs(static_cast<qint64>(minutes) * 60);
        _pauseSyncTimer->start(std::chrono::minutes(minutes));
    } else {
        _syncPausedUntil = QDateTime(); // paused until manually resumed
    }
    qCInfo(lcApplication) << "Sync paused by user for" << minutes << "minutes (0 = indefinite)";
    updatePauseSyncMenuState();
}

void ownCloudGui::resumeSync()
{
    _pauseSyncTimer->stop();
    _syncPaused = false;
    _syncPausedUntil = QDateTime();

    // Re-enables the scheduler; queued and pending folders resume syncing.
    FolderMan::instance()->setSyncEnabled(true);
    qCInfo(lcApplication) << "Sync resumed by user";
    updatePauseSyncMenuState();
}

void ownCloudGui::updatePauseSyncMenuState()
{
    if (_pauseSyncMenu) {
        _pauseSyncMenu->menuAction()->setVisible(!_syncPaused);
    }
    if (_resumeSyncAction) {
        _resumeSyncAction->setVisible(_syncPaused);
        if (_syncPaused && _syncPausedUntil.isValid()) {
            _resumeSyncAction->setText(
                tr("Resume synchronization (paused until %1)").arg(_syncPausedUntil.toString(QStringLiteral("HH:mm"))));
        } else {
            _resumeSyncAction->setText(tr("Resume synchronization"));
        }
    }
}

void ownCloudGui::slotShowTrayMessage(const QString &title, const QString &msg, const QIcon &icon)
{
    _tray->showMessage(title, msg, icon.isNull() ? Resources::getCoreIcon(QStringLiteral("states/information")) : icon);
}

void ownCloudGui::slotShowOptionalTrayMessage(const QString &title, const QString &msg, const QIcon &icon)
{
    ConfigFile cfg;
    if (cfg.optionalDesktopNotifications()) {
        slotShowTrayMessage(title, msg, icon);
    }
}

void ownCloudGui::runAccountWizard()
{
    NewAccountWizard wizard(settingsDialog());
    NewAccountModel model(nullptr);
    NewAccountWizardController wizardController(&model, &wizard, nullptr);
    ownCloudGui::raise();
    int result = wizard.exec();
    if (result == QDialog::Accepted) {
        // the builder needs to be a pointer as it has to wait for the connection state to go to connected
        // it will delete itself once it has completed its mission.
        // pass this as parent only as a safeguard.
        if (!model.isComplete()) {
            QMessageBox::warning(
                _settingsDialog, tr("New account failure"), tr("The information required to create a new account is incomplete. Please run the wizard again."));
        } else {
            NewAccountBuilder *builder = new NewAccountBuilder(model, this);
            connect(builder, &NewAccountBuilder::requestSetUpSyncFoldersForAccount, this, &ownCloudGui::requestSetUpSyncFoldersForAccount);
            connect(builder, &NewAccountBuilder::requestLoadSpacesOnly, this, &ownCloudGui::requestLoadSpacesOnly);
            connect(builder, &NewAccountBuilder::requestFolderWizard, _settingsDialog, &SettingsDialog::runFolderWizard);
            connect(builder, &NewAccountBuilder::unableToCompleteAccountCreation, this, &ownCloudGui::handleAccountSetupError);
            builder->buildAccount();
        }
    } else
        qDebug() << "wizard rejected";
}

void ownCloudGui::handleAccountSetupError(const QString &error)
{
    QMessageBox::warning(_settingsDialog, tr("New account failure"),
        tr("The account could not be created due to an error:\n%1\nPlease check the server's availability then run the wizard again.").arg(error));
}

void ownCloudGui::slotShowSettings()
{
    raise();
}

void ownCloudGui::slotShutdown()
{
    // explicitly close windows. This is somewhat of a hack to ensure
    // that saving the geometries happens ASAP during an OS shutdown

    // those do delete on close
    _settingsDialog->close();
}

void ownCloudGui::slotToggleLogBrowser()
{
    auto logBrowser = new LogBrowser(settingsDialog());
    logBrowser->setAttribute(Qt::WA_DeleteOnClose);
    ownCloudGui::raise();
    logBrowser->open();
}

void ownCloudGui::slotHelp()
{
    QDesktopServices::openUrl(QUrl(Theme::instance()->helpUrl()));
}

void ownCloudGui::raise()
{
    auto window = ocApp()->gui()->settingsDialog();
    window->show();
    window->raise();
    window->activateWindow();

#if defined(Q_OS_WIN)
    // Windows disallows raising a Window when you're not the active application.
    // Use a common hack to attach to the active application
    const auto activeProcessId = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    if (activeProcessId != qApp->applicationPid()) {
        const auto threadId = GetCurrentThreadId();
        // don't step here with a debugger...
        if (AttachThreadInput(threadId, activeProcessId, true))
        {
            const auto hwnd = reinterpret_cast<HWND>(window->winId());
            SetForegroundWindow(hwnd);
            SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            AttachThreadInput(threadId, activeProcessId, false);
        }
    }
#endif
}


void ownCloudGui::slotShowShareDialog(const QString &sharePath, const QString &localPath)
{
    QString file;
    const auto folder = FolderMan::instance()->folderForPath(localPath, &file);
    if (!folder || !folder->accountState()) {
        qCWarning(lcApplication) << "Could not open the share dialog for" << localPath << "- no responsible folder found";
        return;
    }
    // native dialog for creating/managing public links; when the server has
    // the OCS share API disabled the dialog cannot work, so open the server's
    // web interface instead (the private-link PROPFIND works either way)
    if (!folder->accountState()->account()->capabilities().filesSharing().api_enabled) {
        fetchPrivateLinkUrl(folder->accountState()->account(), folder->webDavUrl(), sharePath, this, [](const QUrl &url) {
            const auto queryUrl = Utility::concatUrlPath(url, QString(), {{QStringLiteral("details"), QStringLiteral("sharing")}});
            Utility::openBrowser(queryUrl, nullptr);
        });
        return;
    }
    if (_shareDialog) {
        _shareDialog->close();
    }
    _shareDialog = new ShareDialog(folder->accountState(), sharePath, localPath, nullptr);
    _shareDialog->show();
    _shareDialog->raise();
    _shareDialog->activateWindow();
}

void ownCloudGui::slotShowShareInBrowser(const QString &sharePath, const QString &localPath)
{
    QString file;
    const auto folder = FolderMan::instance()->folderForPath(localPath, &file);
    if (!folder) {
        qCWarning(lcApplication) << "Could not open browser share page for" << localPath << "no responsible folder found";
        return;
    }

    // Frueher an capabilities().filesSharing().sharing_roles gekoppelt — dieser
    // Top-Level-Key wird von oc10-Servern (owncloud.online) nie gesetzt, wodurch
    // die "Teilen"-Aktion still nichts tat. Jetzt an api_enabled gekoppelt
    // (Default true), sodass das Sharing-Panel geoeffnet wird, solange die
    // Share-API nicht ausdruecklich deaktiviert ist.
    if (folder->accountState()->account()->capabilities().filesSharing().api_enabled) {
        fetchPrivateLinkUrl(folder->accountState()->account(), folder->webDavUrl(), sharePath, this, [](const QUrl &url) {
            const auto queryUrl = Utility::concatUrlPath(url, QString(), {{QStringLiteral("details"), QStringLiteral("sharing")}});
            Utility::openBrowser(queryUrl, nullptr);
        });
    } else {
        qCWarning(lcApplication) << "Share API disabled on the server; cannot open share page for" << localPath;
    }
}

void ownCloudGui::slotAbout()
{
    if(!_aboutDialog) {
        _aboutDialog = new AboutDialog(_settingsDialog);
        _aboutDialog->setAttribute(Qt::WA_DeleteOnClose);
        ocApp()->gui()->settingsDialog()->addModalWidget(_aboutDialog);
    }
}


} // end namespace
