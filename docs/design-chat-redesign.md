# Redesigned Chat Message — Design Document (v2)

Status: Draft (for review) · Date: 2026-08-09 · Scope: Protocol, server, client, JSON-RPC, accessibility, compatibility, tests

Incorporates the revised brief. Resolves open decisions A–E and replaces the earlier version-based capability gate with an explicit in-session handshake.

## 1. Objective

Normal chat messages contain **data, not presentation markup**. The server supplies facts (channel ID, timestamp, sender name, plain UTF-8 text); the client supplies presentation (timezone/locale/colours/typography/a11y/URLs/filtering/mute).

```text
Client chat input → Server → legacy client → message 18 (legacy HTML)
                          └─ structured client → message 37 (data) → ChatMessage → Client UI
```

**Invariant: normal user chat text is never interpreted as HTML by the receiver.** Compatibility with existing clients and JSON-RPC semantics preserved where practical.

## 2. Problems solved

1. `PROTMESSID_CHAT_TEXT` (18) carries a server-generated HTML display string (`src/server.cpp:1352-1367`).
2. Server picks colours (palette `src/server.cpp:150-156`) and time formatting.
3. Desktop themes cannot control server HTML (issue #3740).
4. Accessibility constrained by the rich-text model.
5. RPC injection can bypass escaping (`src/serverrpc.cpp:112`, `:132`).
6. URL linkification done via HTML manipulation (`src/chatdlg.cpp:142-157`).
7. Inconsistent length limits (1600 vs 1800 vs unclamped).
8. Welcome coupled to raw-HTML mechanism (`src/clientdlg.cpp:887`).
9. Client JSON-RPC exposes server HTML (`src/clientrpc.cpp:54-62`).
10. Per-channel mute/filtering need structured source info.

## 3. Design decisions (resolved)

| Question | Decision |
| --- | --- |
| Wire format | Channel ID + uint32 epoch-seconds + sender name + UTF-8 text |
| Compatibility | Dual-format: 37 to capable clients, legacy HTML 18 to others |
| **A. Capability** | **Explicit in-session handshake** (new IDs 38/39), not version guessing (§4) |
| Timestamp | Epoch UTC stamped at fan-out; client renders local, locale-aware short format |
| **B. Sender identity** | **Name snapshotted onto the wire at fan-out** (§5.2) — history stable across renames/channel reuse |
| Welcome | Classified **administrative rich-content message**; raw HTML retained (§9) |
| **C. Chat UI** | **Evaluate** `QListWidget` vs `QListView`+model+delegate vs retained `QTextBrowser`; list-based preferred, `QTextBrowser` lowest (§12) |
| URL linkification | Preserved; escape-then-linkify of bare `http(s)://`; confirm-before-open stays |
| Old-format parsing in client | Kept for the compatibility window (new client ↔ old server) |
| **E. Client RPC** | **Structured** `{channelId, timestamp, text, senderName}`, breaking, changelog-flagged (§15.2) |
| Order | Slice 1 protocol+capability+server, Slice 2 client data/model, Slice 3 client UI (§18) |

## 4. Capability negotiation (decision A)

**Explicit in-session handshake, not version numbers** (forks, backports, dev builds, unusual version strings make thresholds unreliable). Version-gate failure mode — server sends 37 to a client that cannot parse it → message silently lost — is unacceptable because unknown IDs are ACKed unconditionally (`src/protocol.cpp:806-887`, `CreateAndImmSendAcknMess` at `:887`).

Model on the split handshake (`PROTMESSID_REQ_SPLIT_MESS_SUPPORT` 34 / `PROTMESSID_SPLIT_MESS_SUPPORTED` 35, `src/protocol.h:85-86`):

- Server sends `REQ_CHAT_TEXT_SUPPORT` (38, server→client) during connection bootstrap, mirroring `CreateReqSplitMessSupportMes()` at `src/server.cpp:417`.
- Client replies `CHAT_TEXT_SUPPORTED` (39, client→server), mirroring `src/channel.cpp:500-505`.
- IDs 38/39 are free (body 0–36; CLM 1000–1999; split 2001).
- Store per-connection `bSupportsStructuredChat` on `CChannel`. Today nothing is retained about client capability: `CChannel::OnVersionAndOSReceived` (`src/channel.cpp:171`) only sets `bUseSequenceNumber` internally; its signal is **not** connected to any server slot (`connectChannelSignalsToServerSlots`, `src/server.cpp:328-362`). Needed: stored member + new signal + server connection.
- **Default `false` until confirmed → legacy 18.** Safe failure = "unknown/new client → legacy", never silent loss. A chat before the capability reply is a benign race handled by the legacy default.
- No generalized capability framework — narrowly scoped to structured chat.

## 5. Protocol message 37

### 5.1 ID

```cpp
#define PROTMESSID_CHAT_TEXT_CHANNEL 37 // chat text with source channel ID, timestamp and sender name
```

Next free body slot. **Server→client only:** clients keep sending chat as legacy 18 (they don't reliably know their server channel ID; the server stamps time and name). Server is the only producer.

### 5.2 Wire format

```text
| 1 byte channel ID | 4 bytes uint32 timestamp | 2 bytes name len | n bytes name | 2 bytes text len | m bytes text |
```

- Little-endian per `PutValOnStream`/`GetValFromStream` (`src/protocol.cpp:2976-3006`; `uint32_t`-based, assert `iNumOfBytes <= 4` at `:2976`, `:2826` — a uint64 would need new helpers). uint32 epoch-seconds fits directly; unambiguous until 2106.
- Name/text length fields are **UTF-8 byte counts** (2-byte) via existing stream helpers (`:2992`, `:2846`).
- **Sender name snapshotted at fan-out** from `vecChannels[iCurChanID].GetName()` (`src/server.cpp:1356`). RPC/255 → empty name → client placeholder. Decode cap `MAX_LEN_FADER_TAG` (16), consistent with CONN_CLIENTS_LIST names (`src/protocol.cpp:1268`, `global.h:284`).
- Timestamp: `QDateTime::currentSecsSinceEpoch()` (UTC) at fan-out.

### 5.3 Text length semantics (decision D)

`MAX_LEN_CHAT_TEXT == 1600` = **QString::size(), UTF-16 code units**, enforced **post-decode** by `GetStringFromStream` (`strOut.size() > iMaxStringLen`, `src/protocol.cpp:2881`); the wire length is the **UTF-8 byte count**. The client input clamp uses the same unit (`src/chatdlg.cpp:111`) — semantics are self-consistent; the new decoder must reuse the same `size()` check.

- Reject on: inconsistent length, truncation (name/timestamp/text), decoded text over limit, trailing bytes (`iPos != vecData.Size()`, as `src/protocol.cpp:1430-1433`).
- `MAX_LEN_CHAT_TEXT_PLUS_HTML` (1800) no longer needed for message 37.
- Max body ≈ 1+4+2+≤48 (name 16×3)+2+≤4800 (text 1600×3 bytes/UTF-16 unit) ≈ **4857 bytes** — ~9 split parts, within 36-part capacity (`MAX_NUM_MESS_SPLIT_PARTS = MAX_SIZE_BYTES_NETW_BUF/550`, `src/protocol.h:126`, `global.h:169`).
- Non-trivial message 37 always takes the split path (`src/protocol.cpp:584`, `:590`) — already true for legacy chat; all current clients negotiate split. Created via `CreateAndSendMessage`; client parses the reassembled body like legacy chat.

## 6. Channel IDs and sentinels

- The 1-byte field carries the **server channel ID** advertised in `PROTMESSID_CONN_CLIENTS_LIST` (`src/protocol.cpp:1213`, `CChannelInfo.iChanID`). The server channel index *is* the ID (`CreateChannelList` passes the index straight into `CChannelInfo`, `src/server.cpp:1319`) — no index→ID mapping exists or is needed.
- **Valid IDs `0..MAX_NUM_CHANNELS-1`** (`MAX_NUM_CHANNELS = 150`). **ID 0 is a real client and is never a sentinel.**
- **`255` (0xFF) is the server/JSON-RPC sentinel** for RPC `broadcastChatMessage`/`privateChatMessage`: outside 0..149, fits the byte. `INVALID_CLIENT_ID = -1` (`src/serverrpc.cpp:51`) doesn't fit a byte and stays internal-only.
- Client reject rule: **"reject > 149 except 255"**, not "reject > MAX_NUM_CHANNELS" (which would wrongly admit 150..254).
- Mapping: JSON-RPC `-1` → internal `INVALID_CLIENT_ID` → wire `255`, converted only at the fan-out boundary.

## 7. Server fan-out

Client chat + both RPC paths converge on **one** branchy fan-out. Today: `CreateAndSendChatTextForAllConChannels` (`src/server.cpp:1352-1367`, formats + fans out; client chat only) vs `SendChatTextToAllConChannels` (`src/server.cpp:1369-1381`, raw unescaped; called by RPC, `src/serverrpc.cpp:112`). Merge the latter into the former.

1. **Clamp** to `MAX_LEN_CHAT_TEXT` up front — today's fan-out never clamps (client UI clamps; welcome at `src/server.cpp:1655`; RPC-private at `src/serverrpc.cpp:125`; RPC-broadcast and client-sourced do not).
2. **Stamp** `QDateTime::currentSecsSinceEpoch()` (UTC).
3. **Determine source ID before indexing `vecChannels`**: use `iCurChanID` directly for client chat; map RPC/`INVALID_CLIENT_ID` to `255`. **Guard: sentinel/-1 must short-circuit before `vecChannels[iCurChanID].GetName()` / `vstrChatColors[iCurChanID % 6]` (`src/server.cpp:1356`, `:1360`) — indexing with 255 or -1 is out of bounds.** Sentinel branch supplies an empty name.
4. **Per client:** `bSupportsStructuredChat` → `CreateChatTextChannelMes ( channelID, timestamp, senderName, strChatText )` (plain, no wire escaping); legacy → existing escaped HTML string via `PROTMESSID_CHAT_TEXT`.

## 8. Legacy message 18

Compatibility window only: names/text escaped, consistent length enforcement, RPC uses the same safe formatting path, no new functionality depends on it. The structured path must **not** reuse the legacy formatted string internally.

## 9. Welcome message (administrative rich-content message)

`CServer::OnClientConnect` (`src/server.cpp:448-459`) keeps sending the raw-HTML welcome via `PROTMESSID_CHAT_TEXT` — administrators depend on HTML/CSS styling. Classified as an **administrative rich-content message**, not normal chat; **normal user chat never enters this path**. Prefix detection (`src/clientdlg.cpp:887`) remains temporarily; the HTML mechanism is not generalized; long-term migration out of scope.

## 10. Client data model

Protocol handlers must not build widgets. Semantic layer between protocol and presentation:

```cpp
struct ChatMessage
{
    uint8_t  channelId;
    uint32_t timestamp;
    QString  senderName;   // wire snapshot (decision B)
    QString  text;
};
```

```text
Protocol → ChatMessage → model/presentation layer → desktop UI + accessibility + filtering/muting + JSON-RPC
```

Not `Protocol → QListWidgetItem`. With the name on the wire, no live lookup is needed for historical identity; channel lookup only serves filtering/muting.

## 11. Client rendering

Plain-data rendering; `<b>hello</b>`, `<img src=x>`, `<a href="...">`, `<script>…</script>` appear as ordinary text.

- New `OnChatTextChannelReceived` builds `ChatMessage` and appends to the chat model.
- `AddChatText` (`src/chatdlg.cpp:134-161`) retained for the legacy path only; remove the `href\s*=|src\s*=` heuristic (`:142`).
- Linkify: apply the HTTP(S)-wrap regex (`:155-156`) to escaped/plain text **after** escaping, never before; confirm-before-open stays.

## 12. Chat UI choice (decision C)

Evaluate, don't assume:

1. `QListWidget` — per-item `Qt::PlainText`, `Qt::UserRole` channel ID; per-item a11y via `QAccessibleItemView`.
2. `QListView` + model + delegate — best fit for `ChatMessage`; links/copy/select/keyboard via delegate + editor flags; same per-item a11y; scales to high volume.
3. Retained `QTextBrowser` with fully client-controlled escaped generation — **rank lowest**: the source of both the a11y ceiling (one text node to screen readers) and the HTML-interpretation surface.

Criteria: a11y, keyboard navigation, select/copy, URL interaction, visual formatting, high-volume performance, ease, maintainability, Qt compatibility. **Requirement is structured data + safe presentation, not a specific widget.** If `QListWidget` needs a heavy custom delegate/hit-test/a11y layer to reproduce clickable URLs, prefer model/view.

Evaluation outcome (Slice 3, 2026-08-09): **retained `QTextBrowser`** with fully client-controlled escaped generation. Rationale: (1) clickable-URL-with-confirm requires an anchor hit-test delegate in any list view — the "heavy layer" the design flags; (2) the legacy old-server path must keep rendering server HTML, so two rendering modes would be needed in a list view; (3) the core requirement — structured data + safe presentation — is fully met by client-side escaping (`EscapeAndLinkifyText`) on the retained widget. A11y improvement delivered via `QAccessibleAnnouncementEvent` live announcements; per-message a11y nodes (list-view benefit) remain a follow-up candidate. The URL linkify regex was tightened to terminate at `&` (HTML-entity boundary in escaped text) so URLs adjacent to escaped markup do not over-match.

## 13. Accessibility

Improve, not merely preserve: accessible name (`src/chatdlg.cpp:58`); per-message exposure (list-based view gives per-item nodes); live announcements — today `QAccessibleValueChangeEvent` (`:137`), prefer `QAccessibleAnnouncementEvent` (API since Qt 6.5; the project's existing guarded use at `src/connectdlg.cpp:1169` is **Qt ≥ 6.8** — match that guard); keyboard nav; selectable/copyable text; meaningful sender/timestamp/text semantics.

## 14. Name/channel lookup

Name is on the wire, so the channel ID is for filtering/muting, not identity. Client channel structures store no name (`CClientChannel`, `src/client.h:131`; CONN_CLIENTS_LIST names remapped in `OnConClientListMesReceived`, `src/client.cpp:353`, `:377`). For lookups: unknown IDs must not crash (`FindClientChannel` → `INVALID_INDEX` for IDs ≥ `MAX_NUM_CHANNELS`, `src/client.cpp:1740`, covering 255); `255`/unknown → "Server"/placeholder; pre-list messages (connect race / UDP loss) → placeholder, repaired on refresh.

## 15. JSON-RPC

### 15.1 Server

`broadcastChatMessage` (`src/serverrpc.cpp:98-114`) and `privateChatMessage` (`:116-138`) route through the merged fan-out (§7) — no HTML construction; wire ID `255`; clamping added to broadcast (`private` already clamps at `:125`). Public API keeps `-1` for RPC messages (`chatMessageReceived`, `src/serverrpc.cpp:86-96`); `-1 → 255` conversion only at the fan-out boundary; 255 never surfaces in the public API.

### 15.2 Client

`jamulusclient/chatTextReceived` (`src/clientrpc.cpp:54-62`) stops exposing server HTML. **Structured:**

```json
{ "channelId": 12, "timestamp": 1786298460, "text": "hello" }
```

plus `senderName` (decision B). Breaking — changelog-flagged.

## 16. Testing

Part of the feature, not deferred. **Dependency:** repo currently has only the commented-out `CTestbench` (`src/protocol.h:161-183`) — land the test-infrastructure PR first or add a small standalone target for 37 round-trips.

- **Serialization:** ASCII, UTF-8, empty, max-length, over-limit, channel 0, channel 149, channel 255, invalid 150–254, malformed length, truncated packet/timestamp, trailing data.
- **Compatibility:** all four old/new server × old/new client combos. Expected: new+old→18; new+new capable→37; old+new→18 (new client keeps the legacy parser).
- **Capability:** absent, supported, malformed/unknown, message before negotiation completes, reconnect, state reset on disconnect. Safe default always legacy.
- **Security:** `<b>hello</b>`, `<img src=x>`, `<a href="https://example.com">`, `<script>alert(1)</script>`, HTML+URL combinations. Invariant: user text can never become executable/unintended HTML.

Status (2026-08-09): the standalone `tests/chatprotocol` target covers serialization, reject rules, split reassembly, capability, the `ChatMessage` data model and the security cases above (via `EscapeAndLinkifyText`/`LinkifyURLs`); 48 assertions passing.
- **Identity (name on wire):** rename after send → old row keeps original name; disconnect + ID reuse → old row keeps original sender.

## 17. Documentation

- `docs/JAMULUS_PROTOCOL.md`: message 37, field order, byte order, timestamp semantics, channel-ID semantics, 255 sentinel, server→client-only, capability negotiation (38/39), relation to legacy 18. State: *message 37 carries semantic chat data, not HTML/presentation markup.* Clarify the legacy note (`:145-147`).
- `docs/JSON-RPC.md`: regenerate via `tools/generate_json_rpc_docs.py`; document HTML removal from client notifications, structured schema, `-1` semantics, breaking implications.

## 18. Implementation plan

**Slice 1 — protocol + capability + server.** No client UI change; existing clients keep parsing legacy HTML.

1. `protocol.h`: add `PROTMESSID_CHAT_TEXT_CHANNEL 37`, `REQ_CHAT_TEXT_SUPPORT 38`, `CHAT_TEXT_SUPPORTED 39`; declare create/evaluate.
2. `protocol.cpp`: 37 create/evaluate (decode caps: name `MAX_LEN_FADER_TAG`, text `MAX_LEN_CHAT_TEXT` post-decode, trailing guard); dispatch cases in `EvaluateMessageBody` (`src/protocol.cpp:806`); 38/39 handshake.
3. `CChannel`: store `bSupportsStructuredChat`; new signal + connection (`connectChannelSignalsToServerSlots`, `src/server.cpp:328-362`); server sends REQ in the connect bootstrap (near `src/server.cpp:417`).
4. Merge fan-out (§7): clamp, stamp, sentinel guard, name snapshot, per-client 37/18 branch; route RPC through it (`src/serverrpc.cpp:112`), keep `-1`.
5. Tests (§16) + protocol/RPC docs (§17).

**Slice 2 — client data/model.** `ChatMessage`; `OnChatTextChannelReceived`; identity semantics; safe text representation; structured RPC notification (§15.2); tests.

Status (2026-08-09): **implemented.** `ChatMessage` struct in `src/chatmessage.h` (plain data: channel ID, epoch timestamp, sender-name wire snapshot, text — never HTML). `CChannel` relays the new protocol signal; `CClient::OnChatTextChannelReceived` builds the `ChatMessage` and emits `CClient::ChatTextChannelReceived`. `jamulusclient/chatTextReceived` now emits structured `{channelId, timestamp, senderName, text}` and no longer exposes server HTML. Note: the structured notification fires only for message 37; a new client against a legacy server (message 18) does not re-expose the legacy HTML string through this notification. Tests extended in `tests/chatprotocol` for the `ChatMessage` data model; full client build verified. Slice 3 (UI) still owns placeholder/identity rendering of unknown/255 channel IDs and the model/view.

**Slice 3 — client UI.** Chat model/view (§12); timestamp formatting; sender rendering; URL interaction; a11y + live announcements (§13); copy/select; legacy fallback. Only the old-server path retains HTML rendering.

Status (2026-08-09): **implemented.** `CChatDlg::AddChatMessage` renders structured messages entirely client-side: local/locale-aware timestamp, stable per-channel colour, escaped sender name and text via the new testable `EscapeAndLinkifyText`/`LinkifyURLs` helpers (`src/util.h`), preserving bare-http(s)-URL linkification after escaping and the confirm-before-open dialog. Legacy `AddChatText` keeps the server-HTML path (linkify only, `href\s*=|src\s*=` heuristic removed per §11). A11y live announcements moved from `QAccessibleValueChangeEvent` to the Qt ≥ 6.8-guarded `QAccessibleAnnouncementEvent` (§13). `CClientDlg::OnChatTextChannelReceived` wires the structured signal into the chat dialog (audio alert, `ShowChatWindow(false)`, welcome detection remains on the legacy path).

## 19. Non-goals

Per-channel mute, rich user formatting, Markdown, arbitrary HTML chat, editing, reactions, threading, persistence, generalized capability framework. Mute/filtering becomes easier (channel identity on messages) but remains follow-up.

## 20. Open items

- Exact names of capability messages 38/39 (per existing conventions).
- Exact JSON-RPC schema field names — settle before Slice 2.
- Test-infrastructure PR vs standalone target sequencing.

Resolved during Slice 2: JSON-RPC field names are `channelId`, `timestamp`, `senderName`, `text` for both `jamulusclient/chatTextReceived` and `jamulusserver/chatMessageReceived`; 38 = `REQ_CHAT_TEXT_SUPPORT`, 39 = `CHAT_TEXT_SUPPORTED`.
