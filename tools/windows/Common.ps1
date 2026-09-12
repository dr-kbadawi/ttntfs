# SPDX-License-Identifier: GPL-2.0
#
# Common.ps1 - shared helpers for the capture scripts. Dot-source it:
#     . "$PSScriptRoot\Common.ps1"
# Windows PowerShell 5.1 compatible (no pwsh needed on the PC).
#
# Safety: every script resolves its target through Get-TargetVolume, which
# refuses C:, the system/boot disk, any non-USB / non-removable disk, and
# (unless -AllowLarge) anything over 64 GB.

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Assert-Admin {
    if (-not (Test-IsAdmin)) {
        throw "Run this from an elevated PowerShell (right-click > Run as administrator)."
    }
}

function Write-Banner {
    param([string]$Text, [string]$Color = 'Yellow')
    $line = '#' * ([Math]::Max(40, $Text.Length + 6))
    Write-Host ''
    Write-Host $line -ForegroundColor $Color
    Write-Host ("#  " + $Text) -ForegroundColor $Color
    Write-Host $line -ForegroundColor $Color
    Write-Host ''
}

function Write-Step { param([string]$Text); Write-Host ("==> " + $Text) -ForegroundColor Cyan }
function Write-Ok   { param([string]$Text); Write-Host ("    ok: " + $Text) -ForegroundColor Green }
function Write-Warn { param([string]$Text); Write-Host ("    WARNING: " + $Text) -ForegroundColor Yellow }

function Invoke-Beep {
    param([int]$Times = 3)
    for ($i = 0; $i -lt $Times; $i++) {
        try { [Console]::Beep(1400, 250) } catch { }
        Start-Sleep -Milliseconds 120
    }
}

# Requires the user to type an exact string; anything else aborts.
function Confirm-Typed {
    param([string]$Expected, [string]$Prompt)
    $answer = Read-Host $Prompt
    if ($answer -cne $Expected) {
        throw "Aborted: expected '$Expected', got '$answer'."
    }
}

function Start-Countdown {
    param([int]$Seconds, [string]$Message = 'starting in')
    for ($s = $Seconds; $s -gt 0; $s--) {
        Write-Host ("    {0} {1}..." -f $Message, $s) -ForegroundColor Magenta
        Start-Sleep -Seconds 1
    }
}

function Get-WindowsBuild {
    $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    $ubr = 0
    if ($cv.PSObject.Properties['UBR']) { $ubr = $cv.UBR }
    $disp = ''
    if ($cv.PSObject.Properties['DisplayVersion']) { $disp = $cv.DisplayVersion }
    $ver = ''
    try { $ver = (cmd /c ver 2>$null | Where-Object { $_ }) -join '' } catch { }
    [pscustomobject]@{
        ProductName    = $cv.ProductName
        DisplayVersion = $disp
        Build          = ('{0}.{1}' -f $cv.CurrentBuildNumber, $ubr)
        Ver            = $ver.Trim()
    }
}

function Get-BuildTag {
    $b = Get-WindowsBuild
    'win' + $b.Build
}

