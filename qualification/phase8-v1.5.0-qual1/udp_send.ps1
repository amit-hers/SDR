param([string]$TargetHost, [int]$Port, [double]$Mbps, [int]$Size=1200, [double]$Duration=10.0)
$udp = New-Object System.Net.Sockets.UdpClient
$udp.Connect($TargetHost, $Port)
$padLen = [Math]::Max(0, $Size - 8)
$pad = New-Object byte[] $padLen
for ($i=0; $i -lt $padLen; $i++) { $pad[$i] = 0x51 }
$pps = ($Mbps * 1e6 / 8) / $Size
$interval = if ($pps -gt 0) { 1.0 / $pps } else { 0.001 }
$seq = [UInt64]0
$sentBytes = [Int64]0
$tStart = Get-Date
$nextSend = $tStart
$end = $tStart.AddSeconds($Duration)
while ((Get-Date) -lt $end) {
  $now = Get-Date
  if ($now -lt $nextSend) { continue }
  $be = [BitConverter]::GetBytes($seq)
  [Array]::Reverse($be)
  $pkt = New-Object byte[] ($be.Length + $pad.Length)
  [Array]::Copy($be, 0, $pkt, 0, $be.Length)
  [Array]::Copy($pad, 0, $pkt, $be.Length, $pad.Length)
  try { [void]$udp.Send($pkt, $pkt.Length) } catch {}
  $sentBytes += $pkt.Length
  $seq += 1
  $nextSend = $nextSend.AddSeconds($interval)
}
$udp.Close()
$elapsed = ((Get-Date) - $tStart).TotalSeconds
$offered = if ($elapsed -gt 0) { $sentBytes * 8 / 1e6 / $elapsed } else { 0 }
Write-Output ("SENT seq_count={0} bytes={1} elapsed_s={2:N2} offered_mbps={3:N3}" -f $seq,$sentBytes,$elapsed,$offered)
