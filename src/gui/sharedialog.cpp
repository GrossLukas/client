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

#include "sharedialog.h"

#include "account.h"
#include "accountstate.h"
#include "networkjobs/jsonjob.h"

#include <QCheckBox>
#include <QClipboard>
#include <QDateEdit>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QLoggingCategory>
#include <QNetworkRequest>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace OCC {

Q_LOGGING_CATEGORY(lcShareDialog, "gui.sharedialog", QtInfoMsg)

namespace {
    // v1 endpoint on purpose: v2.php mirrors OCS errors into the HTTP status,
    // which makes JsonJob skip parsing - the server's error message (e.g. an
    // enforced password policy) would never reach the user. v1 always answers
    // HTTP 200 with the OCS statuscode/message in the body.
    const QString sharesEndpoint = QStringLiteral("ocs/v1.php/apps/files_sharing/api/v1/shares");
    constexpr int publicLinkShareType = 3;

    QNetworkRequest ocsRequest()
    {
        QNetworkRequest req;
        req.setRawHeader(QByteArrayLiteral("OCS-APIREQUEST"), QByteArrayLiteral("true"));
        return req;
    }
}

ShareDialog::ShareDialog(AccountState *accountState, const QString &remotePath, const QString &localPath, QWidget *parent)
    : QDialog(parent)
    , _accountState(accountState)
    , _remotePath(remotePath.startsWith(QLatin1Char('/')) ? remotePath : QLatin1Char('/') + remotePath)
{
    const QString name = QFileInfo(localPath.isEmpty() ? remotePath : localPath).fileName();
    setWindowTitle(tr("Share %1").arg(name));
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumWidth(480);

    auto *mainLayout = new QVBoxLayout(this);

    auto *header = new QLabel(tr("Public links for <b>%1</b>").arg(name.toHtmlEscaped()), this);
    mainLayout->addWidget(header);

    // existing links
    auto *sharesHost = new QWidget(this);
    _sharesLayout = new QVBoxLayout(sharesHost);
    _sharesLayout->setContentsMargins(0, 0, 0, 0);
    _noSharesLabel = new QLabel(tr("Loading shares …"), sharesHost);
    _sharesLayout->addWidget(_noSharesLabel);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(sharesHost);
    scroll->setMinimumHeight(120);
    mainLayout->addWidget(scroll, 1);

    auto *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    mainLayout->addWidget(line);

    // create a new link
    _passwordCheckBox = new QCheckBox(tr("Protect with a password"), this);
    _passwordEdit = new QLineEdit(this);
    _passwordEdit->setEchoMode(QLineEdit::Password);
    _passwordEdit->setEnabled(false);
    connect(_passwordCheckBox, &QCheckBox::toggled, _passwordEdit, &QWidget::setEnabled);
    auto *pwRow = new QHBoxLayout;
    pwRow->addWidget(_passwordCheckBox);
    pwRow->addWidget(_passwordEdit, 1);
    mainLayout->addLayout(pwRow);

    _expireCheckBox = new QCheckBox(tr("Set an expiration date"), this);
    _expireEdit = new QDateEdit(QDate::currentDate().addDays(7), this);
    _expireEdit->setCalendarPopup(true);
    _expireEdit->setMinimumDate(QDate::currentDate().addDays(1));
    _expireEdit->setEnabled(false);
    connect(_expireCheckBox, &QCheckBox::toggled, _expireEdit, &QWidget::setEnabled);
    auto *expRow = new QHBoxLayout;
    expRow->addWidget(_expireCheckBox);
    expRow->addWidget(_expireEdit, 1);
    mainLayout->addLayout(expRow);

    _createButton = new QPushButton(tr("Create public link"), this);
    _createButton->setDefault(true);
    connect(_createButton, &QPushButton::clicked, this, &ShareDialog::createShare);
    mainLayout->addWidget(_createButton);

    _statusLabel = new QLabel(this);
    _statusLabel->setWordWrap(true);
    _statusLabel->setVisible(false);
    mainLayout->addWidget(_statusLabel);

    auto *closeButton = new QPushButton(tr("Close"), this);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    auto *bottomRow = new QHBoxLayout;
    bottomRow->addStretch(1);
    bottomRow->addWidget(closeButton);
    mainLayout->addLayout(bottomRow);

    loadShares();
}

void ShareDialog::showStatus(const QString &message, bool isError)
{
    _statusLabel->setVisible(true);
    // explicit colors that read on light and dark palettes
    _statusLabel->setStyleSheet(isError ? QStringLiteral("color: #e0245e;") : QStringLiteral("color: #00a98c;"));
    _statusLabel->setText(message);
}

void ShareDialog::loadShares()
{
    if (!_accountState || !_accountState->account())
        return;
    if (!_accountState->isConnected()) {
        const QString notConnected = tr("Not connected to the server. Please check the connection and reopen the dialog.");
        if (_noSharesLabel)
            _noSharesLabel->setText(notConnected);
        else
            showStatus(notConnected, true);
        return;
    }
    auto *job = new JsonApiJob(_accountState->account(), sharesEndpoint,
        {{QStringLiteral("path"), _remotePath},
            {QStringLiteral("reshares"), QStringLiteral("false")}},
        ocsRequest(), this);
    connect(job, &JsonApiJob::finishedSignal, this, [this, job] {
        if (!job->ocsSuccess()) {
            // after a reload the no-shares label may be gone (links were listed)
            if (_noSharesLabel)
                _noSharesLabel->setText(tr("Could not load the existing shares."));
            else
                showStatus(tr("Could not load the existing shares."), true);
            qCWarning(lcShareDialog) << "share list failed:" << job->ocsStatus() << job->ocsMessage();
            return;
        }
        QList<QVariantMap> shares;
        const QJsonArray items = job->data().value(QLatin1String("ocs")).toObject().value(QLatin1String("data")).toArray();
        for (const auto &item : items) {
            const QJsonObject share = item.toObject();
            if (share.value(QLatin1String("share_type")).toInt() != publicLinkShareType)
                continue;
            QVariantMap entry;
            // id can arrive as string or number depending on the server
            entry.insert(QStringLiteral("id"), share.value(QLatin1String("id")).toVariant().toString());
            entry.insert(QStringLiteral("url"), share.value(QLatin1String("url")).toString());
            entry.insert(QStringLiteral("expiration"), share.value(QLatin1String("expiration")).toString());
            entry.insert(QStringLiteral("hasPassword"), !share.value(QLatin1String("share_with")).toString().isEmpty());
            shares.append(entry);
        }
        rebuildShareList(shares);
    });
    job->start();
}

