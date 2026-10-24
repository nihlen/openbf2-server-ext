# Bot chat: `Say` (0x010B)

Status: done on 2026-10-24 by a worker thread, committed locally (module `100d53e`). The spec is
`docs/protocol.md` "Bot chat". **No engine change.** OpenBF2 `serverext/r1` stays at `873d1b1`. Paths
without a repo prefix are OpenBF2 paths under `Code/BF2/` in that worktree.

## Why

BF2WebAdmin's Heli2v2 module starts a 2v2 round when both crews have said "rdy" or "go". It counts only
`MessageType.Player` lines on the Global channel from a player in a vehicle with two players
(`Heli2v2Module.HandleAsync(ChatMessageEvent)`). Before this change, bots could not chat, so a round with bots
never started in WebAdmin: the localtest DB had 0 matches after a play session.

## How the engine treats a client's line, and why `say()` does not help

- A client's chat arrives as string chunks. `StringManager::addStringDataChunk`
  (`Game/Common/StringManager.cpp:169-207`) builds a `StringReceivedEvent` from them. It replaces the
  sender with the connection's player id and drops anything but type 2 or a server channel (12, 13). It
  then posts the event on `ECNetwork` (`postEvent(ECNetwork, received, 0)`).
- `GameServer::handleStringReceivedEvent` (`Game/GameServer/GameServer.cpp:4289-4334`) does three things:
  - It blanks the sender only for channels 12 and 13.
  - It relays the line to the clients (`playerMessageEvent`: spam check, dead-to-dead rule,
    `filterMessage`, which on channel 0 is just `sender != receiver`, `:4473-4474`).
  - It runs Python event 39 with `(senderId, text, channelName, flags)`.
- `GameServer::say` (`:4778-4793`) posts the same event, but its sender is always the local human player.
  A dedicated server has no local human player, so the sender is -1. `sayAll` and `sayTeam` use channels
  12 and 13, which are server messages by definition. None of the three can speak as a bot.

So the module posts the event itself, with the bot's player id, channel 0 and type 2. The event's fields
are exactly those of a client's line, except that targetId (header byte 9, `[UNRESOLVED]` in the source)
is 0.

## The event object

`StringReceivedEvent`'s constructor, destructor and vtable are in the engine binary, and the binary exports
no symbols to modules. In the image's `bf2`, `nm -D --defined-only` lists 59 symbols, none of them
`StringReceivedEvent`. The constructor exists only as a local `T` symbol. So the module builds a
layout-compatible copy, `BotChatEvent : EventNode` in `src/Module.cpp`, and `static_assert`s every field
offset and the size against `Game/Common/StringManager.h`. The engine touches the node only in these ways:

- `static_cast<StringReceivedEvent*>` field reads in `handleNetworkEvent` arm 11
  (`GameServer.cpp:1605-1606`), `handleStringReceivedEvent`, `StringMessageEW` and
  `ServerStringMessageEW`;
- `handleStringReceivedEvent`'s temporary write and restore of `m_senderId`;
- the virtual `getType` (returns 11) and `release` (deletes it through the module's own virtual
  destructor, which frees the text buffer with the matching `delete[]`).

`postEventNextUpdate` (`Core/EventManager.cpp:175-188`) queues the node for the next `processEvents`. So
the chat is dispatched outside the module's own tick handler, like a client's line from the network
update. It is not dispatched re-entrantly from inside the category 7 event.

## Results (BF2WebAdmin localtest, 2026-10-24)

Test setup:

- A separate test instance next to the user's session. Container `bf2wa-localtest-openbf2-botchat`
  runs the image `bf2wa-localtest-openbf2:botchat` (module `100d53e`) on game port 16571, bot channel
  16706 and admin channel 4312.
- A second BF2WebAdmin on database `bf2test`.
- bf2-bots sidecar with two all-bot crews.

What the logs show:

- Server log:
  `tick 1897: bot 254 says on channel 0: rdy`, then `tick 1898: chat channel 0 from 254: rdy`. The
  second line is the module's ECNetwork listener seeing the event at the next update, with the bot as
  the sender.
- The sidecar got it back as `Chat` (flags: from a player, sender 254, team 2, channel 0).
- WebAdmin's Heli2v2 counted it. After the other crew's "go" it said `New 2v2 match started`. After the
  kills it said `Round 1 won by US in 29 s`, and it stored the match and rounds in `bf2test`. Heli2v2
  takes only `MessageType.Player` lines, so the admin channel reported a `ChatMessage` with that player
  as the sender.
- The round-cycle counts are in bf2-bots docs/serverext-missions.md "Round flow".

Not verified: a human client seeing the line in its chat box. No client was connected to the test
instance, so client delivery rests on the engine source path above. The Python `onChatMessage` hook was
also not observed, because Python handlers run only in begin-round 3 and the test server never leaves
waiting-for-players (protocol.md Q7). The admin channel's observer runs before that gate.