# Resolves a drive letter to volume/partition/disk and enforces the safety rules.
function Get-TargetVolume {
    param(
        [Parameter(Mandatory = $true)][string]$DriveLetter,
        [switch]$AllowLarge
    )
    $letter = $DriveLetter.Trim().TrimEnd(':', '\').ToUpper()
    if ($letter -notmatch '^[A-Z]$') {
        throw "DriveLetter must be a single letter such as E (got '$DriveLetter')."
    }
    $sys = $env:SystemDrive.TrimEnd(':').ToUpper()
    if ($letter -eq 'C' -or $letter -eq $sys) {
        throw "Refusing to touch ${letter}: (system drive)."
    }
    $vol = Get-Volume -DriveLetter $letter -ErrorAction SilentlyContinue
    if (-not $vol) { throw "No volume is mounted at ${letter}:." }
    $part = Get-Partition -DriveLetter $letter -ErrorAction SilentlyContinue
    if (-not $part) { throw "No partition found for ${letter}: (is it a CD or a network drive?)." }
    $disk = Get-Disk -Number $part.DiskNumber
    if ($disk.IsBoot -or $disk.IsSystem) {
        throw "Refusing: disk $($disk.Number) ($($disk.FriendlyName)) is the boot/system disk."
    }
    $removable = ($disk.BusType -eq 'USB') -or ($vol.DriveType -eq 'Removable')
    if (-not $removable) {
        throw "Refusing: ${letter}: is on disk $($disk.Number) ($($disk.FriendlyName), bus $($disk.BusType), drive type $($vol.DriveType)). Only USB / removable drives are allowed."
    }
    if (-not $AllowLarge -and $disk.Size -gt 64GB) {
        throw "Refusing: disk $($disk.Number) is $([math]::Round($disk.Size / 1GB)) GB. Captures are imaged whole on the Mac; use a 2-8 GB stick, or pass -AllowLarge."
    }
    [pscustomobject]@{
        Letter    = $letter
        Root      = "${letter}:\"
        Volume    = $vol
        Partition = $part
        Disk      = $disk
    }
}

function Show-Target {
    param($Target)
    $d = $Target.Disk; $v = $Target.Volume
    Write-Host ("    target   : {0}:  label '{1}'  fs {2}  {3:N0} bytes" -f $Target.Letter, $v.FileSystemLabel, $v.FileSystem, $v.Size)
    Write-Host ("    disk     : #{0}  {1}  bus {2}  {3:N1} GB  partition style {4}" -f $d.Number, $d.FriendlyName, $d.BusType, ($d.Size / 1GB), $d.PartitionStyle)
    Write-Host ("    windows  : {0} {1} build {2}" -f (Get-WindowsBuild).ProductName, (Get-WindowsBuild).DisplayVersion, (Get-WindowsBuild).Build)
}

# Best effort: the "Quick removal" / "Better performance" policy of a USB disk.
# Partmgr stores DEVICE_REMOVAL_POLICY (wdm.h) under the device's registry key:
#   2 = ExpectOrderlyRemoval  -> "Better performance" (write cache on)
#   3 = ExpectSurpriseRemoval -> "Quick removal" (the default for USB sticks)
# Not every stack writes UserRemovalPolicy; RemovalPolicy is the effective value.
function Get-RemovalPolicy {
    param($Disk)
    $result = [pscustomobject]@{ User = $null; Effective = $null; Text = 'unknown' }
    try {
        $pnp = $Disk.Path   # e.g. \\?\usbstor#disk&ven_...#...#{...}
        $id = $null
        if ($pnp -match '^\\\\\?\\(.+)#\{') { $id = $matches[1] -replace '#', '\' }
        if ($id) {
            $key = "HKLM:\SYSTEM\CurrentControlSet\Enum\$id\Device Parameters\Partmgr"
            if (Test-Path $key) {
                $p = Get-ItemProperty $key
                if ($p.PSObject.Properties['UserRemovalPolicy']) { $result.User = $p.UserRemovalPolicy }
                if ($p.PSObject.Properties['RemovalPolicy']) { $result.Effective = $p.RemovalPolicy }
            }
        }
        $eff = $result.User
        if ($null -eq $eff) { $eff = $result.Effective }
        switch ($eff) {
            2 { $result.Text = 'Better performance (ExpectOrderlyRemoval)' }
            3 { $result.Text = 'Quick removal (ExpectSurpriseRemoval)' }
            1 { $result.Text = 'ExpectNoRemoval' }
            default { $result.Text = 'unknown' }
        }
    } catch { }
    $result
}

# The dirty scenarios need the write cache on (docs/LOGFILE.md 7.1).
function Assert-BetterPerformance {
    param($Target, [switch]$Skip)
    $p = Get-RemovalPolicy $Target.Disk
    Write-Host ("    removal policy from registry: {0} (UserRemovalPolicy={1}, RemovalPolicy={2})" -f $p.Text, $p.User, $p.Effective)
    if ($Skip) { return }
    if ($p.Text -like 'Quick*') {
        Write-Banner "This stick is on 'Quick removal'. The log is flushed within a second; a pull will very likely capture a CLEAN log." Red
    }
    Write-Host "    Device Manager > Disk drives > $($Target.Disk.FriendlyName) > Policies > 'Better performance'"
    Write-Host "    (leave 'Turn off Windows write-cache buffer flushing' unticked; re-plug after changing)"
    Confirm-Typed 'YES' "    Is 'Better performance' selected for this stick? Type YES to continue"
}

# Session folder on C: for one capture: %USERPROFILE%\ntfs-captures\<scenario>-<label>-<winbuild>\
function Get-SessionDir {
    param([string]$Scenario, $Target)
    $label = $Target.Volume.FileSystemLabel
    if (-not $label) { $label = 'nolabel' }
    $name = '{0}-{1}-{2}' -f $Scenario, $label, (Get-BuildTag)
    $dir = Join-Path (Join-Path $env:USERPROFILE 'ntfs-captures') $name
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $dir
}

# fsutil / dir snapshot of a mounted volume into $Dir\<Tag>.*.txt
function Save-VolumeState {
    param([string]$Dir, [string]$Letter, [string]$Tag)
    $drive = "${Letter}:"
    cmd /c "fsutil dirty query $drive"        2>&1 | Out-File -FilePath (Join-Path $Dir "$Tag.dirty.txt") -Encoding UTF8
    cmd /c "fsutil fsinfo ntfsinfo $drive"    2>&1 | Out-File -FilePath (Join-Path $Dir "$Tag.ntfsinfo.txt") -Encoding UTF8
    cmd /c "fsutil fsinfo volumeinfo $drive"  2>&1 | Out-File -FilePath (Join-Path $Dir "$Tag.volumeinfo.txt") -Encoding UTF8
    cmd /c "dir /s /a /-c $drive\"           2>&1 | Out-File -FilePath (Join-Path $Dir "$Tag.dir.txt") -Encoding UTF8
    Write-Ok "volume state saved as $Tag.* in $Dir"
}

function Get-DirtyText {
    param([string]$Letter)
    (cmd /c "fsutil dirty query ${Letter}:" 2>&1 | Out-String).Trim()
}

function Save-Meta {
    param([string]$Dir, [hashtable]$Meta)
    $Meta['savedAt'] = (Get-Date).ToString('o')
    $Meta['windows'] = Get-WindowsBuild
    $path = Join-Path $Dir 'meta.json'
    $Meta | ConvertTo-Json -Depth 8 | Set-Content -Path $path -Encoding UTF8
    Write-Ok "meta.json written: $path"
}

function Start-SessionTranscript {
    param([string]$Dir)
    $path = Join-Path $Dir ('transcript-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.txt')
    try { Start-Transcript -Path $path -Append | Out-Null } catch { Write-Warn "transcript not started: $_" }
    $path
}

function Stop-SessionTranscript {
    try { Stop-Transcript | Out-Null } catch { }
}

# Safely remove (eject) a USB stick the way Explorer does.
function Invoke-SafeEject {
    param($Target)
    $ok = $false
    try {
        $shell = New-Object -ComObject Shell.Application
        $item = $shell.NameSpace(17).ParseName($Target.Root)
        if ($item) { $item.InvokeVerb('Eject'); $ok = $true }
    } catch { Write-Warn "Shell eject failed: $_" }
    if ($ok) {
        Start-Sleep -Seconds 3
        if (Get-Volume -DriveLetter $Target.Letter -ErrorAction SilentlyContinue) {
            $ok = $false
            Write-Warn "volume still present after Eject"
        }
    }
    if (-not $ok) {
        Write-Host "    Eject it yourself: Explorer > right-click $($Target.Letter): > Eject (or the tray icon 'Safely Remove Hardware')."
    }
    Read-Host "    When the stick is OUT, press Enter"
}

# Waits until the same volume (by label) is back; re-resolves the target.
function Wait-Replug {
    param($Target, [switch]$AllowLarge)
    $label = $Target.Volume.FileSystemLabel
    Read-Host "    Plug the stick back in, wait for Explorer to show it, then press Enter"
    for ($i = 0; $i -lt 30; $i++) {
        $v = Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.FileSystemLabel -eq $label -and $_.DriveLetter }
        if ($v) {
            $t = Get-TargetVolume ([string]$v[0].DriveLetter) -AllowLarge:$AllowLarge
            Write-Ok "back as $($t.Letter): ($label)"
            return $t
        }
        Start-Sleep -Seconds 1
    }
    throw "Volume '$label' did not come back."
}

# Big random file for the copy scenarios (B1, D). Random so the data is
# distinguishable from zeroed clusters in the images.
function New-RandomFile {
    param([string]$Path, [long]$Bytes)
    if ((Test-Path $Path) -and (Get-Item $Path).Length -eq $Bytes) { return $Path }
    Write-Step ("creating {0} ({1:N0} bytes of random data)" -f $Path, $Bytes)
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    $fs = [System.IO.File]::Open($Path, 'Create', 'Write', 'None')
    try {
        $chunk = New-Object byte[] (4MB)
        $left = $Bytes
        while ($left -gt 0) {
            $rng.GetBytes($chunk)
            $n = [int][Math]::Min($chunk.Length, $left)
            $fs.Write($chunk, 0, $n)
            $left -= $n
        }
    } finally { $fs.Close() }
    $Path
}

function Write-PullBanner {
    param([string]$What = 'PULL THE STICK NOW')
    Write-Banner $What 'Red'
    Invoke-Beep 3
}

function Write-NextSteps {
    param([string]$Scenario, $Target, [string]$Dir)
    Write-Host ''
    Write-Host "Next (docs/LOGFILE.md 7.2/7.3):" -ForegroundColor Cyan
    Write-Host "  1. Do NOT plug the stick into any Windows PC yet (Windows replays and cleans the log at mount)."
    Write-Host "  2. On the Mac: diskutil unmountDisk /dev/diskN; sudo dd if=/dev/rdiskNs1 of=$Scenario-$($Target.Volume.FileSystemLabel)-$(Get-BuildTag)-dirty.img bs=1m status=progress"
    Write-Host "  3. Back here: plug it in, wait 10 s, then  .\Scenario-E.ps1 -Mode GroundTruth -Scenario $Scenario -DriveLetter $($Target.Letter)"
    Write-Host "  Session folder: $Dir"
}
