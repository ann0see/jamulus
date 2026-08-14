/******************************************************************************\
 * Standalone protocol tests for the redesigned chat message (Slice 1).
 *
 * Covers message 37 (structured chat text) create/evaluate round-trips and
 * decode reject rules, plus the 38/39 capability handshake.
 *
 * Build (from the tests/chatprotocol directory):
 *   qmake && nmake release   (or mingw32-make)
 * Run:
 *   release/chatprotocoltest
\******************************************************************************/

#include <cstdio>
#include <cstdint>

#include <QCoreApplication>
#include <QObject>
#include <QVector>
#include <QDateTime>

#include "protocol.h"
#include "util.h"
#include "chatmessage.h"

/* -------------------------------------------------------------------------- */
// expose the protected stream helpers so that malformed bodies can be crafted
class CProtocolExposed : public CProtocol
{
public:
    using CProtocol::GetValFromStream;
    using CProtocol::PutStringUTF8OnStream;
    using CProtocol::PutValOnStream;
};

/* -------------------------------------------------------------------------- */
// simple test framework
static int iNumTests    = 0;
static int iNumFailures = 0;

static void Check ( const bool bCondition, const char* strTestName )
{
    iNumTests++;
    if ( bCondition )
    {
        std::printf ( "ok:   %s\n", strTestName );
    }
    else
    {
        iNumFailures++;
        std::printf ( "FAIL: %s\n", strTestName );
    }
}

/* -------------------------------------------------------------------------- */
// protocol harness: captures sent frames and received signals
struct SChatReceived
{
    uint8_t  iChannelID;
    uint32_t iTimestamp;
    QString  strSenderName;
    QString  strChatText;
};

class CProtocolHarness
{
public:
    CProtocolExposed            Prot;
    QVector<CVector<uint8_t>>   vecSentFrames;
    QVector<SChatReceived>      vecChatReceived;
    int                         iReqChatTextSupport;
    int                         iChatTextSupported;

    CProtocolHarness()
    {
        iReqChatTextSupport = 0;
        iChatTextSupported  = 0;

        QObject::connect ( &Prot, &CProtocol::MessReadyForSending,
                           [this] ( CVector<uint8_t> vecMessage ) { vecSentFrames.append ( vecMessage ); } );

        QObject::connect ( &Prot, &CProtocol::ChatTextChannelReceived,
                           [this] ( uint8_t iChannelID, uint32_t iTimestamp, QString strSenderName, QString strChatText ) {
                               vecChatReceived.append ( SChatReceived{ iChannelID, iTimestamp, strSenderName, strChatText } );
                           } );

        QObject::connect ( &Prot, &CProtocol::ReqChatTextSupport, [this]() { iReqChatTextSupport++; } );

        QObject::connect ( &Prot, &CProtocol::ChatTextSupported, [this]() { iChatTextSupported++; } );
    }

    void Reset()
    {
        vecSentFrames.clear();
        vecChatReceived.clear();
        iReqChatTextSupport = 0;
        iChatTextSupported  = 0;
        Prot.Reset();
    }
};

// Deliver every frame Tx emitted (immediately or after the simulated ACK
// handshake, which is required to drain split messages) to Rx, so that Rx
// fully reassembles split messages and evaluates them.
static void DeliverAll ( CProtocolHarness& Tx, CProtocolHarness& Rx )
{
    int iTxFrameIdx = 0;
    int iRxFrameIdx = 0;

    while ( true )
    {
        bool bProgress = false;

        // deliver any new frames from Tx to Rx
        for ( ; iTxFrameIdx < Tx.vecSentFrames.size(); iTxFrameIdx++ )
        {
            CVector<uint8_t> vecBody;
            int              iCnt = 0;
            int              iID  = 0;

            if ( CProtocol::ParseMessageFrame ( Tx.vecSentFrames[iTxFrameIdx], Tx.vecSentFrames[iTxFrameIdx].Size(), vecBody, iCnt, iID ) )
            {
                continue;
            }
            Rx.Prot.ParseMessageBody ( vecBody, iCnt, iID );
            bProgress = true;
        }

        // feed ACKs Rx emitted back to Tx (each ACK drains one queued message)
        for ( ; iRxFrameIdx < Rx.vecSentFrames.size(); iRxFrameIdx++ )
        {
            CVector<uint8_t> vecBody;
            int              iCnt = 0;
            int              iID  = 0;

            if ( CProtocol::ParseMessageFrame ( Rx.vecSentFrames[iRxFrameIdx], Rx.vecSentFrames[iRxFrameIdx].Size(), vecBody, iCnt, iID ) )
            {
                continue;
            }
            if ( iID == PROTMESSID_ACKN )
            {
                Tx.Prot.ParseMessageBody ( vecBody, iCnt, iID );
                bProgress = true;
            }
        }

        if ( !bProgress )
        {
            break;
        }
    }
}

