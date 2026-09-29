param([int]$Port=9998, [double]$Timeout=30.0)
$listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Any, $Port)
$listener.Start()
$client = $listener.AcceptTcpClient()
$client.ReceiveTimeout = [int]($Timeout * 1000)
$stream = $client.GetStream()
$buf = New-Object byte[] 65536
$total = 0L
$t0 = Get-Date
while ($true) {
  try { $n = $stream.Read($buf, 0, $buf.Length) } catch { break }
  if ($n -le 0) { break }
  $total += $n
}
$elapsed = ((Get-Date) - $t0).TotalSeconds
$mbps = if ($elapsed -gt 0) { $total * 8 / 1e6 / $elapsed } else { 0 }
Write-Output ("RESULT bytes={0} elapsed_s={1:N2} goodput_mbps={2:N3}" -f $total,$elapsed,$mbps)
$client.Close()
$listener.Stop()
