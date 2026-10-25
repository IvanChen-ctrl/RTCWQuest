<#
.SYNOPSIS
  Read-only inventory of character models inside your RTCW .pk3 files and mod folders.

.DESCRIPTION
  Mirrors the checks the RTCWQuest mercenary makes at runtime (g_merc.c / g_vrlogic.h):
    server : models/players/<m>/wolfanim.cfg and wolfanim.script (or models/players/default.script when the cfg
             says "version" <= 1)  -- missing => "Failed to load animation scripts for model <m>"
    AI     : the animation script defines STATE RELAXED, ALERT and COMBAT
    client : body_<skin>.skin + body.mds (or lower_/upper_<skin>.skin + lower.md3/upper.md3) and head_<skin>.skin
  The pk3 files are opened READ-ONLY as zip archives. Only entry NAMES and the two small text files
  (wolfanim.cfg, wolfanim.script) are read, in memory. NOTHING is extracted, copied or uploaded. The report is
  a text file with file names only: check it before sharing.

  Search order follows the engine (files.c FS_AddGameDirectory): inside one folder pk3 files are sorted
  alphabetically with "sp_" treated as "zz" (so sp_pak* override pak0); later wins. Folders given later in
  -Path (mods / mission packs) override earlier ones. Loose files in a folder override its pk3 files.

.PARAMETER Path
  One or more folders: first the base game folder (e.g. a copy of /sdcard/RTCWQuest/Main), then mod folders.

.PARAMETER Models
  Models always listed in detail (default: american, partisan, infantryss, eliteguard, bj2, civilian).

.PARAMETER Out
  Report file (default rtcw_model_inventory.txt).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\tools\Inventory-RtcwModels.ps1 -Path D:\RTCW\Main, D:\RTCW\Trondheim
#>
param(
    [Parameter(Mandatory = $true)][string[]]$Path,
    [string[]]$Models = @("american", "partisan", "infantryss", "eliteguard", "bj2", "civilian"),
    [string]$Out = "rtcw_model_inventory.txt"
)

Add-Type -AssemblyName System.IO.Compression.FileSystem

$Native = @("american", "partisan", "infantryss", "eliteguard")   # preference order used by the mod
$files = @{}      # lower-case entry name -> @{ Source=..; Zip=..; Entry=..; Loose=.. }
$zips = New-Object System.Collections.Generic.List[object]

function Get-PakSortKey([string]$name) {
    $n = $name.ToLowerInvariant()
    if ($n.StartsWith("sp_")) { $n = "zz" + $n.Substring(2) }
    return $n
}

