param([string]$TargetHost, [int]$Port, [double]$MB=20.0)
$client = New-Object System.Net.Sockets.TcpClient
$client.Connect($TargetHost, $Port)
$stream = $client.GetStream()
$chunk = New-Object byte[] 65536
for ($i=0; $i -lt $chunk.Length; $i++) { $chunk[$i] = 0x51 }
$target = [Int64]($MB * 1e6)
$sent = [Int64]0
$t0 = Get-Date
while ($sent -lt $target) {
  $stream.Write($chunk, 0, $chunk.Length)
  $sent += $chunk.Length
}
$stream.Close()
$elapsed = ((Get-Date) - $t0).TotalSeconds
$mbps = if ($elapsed -gt 0) { $sent * 8 / 1e6 / $elapsed } else { 0 }
Write-Output ("SENT bytes={0} elapsed_s={1:N2} offered_mbps={2:N3}" -f $sent,$elapsed,$mbps)
$client.Close()
