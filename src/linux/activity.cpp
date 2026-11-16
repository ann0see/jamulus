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

#include "activity.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDebug>
#include <QVariantMap>

namespace
{
constexpr int  DBUS_TIMEOUT_MS          = 1000;
constexpr uint INHIBIT_SUSPEND          = 4;
constexpr uint INHIBIT_IDLE             = 8;
constexpr uint INHIBIT_IDLE_AND_SUSPEND = INHIBIT_SUSPEND | INHIBIT_IDLE;

struct SSessionManagerProvider
{
    const char* service;
    const char* path;
    const char* interface;
};

struct SScreenSaverProvider
{
    const char* service;
    const char* path;
};
} // namespace

CActivity::CActivity() : m_active ( false ), m_generation ( 0 ) {}

CActivity::~CActivity() { EndActivity(); }

void CActivity::BeginActivity()
{
    if ( m_active )
    {
        return;
    }

    m_active                 = true;
    const quint64 generation = ++m_generation;

    const QString appId  = QStringLiteral ( "Jamulus" );
    const QString reason = QCoreApplication::translate ( "CActivity", "Connected to a server" );

    if ( !QDBusConnection::sessionBus().isConnected() )
    {
        qWarning() << "Cannot inhibit screensaver/suspend: session bus not available";
        return;
    }

    bool idleCovered          = false;
    bool suspendCovered       = false;
    bool powerManagementTried = false;

    const SSessionManagerProvider sessionManagers[] = { { "org.gnome.SessionManager", "/org/gnome/SessionManager", "org.gnome.SessionManager" },
                                                        { "org.xfce.SessionManager", "/org/xfce/SessionManager", "org.xfce.Session.Manager" } };

    for ( const SSessionManagerProvider& provider : sessionManagers )
    {
        if ( AcquireInhibitor ( provider.service,
                                provider.path,
                                provider.interface,
                                QStringLiteral ( "Inhibit" ),
                                QStringLiteral ( "Uninhibit" ),
                                QVariantList() << appId << 0u << reason << INHIBIT_IDLE_AND_SUSPEND ) )
        {
            idleCovered    = true;
            suspendCovered = true;
            break;
        }

        if ( !ContinueAcquisition ( generation ) )
        {
            return;
        }
    }

    if ( !ContinueAcquisition ( generation ) )
    {
        return;
    }

    if ( !idleCovered )
    {
        const SScreenSaverProvider screenSavers[] = { { "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver" },
                                                      { "org.freedesktop.ScreenSaver", "/ScreenSaver" },
                                                      { "org.xfce.ScreenSaver", "/org/xfce/ScreenSaver" },
                                                      { "org.mate.ScreenSaver", "/org/mate/ScreenSaver" },
                                                      { "org.cinnamon.ScreenSaver", "/org/cinnamon/ScreenSaver" },
                                                      { "org.gnome.ScreenSaver", "/org/gnome/ScreenSaver" } };

        for ( const SScreenSaverProvider& provider : screenSavers )
        {
            if ( AcquireInhibitor ( provider.service,
                                    provider.path,
                                    provider.service,
                                    QStringLiteral ( "Inhibit" ),
                                    QStringLiteral ( "UnInhibit" ),
                                    QVariantList() << appId << reason ) )
            {
                idleCovered = true;
                break;
            }

            if ( !ContinueAcquisition ( generation ) )
            {
                return;
            }
        }

        if ( !ContinueAcquisition ( generation ) )
        {
            return;
        }
    }

    if ( !idleCovered )
    {
        powerManagementTried = true;

        if ( AcquirePowerManagement ( appId, reason ) )
        {
            idleCovered    = true;
            suspendCovered = true;
        }

        if ( !ContinueAcquisition ( generation ) )
        {
            return;
        }
    }

    if ( !idleCovered )
    {
        idleCovered = AcquirePortal ( INHIBIT_IDLE );

        if ( !ContinueAcquisition ( generation ) )
        {
            return;
        }
    }

    if ( !suspendCovered )
    {
        if ( !powerManagementTried )
        {
            powerManagementTried = true;
            suspendCovered       = AcquirePowerManagement ( appId, reason );

            if ( !ContinueAcquisition ( generation ) )
            {
                return;
            }
        }

        if ( !suspendCovered )
        {
            suspendCovered = AcquireLogind ( appId, reason );

            if ( !ContinueAcquisition ( generation ) )
            {
                return;
            }
        }

        if ( !suspendCovered )
        {
            suspendCovered = AcquirePortal ( INHIBIT_SUSPEND );

            if ( !ContinueAcquisition ( generation ) )
            {
                return;
            }
        }
    }

    if ( idleCovered || suspendCovered )
    {
        qDebug() << "Inhibitors active - idle:" << idleCovered << "suspend:" << suspendCovered;
    }
    else
    {
        qWarning() << "Could not inhibit screensaver or suspend on this desktop";
    }
}

void CActivity::EndActivity()
{
    if ( !m_active )
    {
        return;
    }

    m_active = false;
    ++m_generation;
    ReleaseAll();
}

