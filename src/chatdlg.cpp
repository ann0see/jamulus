/******************************************************************************\
 * Copyright (c) 2004-2026
 *
 * Author(s):
 *  Volker Fischer
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

#include "chatdlg.h"

#include <QDateTime>
#include <QLocale>
#if QT_VERSION >= QT_VERSION_CHECK( 6, 8, 0 )
#    include <QAccessibleAnnouncementEvent>
#endif

/* Implementation *************************************************************/
namespace
{
// client-controlled sender colours, stable per channel ID (presentation is a
// client concern; the server only sends data)
const char* const astrChatColors[6] = { "mediumblue", "red", "darkorchid", "green", "maroon", "coral" };
}

CChatDlg::CChatDlg ( QWidget* parent ) : CBaseDlg ( parent, Qt::Window ) // use Qt::Window to get min/max window buttons
{
    setupUi ( this );

    // Add help text to controls -----------------------------------------------
    // chat window
    txvChatWindow->setWhatsThis ( "<b>" + tr ( "Chat Window" ) + ":</b> " + tr ( "The chat window shows a history of all chat messages." ) );

    txvChatWindow->setAccessibleName ( tr ( "Chat history" ) );

    // input message text
    edtLocalInputText->setWhatsThis ( "<b>" + tr ( "Input Message Text" ) + ":</b> " +
                                      tr ( "Enter the chat message text in the edit box and press enter to send the "
                                           "message to the server which distributes the message to all connected "
                                           "clients. Your message will then show up in the chat window." ) );

    edtLocalInputText->setAccessibleName ( tr ( "New chat text edit box" ) );

    // clear chat window and edit line
    txvChatWindow->clear();
    edtLocalInputText->clear();

    // we do not want to show a cursor in the chat history
    txvChatWindow->setCursorWidth ( 0 );

    // set a placeholder text to make sure where to type the message in (#384)
    edtLocalInputText->setPlaceholderText ( tr ( "Type a message here" ) );

    // Menu  -------------------------------------------------------------------
    QMenuBar* pMenu     = new QMenuBar ( this );
    QMenu*    pEditMenu = new QMenu ( tr ( "&Edit" ), this );

    pEditMenu->addAction ( tr ( "Cl&ear Chat History" ), this, SLOT ( OnClearChatHistory() ), QKeySequence ( Qt::CTRL + Qt::Key_E ) );

    pMenu->addMenu ( pEditMenu );
#if defined( Q_OS_IOS )
    QAction* closeAction = pMenu->addAction ( tr ( "&Close" ) );
#endif

#if defined( ANDROID ) || defined( Q_OS_ANDROID )
    pEditMenu->addAction ( tr ( "&Close" ), this, SLOT ( OnCloseClicked() ), QKeySequence ( Qt::CTRL + Qt::Key_W ) );
#endif

    // Now tell the layout about the menu
    layout()->setMenuBar ( pMenu );

    // Connections -------------------------------------------------------------
    QObject::connect ( edtLocalInputText, &QLineEdit::textChanged, this, &CChatDlg::OnLocalInputTextTextChanged );

    QObject::connect ( butSend, &QPushButton::clicked, this, &CChatDlg::OnSendText );

    QObject::connect ( txvChatWindow, &QTextBrowser::anchorClicked, this, &CChatDlg::OnAnchorClicked );

#if defined( Q_OS_IOS )
    QObject::connect ( closeAction, &QAction::triggered, this, &CChatDlg::OnCloseClicked );
#endif
}

void CChatDlg::OnLocalInputTextTextChanged ( const QString& strNewText )
{
    // check and correct length
    if ( strNewText.length() > MAX_LEN_CHAT_TEXT )
    {
        // text is too long, update control with shortened text
        edtLocalInputText->setText ( strNewText.left ( MAX_LEN_CHAT_TEXT ) );
    }
}

void CChatDlg::OnSendText()
{
    // send new text and clear line afterwards, do not send an empty message
    if ( !edtLocalInputText->text().isEmpty() )
    {
        emit NewLocalInputText ( edtLocalInputText->text() );
        edtLocalInputText->clear();
    }
}

