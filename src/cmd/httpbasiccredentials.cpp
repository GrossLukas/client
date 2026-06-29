/*
 * Copyright (C) 2026 by BW-Tech GmbH (owncloud.online)
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

#include "httpbasiccredentials.h"

#include "accessmanager.h"

#include <QNetworkReply>
#include <QNetworkRequest>

namespace OCC {

namespace {
    // AccessManager that injects the Basic-auth Authorization header on every
    // request, mirroring how the GUI's CredentialsAccessManager injects a Bearer
    // token.
    class BasicAccessManager : public AccessManager
    {
    public:
        BasicAccessManager(const QByteArray &authHeader, QObject *parent)
            : AccessManager(parent)
            , _authHeader(authHeader)
        {
        }

    protected:
        QNetworkReply *createRequest(Operation op, const QNetworkRequest &request, QIODevice *outgoingData) override
        {
            QNetworkRequest req(request);
            req.setRawHeader(QByteArrayLiteral("Authorization"), _authHeader);
            return AccessManager::createRequest(op, req, outgoingData);
        }

    private:
        QByteArray _authHeader;
    };
}

HttpBasicCredentials::HttpBasicCredentials(Account *account, const QString &user, const QString &password)
    : AbstractCredentials(account, account)
    , _user(user)
    , _password(password)
{
}

QByteArray HttpBasicCredentials::authHeaderValue() const
{
    const QByteArray userAndPass = (_user + QLatin1Char(':') + _password).toUtf8();
    return QByteArrayLiteral("Basic ") + userAndPass.toBase64();
}

AccessManager *HttpBasicCredentials::createAccessManager() const
{
    return new BasicAccessManager(authHeaderValue(), nullptr);
}

void HttpBasicCredentials::fetchFromKeychain()
{
    // Credentials come from the command line; nothing to fetch.
    _wasEverFetched = true;
    Q_EMIT fetched();
}

void HttpBasicCredentials::askFromUser()
{
    // The CLI takes credentials as arguments; this should never be reached.
    Q_EMIT authenticationFailed();
}

bool HttpBasicCredentials::stillValid(QNetworkReply *reply)
{
    // A 401/403 means the supplied user/password is wrong; basic auth has no token
    // to refresh, so the credentials are only "still valid" if we were not rejected.
    const int status = reply ? reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0;
    return status != 401 && status != 403;
}

}
