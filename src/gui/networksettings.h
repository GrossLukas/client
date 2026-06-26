/*
 * Copyright (C) by Daniel Molkentin <danimo@owncloud.com>
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

#ifndef MIRALL_NETWORKSETTINGS_H
#define MIRALL_NETWORKSETTINGS_H

#include "libsync/creds/credentialmanager.h"

#include <QWidget>


class QCheckBox;
class QSpinBox;

namespace OCC {

namespace Ui {
    class NetworkSettings;
}

/**
 * @brief The NetworkSettings class
 * @ingroup gui
 */
class NetworkSettings : public QWidget
{
    Q_OBJECT

public:
    explicit NetworkSettings(QWidget *parent = nullptr);
    ~NetworkSettings() override;

private Q_SLOTS:
    void saveProxySettings();
    void saveMeteredSettings();
    void saveBWLimitSettings();

    /// Red marking of host field if empty and enabled
    void checkEmptyProxyHost();

    void checkAccountLocalhost();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void loadProxySettings();
    void removeBWLimitSettings();
    void loadMeteredSettings();
    void setupBWLimitUi();
    void loadBWLimitSettings();
    CredentialManager *_credentialManager;

    Ui::NetworkSettings *_ui;

    // Bandwidth limit controls (built programmatically in setupBWLimitUi)
    QCheckBox *_uploadLimitCheckBox = nullptr;
    QSpinBox *_uploadLimitSpinBox = nullptr;
    QCheckBox *_downloadLimitCheckBox = nullptr;
    QSpinBox *_downloadLimitSpinBox = nullptr;
};


} // namespace OCC
#endif // MIRALL_NETWORKSETTINGS_H
