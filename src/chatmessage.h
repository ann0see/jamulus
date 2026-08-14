/******************************************************************************\
 * Copyright (c) 2026
 *
 * This file is part of Jamulus.
 *
 * Author(s):
 *  Jamulus contributors
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
 \******************************************************************************/

#pragma once

#include <QMetaType>
#include <QString>

#include <cstdint>

// Semantic chat message data as carried by protocol message 37. Contains
// facts, not presentation markup: the sender name is the wire snapshot taken
// at server fan-out, and the channel ID serves filtering/muting rather than
// identity. SERVER_CHAT_CHANNEL_ID (255) marks server/RPC-originated messages.
struct ChatMessage
{
    uint8_t  channelId;  // server channel ID, or SERVER_CHAT_CHANNEL_ID (255) for server/RPC messages
    uint32_t timestamp;  // epoch seconds (UTC), stamped at the server
    QString  senderName; // sender name snapshotted on the wire (empty for server/RPC messages)
    QString  text;       // plain UTF-8 chat text, never HTML
};

Q_DECLARE_METATYPE ( ChatMessage )
