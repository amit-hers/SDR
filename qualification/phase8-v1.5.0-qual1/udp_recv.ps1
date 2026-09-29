param([int]$Port=9999, [double]$Duration=12.0)
$udp = New-Object System.Net.Sockets.UdpClient($Port)
$udp.Client.ReceiveTimeout = 1000
$seen = New-Object 'System.Collections.Generic.HashSet[UInt64]'
$bytesTotal = 0L
$firstTs = $null
$lastTs = $null
$maxSeq = -1L
$deadline = (Get-Date).AddSeconds($Duration + 3.0)
$remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
while ((Get-Date) -lt $deadline) {
  try {
    $data = $udp.Receive([ref]$remote)
  } catch {
    if ($firstTs -and ((Get-Date) - $lastTs).TotalSeconds -gt 3.0) { break }
    continue
  }
  if ($data.Length -lt 8) { continue }
  # network byte order (big-endian) 8-byte seq: reverse for little-endian host
  $be = $data[0..7]
  [Array]::Reverse($be)
  $seq = [BitConverter]::ToUInt64($be, 0)
  $now = Get-Date
  if (-not $firstTs) { $firstTs = $now }
  $lastTs = $now
  [void]$seen.Add($seq)
  if ([int64]$seq -gt $maxSeq) { $maxSeq = [int64]$seq }
  $bytesTotal += $data.Length
}
$udp.Close()
$elapsed = 0.0
if ($firstTs -and $lastTs -and $lastTs -gt $firstTs) { $elapsed = ($lastTs - $firstTs).TotalSeconds }
$delivered = $seen.Count
$expected = if ($maxSeq -ge 0) { $maxSeq + 1 } else { 0 }
$lost = [Math]::Max(0, $expected - $delivered)
$lossPct = if ($expected -gt 0) { 100.0 * $lost / $expected } else { 0 }
$mbps = if ($elapsed -gt 0) { $bytesTotal * 8 / 1e6 / $elapsed } else { 0 }
Write-Output ("RESULT delivered_unique={0} expected={1} lost={2} loss_pct={3:N2} bytes={4} elapsed_s={5:N2} delivered_mbps={6:N3}" -f $delivered,$expected,$lost,$lossPct,$bytesTotal,$elapsed,$mbps)
