# Shared helpers for the In-Flight harness scripts (dot-sourced). ASCII only.

# Writes a short silent 16-bit mono 48 kHz PCM WAV. Used as a hardware-free audio source for the
# fixture mic: an ffmpeg_source always loads and has the audio flag, unlike a wasapi capture of a
# device that does not exist.
function New-SilentWav([string]$Path, [double]$Seconds = 1.0) {
  $rate = 48000
  $dataLen = [int]($rate * $Seconds) * 2
  $ms = New-Object IO.MemoryStream
  $bw = New-Object IO.BinaryWriter($ms)
  $bw.Write([Text.Encoding]::ASCII.GetBytes("RIFF"))
  $bw.Write([int](36 + $dataLen))
  $bw.Write([Text.Encoding]::ASCII.GetBytes("WAVE"))
  $bw.Write([Text.Encoding]::ASCII.GetBytes("fmt "))
  $bw.Write([int]16)
  $bw.Write([int16]1)
  $bw.Write([int16]1)
  $bw.Write([int]$rate)
  $bw.Write([int]($rate * 2))
  $bw.Write([int16]2)
  $bw.Write([int16]16)
  $bw.Write([Text.Encoding]::ASCII.GetBytes("data"))
  $bw.Write([int]$dataLen)
  $bw.Write((New-Object byte[] $dataLen))
  $bw.Flush()
  New-Item -ItemType Directory -Force (Split-Path -Parent $Path) | Out-Null
  [IO.File]::WriteAllBytes($Path, $ms.ToArray())
}

# Writes a looping-friendly 16-bit mono 48 kHz PCM sine WAV (default 1 kHz, -20 dBFS peak, 2 s).
# 48000 / 1000 = 48 samples per cycle, so 2 s is exactly 2000 cycles and the loop point is seamless.
function New-SineWav([string]$Path, [double]$Seconds = 2.0, [double]$Hz = 1000.0, [double]$PeakDb = -20.0) {
  $rate = 48000
  $n = [int]($rate * $Seconds)
  $amp = [math]::Pow(10.0, $PeakDb / 20.0) * 32767.0
  $dataLen = $n * 2
  $ms = New-Object IO.MemoryStream
  $bw = New-Object IO.BinaryWriter($ms)
  $bw.Write([Text.Encoding]::ASCII.GetBytes("RIFF"))
  $bw.Write([int](36 + $dataLen))
  $bw.Write([Text.Encoding]::ASCII.GetBytes("WAVE"))
  $bw.Write([Text.Encoding]::ASCII.GetBytes("fmt "))
  $bw.Write([int]16)
  $bw.Write([int16]1)
  $bw.Write([int16]1)
  $bw.Write([int]$rate)
  $bw.Write([int]($rate * 2))
  $bw.Write([int16]2)
  $bw.Write([int16]16)
  $bw.Write([Text.Encoding]::ASCII.GetBytes("data"))
  $bw.Write([int]$dataLen)
  for ($i = 0; $i -lt $n; $i++) {
    $bw.Write([int16][math]::Round($amp * [math]::Sin(2.0 * [math]::PI * $Hz * $i / $rate)))
  }
  $bw.Flush()
  New-Item -ItemType Directory -Force (Split-Path -Parent $Path) | Out-Null
  [IO.File]::WriteAllBytes($Path, $ms.ToArray())
}
# Waits (never kills) for any process running THIS testbed's obs64.exe to exit. $ObsExe is the full
# path of <testbed>\obs\bin\64bit\obs64.exe. Returns $true when none is left, $false after the timeout.
function Wait-TestbedObsExit([string]$ObsExe, [int]$Seconds = 30) {
  $full = [IO.Path]::GetFullPath($ObsExe)
  for ($i = 0; $i -le $Seconds; $i++) {
    $left = @(Get-Process -Name obs64 -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ([IO.Path]::GetFullPath($_.Path) -eq $full) })
    if ($left.Count -eq 0) { return $true }
    if ($i -lt $Seconds) { Start-Sleep -Seconds 1 }
  }
  return $false
}