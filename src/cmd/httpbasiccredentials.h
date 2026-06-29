/*
 * Copyright (C) 2026 by BW-Tech GmbH (owncloud.online)
 *
 * Minimal HTTP Basic-auth credentials for the command-line client (owncloudcmd).
 * The GUI uses OAuth (gui/creds/Credentials); the CLI can't open a browser, so it
 * authenticates with a username/password supplied on the command line. ownCloud 10
 * / owncloud.online servers accept Basic auth on WebDAV/OCS.
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

#include "creds/abstractcredentials.h"

#include <QString>

namespace OCC {

class HttpBasicCredentials : public AbstractCredentials
{
    Q_OBJECT
public:
    HttpBasicCredentials(Account *account, const QString &user, const QString &password);

    AccessManager *createAccessManager() const override;
    bool ready() const override { return !_user.isEmpty(); }
    void fetchFromKeychain() override;
    void askFromUser() override;
    bool stillValid(QNetworkReply *reply) override;
    void persist() override {}
    void invalidateToken() override {}
    void forgetSensitiveData() override {}
    bool refreshAccessToken() override { return false; }

    /** The full "Basic <base64(user:pass)>" Authorization header value. */
    QByteArray authHeaderValue() const;

private:
    QString _user;
    QString _password;
};

}
