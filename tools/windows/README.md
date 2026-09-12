# tools/windows — `$LogFile` capture toolkit for the Windows PC

PowerShell scripts that run the capture session in `docs/LOGFILE.md` §7 on a
physical Windows PC (Windows PowerShell 5.1, no pwsh needed). They format the
sticks, perform each scenario's writes, tell you the exact moment to pull the
stick, record `fsutil`/`chkdsk`/`dir` ground truth, and dump the raw `$LogFile`.

Every script takes the drive letter **explicitly** and refuses `C:`, the
system/boot disk, and anything that is not USB/removable. Destructive steps require typing the drive
letter or `YES`.

```
Common.ps1          shared helpers (safety checks, session folder, fsutil snapshots, eject/re-plug)
Prepare-Drive.ps1   format a stick NTFS with label + cluster size
Scenario-A.ps1      file + dir created, then pull
Scenario-B.ps1      -Mode B1 hibernate mid-copy / -Mode B2 Fast Startup hybrid shutdown
Scenario-C.ps1      multi-block $I30 index, then del/create/rename and pull
Scenario-D.ps1      1 MiB file extended by 300 MB, pull mid-copy (run on both sticks)
Scenario-E.ps1      -Mode E1 clean baseline / GroundTruth (§7.3) / Verdict (E3 chkdsk /f)
Dump-LogFile.ps1    raw $LogFile + boot sector + meta.json via \\.\X: (MFT record 2 parsing)
```

