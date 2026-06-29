/*
 * Copyright (C) 2026 by BW-Tech GmbH (owncloud.online)
 *
 * Minimal command-line sync client (owncloudcmd) for owncloud.online. Reintroduced
 * after upstream dropped it (DC-46) so the desktop sync engine can be driven
 * headless for testing, CI and headless-server / NAS sync. Authenticates with HTTP
 * Basic auth (username/password) since there is no browser for OAuth.
 *
 * Usage: owncloudcmd <local_dir> <server_url> -u <user> -p <password> [--path <remote>]
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

#include "account.h"
#include "common/syncjournaldb.h"
#include "common/vfs.h"
#include "configfile.h"
#include "networkjobs/checkserverjobfactory.h"
#include "networkjobs/jsonjob.h"
#include "syncengine.h"

#include "httpbasiccredentials.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QJsonObject>
#include <QTimer>
#include <QUuid>

using namespace OCC;

namespace {

void startSync(Account *account, const QString &localDir, const QString &remotePath)
{
    const QString dbPath = localDir + SyncJournalDb::makeDbName(localDir);
    auto *db = new SyncJournalDb(dbPath, qApp);

    SyncOptions opt{ QSharedPointer<Vfs>(VfsPluginManager::instance().createVfsFromPlugin(Vfs::Off).release()) };
    opt.fillFromEnvironmentVariables();
    opt.verifyChunkSizes();

    auto *engine = new SyncEngine(account, account->davUrl(), localDir, remotePath, db);
    engine->setSyncOptions(opt);
    engine->setParent(db);
    SyncEngine::minimumFileAgeForUpload = std::chrono::seconds(0);

    QObject::connect(engine, &SyncEngine::finished, qApp, [](bool ok) {
        if (ok) {
            qInfo() << "Sync finished successfully";
            qApp->exit(0);
        } else {
            qWarning() << "Sync failed";
            qApp->exit(1);
        }
    });
    QObject::connect(engine, &SyncEngine::syncError, qApp,
        [](const QString &error, ErrorCategory) { qWarning() << "Sync error:" << error; });

    engine->loadDefaultExcludes();
    engine->addExcludeList(ConfigFile::excludeFileFromSystem());
    engine->reloadExcludes();
    engine->startSync();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("owncloudcmd"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("owncloud.online command-line sync client"));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("local_dir"), QStringLiteral("Local folder to sync"));
    parser.addPositionalArgument(QStringLiteral("server_url"), QStringLiteral("Server root URL, e.g. https://cloud.example.com"));
    QCommandLineOption userOpt({ QStringLiteral("u"), QStringLiteral("user") }, QStringLiteral("User name"), QStringLiteral("user"));
    QCommandLineOption passOpt({ QStringLiteral("p"), QStringLiteral("password") }, QStringLiteral("Password"), QStringLiteral("password"));
    QCommandLineOption pathOpt(QStringLiteral("path"), QStringLiteral("Remote folder (default /)"), QStringLiteral("path"), QStringLiteral("/"));
    parser.addOption(userOpt);
    parser.addOption(passOpt);
    parser.addOption(pathOpt);
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.size() < 2) {
        qCritical() << "Usage: owncloudcmd <local_dir> <server_url> -u <user> -p <password> [--path <remote>]";
        return EXIT_FAILURE;
    }

    const QString localDir = QDir(args.at(0)).absolutePath() + QLatin1Char('/');
    const QUrl serverUrl = QUrl::fromUserInput(args.at(1));
    const QString user = parser.value(userOpt);
    const QString password = parser.value(passOpt);
    const QString remotePath = parser.value(pathOpt);

    if (user.isEmpty() || password.isEmpty()) {
        qCritical() << "A user (-u) and password (-p) are required";
        return EXIT_FAILURE;
    }

    auto *account = new Account(QUuid::createUuid(), user, serverUrl);
    account->setCredentials(new HttpBasicCredentials(account, user, password));

    QTimer::singleShot(0, &app, [account, localDir, remotePath] {
        auto *checkJob = CheckServerJobFactory::createFromAccount(account, false).startJob(account->url(), qApp);
        QObject::connect(checkJob, &CoreJob::finished, qApp, [account, checkJob, localDir, remotePath] {
            if (!checkJob->success()) {
                qCritical() << "Could not reach the server:" << checkJob->errorMessage();
                qApp->exit(EXIT_FAILURE);
                return;
            }

            auto *capsJob = new JsonApiJob(account, QStringLiteral("ocs/v2.php/cloud/capabilities"), {}, {}, qApp);
            QObject::connect(capsJob, &JsonApiJob::finishedSignal, qApp, [account, capsJob, localDir, remotePath] {
                if (capsJob->reply()->error() != QNetworkReply::NoError) {
                    qCritical() << "Failed to fetch capabilities:" << capsJob->reply()->errorString();
                    qApp->exit(EXIT_FAILURE);
                    return;
                }
                const QJsonObject caps = capsJob->data()
                                             .value(QStringLiteral("ocs"))
                                             .toObject()
                                             .value(QStringLiteral("data"))
                                             .toObject()
                                             .value(QStringLiteral("capabilities"))
                                             .toObject();
                account->setCapabilities({ account->url(), caps.toVariantMap() });
                // davUser is the login user passed to the Account constructor, so no
                // extra ocs/cloud/user round trip is needed here.
                startSync(account, localDir, remotePath);
            });
            capsJob->start();
        });
    });

    return app.exec();
}