void ShareDialog::rebuildShareList(const QList<QVariantMap> &shares)
{
    // clear everything below the layout (the no-shares label is rebuilt too)
    while (QLayoutItem *item = _sharesLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    _noSharesLabel = nullptr;

    if (shares.isEmpty()) {
        _noSharesLabel = new QLabel(tr("No public links yet."), this);
        _sharesLayout->addWidget(_noSharesLabel);
        return;
    }

    for (const QVariantMap &share : shares) {
        const QString url = share.value(QStringLiteral("url")).toString();
        const QString shareId = share.value(QStringLiteral("id")).toString();

        auto *row = new QWidget(this);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);

        QStringList details;
        if (share.value(QStringLiteral("hasPassword")).toBool())
            details.append(tr("password protected"));
        const QString expiration = share.value(QStringLiteral("expiration")).toString();
        if (!expiration.isEmpty()) {
            const QDate expiryDate = QDate::fromString(expiration.left(10), Qt::ISODate);
            details.append(tr("expires %1").arg(expiryDate.isValid() ? QLocale().toString(expiryDate, QLocale::ShortFormat) : expiration.left(10)));
        }

        auto *urlLabel = new QLabel(details.isEmpty()
                ? url.toHtmlEscaped()
                : QStringLiteral("%1 <i>(%2)</i>").arg(url.toHtmlEscaped(), details.join(QStringLiteral(", "))),
            row);
        urlLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        rowLayout->addWidget(urlLabel, 1);

        auto *copyButton = new QToolButton(row);
        copyButton->setText(tr("Copy link"));
        connect(copyButton, &QToolButton::clicked, this, [this, url] { copyToClipboard(url); });
        rowLayout->addWidget(copyButton);

        auto *deleteButton = new QToolButton(row);
        deleteButton->setText(tr("Delete"));
        connect(deleteButton, &QToolButton::clicked, this, [this, shareId] { deleteShare(shareId); });
        rowLayout->addWidget(deleteButton);

        _sharesLayout->addWidget(row);
    }
}

void ShareDialog::createShare()
{
    if (!_accountState || !_accountState->account())
        return;
    if (_passwordCheckBox->isChecked() && _passwordEdit->text().isEmpty()) {
        showStatus(tr("Please enter a password, or uncheck the password option."), true);
        return;
    }

    SimpleNetworkJob::UrlQuery arguments = {
        {QStringLiteral("path"), _remotePath},
        {QStringLiteral("shareType"), QString::number(publicLinkShareType)},
        {QStringLiteral("permissions"), QStringLiteral("1")},
    };
    if (_passwordCheckBox->isChecked())
        arguments.append({QStringLiteral("password"), _passwordEdit->text()});
    if (_expireCheckBox->isChecked())
        arguments.append({QStringLiteral("expireDate"), _expireEdit->date().toString(QStringLiteral("yyyy-MM-dd"))});

    _createButton->setEnabled(false);
    auto *job = new JsonApiJob(_accountState->account(), sharesEndpoint, QByteArrayLiteral("POST"), arguments, ocsRequest(), this);
    connect(job, &JsonApiJob::finishedSignal, this, [this, job] {
        _createButton->setEnabled(true);
        if (!job->ocsSuccess()) {
            // the server message explains e.g. an enforced password or expiry policy
            const QString reason = job->ocsMessage();
            showStatus(reason.isEmpty() ? tr("Creating the public link failed.")
                                        : tr("Creating the public link failed: %1").arg(reason),
                true);
            return;
        }
        const QString url = job->data().value(QLatin1String("ocs")).toObject().value(QLatin1String("data")).toObject().value(QLatin1String("url")).toString();
        if (!url.isEmpty())
            copyToClipboard(url);
        _passwordEdit->clear();
        loadShares();
    });
    job->start();
}

void ShareDialog::deleteShare(const QString &shareId)
{
    if (!_accountState || !_accountState->account() || shareId.isEmpty())
        return;
    auto *job = new JsonApiJob(_accountState->account(), sharesEndpoint + QLatin1Char('/') + shareId, QByteArrayLiteral("DELETE"),
        {}, ocsRequest(), this);
    connect(job, &JsonApiJob::finishedSignal, this, [this, job] {
        if (!job->ocsSuccess()) {
            showStatus(tr("Deleting the public link failed."), true);
            return;
        }
        showStatus(tr("Public link deleted."), false);
        loadShares();
    });
    job->start();
}

void ShareDialog::copyToClipboard(const QString &url)
{
    QGuiApplication::clipboard()->setText(url);
    showStatus(tr("Link copied to the clipboard: %1").arg(url), false);
}

} // namespace OCC