Everything a script produces lands in `%USERPROFILE%\ntfs-captures\<name>-<label>-win<build>\`
(`meta.json`, `transcript-*.txt`, `*.dirty.txt`, `*.ntfsinfo.txt`, `*.dir.txt`, ...).

## 0. Prerequisites (10 minutes)

1. Two USB sticks, 2–8 GB (whole-partition images must be practical on the Mac).
2. An **elevated** PowerShell: Start → type `powershell` → right-click → Run as
   administrator. In it:
   ```powershell
   Set-ExecutionPolicy -Scope Process Bypass -Force
   cd <where you copied tools\windows>
   ```
3. Note the Windows build (`winver`); the scripts record it in every `meta.json`.
4. Copy this folder to the PC (USB stick, or share). Nothing is installed.

## 1. Prepare the sticks

```powershell
.\Prepare-Drive.ps1 -DriveLetter E -Label CAP1                   # 4 KiB clusters
.\Prepare-Drive.ps1 -DriveLetter F -Label CAP512 -ClusterSize 512 # 512-byte clusters
```

The script shows the disk it resolved (number, model, bus, size), warns in red,
and asks you to type the letter. It quick-formats, saves `fresh.ntfsinfo.txt`,
and prints the registry reading of the removal policy.

Then, for each stick, **switch the write-cache policy** (needed for every dirty
scenario; on "Quick removal" the log is flushed within a second and a pull
captures a clean log):
Device Manager → Disk drives → *the stick* → Policies → **Better performance**
(leave "Turn off Windows write-cache buffer flushing" unticked). Unplug and
re-plug. Each scenario prints the registry value it sees (`UserRemovalPolicy`
2 = Better performance, 3 = Quick removal) and asks you to confirm with `YES`.

## 2. The dirty captures (A, C, D, D-512, B1, B2)

Each script: saves the *before* state, counts down 5 s, performs the exact
commands from `docs/LOGFILE.md` §7.4, beeps and shows a red **PULL THE STICK
NOW** banner, waits for Enter, writes `meta.json` with timestamps, and prints
what to do next.

```powershell
.\Scenario-A.ps1 -DriveLetter E
.\Scenario-C.ps1 -DriveLetter E          # ejects/re-plugs once in the middle (asks you)
.\Scenario-D.ps1 -DriveLetter E          # 300 MB random source file is created in %TEMP% on first run
.\Scenario-D.ps1 -DriveLetter F -Tag D-512
.\Scenario-B.ps1 -Mode B1 -DriveLetter E # HIBERNATES the PC ~0.8 s into a 200 MB copy
.\Scenario-B.ps1 -Mode B2 -DriveLetter E # enables Fast Startup, hybrid shutdown
```

**After every pull: do not plug the stick into any Windows PC.** Windows replays
and cleans the log at mount (also via autoplay after a reboot). Walk to the Mac
and image first (§7.2):

```
diskutil list                              # e.g. disk4, partition disk4s1
diskutil unmountDisk /dev/disk4
sudo dd if=/dev/rdisk4s1 of=A-CAP1-win26100.1234-dirty.img bs=1m status=progress
tools/.local/bin/ntfscat -i 2 -a 0x80 A-CAP1-...-dirty.img > A-CAP1-...-dirty.logfile
./build-logfile/ntfslog -v A-CAP1-...-dirty.img > A-CAP1-...-dirty.ntfslog.txt
```

Use the name the script printed (`<scenario>-<label>-win<build>-dirty.img`).
For B1/B2: after the PC is off, pull the stick, image, and **boot the PC without
the stick** before plugging it back in.

Between scenarios on the same stick, delete the previous scenario's files and
eject properly (or re-run `Prepare-Drive.ps1`); the scripts refuse to run when
their target names already exist.

## 3. Ground truth on Windows (§7.3), after the Mac image exists

Plug the stick back into the PC, wait 10 s (Windows replays), then:

```powershell
.\Scenario-E.ps1 -Mode GroundTruth -Scenario A -DriveLetter E
```

runs `chkdsk E:` (read-only; must report no errors), `fsutil dirty query`,
`fsutil fsinfo ntfsinfo`, `dir /s`, dumps the **post-replay** `$LogFile`
(`logfile-A-replayed.bin`), and ejects. Then image the stick again on the Mac as
`A-CAP1-win<build>-replayed.img` — that image is the reference our replay must
reproduce (E2). Repeat with `-Scenario C`, `D`, `D-512`, `B1`, `B2`.

## 4. Baselines and verdict

```powershell
.\Scenario-E.ps1 -Mode E1 -DriveLetter E
```
does A's operations and then ejects properly. Image on the Mac: `ntfslog` must
say CLEAN and show restart pages version 1.1 (the downgrade-at-dismount belief;
also a real initialized 1.1 restart page to diff against `mark_clean`).

E3 — after our replay (`ntfslog --apply work.img`) was written to a stick with
`sudo dd if=work.img of=/dev/rdisk4s1 bs=1m`, plug it into the PC and run

```powershell
.\Scenario-E.ps1 -Mode Verdict -Scenario A -DriveLetter E
```

It runs `chkdsk E: /f` (must find nothing), and compares `dir /s` against the
GroundTruth listing of the same scenario when that session folder exists.
PASS/FAIL is printed and stored in `meta.json`.

## 5. Dumping `$LogFile` on Windows

```powershell
.\Dump-LogFile.ps1 -DriveLetter E -Scenario A-replayed
```

Opens `\\.\E:` read-only, parses the boot sector (bytes/sector, cluster size,
MFT LCN, MFT record size, serial), reads MFT record 2, applies the fixups,
decodes the unnamed `$DATA` mapping pairs and copies the runs in VCN order to
`logfile-<scenario>.bin`; also writes `bootsector-<scenario>.bin` and a
`meta.json` with geometry, runs, size, SHA-256, `fsutil dirty query`, and the
first page signature (`RSTR` expected). No third-party tool is involved. A dump
taken on Windows is always post-replay; the pre-replay state exists only in the
Mac image (`ntfscat -i 2 -a 0x80` extracts the same bytes there). Documented
free alternative: run NTFS-3G's `ntfscat` on the Mac against the dd image or
`/dev/rdiskNs1` — same output; there is no need for a Windows-side tool.

## 6. What to bring back to the Mac

From the Mac side (already there): `<scenario>-<label>-win<build>-dirty.img`
and `-replayed.img` for each scenario, plus `E1-...-clean.img` (each the size of
the partition: 2–8 GB; `zstd`/`gzip` compress the empty space well).

From the PC, the whole `%USERPROFILE%\ntfs-captures\` tree (copy it to a stick
or share). Expected contents and sizes:

| file | size | what |
|---|---|---|
| `<scenario>-<label>-win<build>\meta.json` | 1–3 KB | timestamps, commands, policy, geometry |
| `...\transcript-*.txt` | 5–50 KB | console transcript of the run |
| `...\before.*`, `...\replugged.*` | KBs | `fsutil dirty query`, `ntfsinfo`, `volumeinfo`, `dir /s` before the pull |
| `<scenario>-replayed-...\<scenario>-replayed.chkdsk.txt` | 1–2 KB | read-only chkdsk output (must be clean) |
| `<scenario>-replayed-...\<scenario>-replayed.txt` | KBs | dirty query + ntfsinfo + dir /s (§7.3 listing) |
| `<scenario>-replayed-...\logfile-<scenario>-replayed.bin` | = `$LogFile` size, typically 64 MiB (`meta.json` → `logfile.dataSize`) | post-replay journal |
| `<scenario>-replayed-...\bootsector-*.bin` | 512 B | boot sector |
| `prepared-<label>-...\fresh.ntfsinfo.txt` | 1 KB | geometry right after format |
| `<scenario>-verdict-...\*.chkdsk-f.txt`, `listing-diff.txt` | KBs | E3 verdict |

Pair each Mac image with its session folder by scenario, label and build in the
names; the `winver` string is inside `meta.json` (`windows.ProductName`,
`DisplayVersion`, `Build`).

## Notes

- The scripts were written on a Mac without PowerShell available and were
  syntax-checked by reading (see `docs/progress/fskit.md`); the first run on the
  PC is the real test. They are Windows PowerShell 5.1 compatible (no
  ternaries, no `??`).
- `Assert-BetterPerformance` reads Partmgr's `UserRemovalPolicy`/`RemovalPolicy`
  from `HKLM\SYSTEM\CurrentControlSet\Enum\<device>\Device Parameters\Partmgr`
  (`DEVICE_REMOVAL_POLICY`: 2 = orderly = Better performance, 3 = surprise =
  Quick removal). Some stacks do not write it; the Device Manager dialog is the
  truth and the script asks you to confirm.
- Eject uses the Explorer verb (`Shell.Application` namespace 17 → `Eject`); if
  that fails the script asks you to eject by hand and press Enter.
- `Scenario-B` writes `meta.json` and stops the transcript **before** the
  shutdown, so nothing is lost when the PC goes down.
