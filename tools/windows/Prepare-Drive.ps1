# SPDX-License-Identifier: GPL-2.0
<#
.SYNOPSIS
  Formats a USB stick as NTFS for the $LogFile captures (docs/LOGFILE.md 7.1).

.DESCRIPTION
  Quick-formats the volume at -DriveLetter as NTFS with the given label and
  cluster size, then records fsutil ntfsinfo for the session. Refuses C:, the
  system/boot disk, any non-USB / non-removable disk and (without -AllowLarge)
  disks over 64 GB. You must type the drive letter to confirm.

  Two sticks are wanted: 4 KiB clusters (default) and 512-byte clusters:
      .\Prepare-Drive.ps1 -DriveLetter E -Label CAP1
      .\Prepare-Drive.ps1 -DriveLetter F -Label CAP512 -ClusterSize 512

.PARAMETER DriveLetter
  Drive letter of the stick, e.g. E. Mandatory; never guessed.
.PARAMETER Label
  Volume label (1-32 characters).
.PARAMETER ClusterSize
  Allocation unit in bytes: 512, 1024, 2048, 4096 (default), 8192, ..., 65536.
.PARAMETER Full
  Full format instead of quick (slow; not needed).
.PARAMETER AllowLarge
  Allow disks over 64 GB.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DriveLetter,
    [string]$Label = 'CAP1',
    [ValidateSet(512, 1024, 2048, 4096, 8192, 16384, 32768, 65536)][int]$ClusterSize = 4096,
    [switch]$Full,
    [switch]$AllowLarge
)
. "$PSScriptRoot\Common.ps1"

Assert-Admin
if ($Label -notmatch '^[^\\/:*?"<>|]{1,32}$') { throw "Label must be 1-32 characters without \ / : * ? `" < > |" }

$t = Get-TargetVolume $DriveLetter -AllowLarge:$AllowLarge
Write-Step "target"
Show-Target $t

$files = @(Get-ChildItem -Path $t.Root -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne 'System Volume Information' })
Write-Banner ("THIS ERASES EVERYTHING ON {0}:  ('{1}', {2} top-level items, {3:N1} GB)" -f $t.Letter, $t.Volume.FileSystemLabel, $files.Count, ($t.Volume.Size / 1GB)) 'Red'
Write-Host "    disk #$($t.Disk.Number) $($t.Disk.FriendlyName) on bus $($t.Disk.BusType)"
Write-Host "    new: NTFS, label '$Label', cluster $ClusterSize bytes, $(if ($Full) {'full'} else {'quick'}) format"
Confirm-Typed $t.Letter "    Type the drive letter ($($t.Letter)) to confirm"

Write-Step "formatting"
try {
    $r = Format-Volume -DriveLetter $t.Letter -FileSystem NTFS -NewFileSystemLabel $Label `
                       -AllocationUnitSize $ClusterSize -Full:$Full -Force -Confirm:$false
    Write-Ok ("formatted: {0} {1} cluster {2}" -f $r.FileSystemLabel, $r.FileSystem, $r.AllocationUnitSize)
} catch {
    Write-Warn "Format-Volume failed: $_"
    Write-Host "    Manual equivalent:  format $($t.Letter): /fs:ntfs /q /v:$Label /a:$ClusterSize /y"
    throw
}

Start-Sleep -Seconds 2
$t = Get-TargetVolume $t.Letter -AllowLarge:$AllowLarge
$dir = Get-SessionDir 'prepared' $t
Save-VolumeState $dir $t.Letter 'fresh'
Save-Meta $dir @{
    step        = 'prepare'
    driveLetter = $t.Letter
    label       = $Label
    clusterSize = $ClusterSize
    diskNumber  = $t.Disk.Number
    diskName    = $t.Disk.FriendlyName
    diskBus     = [string]$t.Disk.BusType
    diskBytes   = $t.Disk.Size
    volumeBytes = $t.Volume.Size
    removalPolicy = (Get-RemovalPolicy $t.Disk).Text
}
Get-Content (Join-Path $dir 'fresh.ntfsinfo.txt') | Select-Object -First 12 | ForEach-Object { "    $_" }
Write-Host ''
Write-Host "Now set the write-cache policy for the dirty scenarios:" -ForegroundColor Cyan
Write-Host "  Device Manager > Disk drives > $($t.Disk.FriendlyName) > Policies > 'Better performance', then unplug and re-plug."
Write-Host "  Current registry reading: $((Get-RemovalPolicy $t.Disk).Text)"
