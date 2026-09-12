# SPDX-License-Identifier: GPL-2.0
<#
.SYNOPSIS
  Copies the raw $LogFile (MFT record 2, unnamed $DATA) of a mounted NTFS
  volume into logfile-<scenario>.bin, plus the boot sector and a meta JSON.

.DESCRIPTION
  Opens \\.\X: read-only (elevated), parses the boot sector, reads MFT
  record 2 from the MFT's first extent, applies the update-sequence fixups,
  decodes the non-resident $DATA mapping pairs and copies every run in VCN
  order. No third-party tool. Output goes to the session folder
  %USERPROFILE%\ntfs-captures\<scenario>-<label>-<build>\ unless -OutDir.

  A dump taken on Windows is always POST-replay: Windows replays and cleans
  the log when it mounts the volume. The pre-replay (dirty) state can only be
  captured on the Mac (docs/LOGFILE.md 7.2: dd the partition, then
  ntfscat -i 2 -a 0x80 image > x.logfile). Use this script for:
    * E1 baselines (clean, freshly dismounted logs) - after a re-plug
    * the post-replay log of every scenario, next to the ground-truth listing
    * checking what a stick looks like right now

  Free-tool alternative (documented, not required): NTFS-3G's ntfscat on the
  Mac against the dd image or the device node does the same extraction.

.EXAMPLE
  .\Dump-LogFile.ps1 -DriveLetter E -Scenario A-replayed
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DriveLetter,
    [Parameter(Mandatory = $true)][string]$Scenario,
    [string]$OutDir
)
. "$PSScriptRoot\Common.ps1"

Assert-Admin
$t = Get-TargetVolume $DriveLetter
Write-Step "target"
Show-Target $t
if ($t.Volume.FileSystem -ne 'NTFS') { throw "$($t.Letter): is $($t.Volume.FileSystem), not NTFS." }
if (-not $OutDir) { $OutDir = Get-SessionDir $Scenario $t }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$safe = $Scenario -replace '[^A-Za-z0-9._-]', '_'
$outBin = Join-Path $OutDir "logfile-$safe.bin"
$outBoot = Join-Path $OutDir "bootsector-$safe.bin"

# ---- raw volume access (aligned reads) --------------------------------------
$path = "\\.\$($t.Letter):"
Write-Step "opening $path read-only"
$fs = New-Object System.IO.FileStream($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite, 1)
$script:sector = 512

function Read-Aligned {
    param([long]$Offset, [long]$Length)
    $ss = $script:sector
    $start = [long][Math]::Floor($Offset / $ss) * $ss
    $end = [long][Math]::Ceiling(($Offset + $Length) / $ss) * $ss
    $buf = New-Object byte[] ($end - $start)
    $null = $fs.Seek($start, [System.IO.SeekOrigin]::Begin)
    $done = 0
    while ($done -lt $buf.Length) {
        $n = $fs.Read($buf, $done, $buf.Length - $done)
        if ($n -le 0) { throw "short read at $($start + $done)" }
        $done += $n
    }
    $skip = $Offset - $start
    if ($skip -eq 0 -and $buf.Length -eq $Length) { return $buf }
    $out = New-Object byte[] $Length
    [Array]::Copy($buf, $skip, $out, 0, $Length)
    $out
}
function U16 { param([byte[]]$b, [int]$o); [System.BitConverter]::ToUInt16($b, $o) }
function U32 { param([byte[]]$b, [int]$o); [System.BitConverter]::ToUInt32($b, $o) }
function U64 { param([byte[]]$b, [int]$o); [System.BitConverter]::ToUInt64($b, $o) }

