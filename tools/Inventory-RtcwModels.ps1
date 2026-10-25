<#
.SYNOPSIS
  Read-only inventory of the character model files inside your RTCW .pk3 files.

.DESCRIPTION
  Opens every *.pk3 in the folder (a pk3 is a zip) READ-ONLY and lists only the ENTRY NAMES under
  models/players/<model>/ that the RTCWQuest mercenary needs. It does not extract, copy, modify or
  upload anything. The report is a small text file containing file names only; check it before sharing.

  For each model it reports whether the files the engine opens are present:
    wolfanim.cfg, wolfanim.script             (server: missing => "Failed to load animation scripts")
    body_default.skin + body.mds              (skeletal character)  or  lower/upper_default.skin + lower/upper.md3
    head_default.skin
  and which pk3 contains each of them.

.PARAMETER Path
  Folder with the pk3 files, e.g. the copy of /sdcard/RTCWQuest/Main (default: current folder).

.PARAMETER Models
  Models to report in detail (default: american, partisan, civilian, bj2, infantryss).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\Inventory-RtcwModels.ps1 -Path D:\RTCW\Main
#>
param(
    [string]$Path = ".",
    [string[]]$Models = @("american", "partisan", "civilian", "bj2", "infantryss"),
    [string]$Out = "rtcw_model_inventory.txt"
)

Add-Type -AssemblyName System.IO.Compression.FileSystem

$pk3s = Get-ChildItem -LiteralPath $Path -Filter *.pk3 -File | Sort-Object Name
if (-not $pk3s) { Write-Error "No .pk3 files found in '$Path'"; exit 1 }

# model -> file -> list of pk3 names
$found = @{}
$allModels = New-Object System.Collections.Generic.SortedSet[string]

foreach ($pk3 in $pk3s) {
    $zip = $null
    try {
        $zip = [System.IO.Compression.ZipFile]::OpenRead($pk3.FullName)   # read-only
        foreach ($e in $zip.Entries) {
            $name = $e.FullName.ToLowerInvariant()
            if ($name -match '^models/players/([^/]+)/(.+)$') {
                $m = $Matches[1]; $f = $Matches[2]
                [void]$allModels.Add($m)
                if ($Models -contains $m) {
                    if (-not $found.ContainsKey($m)) { $found[$m] = @{} }
                    if (-not $found[$m].ContainsKey($f)) { $found[$m][$f] = New-Object System.Collections.Generic.List[string] }
                    $found[$m][$f].Add($pk3.Name)
                }
            }
        }
    } catch {
        Write-Warning "Could not read $($pk3.Name): $($_.Exception.Message)"
    } finally {
        if ($zip) { $zip.Dispose() }
    }
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("RTCW model inventory (file names only)  $(Get-Date -Format s)")
$lines.Add("pk3 files scanned: " + (($pk3s | ForEach-Object { $_.Name }) -join ", "))
$lines.Add("")
$lines.Add("All model folders found under models/players/: " + ($allModels -join ", "))
$lines.Add("")

$needed = @(
    @{ k = "wolfanim.cfg";        why = "server animation config (required)" },
    @{ k = "wolfanim.script";     why = "server animation script (required)" },
    @{ k = "body_default.skin";   why = "skeletal body skin" },
    @{ k = "body.mds";            why = "skeletal body mesh" },
    @{ k = "lower_default.skin";  why = "legacy legs skin" },
    @{ k = "upper_default.skin";  why = "legacy torso skin" },
    @{ k = "lower.md3";           why = "legacy legs mesh" },
    @{ k = "upper.md3";           why = "legacy torso mesh" },
    @{ k = "head_default.skin";   why = "head skin" }
)

foreach ($m in $Models) {
    $lines.Add("== model '$m' ==")
    if (-not $allModels.Contains($m)) {
        $lines.Add("   NOT PRESENT: no entries under models/players/$m/")
        $lines.Add("")
        continue
    }
    foreach ($n in $needed) {
        if ($found[$m].ContainsKey($n.k)) {
            $lines.Add(("   [x] {0,-20} {1}  <- {2}" -f $n.k, $n.why, (($found[$m][$n.k] | Select-Object -Unique) -join ", ")))
        } else {
            $lines.Add(("   [ ] {0,-20} {1}" -f $n.k, $n.why))
        }
    }
    $has = { param($k) $found[$m].ContainsKey($k) }
    $skeletal = (& $has "body_default.skin") -and (& $has "body.mds")
    $legacy = (& $has "lower_default.skin") -and (& $has "upper_default.skin") -and (& $has "lower.md3") -and (& $has "upper.md3")
    $anim = (& $has "wolfanim.cfg") -and (& $has "wolfanim.script")
    $verdict = if ($anim -and ($skeletal -or $legacy) -and (& $has "head_default.skin")) { "COMPLETE (usable as a mercenary)" } else { "INCOMPLETE (the mod will not spawn it)" }
    $lines.Add("   verdict: $verdict")
    $lines.Add("   other files in the folder: " + ((($found[$m].Keys | Where-Object { $needed.k -notcontains $_ } | Sort-Object | Select-Object -First 25) -join ", ")))
    $lines.Add("")
}

$lines | Set-Content -LiteralPath $Out -Encoding UTF8
$lines | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "Report written to $((Resolve-Path $Out).Path) (file names only; nothing was extracted)."
