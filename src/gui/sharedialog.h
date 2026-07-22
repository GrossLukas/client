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

#include <QDialog>
#include <QPointer>

class QCheckBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace OCC {

class AccountState;

/**
 * @brief Native sharing dialog: create and manage public links for a synced
 * file or folder via the server's OCS share API - no browser required.
 *
 * Lists the existing public links (copy to clipboard, delete), and creates
 * new ones with an optional password and expiry date. Opened from the
 * Explorer context menu ("Share with owncloud.online").
 *
 * @ingroup gui
 */
class ShareDialog : public QDialog
{
    Q_OBJECT
public:
    /// remotePath is the server-relative path of the item ("/Documents/report.odt")
    ShareDialog(AccountState *accountState, const QString &remotePath, const QString &localPath, QWidget *parent = nullptr);

private slots:
    void loadShares();
    void createShare();

private:
    void showStatus(const QString &message, bool isError);
    void rebuildShareList(const QList<QVariantMap> &shares);
    void deleteShare(const QString &shareId);
    void copyToClipboard(const QString &url);

    QPointer<AccountState> _accountState;
    QString _remotePath;

    QVBoxLayout *_sharesLayout = nullptr;
    QLabel *_noSharesLabel = nullptr;
    QCheckBox *_passwordCheckBox = nullptr;
    QLineEdit *_passwordEdit = nullptr;
    QCheckBox *_expireCheckBox = nullptr;
    QDateEdit *_expireEdit = nullptr;
    QPushButton *_createButton = nullptr;
    QLabel *_statusLabel = nullptr;
};

} // namespace OCC