try {
    # ---- boot sector ---------------------------------------------------------
    Write-Step "boot sector"
    $boot = Read-Aligned 0 512
    $oem = [System.Text.Encoding]::ASCII.GetString($boot, 3, 8)
    if ($oem -ne 'NTFS    ') { throw "OEM id is '$oem', not NTFS." }
    $bps = [int](U16 $boot 0x0B)
    $spc = [int]$boot[0x0D]
    if ($spc -gt 0x80) { $spc = [int][Math]::Pow(2, 256 - $spc) }
    $cluster = $bps * $spc
    $totalSectors = U64 $boot 0x28
    $mftLcn = U64 $boot 0x30
    $mftMirrLcn = U64 $boot 0x38
    $cpm = [int]$boot[0x40]
    if ($cpm -gt 127) { $cpm = $cpm - 256 }          # signed: negative means 2^-n bytes
    if ($cpm -lt 0) { $mftRec = [int][Math]::Pow(2, -$cpm) } else { $mftRec = $cpm * $cluster }
    $serial = U64 $boot 0x48
    $script:sector = $bps
    [System.IO.File]::WriteAllBytes($outBoot, $boot)
    Write-Host ("    bytes/sector {0}, cluster {1}, MFT record {2}, MFT LCN {3}, serial {4:X16}" -f $bps, $cluster, $mftRec, $mftLcn, $serial)

    # ---- MFT record 2 ($LogFile) -------------------------------------------
    Write-Step "MFT record 2 (`$LogFile)"
    $recOff = [long]$mftLcn * $cluster + 2 * $mftRec
    $rec = Read-Aligned $recOff $mftRec
    $magic = [System.Text.Encoding]::ASCII.GetString($rec, 0, 4)
    if ($magic -ne 'FILE') { throw "record 2 magic is '$magic' (expected FILE); MFT start not where the boot sector says." }
    # fixups: NTFS_BLOCK_SIZE is 512 regardless of the sector size
    $usaOfs = U16 $rec 4; $usaCnt = U16 $rec 6
    $usn = U16 $rec $usaOfs
    for ($i = 1; $i -lt $usaCnt; $i++) {
        $pos = $i * 512 - 2
        if ((U16 $rec $pos) -ne $usn) { throw "fixup mismatch in record 2 block $i (torn record?)" }
        $rec[$pos] = $rec[$usaOfs + 2 * $i]; $rec[$pos + 1] = $rec[$usaOfs + 2 * $i + 1]
    }
    $flags = U16 $rec 0x16
    if (($flags -band 1) -eq 0) { throw "record 2 is not in use" }
    $aoff = [int](U16 $rec 0x14)
    $found = $null
    while ($aoff + 8 -le $rec.Length) {
        $type = U32 $rec $aoff
        if ($type -eq 0xFFFFFFFF) { break }
        $alen = [int](U32 $rec ($aoff + 4))
        if ($alen -le 0) { break }
        $nonres = $rec[$aoff + 8]; $nlen = $rec[$aoff + 9]
        if ($type -eq 0x80 -and $nlen -eq 0) {
            if ($nonres -ne 1) { throw "`$LogFile `$DATA is resident?!" }
            $found = [pscustomobject]@{
                lowestVcn       = U64 $rec ($aoff + 0x10)
                highestVcn      = U64 $rec ($aoff + 0x18)
                mpOffset        = [int](U16 $rec ($aoff + 0x20))
                allocatedSize   = U64 $rec ($aoff + 0x28)
                dataSize        = U64 $rec ($aoff + 0x30)
                initializedSize = U64 $rec ($aoff + 0x38)
                base            = $aoff
            }
            break
        }
        if ($type -eq 0x20) { Write-Warn "record 2 has an `$ATTRIBUTE_LIST; if `$DATA is missing below it lives in an extension record (not handled)" }
        $aoff += $alen
    }
    if (-not $found) { throw "unnamed `$DATA attribute not found in record 2" }
    Write-Host ("    data size {0:N0}, allocated {1:N0}, initialized {2:N0}, VCNs {3}-{4}" -f $found.dataSize, $found.allocatedSize, $found.initializedSize, $found.lowestVcn, $found.highestVcn)

    # ---- mapping pairs -> runs ----------------------------------------------
    $p = $found.base + $found.mpOffset
    $runs = @()
    $vcn = [long]$found.lowestVcn
    $lcn = [long]0
    while ($p -lt $rec.Length -and $rec[$p] -ne 0) {
        $hdr = $rec[$p]; $lb = $hdr -band 0x0F; $ob = ($hdr -shr 4) -band 0x0F
        $p++
        $len = [long]0
        for ($i = 0; $i -lt $lb; $i++) { $len = $len -bor ([long]$rec[$p + $i] -shl (8 * $i)) }
        $p += $lb
        if ($ob -eq 0) { throw "sparse run in `$LogFile (unexpected)" }
        $delta = [long]0
        for ($i = 0; $i -lt $ob; $i++) { $delta = $delta -bor ([long]$rec[$p + $i] -shl (8 * $i)) }
        if (($rec[$p + $ob - 1] -band 0x80) -ne 0) { $delta = $delta - ([long]1 -shl (8 * $ob)) }
        $p += $ob
        $lcn += $delta
        $runs += [pscustomobject]@{ vcn = $vcn; lcn = $lcn; length = $len }
        $vcn += $len
    }
    if ($runs.Count -eq 0) { throw "empty run list" }
    $runs | ForEach-Object { Write-Host ("    run vcn {0,8} lcn {1,10} len {2,6} clusters" -f $_.vcn, $_.lcn, $_.length) }

    # ---- copy ----------------------------------------------------------------
    Write-Step "copying to $outBin"
    $out = [System.IO.File]::Open($outBin, 'Create', 'Write', 'None')
    try {
        $remaining = [long]$found.dataSize
        $chunkClusters = [Math]::Max(1, [int](4MB / $cluster))
        foreach ($r in $runs) {
            $left = [long]$r.length
            $c = [long]$r.lcn
            while ($left -gt 0 -and $remaining -gt 0) {
                $n = [Math]::Min($left, $chunkClusters)
                $bytes = [long]$n * $cluster
                if ($bytes -gt $remaining) { $bytes = $remaining }
                $buf = Read-Aligned ([long]$c * $cluster) $bytes
                $out.Write($buf, 0, $buf.Length)
                $remaining -= $bytes; $left -= $n; $c += $n
            }
        }
    } finally { $out.Close() }
    $size = (Get-Item $outBin).Length
    $hash = (Get-FileHash -Algorithm SHA256 $outBin).Hash
    Write-Ok ("{0:N0} bytes, sha256 {1}" -f $size, $hash)
    $first = New-Object byte[] 8
    $in = [System.IO.File]::OpenRead($outBin)
    try { $null = $in.Read($first, 0, 8) } finally { $in.Close() }
    $sig = [System.Text.Encoding]::ASCII.GetString($first, 0, 4)
    Write-Host ("    first page signature: '{0}' (RSTR = restart page, CHKD = chkdsk wrote it, 0xFF.. = never written)" -f $sig)

    Save-Meta $OutDir @{
        step          = 'dump-logfile'
        scenario      = $Scenario
        note          = 'Dumped on Windows: this is the POST-replay state of the log.'
        driveLetter   = $t.Letter
        label         = $t.Volume.FileSystemLabel
        serialHex     = ('{0:X16}' -f $serial)
        bytesPerSector = $bps
        clusterSize   = $cluster
        mftRecordSize = $mftRec
        mftLcn        = $mftLcn
        mftMirrLcn    = $mftMirrLcn
        totalSectors  = $totalSectors
        logfile       = @{
            dataSize = $found.dataSize; allocatedSize = $found.allocatedSize; initializedSize = $found.initializedSize
            runs = @($runs | ForEach-Object { @{ vcn = $_.vcn; lcn = $_.lcn; length = $_.length } })
            file = $outBin; sha256 = $hash; bytes = $size; firstPageSignature = $sig
        }
        bootSectorFile = $outBoot
        dirtyQuery    = (Get-DirtyText $t.Letter)
        diskName      = $t.Disk.FriendlyName
        removalPolicy = (Get-RemovalPolicy $t.Disk).Text
    }
} finally {
    $fs.Close()
}
Write-Host ''
Write-Host "Bring back to the Mac: $outBin ($([math]::Round($size/1MB,1)) MiB), $outBoot, meta.json" -ForegroundColor Cyan
