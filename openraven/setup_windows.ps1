# OpenRaven - native Windows setup + launcher (called by start_windows.bat).
# 1. Finds the player's game archives (.zip / .7z / .rar, any layout, also archives inside archives)
#    and puts the files where the loader wants them:
#      <root>\pm\...  (game data)  and  <root>\openraven\3.11-dec\pm\g7\halo\{game,halo.so}
#    Program files are recognised by size + SHA-256, so names and folders do not matter.
# 2. Starts openraven\win\ravenloader.exe.
# 3. After the first good run, offers a desktop shortcut (asked once).
# Paths use -LiteralPath everywhere: the game folder names contain [ ] brackets.
param([string[]]$Archives)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Dec  = Join-Path $Root 'openraven\3.11-dec\pm\g7\halo'
$Win  = Join-Path $Root 'openraven\win'
$Tmp  = Join-Path $Root 'openraven\_setup_tmp'

# Known files (SHA-256 + size). DEC_GAME = decrypted 3.11 game, DEC_HALOSO = its halo.so.
$DEC_GAME   = 'e88ba3543d5613c828d14aba26329a3b35b362d5dd7f817db7178c86a3b2fe98'
$DEC_HALOSO = 'f000a002e74ba378af9363029259cb5d848fa1e0573ca854c83e4e12e13e551f'
$SIZE_GAME = 33554432; $SIZE_HALOSO = 880528
$DATA_GB = 11   # game data, extracted (used when the archive does not list sizes)
$script:DataOnlyPack = $null   # an archive that had the game data but no program files

function Say($m, $c = 'Cyan') { Write-Host "[OpenRaven] $m" -ForegroundColor $c }
function Hash($p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower() }
function Good($p, $h) { (Test-Path -LiteralPath $p) -and ((Hash $p) -eq $h) }
function GB($b) { [math]::Round($b / 1GB, 1) }

$SevenZip = @("$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe") | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
# Entries as @{Path; Size} (Size = -1 when unknown: tar.exe listing).
function List-Archive($a) {
    if ($SevenZip) {
        $cur = $null
        foreach ($l in (& $SevenZip l -slt -ba $a)) {
            if ($l -like 'Path = *') { if ($cur) { $cur }; $cur = [pscustomobject]@{ Path = ($l.Substring(7) -replace '\\', '/'); Size = -1; Dir = $false } }
            elseif ($cur -and $l -like 'Size = *') { [long]$s = 0; if ([long]::TryParse($l.Substring(7), [ref]$s)) { $cur.Size = $s } }
            elseif ($cur -and $l -like 'Folder = +') { $cur.Dir = $true }
        }
        if ($cur) { $cur }
    } else {
        & tar.exe -tf $a | ForEach-Object { [pscustomobject]@{ Path = ($_ -replace '\\', '/'); Size = -1; Dir = $_.EndsWith('/') } }
    }
}
function Extract($a, $dest, [string[]]$members) {
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    if ($SevenZip) { & $SevenZip x -y -bso0 -bsp1 "-o$dest" $a @members | Out-Host }
    else { & tar.exe -xf $a -C $dest @members }
    if ($LASTEXITCODE -ne 0) { throw "could not extract $a (exit $LASTEXITCODE). Installing 7-Zip (7-zip.org) usually fixes this." }
}
function Need-Space($bytes, $what) {
    $drive = Get-PSDrive -Name ((Get-Item -LiteralPath $Root).PSDrive.Name)
    $free = $drive.Free
    $need = $bytes + 512MB   # a little room for logs and settings
    if ($free -lt $need) {
        throw ("Not enough free disk space on drive $($drive.Name): to unpack $what. " +
               "Needed: about $(GB $need) GB, free: $(GB $free) GB. Free up $(GB ($need - $free)) GB (or move the OpenRaven folder to a bigger drive) and run start_windows.bat again.")
    }
}

