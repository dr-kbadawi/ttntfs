# SPDX-License-Identifier: GPL-2.0
<#
.SYNOPSIS
  Scenario E (docs/LOGFILE.md 7.4 / 7.3): baselines and verdicts.

.DESCRIPTION
  -Mode E1          same operations as A, then Safely Remove. Image on the Mac:
                    ntfslog must say CLEAN, version 1.1 (downgrade-at-dismount).
  -Mode GroundTruth after a dirty capture was imaged on the Mac and the stick is
                    back in the PC (Windows replayed at mount): chkdsk X:
                    (read-only), fsutil dirty query, fsutil fsinfo ntfsinfo,
                    dir /s, and the post-replay $LogFile dump -> the reference
                    image is then taken on the Mac again (<scenario>-replayed.img).
  -Mode Verdict     E3: the stick holds OUR replay result (dd of work.img).
                    chkdsk X: /f must find nothing; dir /s must match the
                    GroundTruth listing of -Scenario (compared automatically
                    when that session folder exists).

.EXAMPLE
  .\Scenario-E.ps1 -Mode E1 -DriveLetter E
  .\Scenario-E.ps1 -Mode GroundTruth -Scenario A -DriveLetter E
  .\Scenario-E.ps1 -Mode Verdict -Scenario A -DriveLetter E
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('E1', 'GroundTruth', 'Verdict')][string]$Mode,
    [Parameter(Mandatory = $true)][string]$DriveLetter,
    [string]$Scenario = '',
    [switch]$SkipPolicyCheck,
    [switch]$AllowLarge
)
. "$PSScriptRoot\Common.ps1"
Assert-Admin
if ($Mode -ne 'E1' -and -not $Scenario) { throw "-Scenario (A, B1, B2, C, D, D-512, ...) is required for $Mode" }
$t = Get-TargetVolume $DriveLetter -AllowLarge:$AllowLarge
$name = if ($Mode -eq 'E1') { 'E1' } elseif ($Mode -eq 'GroundTruth') { "$Scenario-replayed" } else { "$Scenario-verdict" }
$dir = Get-SessionDir $name $t
$transcript = Start-SessionTranscript $dir
try {
    Write-Step "Scenario E / $Mode on $($t.Letter): -> $dir"
    Show-Target $t

    if ($Mode -eq 'E1') {
        Assert-BetterPerformance $t -Skip:$SkipPolicyCheck
        foreach ($n in 'A.txt', 'Adir') {
            if (Test-Path (Join-Path $t.Root $n)) { throw "$n already exists on $($t.Letter):; start from a fresh stick." }
        }
        Save-VolumeState $dir $t.Letter 'before'
        $cmd = "echo hello > $($t.Letter):\A.txt & mkdir $($t.Letter):\Adir & echo x > $($t.Letter):\Adir\inner.txt"
        cmd /c $cmd | Out-Null
        Start-Sleep -Seconds 2
        Save-VolumeState $dir $t.Letter 'after'
        Save-Meta $dir @{
            scenario = 'E1'; description = 'A operations then Safely Remove: expect CLEAN log, restart pages version 1.1'
            driveLetter = $t.Letter; label = $t.Volume.FileSystemLabel; command = $cmd
            removalPolicy = (Get-RemovalPolicy $t.Disk).Text; diskName = $t.Disk.FriendlyName
        }
        Write-Step "safely removing"
        Invoke-SafeEject $t
        Write-Host ''
        Write-Host "On the Mac: dd the partition as E1-$($t.Volume.FileSystemLabel)-$(Get-BuildTag)-clean.img; ./build-logfile/ntfslog -v must report CLEAN and version 1.1." -ForegroundColor Cyan
        return
    }

    if ($Mode -eq 'GroundTruth') {
        Write-Host "    (Windows replayed the log when it mounted the stick; this is the reference state.)"
        Start-Sleep -Seconds 2
        $chk = Join-Path $dir "$Scenario-replayed.chkdsk.txt"
        Write-Step "chkdsk $($t.Letter): (read-only)"
        cmd /c "chkdsk $($t.Letter):" 2>&1 | Tee-Object -FilePath $chk | ForEach-Object { "    $_" }
        $chkExit = $LASTEXITCODE
        Write-Host "    chkdsk exit code $chkExit (0 = no errors)"
        Save-VolumeState $dir $t.Letter "$Scenario-replayed"
        $all = Join-Path $dir "$Scenario-replayed.txt"
        Get-Content (Join-Path $dir "$Scenario-replayed.dirty.txt"), (Join-Path $dir "$Scenario-replayed.ntfsinfo.txt"), (Join-Path $dir "$Scenario-replayed.dir.txt") | Set-Content $all -Encoding UTF8
        Write-Step "post-replay `$LogFile dump"
        & "$PSScriptRoot\Dump-LogFile.ps1" -DriveLetter $t.Letter -Scenario "$Scenario-replayed" -OutDir $dir -AllowLarge:$AllowLarge
        $meta = Get-Content (Join-Path $dir 'meta.json') -Raw | ConvertFrom-Json
        Save-Meta $dir @{
            scenario = "$Scenario-replayed"; description = 'ground truth after Windows replay (LOGFILE.md 7.3)'
            driveLetter = $t.Letter; label = $t.Volume.FileSystemLabel
            chkdskExitCode = $chkExit; chkdskFile = $chk; listing = $all
            logfileDump = $meta.logfile
            diskName = $t.Disk.FriendlyName
        }
        Write-Step "safely removing; image again on the Mac as $Scenario-$($t.Volume.FileSystemLabel)-$(Get-BuildTag)-replayed.img"
        Invoke-SafeEject $t
        return
    }

    # Verdict (E3)
    $chk = Join-Path $dir "$Scenario-verdict.chkdsk-f.txt"
    Write-Banner "chkdsk /f WILL MODIFY $($t.Letter): if it finds anything. This stick must hold OUR replay result (work.img)." 'Red'
    Confirm-Typed $t.Letter "    Type the drive letter ($($t.Letter)) to confirm"
    Save-VolumeState $dir $t.Letter 'before-chkdsk'
    Write-Step "chkdsk $($t.Letter): /f"
    cmd /c "echo y | chkdsk $($t.Letter): /f" 2>&1 | Tee-Object -FilePath $chk | ForEach-Object { "    $_" }
    $chkExit = $LASTEXITCODE
    Write-Host "    chkdsk exit code $chkExit (0 = nothing to fix)"
    Save-VolumeState $dir $t.Letter 'after-chkdsk'
    $verdict = if ($chkExit -eq 0) { 'PASS: chkdsk /f found nothing' } else { "FAIL: chkdsk /f exit $chkExit" }
    # compare listing with the ground truth session, if present
    $gtDir = Join-Path (Join-Path $env:USERPROFILE 'ntfs-captures') ("$Scenario-replayed-{0}-{1}" -f $t.Volume.FileSystemLabel, (Get-BuildTag))
    $listingVerdict = 'no ground-truth listing found'
    $gtList = Join-Path $gtDir "$Scenario-replayed.dir.txt"
    if (Test-Path $gtList) {
        $norm = { param($f) Get-Content $f | Where-Object { $_ -match '^\s*\d{2}[./-]' -or $_ -match '^\s*Directory of' } | ForEach-Object { ($_ -replace '\s+', ' ').Trim() } }
        $a = & $norm $gtList
        $b = & $norm (Join-Path $dir 'before-chkdsk.dir.txt')
        $diff = Compare-Object $a $b
        if ($diff) {
            $listingVerdict = "FAIL: $($diff.Count) listing differences"
            $diff | Out-File (Join-Path $dir 'listing-diff.txt') -Encoding UTF8
            $diff | Select-Object -First 20 | ForEach-Object { "    $($_.SideIndicator) $($_.InputObject)" }
        } else { $listingVerdict = 'PASS: dir /s matches the ground truth' }
    }
    Write-Banner "$verdict; $listingVerdict" ($(if ($verdict -like 'PASS*' -and $listingVerdict -notlike 'FAIL*') { 'Green' } else { 'Red' }))
    Save-Meta $dir @{
        scenario = "$Scenario-verdict"; description = 'E3: chkdsk /f verdict on our replay (LOGFILE.md 7.4 E3)'
        driveLetter = $t.Letter; label = $t.Volume.FileSystemLabel
        chkdskExitCode = $chkExit; chkdskFile = $chk; verdict = $verdict; listingVerdict = $listingVerdict
        groundTruthDir = $gtDir; diskName = $t.Disk.FriendlyName
    }
} finally { Stop-SessionTranscript }