foreach ($folder in $Path) {
    if (-not (Test-Path -LiteralPath $folder -PathType Container)) { Write-Error "Not a folder: $folder"; exit 1 }
    $label = Split-Path -Leaf (Resolve-Path -LiteralPath $folder)
    $paks = Get-ChildItem -LiteralPath $folder -Filter *.pk3 -File | Sort-Object { Get-PakSortKey $_.Name }
    foreach ($pk in $paks) {
        try {
            $zip = [System.IO.Compression.ZipFile]::OpenRead($pk.FullName)    # read-only
            $zips.Add($zip)
            foreach ($e in $zip.Entries) {
                if ($e.FullName.EndsWith("/")) { continue }
                $files[$e.FullName.ToLowerInvariant()] = @{ Source = "$label/$($pk.Name)"; Zip = $zip; Entry = $e; Loose = $null }
            }
        } catch {
            Write-Warning "Could not read $($pk.Name): $($_.Exception.Message)"
        }
    }
    $root = (Resolve-Path -LiteralPath $folder).Path.TrimEnd('\')
    $modelsDir = Join-Path $root "models"
    if (Test-Path -LiteralPath $modelsDir) {
        Get-ChildItem -LiteralPath $modelsDir -Recurse -File | ForEach-Object {
            $rel = $_.FullName.Substring($root.Length + 1).Replace('\', '/').ToLowerInvariant()
            $files[$rel] = @{ Source = "$label/(loose)"; Zip = $null; Entry = $null; Loose = $_.FullName }
        }
    }
}

function Test-Has([string]$name) { return $files.ContainsKey($name.ToLowerInvariant()) }

function Read-Text([string]$name) {
    $f = $files[$name.ToLowerInvariant()]
    if ($f.Loose) { return [System.IO.File]::ReadAllText($f.Loose) }
    $sr = New-Object System.IO.StreamReader($f.Entry.Open())
    try { return $sr.ReadToEnd() } finally { $sr.Dispose() }
}

function Get-Tokens([string]$text) {
    $t = [regex]::Replace($text, '/\*.*?\*/', ' ', 'Singleline')
    $t = [regex]::Replace($t, '//[^\r\n]*', ' ')
    $t = $t.Replace('{', ' { ').Replace('}', ' } ')
    return @($t -split '\s+' | Where-Object { $_ -ne '' })
}

function Get-CfgVersion([string]$text) {
    $tok = Get-Tokens $text
    for ($i = 0; $i -lt $tok.Count; $i++) {
        if ($tok[$i] -ieq 'version') {
            if ($i + 1 -lt $tok.Count -and $tok[$i + 1] -match '^-?\d+') { return [int]$Matches[0] }
            return 0
        }
    }
    return 0
}

function Get-ScriptStates([string]$text) {
    $tok = Get-Tokens $text
    $states = New-Object System.Collections.Generic.HashSet[string]
    for ($i = 0; $i -lt $tok.Count - 1; $i++) {
        if ($tok[$i] -ieq 'state') {
            $n = $tok[$i + 1].ToLowerInvariant()
            if ($n -in @('relaxed', 'query', 'alert', 'combat')) { [void]$states.Add($n) }
        }
    }
    return $states
}

function Get-Skins([string]$model, [string]$prefix) {
    $re = '^models/players/' + [regex]::Escape($model) + '/' + [regex]::Escape($prefix) + '(.+)\.skin$'
    $res = @()
    foreach ($k in $files.Keys) { if ($k -match $re) { $res += $Matches[1] } }
    return @($res | Sort-Object)
}

function Select-Skin($names) {
    if (-not $names -or $names.Count -eq 0) { return $null }
    if ($names -contains 'default') { return 'default' }
    return $names[0]
}

function Test-Model([string]$model) {
    $r = @{ Model = $model; Missing = New-Object System.Collections.Generic.List[string] }
    $base = "models/players/$model/"
    $hasCfg = Test-Has ($base + "wolfanim.cfg")
    $version = 0
    if ($hasCfg) { $version = Get-CfgVersion (Read-Text ($base + "wolfanim.cfg")) }
    $script = $null
    if (Test-Has ($base + "wolfanim.script")) { $script = $base + "wolfanim.script" }
    elseif ($hasCfg -and $version -le 1 -and (Test-Has "models/players/default.script")) { $script = "models/players/default.script" }
    $states = $null
    if ($script) { $states = Get-ScriptStates (Read-Text $script) }

    if (-not $hasCfg) { $r.Missing.Add($base + "wolfanim.cfg") }
    if (-not $script) { $r.Missing.Add($base + "wolfanim.script") }
    elseif (-not ($states.Contains('relaxed') -and $states.Contains('alert') -and $states.Contains('combat'))) {
        $r.Missing.Add("animation script $script lacks the AI states RELAXED/ALERT/COMBAT")
    }

    $body = Get-Skins $model "body_"
    $lower = Get-Skins $model "lower_"
    $upper = Get-Skins $model "upper_"
    $head = Get-Skins $model "head_"
    $common = @($lower | Where-Object { $upper -contains $_ })
    $skeletal = ($body.Count -gt 0) -and (Test-Has ($base + "body.mds"))
    $legacy = ($common.Count -gt 0) -and (Test-Has ($base + "lower.md3")) -and (Test-Has ($base + "upper.md3"))
    if (-not $skeletal -and -not $legacy) {
        if ($body.Count -gt 0) { $r.Missing.Add($base + "body.mds") }
        elseif ($common.Count -eq 0) { $r.Missing.Add($base + "body_<skin>.skin (or lower_/upper_<skin>.skin)") }
        else { $r.Missing.Add("legacy body parts (lower.md3 / upper.md3)") }
    }
    if ($head.Count -eq 0) { $r.Missing.Add($base + "head_<skin>.skin") }

    $r.BodySkin = if ($body.Count -gt 0) { Select-Skin $body } else { Select-Skin $common }
    $r.HeadSkin = Select-Skin $head
    $r.Version = $version
    $r.Script = $script
    $r.Ok = ($r.Missing.Count -eq 0)
    $srcs = @()
    foreach ($f in @("wolfanim.cfg", "wolfanim.script")) { if (Test-Has ($base + $f)) { $srcs += $files[($base + $f)].Source } }
    $r.Sources = ($srcs | Select-Object -Unique) -join ", "
    return $r
}

# all model folders
$allModels = New-Object System.Collections.Generic.SortedSet[string]
foreach ($k in $files.Keys) { if ($k -match '^models/players/([^/]+)/') { [void]$allModels.Add($Matches[1]) } }

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("RTCW model inventory (file names only)  $(Get-Date -Format s)")
$lines.Add("Folders (low -> high priority): " + ($Path -join ", "))
$lines.Add("Model folders found: " + ($allModels -join ", "))
$lines.Add("")

$viable = New-Object System.Collections.Generic.List[string]
foreach ($m in $allModels) {
    $r = Test-Model $m
    if ($r.Ok) { $viable.Add($m) }
    if ($r.Ok -or ($Models -contains $m) -or ($Native -contains $m)) {
        $tag = if ($r.Ok) { "COMPLETE" } else { "INCOMPLETE" }
        $nat = if ($Native -contains $m) { " (native pairing)" } else { "" }
        $lines.Add("== ${m}: $tag$nat")
        if ($r.Ok) {
            $lines.Add("   body skin '$($r.BodySkin)', head skin '$($r.HeadSkin)', cfg version $($r.Version), script $($r.Script), from $($r.Sources)")
        }
        foreach ($mi in $r.Missing) { $lines.Add("   missing: $mi") }
    }
}
foreach ($m in $Models) {
    if (-not $allModels.Contains($m.ToLowerInvariant())) {
        $lines.Add("== ${m}: NOT PRESENT (no models/players/$m/ entries in any pk3 or folder)")
    }
}
$lines.Add("")
$lines.Add("Complete models: " + $(if ($viable.Count) { $viable -join ", " } else { "none" }))
$lines.Add("The mod prefers (in order): " + ($Native -join ", ") + ". Any other complete model can be forced in game with:  merc type <model>")
$lines.Add("NOTE: complete means 'the files the engine needs exist'. Check that a model is a humanoid soldier before forcing it.")

$lines | Set-Content -LiteralPath $Out -Encoding UTF8
$lines | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "Report written to $((Resolve-Path $Out).Path) (file names only; nothing was extracted)."
foreach ($z in $zips) { $z.Dispose() }