/* -------------------------------------------------------------------------- */
int main ( int argc, char* argv[] )
{
    QCoreApplication app ( argc, argv );

    CProtocolHarness Tx;
    CProtocolHarness Rx;

    // --- message 37 round-trips (non-split) --------------------------------
    {
        const uint8_t  iChannelID = 7;
        const uint32_t iTimestamp = 1786298460U;
        const QString  strName    = "Alice";
        const QString  strText    = "hello";

        Tx.Prot.CreateChatTextChannelMes ( iChannelID, iTimestamp, strName, strText );
        DeliverAll ( Tx, Rx );

        Check ( Rx.vecChatReceived.size() == 1, "37 round-trip: exactly one message received" );
        if ( Rx.vecChatReceived.size() == 1 )
        {
            Check ( Rx.vecChatReceived[0].iChannelID == iChannelID, "37 round-trip: channel ID" );
            Check ( Rx.vecChatReceived[0].iTimestamp == iTimestamp, "37 round-trip: timestamp" );
            Check ( Rx.vecChatReceived[0].strSenderName == strName, "37 round-trip: sender name" );
            Check ( Rx.vecChatReceived[0].strChatText == strText, "37 round-trip: text" );
        }
        Tx.Reset();
        Rx.Reset();
    }

    // UTF-8 text and name
    {
        const uint8_t  iChannelID = 3;
        const uint32_t iTimestamp = 1700000000U;
        const QString  strName    = QString::fromUtf8 ( "\xc3\x9c""n\xc3\xaf""code \xe2\x98\xba" );    // "Ünïcode ☺"
        const QString  strText    = QString::fromUtf8 ( "h\xc3\xa9""llo w\xc3\xb6""rld \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e" ); // "héllo wörld 日本語"

        Tx.Prot.CreateChatTextChannelMes ( iChannelID, iTimestamp, strName, strText );
        DeliverAll ( Tx, Rx );

        Check ( Rx.vecChatReceived.size() == 1, "37 UTF-8: message received" );
        if ( Rx.vecChatReceived.size() == 1 )
        {
            Check ( Rx.vecChatReceived[0].iChannelID == iChannelID, "37 UTF-8: channel ID" );
            Check ( Rx.vecChatReceived[0].iTimestamp == iTimestamp, "37 UTF-8: timestamp" );
            Check ( Rx.vecChatReceived[0].strSenderName == strName, "37 UTF-8: sender name" );
            Check ( Rx.vecChatReceived[0].strChatText == strText, "37 UTF-8: text" );
        }
        Tx.Reset();
        Rx.Reset();
    }

    // empty name and empty text
    {
        Tx.Prot.CreateChatTextChannelMes ( 0, 0U, QString(), QString() );
        DeliverAll ( Tx, Rx );

        Check ( Rx.vecChatReceived.size() == 1, "37 empty: message received" );
        if ( Rx.vecChatReceived.size() == 1 )
        {
            Check ( Rx.vecChatReceived[0].iChannelID == 0, "37 empty: channel ID" );
            Check ( Rx.vecChatReceived[0].iTimestamp == 0U, "37 empty: timestamp" );
            Check ( Rx.vecChatReceived[0].strSenderName.isEmpty(), "37 empty: sender name" );
            Check ( Rx.vecChatReceived[0].strChatText.isEmpty(), "37 empty: text" );
        }
        Tx.Reset();
        Rx.Reset();
    }

    // boundary channel IDs (0, 149 and the 255 server/RPC sentinel)
    {
        const uint8_t aiChannelIDs[3] = { 0, MAX_NUM_CHANNELS - 1, SERVER_CHAT_CHANNEL_ID };
        const char*   astrChannelNames[3] = { "channel 0", "channel 149", "channel 255" };

        for ( int i = 0; i < 3; i++ )
        {
            Tx.Prot.CreateChatTextChannelMes ( aiChannelIDs[i], 100U, "n", "t" );
            DeliverAll ( Tx, Rx );

            Check ( ( Rx.vecChatReceived.size() == 1 ) && ( Rx.vecChatReceived[0].iChannelID == aiChannelIDs[i] ),
                    astrChannelNames[i] );
            Tx.Reset();
            Rx.Reset();
        }
    }

    // maximum length text (MAX_LEN_CHAT_TEXT == 1600), sent unsplit
    {
        const QString strText ( MAX_LEN_CHAT_TEXT, QChar ( 'a' ) );
        Tx.Prot.CreateChatTextChannelMes ( 1, 100U, "n", strText );
        DeliverAll ( Tx, Rx );

        Check ( ( Rx.vecChatReceived.size() == 1 ) && ( Rx.vecChatReceived[0].strChatText == strText ),
                "37 max length text round-trip" );
        Tx.Reset();
        Rx.Reset();
    }

    // large text which takes the split message path (body > MESS_SPLIT_PART_SIZE_BYTES)
    {
        Tx.Prot.SetSplitMessageSupported ( true );
        const QString strText ( MAX_LEN_CHAT_TEXT, QChar ( 'x' ) );
        Tx.Prot.CreateChatTextChannelMes ( 5, 200U, "split-test", strText );
        DeliverAll ( Tx, Rx );

        Check ( Rx.vecChatReceived.size() == 1, "37 split: message reassembled" );
        if ( Rx.vecChatReceived.size() == 1 )
        {
            Check ( Rx.vecChatReceived[0].iChannelID == 5, "37 split: channel ID" );
            Check ( Rx.vecChatReceived[0].iTimestamp == 200U, "37 split: timestamp" );
            Check ( Rx.vecChatReceived[0].strSenderName == "split-test", "37 split: sender name" );
            Check ( Rx.vecChatReceived[0].strChatText == strText, "37 split: text" );
        }
        Tx.Reset();
        Rx.Reset();
    }

    // --- ChatMessage data model (Slice 2) -----------------------------------
    {
        // a ChatMessage carries the wire facts verbatim: channel ID, epoch
        // timestamp, the sender name snapshot and plain text (never HTML)
        ChatMessage msg{ 12, 1786298460U, "Alice", "<b>hello</b> & <script>alert(1)</script>" };

        Check ( msg.channelId == 12, "ChatMessage: channel ID" );
        Check ( msg.timestamp == 1786298460U, "ChatMessage: timestamp" );
        Check ( msg.senderName == "Alice", "ChatMessage: sender name" );
        Check ( msg.text == "<b>hello</b> & <script>alert(1)</script>", "ChatMessage: text is plain data, not markup" );

        // text never silently changes representation between wire and model
        const QString strText = QString::fromUtf8 ( "h\xc3\xa9""llo w\xc3\xb6""rld \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e" ); // "héllo wörld 日本語"
        msg.text = strText;
        Check ( msg.text == strText, "ChatMessage: UTF-8 text preserved" );

        // the wire fields of a message 37 round-trip map 1:1 into a ChatMessage
        {
            const uint8_t  iChannelID = 9;
            const uint32_t iTimestamp = 1786300000U;
            const QString  strName    = "Bob";
            const QString  strText    = "round trip";

            Tx.Prot.CreateChatTextChannelMes ( iChannelID, iTimestamp, strName, strText );
            DeliverAll ( Tx, Rx );

            bool bMapped = ( Rx.vecChatReceived.size() == 1 );
            if ( bMapped )
            {
                const ChatMessage msgRoundTrip{ Rx.vecChatReceived[0].iChannelID,
                                                Rx.vecChatReceived[0].iTimestamp,
                                                Rx.vecChatReceived[0].strSenderName,
                                                Rx.vecChatReceived[0].strChatText };
                bMapped = ( msgRoundTrip.channelId == iChannelID ) && ( msgRoundTrip.timestamp == iTimestamp ) &&
                          ( msgRoundTrip.senderName == strName ) && ( msgRoundTrip.text == strText );
            }
            Check ( bMapped, "ChatMessage: wire fields map 1:1 into the data model" );
            Tx.Reset();
            Rx.Reset();
        }

        // server/RPC-originated messages carry the wire sentinel channel ID
        {
            const ChatMessage msgServer{ SERVER_CHAT_CHANNEL_ID, 100U, QString(), "server note" };
            Check ( msgServer.channelId == 255, "ChatMessage: server/RPC sentinel channel ID" );
            Check ( msgServer.senderName.isEmpty(), "ChatMessage: server/RPC messages have no sender name" );
        }
    }

    // --- safe chat text rendering (Slice 3) ---------------------------------
    // invariant: user text is escaped before linkification and can never become
    // executable or unintended HTML
    {
        // markup is escaped, never interpreted
        const QString strEscapedBold = EscapeAndLinkifyText ( "<b>hello</b>" );
        Check ( !strEscapedBold.contains ( "<b>" ) && strEscapedBold.contains ( "&lt;b&gt;hello&lt;/b&gt;" ),
                "safe render: <b> is escaped" );

        const QString strEscapedImg = EscapeAndLinkifyText ( "<img src=x>" );
        Check ( !strEscapedImg.contains ( "<img" ) && strEscapedImg.contains ( "&lt;img src=x&gt;" ),
                "safe render: <img> is escaped" );

        const QString strEscapedScript = EscapeAndLinkifyText ( "<script>alert(1)</script>" );
        Check ( !strEscapedScript.contains ( "<script" ) && strEscapedScript.contains ( "&lt;script&gt;alert(1)&lt;/script&gt;" ),
                "safe render: <script> is escaped" );

        // bare http(s) URLs are wrapped after escaping
        const QString strLinked = EscapeAndLinkifyText ( "see https://example.com now" );
        Check ( strLinked.contains ( "<a href=\"https://example.com\">https://example.com</a>" ),
                "safe render: bare URL is linkified" );

        // terminating punctuation is excluded from the link
        const QString strPunct = EscapeAndLinkifyText ( "https://example.com." );
        Check ( strPunct.contains ( "<a href=\"https://example.com\">https://example.com</a>." ),
                "safe render: trailing punctuation excluded from link" );

        // query-string URLs survive HTML-escaping and stay whole (the & becomes
        // &amp;, which must be kept inside the link so the href is not truncated)
        const QString strQuery = EscapeAndLinkifyText ( "watch https://example.com/watch?v=X&t=5 now" );
        Check ( strQuery.contains ( "<a href=\"https://example.com/watch?v=X&amp;t=5\">https://example.com/watch?v=X&amp;t=5</a>" ),
                "safe render: query-string URL kept whole" );

        // a bare & in text is escaped and does not swallow surrounding words
        const QString strAmp = EscapeAndLinkifyText ( "a & b then https://x.com/?y=1&z=2" );
        Check ( strAmp.contains ( "a &amp; b" ) && strAmp.contains ( "<a href=\"https://x.com/?y=1&amp;z=2\">https://x.com/?y=1&amp;z=2</a>" ),
                "safe render: entity boundary respected in plain text" );

        // user-authored HTML around a URL is escaped, then the URL is linkified
        const QString strMixed = EscapeAndLinkifyText ( "<a href=\"https://example.com\">x</a>" );
        Check ( strMixed.contains ( "&lt;a href=&quot;" ) && strMixed.contains ( "<a href=\"https://example.com\">https://example.com</a>" ),
                "safe render: HTML+URL combination stays escaped" );

        // legacy path: LinkifyURLs leaves already-generated HTML tags intact
        QString strLegacy = "<font color=\"red\">(time) <b>name</b></font> https://example.com";
        LinkifyURLs ( strLegacy );
        Check ( strLegacy.contains ( "<font color=\"red\">" ) && strLegacy.contains ( "<b>name</b>" ) &&
                    strLegacy.contains ( "<a href=\"https://example.com\">https://example.com</a>" ),
                "safe render: legacy HTML is preserved and URLs linkified" );
    }

    // --- message 37 decode reject rules ------------------------------------
    // text over MAX_LEN_CHAT_TEXT
    {
        CProtocolExposed Tmp;
        CVector<uint8_t> vecBody ( 1 + 4 + 2 + 4 + 2 + ( MAX_LEN_CHAT_TEXT + 1 ) );
        int              iPos = 0;
        Tmp.PutValOnStream ( vecBody, iPos, 1, 1 );
        Tmp.PutValOnStream ( vecBody, iPos, 100U, 4 );
        Tmp.PutStringUTF8OnStream ( vecBody, iPos, QByteArray ( "name" ) );
        Tmp.PutStringUTF8OnStream ( vecBody, iPos, QByteArray ( MAX_LEN_CHAT_TEXT + 1, 'a' ) );

        Rx.Prot.ParseMessageBody ( vecBody, 0, PROTMESSID_CHAT_TEXT_CHANNEL );
        Check ( Rx.vecChatReceived.size() == 0, "37 reject: text over MAX_LEN_CHAT_TEXT" );
        Rx.Reset();
    }

    // name over MAX_LEN_FADER_TAG
    {
        CProtocolExposed Tmp;
        CVector<uint8_t> vecBody ( 1 + 4 + 2 + ( MAX_LEN_FADER_TAG + 1 ) + 2 + 3 );
        int              iPos = 0;
        Tmp.PutValOnStream ( vecBody, iPos, 1, 1 );
        Tmp.PutValOnStream ( vecBody, iPos, 100U, 4 );
        Tmp.PutStringUTF8OnStream ( vecBody, iPos, QByteArray ( MAX_LEN_FADER_TAG + 1, 'n' ) );
        Tmp.PutStringUTF8OnStream ( vecBody, iPos, QByteArray ( "abc" ) );

        Rx.Prot.ParseMessageBody ( vecBody, 0, PROTMESSID_CHAT_TEXT_CHANNEL );
        Check ( Rx.vecChatReceived.size() == 0, "37 reject: name over MAX_LEN_FADER_TAG" );
        Rx.Reset();
    }

    // truncated body (channel ID and timestamp present but too short overall)
    {
        CProtocolExposed Tmp;
        CVector<uint8_t> vecBody ( 1 + 3 );
        int              iPos = 0;
        Tmp.PutValOnStream ( vecBody, iPos, 1, 1 );
        Tmp.PutValOnStream ( vecBody, iPos, 0x1234U, 3 ); // truncated timestamp

        Rx.Prot.ParseMessageBody ( vecBody, 0, PROTMESSID_CHAT_TEXT_CHANNEL );
        Check ( Rx.vecChatReceived.size() == 0, "37 reject: truncated body" );
        Rx.Reset();
    }

    // malformed text length (claims more bytes than available)
    {
        CProtocolExposed Tmp;
        CVector<uint8_t> vecBody ( 1 + 4 + 2 + 4 + 2 + 10 );
        int              iPos = 0;
        Tmp.PutValOnStream ( vecBody, iPos, 1, 1 );
        Tmp.PutValOnStream ( vecBody, iPos, 100U, 4 );
        Tmp.PutStringUTF8OnStream ( vecBody, iPos, QByteArray ( "name" ) );
        Tmp.PutValOnStream ( vecBody, iPos, 100, 2 ); // text length claims 100 bytes
        for ( int i = 0; i < 10; i++ )
        {
            Tmp.PutValOnStream ( vecBody, iPos, 'x', 1 ); // only 10 bytes present
        }

        Rx.Prot.ParseMessageBody ( vecBody, 0, PROTMESSID_CHAT_TEXT_CHANNEL );
        Check ( Rx.vecChatReceived.size() == 0, "37 reject: malformed text length" );
        Rx.Reset();
    }

    // trailing data after the chat text
    {
        CProtocolExposed Tmp;
        CVector<uint8_t> vecBody ( 1 + 4 + 2 + 4 + 2 + 3 + 1 );
        int              iPos = 0;
        Tmp.PutValOnStream ( vecBody, iPos, 1, 1 );
        Tmp.PutValOnStream ( vecBody, iPos, 100U, 4 );
        Tmp.PutStringUTF8OnStream ( vecBody, iPos, QByteArray ( "name" ) );
        Tmp.PutStringUTF8OnStream ( vecBody, iPos, QByteArray ( "abc" ) );
        Tmp.PutValOnStream ( vecBody, iPos, 0, 1 ); // trailing byte

        Rx.Prot.ParseMessageBody ( vecBody, 0, PROTMESSID_CHAT_TEXT_CHANNEL );
        Check ( Rx.vecChatReceived.size() == 0, "37 reject: trailing data" );
        Rx.Reset();
    }

    // --- capability handshake 38/39 ----------------------------------------
    {
        Tx.Prot.CreateReqChatTextSupportMes();
        DeliverAll ( Tx, Rx );
        Check ( Rx.iReqChatTextSupport == 1, "38: ReqChatTextSupport received" );
        Tx.Reset();
        Rx.Reset();
    }
    {
        Tx.Prot.CreateChatTextSupportedMes();
        DeliverAll ( Tx, Rx );
        Check ( Rx.iChatTextSupported == 1, "39: ChatTextSupported received" );
        Tx.Reset();
        Rx.Reset();
    }

    // unknown message IDs are acknowledged but not evaluated (no silent loss)
    {
        Rx.Prot.ParseMessageBody ( CVector<uint8_t> ( 0 ), 0, 40 );
        Check ( Rx.vecChatReceived.size() == 0, "unknown message: not evaluated" );

        bool bAckSent = false;
        for ( const CVector<uint8_t>& vecFrame : Rx.vecSentFrames )
        {
            CVector<uint8_t> vecBody;
            int              iCnt = 0;
            int              iID  = 0;
            if ( !CProtocol::ParseMessageFrame ( vecFrame, vecFrame.Size(), vecBody, iCnt, iID ) && ( iID == PROTMESSID_ACKN ) )
            {
                bAckSent = true;
                break;
            }
        }
        Check ( bAckSent, "unknown message: acknowledged unconditionally" );
        Rx.Reset();
    }

    // --- summary -----------------------------------------------------------
    std::printf ( "\n%d tests, %d failure(s)\n", iNumTests, iNumFailures );

    return ( iNumFailures == 0 ) ? 0 : 1;
}
