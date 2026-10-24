param(
  [Parameter(Position = 0)][string]$Device,
  [Parameter(Position = 1)][string]$Firmware
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($Device)) {
  Write-Host 'Usage: ota_update.bat DEVICE_IP [application.bin]'
  Write-Host 'Build first; the default image is build\SmartInfo7.bin.'
  exit 2
}

try {
  $hostType = [Uri]::CheckHostName($Device)
  if ($hostType -notin @([UriHostNameType]::IPv4, [UriHostNameType]::Dns)) {
    throw 'Specify a device IP or hostname, without http://, a path, or a port.'
  }
  $root = Split-Path -Parent $PSScriptRoot
  if ([string]::IsNullOrWhiteSpace($Firmware)) {
    $Firmware = Join-Path $root 'build\SmartInfo7.bin'
  }
  $imagePath = (Resolve-Path -LiteralPath $Firmware).ProviderPath
  $image = [IO.File]::ReadAllBytes($imagePath)
  if ($image.Length -lt 288 -or $image[0] -ne 0xe9 -or
      [BitConverter]::ToString($image, 32, 4) -cne '32-54-CD-AB') {
    throw 'Select an ESP-IDF application .bin, not a bootloader or merged flash image.'
  }
  $expectedHash = [BitConverter]::ToString($image, 176, 32).Replace('-', '').ToLowerInvariant()
  $project = [Text.Encoding]::ASCII.GetString($image, 80, 32).TrimEnd([char]0)
  $token = $env:SMARTINFO_OTA_TOKEN
  if ([string]::IsNullOrWhiteSpace($token)) {
    $config = Get-Content -LiteralPath (Join-Path $root 'sdkconfig') -Raw
    $match = [regex]::Match($config, '(?m)^CONFIG_DEVICE_SERVER_AUTH_TOKEN="([^"]*)"\r?$')
    $token = $match.Groups[1].Value
  }
  if ($token -cnotmatch '^[0-9a-fA-F]{64}$') {
    throw 'Configure a 64-character hexadecimal OTA token in sdkconfig or SMARTINFO_OTA_TOKEN.'
  }
  $curl = (Get-Command curl.exe -CommandType Application | Select-Object -First 1).Source
  $base = 'http://' + $Device
  $curlArgs = @('--noproxy', '*', '--show-error', '--fail-with-body',
                '--connect-timeout', '5', '-H', ('Authorization: Bearer ' + $token))

  function Get-DeviceStatus {
    $json = & $curl @curlArgs --silent --max-time 5 "$base/status" 2>$null
    if ($LASTEXITCODE -ne 0) { throw "Status request failed (curl exit $LASTEXITCODE)." }
    return ($json | ConvertFrom-Json)
  }

  $before = Get-DeviceStatus
  if ($before.project -cne $project) { throw 'The image belongs to a different project than this device.' }
  if ($before.pending_verify -or $before.ota_phase -in @('uploading', 'verifying', 'rebooting')) {
    throw 'Device is busy or still confirming its previous boot. Try again shortly.'
  }
  Write-Host "Uploading $imagePath to $Device..."
  & $curl @curlArgs --progress-bar --max-time 190 -H 'Content-Type: application/octet-stream' `
    --data-binary "@$imagePath" "$base/ota"
  if ($LASTEXITCODE -ne 0) { throw "Upload failed (curl exit $LASTEXITCODE)." }

  Write-Host "`nWaiting for restart and startup confirmation..."
  $deadline = [DateTime]::UtcNow.AddSeconds(90)
  $newSlotSeen = $false
  while ([DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Seconds 2
    try { $current = Get-DeviceStatus } catch { continue }
    if ($current.slot -ne $before.slot) {
      $newSlotSeen = $true
      if ($current.build_hash -ceq $expectedHash -and $current.boot_state -eq 2 -and
          -not $current.pending_verify) {
        Write-Host "OTA successful: $($current.slot), build $($current.build_hash)."
        exit 0
      }
    } elseif ($newSlotSeen) {
      throw 'The new firmware rolled back to the previous slot.'
    }
  }
  throw 'Could not confirm the new build within 90 seconds. Check power, Wi-Fi, IP address, and rollback status.'
} catch {
  Write-Host ('ERROR: ' + $_.Exception.Message) -ForegroundColor Red
  exit 1
}
