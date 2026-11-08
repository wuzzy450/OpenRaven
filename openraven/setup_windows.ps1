# OpenRaven - native Windows setup + launcher (called by start_windows.bat).
# 1. Finds the player's game archives (.zip / .7z / .rar, any layout) and puts the files where the
#    loader wants them:  <root>\pm\...  (game data)  and  <root>\openraven\3.11-dec\pm\g7\halo\{game,halo.so}
#    Files are recognised by SHA-256, so it does not matter which archive or folder they come from.
# 2. Starts openraven\win\ravenloader.exe.
# Paths use -LiteralPath everywhere: the game folder names contain [ ] brackets.
param([string[]]$Archives)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Dec  = Join-Path $Root 'openraven\3.11-dec\pm\g7\halo'
$Win  = Join-Path $Root 'openraven\win'
$Tmp  = Join-Path $Root 'openraven\_setup_tmp'

# Known files (SHA-256). DEC_GAME = decrypted 3.11 game, DEC_HALOSO = its halo.so.
$DEC_GAME   = 'e88ba3543d5613c828d14aba26329a3b35b362d5dd7f817db7178c86a3b2fe98'
$DEC_HALOSO = 'f000a002e74ba378af9363029259cb5d848fa1e0573ca854c83e4e12e13e551f'

function Say($m, $c = 'Cyan') { Write-Host "[OpenRaven] $m" -ForegroundColor $c }
function Hash($p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower() }
function Good($p, $h) { (Test-Path -LiteralPath $p) -and ((Hash $p) -eq $h) }

$SevenZip = @("$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe") | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
function List-Archive($a) {
    if ($SevenZip) { & $SevenZip l -slt -ba $a | Where-Object { $_ -like 'Path = *' } | ForEach-Object { $_.Substring(7) } }
    else { & tar.exe -tf $a }
}
function Extract($a, $dest, [string[]]$members) {
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    if ($SevenZip) { & $SevenZip x -y -bso0 -bsp1 "-o$dest" $a @members | Out-Host }
    else { & tar.exe -xf $a -C $dest @members }
    if ($LASTEXITCODE -ne 0) { throw "could not extract $a (exit $LASTEXITCODE). Installing 7-Zip (7-zip.org) usually fixes this." }
}

function Have-Data { Test-Path -LiteralPath (Join-Path $Root 'pm\g7\halo\data') }
function Have-Dec  { (Good (Join-Path $Dec 'game') $DEC_GAME) -and (Good (Join-Path $Dec 'halo.so') $DEC_HALOSO) }

# Look through one archive and take what it has.
function Use-Archive($a) {
    Say "Checking $a"
    $entries = @(List-Archive $a | ForEach-Object { $_ -replace '\\', '/' })
    # game data: the folder that contains pm/g7/halo/data
    $data = $entries | Where-Object { $_ -match '(^|/)pm/g7/halo/data/' } | Select-Object -First 1
    if ($data -and -not (Have-Data)) {
        $prefix = $data.Substring(0, $data.IndexOf('pm/g7/halo/data/'))
        Say "Extracting game data (about 10 GB, takes a few minutes)..." Yellow
        if (Test-Path -LiteralPath $Tmp) { Remove-Item -LiteralPath $Tmp -Recurse -Force }
        Extract $a $Tmp @()
        $src = Join-Path $Tmp ($prefix + 'pm')
        if (Test-Path -LiteralPath (Join-Path $Root 'pm')) {
            # keep anything already there, add the rest
            Get-ChildItem -LiteralPath $src | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $Root 'pm') -Recurse -Force }
        } else { Move-Item -LiteralPath $src -Destination (Join-Path $Root 'pm') }
    }
    # decrypted game / halo.so: any entry named .../game or .../halo.so, checked by hash
    $cands = $entries | Where-Object { $_ -match '(^|/)(game|halo\.so)$' }
    if ($cands -and -not (Have-Dec)) {
        $t = Join-Path $Tmp 'cand'
        if (Test-Path -LiteralPath $t) { Remove-Item -LiteralPath $t -Recurse -Force }
        # files may already be on disk from the data step
        $onDisk = $cands | ForEach-Object { Join-Path $Tmp $_ } | Where-Object { Test-Path -LiteralPath $_ }
        if (-not $onDisk) { Extract $a $t @($cands); $base = $t } else { $base = $Tmp }
        New-Item -ItemType Directory -Force -Path $Dec | Out-Null
        foreach ($c in $cands) {
            $p = Join-Path $base $c
            if (-not (Test-Path -LiteralPath $p)) { continue }
            $h = Hash $p
            if ($h -eq $DEC_GAME)   { Copy-Item -LiteralPath $p -Destination (Join-Path $Dec 'game') -Force; Say "Found the game program" Green }
            if ($h -eq $DEC_HALOSO) { Copy-Item -LiteralPath $p -Destination (Join-Path $Dec 'halo.so') -Force; Say "Found halo.so" Green }
        }
    }
    if (Test-Path -LiteralPath $Tmp) { Remove-Item -LiteralPath $Tmp -Recurse -Force }
}

function Find-Archives {
    $dirs = @($Root, (Split-Path -Parent $Root), (Join-Path $env:USERPROFILE 'Downloads'), (Join-Path $env:USERPROFILE 'Desktop'))
    foreach ($d in $dirs) {
        if (-not (Test-Path -LiteralPath $d)) { continue }
        Get-ChildItem -LiteralPath $d -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -in '.zip', '.7z', '.rar' -and $_.Name -match 'raven|halo|3\.11' } |
            ForEach-Object { $_.FullName }
    }
}
function Pick-Archives {
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = 'Select your Halo: Fireteam Raven game archive (.zip / .7z / .rar)'
    $dlg.Filter = 'Archives (*.zip;*.7z;*.rar)|*.zip;*.7z;*.rar|All files (*.*)|*.*'
    $dlg.Multiselect = $true
    $dlg.InitialDirectory = Join-Path $env:USERPROFILE 'Downloads'
    if ($dlg.ShowDialog() -eq 'OK') { return $dlg.FileNames }
    return @()
}

try {
    Write-Host '================================================================'
    Write-Host '  OpenRaven - Halo: Fireteam Raven (v3.11), native Windows'
    Write-Host '================================================================'
    if (-not ((Have-Data) -and (Have-Dec))) {
        $list = @($Archives) + @(Find-Archives) | Where-Object { $_ } | Select-Object -Unique
        foreach ($a in $list) { if (-not ((Have-Data) -and (Have-Dec))) { Use-Archive $a } }
        while (-not ((Have-Data) -and (Have-Dec))) {
            $miss = @(); if (-not (Have-Data)) { $miss += 'your game archive' }
            if (-not (Have-Dec)) { $miss += 'an archive with the v3.11 game program files (game + halo.so; your game archive did not have them)' }
            Say ("Still needed: " + ($miss -join ' and ')) Yellow
            $picked = Pick-Archives
            if (-not $picked) { throw ("Setup not finished. Missing: " + ($miss -join ' and ')) }
            foreach ($a in $picked) { Use-Archive $a }
        }
        Say 'Game files ready.' Green
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $Root 'pm\g7\dump'), (Join-Path $Root 'pm\g7\halo\pmuser\messages') | Out-Null

    Say 'Starting the game. F1 = settings overlay. Log: ravenloader.log in the game folder.' Green
    $ErrorActionPreference = 'Continue'   # the game writes to the console; that is not an error
    & (Join-Path $Win 'ravenloader.exe') $Root
    exit 0
} catch {
    Say "Error: $($_.Exception.Message)" Red
    exit 1
}