function Have-Data { Test-Path -LiteralPath (Join-Path $Root 'pm\g7\halo\data') }
function Have-Dec  { (Good (Join-Path $Dec 'game') $DEC_GAME) -and (Good (Join-Path $Dec 'halo.so') $DEC_HALOSO) }
function Ready     { (Have-Data) -and (Have-Dec) }

# Copy any of the given files that is the game program or halo.so (by hash).
function Take-Program([string[]]$paths) {
    New-Item -ItemType Directory -Force -Path $Dec | Out-Null
    foreach ($p in $paths) {
        if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { continue }
        $len = (Get-Item -LiteralPath $p).Length
        if ($len -ne $SIZE_GAME -and $len -ne $SIZE_HALOSO) { continue }
        $h = Hash $p
        if ($h -eq $DEC_GAME)   { Copy-Item -LiteralPath $p -Destination (Join-Path $Dec 'game') -Force; Say "Found the game program ($(Split-Path -Leaf $p))" Green }
        if ($h -eq $DEC_HALOSO) { Copy-Item -LiteralPath $p -Destination (Join-Path $Dec 'halo.so') -Force; Say "Found halo.so ($(Split-Path -Leaf $p))" Green }
    }
}

# Look through one archive and take what it has. $depth > 0 = an archive found inside another one.
function Use-Archive($a, $depth = 0) {
    Say "Checking $(Split-Path -Leaf $a)"
    $tmpA = Join-Path $Tmp "a$depth"
    $entries = @(List-Archive $a | Where-Object { -not $_.Dir })
    $sizes = ($entries | Where-Object { $_.Size -ge 0 } | Measure-Object -Property Size -Sum).Sum
    $sizesKnown = $entries.Count -gt 0 -and -not ($entries | Where-Object { $_.Size -lt 0 })
    $extracted = $false

    # game data: the folder that contains pm/g7/halo/data
    $data = $entries | Where-Object { $_.Path -match '(^|/)pm/g7/halo/data/' } | Select-Object -First 1
    if ($data -and -not (Have-Data)) {
        $prefix = $data.Path.Substring(0, $data.Path.IndexOf('pm/g7/halo/data/'))
        Need-Space ($(if ($sizesKnown) { $sizes } else { $DATA_GB * 1GB })) 'the game data'
        Say "Extracting game data (about $(if ($sizesKnown) { GB $sizes } else { $DATA_GB }) GB, takes a few minutes)..." Yellow
        if (Test-Path -LiteralPath $tmpA) { Remove-Item -LiteralPath $tmpA -Recurse -Force }
        Extract $a $tmpA @()
        $extracted = $true
        $src = Join-Path $tmpA ($prefix + 'pm')
        if (Test-Path -LiteralPath (Join-Path $Root 'pm')) {
            # keep anything already there, add the rest
            Get-ChildItem -LiteralPath $src | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $Root 'pm') -Recurse -Force }
        } else { Move-Item -LiteralPath $src -Destination (Join-Path $Root 'pm') }
    }

    # decrypted game / halo.so: by name, or (renamed) by exact size; then checked by hash
    if (-not (Have-Dec)) {
        $cands = @($entries | Where-Object { $_.Path -match '(^|/)(game|halo\.so)$' -or $_.Size -eq $SIZE_GAME -or $_.Size -eq $SIZE_HALOSO } |
                   Where-Object { $_.Path -notmatch '(^|/)pm/g7/halo/data/' } | ForEach-Object { $_.Path })
        if ($cands) {
            if (-not $extracted) {
                if (Test-Path -LiteralPath $tmpA) { Remove-Item -LiteralPath $tmpA -Recurse -Force }
                Extract $a $tmpA @($cands)
            }
            Take-Program @($cands | ForEach-Object { Join-Path $tmpA $_ })
        }
        if ($data -and -not (Have-Dec)) { $script:DataOnlyPack = Split-Path -Leaf $a }
    }

    # archives inside this archive (for example a .zip holding the .7z)
    if (-not (Ready) -and $depth -lt 2) {
        $inner = @($entries | Where-Object { $_.Path -match '\.(zip|7z|rar)$' })
        foreach ($i in $inner) {
            if (Ready) { break }
            $p = Join-Path $tmpA $i.Path
            if (-not (Test-Path -LiteralPath $p)) {
                if ($i.Size -gt 0) { Need-Space $i.Size "$($i.Path) (an archive inside $(Split-Path -Leaf $a))" }
                Extract $a $tmpA @($i.Path)
            }
            Use-Archive $p ($depth + 1)
        }
    }
    if (Test-Path -LiteralPath $tmpA) { Remove-Item -LiteralPath $tmpA -Recurse -Force }
    if ($depth -eq 0 -and (Test-Path -LiteralPath $Tmp)) { Remove-Item -LiteralPath $Tmp -Recurse -Force }
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

