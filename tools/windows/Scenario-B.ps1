# SPDX-License-Identifier: GPL-2.0
<#
.SYNOPSIS
  Scenario B (docs/LOGFILE.md 7.4): hibernation (B1) or Fast Startup hybrid
  shutdown (B2) with writes pending on the stick.

.DESCRIPTION
  B1: starts  copy <200 MB random file> X:\B1.bin  in a separate window and
      ~0.8 s later runs  shutdown /h  (hibernate; enabled if needed).
  B2: makes sure Fast Startup is enabled (HiberbootEnabled=1, hibernate on),
      writes  echo fs > X:\B2.txt  and runs  shutdown /s /hybrid /t 0.
  Both: meta.json is written BEFORE the shutdown. When the PC is off, pull the
  stick, image it on the Mac, then boot the PC WITHOUT the stick (so a resumed
  session does not find it), then plug it in for the ground truth.

.EXAMPLE
  .\Scenario-B.ps1 -Mode B1 -DriveLetter E
  .\Scenario-B.ps1 -Mode B2 -DriveLetter E
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('B1', 'B2')][string]$Mode,
    [Parameter(Mandatory = $true)][string]$DriveLetter,
    [string]$SourceFile = (Join-Path $env:TEMP 'ntfs-capture-200MB.bin'),
    [int]$ShutdownDelayMs = 800,
    [switch]$SkipPolicyCheck
)
. "$PSScriptRoot\Common.ps1"
Assert-Admin
$t = Get-TargetVolume $DriveLetter
$dir = Get-SessionDir $Mode $t
$transcript = Start-SessionTranscript $dir
try {
    Write-Step "Scenario $Mode on $($t.Letter): -> $dir"
    Show-Target $t
    Assert-BetterPerformance $t -Skip:$SkipPolicyCheck

    # hibernation must be available for both /h and hybrid shutdown; turning it
    # on is idempotent, and 'powercfg /a' lists Hibernate under both the
    # available and the not-available headings, so it is not parsed.
    cmd /c 'powercfg /hibernate on' 2>&1 | Out-Null
    cmd /c 'powercfg /a' 2>&1 | ForEach-Object { "    $_" }
    $hb = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power' -ErrorAction SilentlyContinue
    $fastStartup = ($hb -and $hb.PSObject.Properties['HiberbootEnabled'] -and $hb.HiberbootEnabled -eq 1)
    Write-Host "    Fast Startup (HiberbootEnabled): $fastStartup"
    if ($Mode -eq 'B2' -and -not $fastStartup) {
        Write-Warn "enabling Fast Startup (HiberbootEnabled=1)"
        Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power' -Name HiberbootEnabled -Value 1 -Type DWord
        $fastStartup = $true
    }
    Save-VolumeState $dir $t.Letter 'before'

    $target = if ($Mode -eq 'B1') { "$($t.Letter):\B1.bin" } else { "$($t.Letter):\B2.txt" }
    if (Test-Path $target) { throw "$target already exists; clean the stick first (safely remove/re-plug afterwards)." }
    if ($Mode -eq 'B1') { $null = New-RandomFile $SourceFile 200MB }

    Write-Banner "The PC will $(if ($Mode -eq 'B1') {'HIBERNATE'} else {'HYBRID-SHUTDOWN'}) in a moment. Save your other work now." 'Red'
    Write-Host "    After it is OFF: pull the stick, image it on the Mac, boot the PC WITHOUT the stick."
    Confirm-Typed 'YES' "    Type YES to continue"

    $description = if ($Mode -eq 'B1') { 'copy of 200 MB in progress, shutdown /h' } else { 'echo fs > B2.txt, shutdown /s /hybrid /t 0 (Fast Startup)' }
    $expected = if ($Mode -eq 'B1') { 'log open with the copy transactions (hibernation does not dismount)' } else { 'either dirty (like internal volumes) or flushed; record which' }
    Save-Meta $dir @{
        scenario      = $Mode
        description   = $description
        driveLetter   = $t.Letter
        label         = $t.Volume.FileSystemLabel
        sourceFile    = $SourceFile
        target        = $target
        fastStartup   = $fastStartup
        shutdownDelayMs = $ShutdownDelayMs
        startedAt     = (Get-Date).ToString('o')
        removalPolicy = (Get-RemovalPolicy $t.Disk).Text
        diskName      = $t.Disk.FriendlyName
        expected      = $expected
    }
    Stop-SessionTranscript
    Start-Countdown 3 'going down in'
    if ($Mode -eq 'B1') {
        Start-Process -FilePath 'cmd.exe' -ArgumentList "/c copy /b `"$SourceFile`" `"$target`"" -WindowStyle Minimized
        Start-Sleep -Milliseconds $ShutdownDelayMs
        Write-PullBanner "HIBERNATING - when the PC is off, PULL THE STICK"
        cmd /c 'shutdown /h'
    } else {
        cmd /c "echo fs > $target" | Out-Null
        Write-PullBanner "HYBRID SHUTDOWN - when the PC is off, PULL THE STICK"
        cmd /c 'shutdown /s /hybrid /t 0'
    }
} finally { Stop-SessionTranscript }
