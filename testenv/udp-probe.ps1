# Sends a GameSpy v3 basic query from Windows to a BF2 server's query port and prints the reply.
# A reply proves a full Windows -> server -> Windows UDP round trip to that host:port, which is the
# path a Windows game client's server browser uses. The game port itself is not probed (it only
# answers the client's connection handshake).
#
#   .\udp-probe.ps1 -Target 127.0.0.1 -Port 29903
param(
    [string]$Target = '127.0.0.1',
    [int]$Port = 29903,
    [int]$TimeoutMs = 2000
)
$udp = New-Object System.Net.Sockets.UdpClient
$udp.Client.ReceiveTimeout = $TimeoutMs
# FE FD 00 <4-byte id> FF FF FF 01 : request all server keys, players and teams.
[byte[]]$q = 0xFE, 0xFD, 0x00, 0x53, 0x45, 0x58, 0x54, 0xFF, 0xFF, 0xFF, 0x01
try {
    [void]$udp.Send($q, $q.Length, $Target, $Port)
    $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
    $r = $udp.Receive([ref]$ep)
    $text = [System.Text.Encoding]::ASCII.GetString($r) -replace "`0", '|'
    "OK $($r.Length) bytes from $($ep): $($text.Substring(0, [Math]::Min(300, $text.Length)))"
} catch {
    "NO REPLY from ${Target}:$Port ($($_.Exception.InnerException.Message))"
} finally {
    $udp.Close()
}
