# Configuration

The module reads `serverext.cfg` from the server's working directory when it loads. This page lists every key.

## File format

- One setting per line, as `key value` or `key = value`.
- `#` and `//` start a comment.
- Keys are case-sensitive.
- A missing file means defaults: the bot channel listens on `127.0.0.1:16700` and the admin channel is off.
- An unknown key is logged as a warning and ignored.
- A malformed or out-of-range value is an error. The module then refuses to load (`initDll` returns false), so a typo
  never leaves it listening or connecting somewhere unexpected. The server logs each problem with its line number.

The file holds the admin channel's key. Make it readable only by the user the server runs as.

## Bot channel

The protocol is in [protocol.md](protocol.md).

| Key | Default | Range | Meaning |
|---|---|---|---|
| `botListen` | `127.0.0.1:16700` | IPv4 address and port | Where the bot channel listens. The channel has no authentication, so keep it on loopback. In a container it can bind `0.0.0.0` when the port is published on `127.0.0.1` only. Port 16567, the default game port, is rejected. |
| `maxConnections` | 4 | 1 to 8 | Bot channel connections open at once. |
| `watchdogTicks` | 6 | 1 to 3000 | Ticks without input before a bot's input is neutralised. 6 ticks are 200 ms. |
| `enterRange` | 30 | 1 to 1000 | Metres within which a bot can enter a vehicle. |
| `projectileRange` | 1000 | 10 to 5000 | Metres around a connection's bots within which projectiles are reported. |
| `spawnFallbackSeconds` | 5 | 1 to 3600 | Seconds a bot waits on a blocked spawn group before the module tries the next one. |
| `spawnBlockedSeconds` | 15 | 1 to 3600 | Seconds without a spawn, with every group tried, before `SpawnBlocked` is sent. |
| `allowRoundRestart` | 0 | 0 or 1 | Allow `RestartRound`. |
| `allowTeleport` | 0 | 0 or 1 | Allow `Teleport`. For test servers. |
| `stepMode` | 0 | 0 or 1 | Install the module's tick source, so a client can run the server faster than real time ([stepped time](protocol.md#stepped-time)). Never on a server humans play on. |

## Admin channel

The protocol and its security model are in [admin-channel.md](admin-channel.md). The admin channel is off unless
`adminConnect` is set; the other admin keys are then ignored with a warning.

| Key | Default | Range | Meaning |
|---|---|---|---|
| `adminConnect` | none | `host:port` | The admin client's listener. The module dials out to it and reconnects with backoff. The host is a DNS name or an IPv4 address. Port 16567 is rejected. |
| `adminServerId` | required | 1 to 64 printable ASCII bytes, no spaces | The id this server presents in the handshake. The client looks the key up by it. |
| `adminKey` | required | 32 to 128 hex digits (16 to 64 bytes) | The key shared with the client. Both sides prove they hold it with HMAC-SHA256. |
| `adminTls` | 1 | 0 or 1 | TLS 1.3 on the link. 0 sends in the clear and is accepted only when `adminConnect` is `localhost` or a `127.x.x.x` address. |
| `adminCertSha256` | required with `adminTls 1` | 64 hex digits | The pin: the SHA-256 of the client certificate's SubjectPublicKeyInfo. The module accepts no other certificate. |
| `adminSendIdentity` | 1 with TLS, 0 without | 0 or 1 | Send players' IP addresses and CD-key hashes. |
| `adminAllowConsole` | 1 | 0 or 1 | Allow the `Console` command, which runs any console line. |
| `adminAllowObjects` | 1 | 0 or 1 | Allow `SpawnObject` and `DeleteObject`. |
| `adminAllowScripts` | 1 | 0 or 1 | Allow `StartScript`, which runs JavaScript the client sends inside the server process. |
| `adminQueueBytes` | 4194304 | 65536 to 67108864 | Outbound queue size beyond which events are dropped. At four times this size the module closes the link. |

The three `adminAllow*` switches bound what a leaked key can do. A server that only needs kicks, bans, chat and the
event stream can set all three to 0.

## Example

A server with both channels: bots from a process on the same host, and an admin client reached over the internet.

```
# Bot channel
botListen       127.0.0.1:16700
allowRoundRestart 1

# Admin channel
adminConnect    admin.example.org:4310
adminServerId   my-server-1
adminKey        00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff
adminCertSha256 5f2c0e7d9a1b4c3e8f6a7b0c1d2e3f405f2c0e7d9a1b4c3e8f6a7b0c1d2e3f40
adminAllowConsole 0
```

Generate a key with `openssl rand -hex 32`. The pin comes from the admin client: BF2WebAdmin prints it with
`--admin-pin`, and `tools/adminclient/adminclient.py --key <hex> --print-pin` prints the reference client's.