function Offer-Shortcut {
    $flag = Join-Path $Root 'openraven\.shortcut_asked'
    if (Test-Path -LiteralPath $flag) { return }
    try {
        Set-Content -LiteralPath $flag -Value 'asked' -Encoding ascii
        $desk = [Environment]::GetFolderPath('Desktop')
        $lnk = Join-Path $desk 'Halo Fireteam Raven (OpenRaven).lnk'
        if (Test-Path -LiteralPath $lnk) { return }
        $ans = Read-Host '[OpenRaven] Put a "Halo Fireteam Raven" shortcut on your desktop? (Y/n)'
        if ($ans -and $ans.Trim().ToLower().StartsWith('n')) { return }
        $sh = New-Object -ComObject WScript.Shell
        $s = $sh.CreateShortcut($lnk)
        $s.TargetPath = Join-Path $Root 'start_windows.bat'
        $s.WorkingDirectory = $Root
        $s.Description = 'Halo: Fireteam Raven (OpenRaven)'
        $s.Save()
        Say 'Shortcut added to the desktop.' Green
    } catch { Say "Could not add the shortcut: $($_.Exception.Message)" Yellow }
}

try {
    Write-Host '================================================================'
    Write-Host '  OpenRaven - Halo: Fireteam Raven (v3.11), native Windows'
    Write-Host '================================================================'
    if (-not (Ready)) {
        $list = @($Archives) + @(Find-Archives) | Where-Object { $_ } | Select-Object -Unique
        foreach ($a in $list) { if (-not (Ready)) { Use-Archive $a } }
        while (-not (Ready)) {
            if (-not (Have-Data)) { Say 'Still needed: your game archive (the one with the pm\g7\halo\data folder).' Yellow }
            if (-not (Have-Dec)) {
                if ($script:DataOnlyPack) {
                    Say "Got the game data from '$($script:DataOnlyPack)'. Still needed: the decrypted v3.11 game program (game + halo.so)." Yellow
                    Say 'Game archives never contain it; it comes as a separate small pack (usually 3.11-dec.zip, about 11 MB). Pick that file.' Yellow
                } else {
                    Say 'Still needed: the decrypted v3.11 game program (game + halo.so, usually 3.11-dec.zip, about 11 MB). None of the archives checked had it.' Yellow
                }
            }
            $picked = Pick-Archives
            if (-not $picked) {
                $miss = @(); if (-not (Have-Data)) { $miss += 'game data' }; if (-not (Have-Dec)) { $miss += 'game + halo.so' }
                throw ("Setup not finished. Missing: " + ($miss -join ' and '))
            }
            foreach ($a in $picked) { Use-Archive $a }
        }
        Say 'Game files ready.' Green
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $Root 'pm\g7\dump'), (Join-Path $Root 'pm\g7\halo\pmuser\messages') | Out-Null

    Say 'Starting the game. F1 = settings overlay. Log: ravenloader.log in the game folder.' Green
    $ErrorActionPreference = 'Continue'   # the game writes to the console; that is not an error
    Set-Location -LiteralPath $Root       # ravenloader.log goes to the game folder
    & (Join-Path $Win 'ravenloader.exe') $Root
    if ($LASTEXITCODE -eq 0) { Offer-Shortcut }
    exit 0
} catch {
    Say "Error: $($_.Exception.Message)" Red
    exit 1
}
