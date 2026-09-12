# SPDX-License-Identifier: GPL-2.0
<#
.SYNOPSIS
  Scenario A (docs/LOGFILE.md 7.4): file + directory created, then power cut.
  Transactions expected in the log: MFT record init, $I30 index entries,
  $Bitmap / $MFT bitmap bits.

.DESCRIPTION
  1. Checks the stick is on 'Better performance' (write cache on).
  2. Saves the 'before' state (fsutil, dir).
  3. Runs exactly:  echo hello > X:\A.txt & mkdir X:\Adir & echo x > X:\Adir\inner.txt
  4. Tells you the exact moment to PULL the stick (within 1-2 s of WRITTEN).
  5. Writes meta.json to the session folder and prints the Mac imaging steps.

.EXAMPLE
  .\Scenario-A.ps1 -DriveLetter E
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DriveLetter,
    [switch]$SkipPolicyCheck,
    [switch]$AllowLarge
)
. "$PSScriptRoot\Common.ps1"
Assert-Admin
$t = Get-TargetVolume $DriveLetter -AllowLarge:$AllowLarge
$dir = Get-SessionDir 'A' $t
$transcript = Start-SessionTranscript $dir
try {
    Write-Step "Scenario A on $($t.Letter): -> $dir"
    Show-Target $t
    Assert-BetterPerformance $t -Skip:$SkipPolicyCheck
    foreach ($n in 'A.txt', 'Adir') {
        if (Test-Path (Join-Path $t.Root $n)) { throw "$n already exists on $($t.Letter):; start from a fresh Prepare-Drive.ps1 or delete it and safely remove/re-plug first." }
    }
    Save-VolumeState $dir $t.Letter 'before'

    Write-Banner "Get ready to pull $($t.Letter): - hand on the stick. Pull it 1-2 seconds AFTER the red WRITTEN banner." 'Yellow'
    Read-Host "    Press Enter to start the 5 s countdown"
    Start-Countdown 5 'writing in'
    $cmd = "echo hello > $($t.Letter):\A.txt & mkdir $($t.Letter):\Adir & echo x > $($t.Letter):\Adir\inner.txt"
    $t0 = Get-Date
    cmd /c $cmd | Out-Null
    $t1 = Get-Date
    Write-PullBanner "WRITTEN ($(($t1 - $t0).TotalMilliseconds) ms) - PULL THE STICK NOW"
    Read-Host "    Press Enter AFTER the stick is out"
    $pulled = Get-Date

    Save-Meta $dir @{
        scenario      = 'A'
        description   = 'file + dir created, stick pulled 1-2 s later (power cut equivalent)'
        driveLetter   = $t.Letter
        label         = $t.Volume.FileSystemLabel
        command       = $cmd
        writtenAt     = $t1.ToString('o')
        pulledAt      = $pulled.ToString('o')
        secondsToPull = [math]::Round(($pulled - $t1).TotalSeconds, 1)
        removalPolicy = (Get-RemovalPolicy $t.Disk).Text
        diskName      = $t.Disk.FriendlyName
        expected      = 'State DIRTY, restart page version 2.0 (Windows 8+), records for MFT init / $I30 / bitmaps'
    }
    Write-NextSteps 'A' $t $dir
} finally { Stop-SessionTranscript }
