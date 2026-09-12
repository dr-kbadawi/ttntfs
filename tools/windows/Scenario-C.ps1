# SPDX-License-Identifier: GPL-2.0
<#
.SYNOPSIS
  Scenario C (docs/LOGFILE.md 7.4): pending directory index update
  ($INDEX_ALLOCATION ops, index block split, WriteEndOfIndexBuffer).

.DESCRIPTION
  Step 1 (clean): mkdir X:\idx and 80 files with long names so the index
          leaves $INDEX_ROOT and fills several INDX blocks; then safely remove
          and re-plug so this part is committed.
  Step 2 (dirty): del #7 & echo y > zz-new-entry.txt & ren #40 renamed.txt,
          then PULL immediately.

.EXAMPLE
  .\Scenario-C.ps1 -DriveLetter E
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DriveLetter,
    [int]$Files = 80,
    [switch]$SkipPolicyCheck,
    [switch]$AllowLarge
)
. "$PSScriptRoot\Common.ps1"
Assert-Admin
$t = Get-TargetVolume $DriveLetter -AllowLarge:$AllowLarge
$dir = Get-SessionDir 'C' $t
$transcript = Start-SessionTranscript $dir
try {
    Write-Step "Scenario C on $($t.Letter): -> $dir"
    Show-Target $t
    Assert-BetterPerformance $t -Skip:$SkipPolicyCheck
    $idx = "$($t.Letter):\idx"
    if (Test-Path $idx) { throw "$idx already exists; delete it, safely remove, re-plug, and start again." }
    $stem = 'a-deliberately-long-file-name-to-fill-index-blocks-'

    Write-Step "step 1: $Files files in $idx (clean part)"
    Save-VolumeState $dir $t.Letter 'before'
    cmd /c "mkdir $idx" | Out-Null
    cmd /c "for /L %i in (1,1,$Files) do @echo x > $idx\$stem%i.txt" | Out-Null
    $count = @(Get-ChildItem $idx).Count
    Write-Ok "$count files created"
    Save-VolumeState $dir $t.Letter 'after-step1'
    Write-Step "safely remove and re-plug so step 1 is committed"
    Invoke-SafeEject $t
    $t = Wait-Replug $t -AllowLarge:$AllowLarge
    Save-VolumeState $dir $t.Letter 'replugged'
    if (@(Get-ChildItem "$($t.Letter):\idx").Count -ne $count) { throw "file count changed after re-plug" }

    Write-Step "step 2: delete + create + rename in the index, then pull"
    $cmd = "del $($t.Letter):\idx\$($stem)7.txt & echo y > $($t.Letter):\idx\zz-new-entry.txt & ren $($t.Letter):\idx\$($stem)40.txt renamed.txt"
    Write-Host "    $cmd"
    Write-Banner "Hand on the stick. Pull it IMMEDIATELY after the red banner." 'Yellow'
    Read-Host "    Press Enter to start the 5 s countdown"
    Start-Countdown 5 'writing in'
    $t0 = Get-Date
    cmd /c $cmd | Out-Null
    $t1 = Get-Date
    Write-PullBanner "WRITTEN ($(($t1 - $t0).TotalMilliseconds) ms) - PULL THE STICK NOW"
    Read-Host "    Press Enter AFTER the stick is out"
    $pulled = Get-Date
    Save-Meta $dir @{
        scenario      = 'C'
        description   = 'index update (del/create/rename in a multi-block $I30) then pull'
        driveLetter   = $t.Letter
        label         = $t.Volume.FileSystemLabel
        files         = $Files
        command       = $cmd
        writtenAt     = $t1.ToString('o')
        pulledAt      = $pulled.ToString('o')
        secondsToPull = [math]::Round(($pulled - $t1).TotalSeconds, 1)
        removalPolicy = (Get-RemovalPolicy $t.Disk).Text
        diskName      = $t.Disk.FriendlyName
        expected      = 'INDX block ops (AddIndexEntryAllocation / DeleteIndexEntryAllocation / WriteEndOfIndexBuffer), fixups'
    }
    Write-NextSteps 'C' $t $dir
} finally { Stop-SessionTranscript }