bool CActivity::ContinueAcquisition ( quint64 generation )
{
    if ( m_active && generation == m_generation )
    {
        return true;
    }

    ReleaseAll();
    return false;
}

bool CActivity::AcquireInhibitor ( const QString&      service,
                                   const QString&      path,
                                   const QString&      interface,
                                   const QString&      method,
                                   const QString&      releaseMethod,
                                   const QVariantList& arguments )
{
    QDBusMessage message = QDBusMessage::createMethodCall ( service, path, interface, method );
    message.setArguments ( arguments );

    const QDBusMessage reply = QDBusConnection::sessionBus().call ( message, QDBus::Block, DBUS_TIMEOUT_MS );

    if ( reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty() )
    {
        qDebug() << "D-Bus inhibit failed for" << service << ":" << reply.errorMessage();
        return false;
    }

    m_inhibitors.append ( SInhibitor{ service, path, interface, releaseMethod, reply.arguments().first().toUInt() } );
    qDebug() << "D-Bus inhibitor acquired via" << service;

    return true;
}

bool CActivity::AcquirePowerManagement ( const QString& appId, const QString& reason )
{
    const QString services[] = { QStringLiteral ( "org.freedesktop.PowerManagement" ),
                                 QStringLiteral ( "org.freedesktop.PowerManagement.Inhibit" ),
                                 QStringLiteral ( "org.xfce.PowerManager" ) };

    for ( const QString& service : services )
    {
        if ( AcquireInhibitor ( service,
                                QStringLiteral ( "/org/freedesktop/PowerManagement/Inhibit" ),
                                QStringLiteral ( "org.freedesktop.PowerManagement.Inhibit" ),
                                QStringLiteral ( "Inhibit" ),
                                QStringLiteral ( "UnInhibit" ),
                                QVariantList() << appId << reason ) )
        {
            return true;
        }
    }

    return false;
}

bool CActivity::AcquirePortal ( uint flags )
{
    QDBusMessage message = QDBusMessage::createMethodCall ( QStringLiteral ( "org.freedesktop.portal.Desktop" ),
                                                            QStringLiteral ( "/org/freedesktop/portal/desktop" ),
                                                            QStringLiteral ( "org.freedesktop.portal.Inhibit" ),
                                                            QStringLiteral ( "Inhibit" ) );
    message << QString() << flags << QVariantMap();

    const QDBusMessage reply = QDBusConnection::sessionBus().call ( message, QDBus::Block, DBUS_TIMEOUT_MS );

    if ( reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty() )
    {
        qDebug() << "Portal inhibit failed:" << reply.errorMessage();
        return false;
    }

    const QString requestPath = reply.arguments().first().value<QDBusObjectPath>().path();

    if ( requestPath.isEmpty() )
    {
        return false;
    }

    m_portalRequests.append ( requestPath );
    qDebug() << "Portal inhibitor acquired for flags" << flags;

    return true;
}

bool CActivity::AcquireLogind ( const QString& appId, const QString& reason )
{
    if ( !QDBusConnection::systemBus().isConnected() )
    {
        return false;
    }

    QDBusMessage message = QDBusMessage::createMethodCall ( QStringLiteral ( "org.freedesktop.login1" ),
                                                            QStringLiteral ( "/org/freedesktop/login1" ),
                                                            QStringLiteral ( "org.freedesktop.login1.Manager" ),
                                                            QStringLiteral ( "Inhibit" ) );
    message << QStringLiteral ( "sleep" ) << appId << reason << QStringLiteral ( "block" );

    const QDBusMessage                        reply = QDBusConnection::systemBus().call ( message, QDBus::Block, DBUS_TIMEOUT_MS );
    const QDBusReply<QDBusUnixFileDescriptor> fdReply ( reply );

    if ( !fdReply.isValid() )
    {
        qDebug() << "logind inhibit failed:" << fdReply.error().message();
        return false;
    }

    m_logindFd = fdReply.value();
    qDebug() << "logind sleep inhibitor acquired";

    return true;
}

void CActivity::ReleaseAll()
{
    for ( const SInhibitor& inhibitor : m_inhibitors )
    {
        QDBusMessage message = QDBusMessage::createMethodCall ( inhibitor.service, inhibitor.path, inhibitor.interface, inhibitor.releaseMethod );
        message << inhibitor.cookie;
        QDBusConnection::sessionBus().call ( message, QDBus::NoBlock );
    }

    m_inhibitors.clear();

    for ( const QString& requestPath : m_portalRequests )
    {
        QDBusMessage message = QDBusMessage::createMethodCall ( QStringLiteral ( "org.freedesktop.portal.Desktop" ),
                                                                requestPath,
                                                                QStringLiteral ( "org.freedesktop.portal.Request" ),
                                                                QStringLiteral ( "Close" ) );
        QDBusConnection::sessionBus().call ( message, QDBus::NoBlock );
    }

    m_portalRequests.clear();

    m_logindFd = QDBusUnixFileDescriptor();
}