void CChatDlg::OnClearChatHistory()
{
    // clear chat window
    txvChatWindow->clear();
}

void CChatDlg::AddChatText ( QString strChatText )
{
    // legacy (message 18) path: the server sent already-escaped HTML; we only
    // linkify bare http(s):// URLs, the text itself is never re-interpreted
    LinkifyURLs ( strChatText );

    AnnounceNewChatMessage ( strChatText );

    // add new text in chat window
    txvChatWindow->append ( strChatText );
}

void CChatDlg::AddChatMessage ( const ChatMessage& message )
{
    // announce the plain content (sender and text) to screen readers
    QString strAnnouncement;
    if ( !message.senderName.isEmpty() )
    {
        strAnnouncement = message.senderName + ": " + message.text;
    }
    else
    {
        strAnnouncement = message.text;
    }
    AnnounceNewChatMessage ( strAnnouncement );

    // add new structured message in chat window
    txvChatWindow->append ( FormatChatMessage ( message ) );
}

void CChatDlg::AnnounceNewChatMessage ( const QString& strAnnouncement )
{
#if QT_VERSION >= QT_VERSION_CHECK( 6, 8, 0 )
    // prefer a proper live region announcement over the value-change event
    QAccessible::updateAccessibility ( new QAccessibleAnnouncementEvent ( txvChatWindow, strAnnouncement ) );
#else
    QAccessible::updateAccessibility ( new QAccessibleValueChangeEvent ( txvChatWindow, strAnnouncement ) );
#endif
}

QString CChatDlg::FormatChatMessage ( const ChatMessage& message ) const
{
    // the client supplies all presentation: local, locale-aware time, a stable
    // per-channel sender colour and escaped plain text; user data is escaped so
    // that it is never interpreted as HTML
    const QString strTime = QLocale().toString ( QDateTime::fromSecsSinceEpoch ( message.timestamp ).toLocalTime().time(),
                                                 QLocale::ShortFormat );

    QString strSenderName = message.senderName;
    if ( strSenderName.isEmpty() )
    {
        // server/RPC-originated messages carry the wire sentinel channel ID and
        // no sender name; unknown channels get a neutral placeholder
        strSenderName = ( message.channelId == SERVER_CHAT_CHANNEL_ID ) ? tr ( "Server" ) : tr ( "Unknown" );
    }

    const QString sCurColor = astrChatColors[message.channelId % 6];

    const QString strHeader =
        "<font color=\"" + sCurColor + "\">(" + strTime + ") <b>" + strSenderName.toHtmlEscaped() + "</b></font> ";

    return strHeader + EscapeAndLinkifyText ( message.text );
}

void CChatDlg::OnAnchorClicked ( const QUrl& Url )
{
    // only allow http(s) URLs to be opened in an external browser
    if ( Url.scheme() == QLatin1String ( "https" ) || Url.scheme() == QLatin1String ( "http" ) )
    {
        if ( QMessageBox::question ( this,
                                     APP_NAME,
                                     tr ( "Do you want to open the link '%1' in your browser?" ).arg ( "<b>" + Url.toString() + "</b>" ),
                                     QMessageBox::Yes | QMessageBox::No ) == QMessageBox::Yes )
        {
            QDesktopServices::openUrl ( Url );
        }
    }
}

#if defined( Q_OS_IOS ) || defined( ANDROID ) || defined( Q_OS_ANDROID )
void CChatDlg::OnCloseClicked()
{
    // on mobile add a close button or menu entry
#    if defined( Q_OS_IOS )
    // On Qt6, iOS crashes if we call close() due to unknown reasons, therefore we just hide() the dialog. A Qt bug is suspected.
    // Checkout https://github.com/jamulussoftware/jamulus/pull/3413
    hide();
#    endif
#    if defined( ANDROID ) || defined( Q_OS_ANDROID )
    close();
#    endif
}
#endif