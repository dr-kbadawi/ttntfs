# SPDX-License-Identifier: GPL-2.0
<#
.SYNOPSIS
  Scenario D (docs/LOGFILE.md 7.4): large file extension (UpdateMappingPairs,
  SetNewAttributeSizes, SetBitsInNonresidentBitMap on $Bitmap, maybe $MFT
  growth). Run it on the 4 KiB stick AND on the 512-byte-cluster stick
  (several LCNs per record).

.DESCRIPTION
  Step 1 (clean): fsutil file createnew X:\D.bin 1048576, safely remove, re-plug.
  Step 2 (dirty): type <300 MB random file> >> X:\D.bin (append = extend the
          existing attribute) in a separate window; the banner tells you to
          pull -PullAfterSeconds (default 2.5) into the copy.

.EXAMPLE
  .\Scenario-D.ps1 -DriveLetter E
  .\Scenario-D.ps1 -DriveLetter F -Tag D-512
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DriveLetter,
    [string]$Tag = 'D',
    [string]$SourceFile = (Join-Path $env:TEMP 'ntfs-capture-300MB.bin'),
    [double]$PullAfterSeconds = 2.5,
    [switch]$SkipPolicyCheck,
    [switch]$AllowLarge
)
. "$PSScriptRoot\Common.ps1"
Assert-Admin
$t = Get-TargetVolume $DriveLetter -AllowLarge:$AllowLarge
$dir = Get-SessionDir $Tag $t
$transcript = Start-SessionTranscript $dir
try {
    Write-Step "Scenario $Tag on $($t.Letter): -> $dir"
    Show-Target $t
    Assert-BetterPerformance $t -Skip:$SkipPolicyCheck
    $target = "$($t.Letter):\D.bin"
    if (Test-Path $target) { throw "$target already exists; delete it, safely remove, re-plug, and start again." }
    $null = New-RandomFile $SourceFile 300MB
    if ($t.Volume.SizeRemaining -lt 400MB) { throw "less than 400 MB free on $($t.Letter):" }

    Write-Step "step 1: fsutil file createnew $target 1048576 (clean part)"
    Save-VolumeState $dir $t.Letter 'before'
    cmd /c "fsutil file createnew $target 1048576" | Out-Null
    if ((Get-Item $target).Length -ne 1048576) { throw "createnew did not produce a 1 MiB file" }
    Invoke-SafeEject $t
    $t = Wait-Replug $t -AllowLarge:$AllowLarge
    $target = "$($t.Letter):\D.bin"
    Save-VolumeState $dir $t.Letter 'replugged'

    Write-Step "step 2: append 300 MB, pull $PullAfterSeconds s in"
    Write-Banner "Hand on the stick. Pull it when the red banner appears ($PullAfterSeconds s into the copy)." 'Yellow'
    Read-Host "    Press Enter to start the 5 s countdown"
    Start-Countdown 5 'appending in'
    $t0 = Get-Date
    $proc = Start-Process -FilePath 'cmd.exe' -ArgumentList "/c type `"$SourceFile`" >> `"$target`"" -WindowStyle Minimized -PassThru
    Start-Sleep -Milliseconds ([int]($PullAfterSeconds * 1000))
    Write-PullBanner "PULL THE STICK NOW (copy running $PullAfterSeconds s)"
    Read-Host "    Press Enter AFTER the stick is out"
    $pulled = Get-Date
    try { if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } } catch { }
    Save-Meta $dir @{
        scenario      = $Tag
        description   = 'append 300 MB to an existing 1 MiB file; pulled mid-copy'
        driveLetter   = $t.Letter
        label         = $t.Volume.FileSystemLabel
        clusterSizeHint = 'see fresh.ntfsinfo.txt from Prepare-Drive (Bytes Per Cluster)'
        sourceFile    = $SourceFile
        target        = $target
        copyStartedAt = $t0.ToString('o')
        pulledAt      = $pulled.ToString('o')
        secondsIntoCopy = [math]::Round(($pulled - $t0).TotalSeconds, 1)
        removalPolicy = (Get-RemovalPolicy $t.Disk).Text
        diskName      = $t.Disk.FriendlyName
        expected      = 'UpdateMappingPairs / SetNewAttributeSizes / SetBitsInNonresidentBitMap; with 512-byte clusters lcns_to_follow > 1'
    }
    Write-NextSteps $Tag $t $dir
} finally { Stop-SessionTranscript }
