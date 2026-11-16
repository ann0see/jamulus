/******************************************************************************\
 * Copyright (c) 2004-2026
 *
 * Author(s):
 *  ann0see
 *
 * As of Jamulus 3.12.1dev (commit eb172d47): All new source code contributions must be licensed
 * under AGPL 3.0 or any later version.
 *
 * Existing code: Code contributed before 3.12.1dev (commit eb172d47) was licensed under GPL 2.0+.
 * This code will be licensed under GPL 3.0 (or any later version) from
 * 3.12.1dev (commit eb172d47).  When distributed as part of Jamulus, the AGPL 3.0 terms govern
 * the combined work, including network use provisions.
 *
 ******************************************************************************
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
\******************************************************************************/

#pragma once

#include <QDBusUnixFileDescriptor>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>

class CActivity
{
public:
    CActivity();
    ~CActivity();

    void BeginActivity();
    void EndActivity();

private:
    struct SInhibitor
    {
        QString service;
        QString path;
        QString interface;
        QString releaseMethod;
        uint    cookie;
    };

    bool ContinueAcquisition ( quint64 generation );
    bool AcquireInhibitor ( const QString&      service,
                            const QString&      path,
                            const QString&      interface,
                            const QString&      method,
                            const QString&      releaseMethod,
                            const QVariantList& arguments );
    bool AcquirePowerManagement ( const QString& appId, const QString& reason );
    bool AcquirePortal ( uint flags );
    bool AcquireLogind ( const QString& appId, const QString& reason );
    void ReleaseAll();

    QList<SInhibitor>       m_inhibitors;
    QStringList             m_portalRequests;
    QDBusUnixFileDescriptor m_logindFd;
    bool                    m_active;
    quint64                 m_generation;
};
