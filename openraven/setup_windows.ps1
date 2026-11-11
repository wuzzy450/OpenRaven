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
$script:NetZip = @{}   # zips that tar.exe could not read; handled with .NET instead
$ReportFile = Join-Path $PSScriptRoot 'zip_report.txt'
function Log-Report($text) { try { Add-Content -LiteralPath $ReportFile -Value $text -Encoding utf8 } catch {} }

# Detailed report on a zip (reads its central directory directly), written to openraven\zip_report.txt.
# Players can send this file when setup fails.
function Zip-Report($a) {
    $L = New-Object System.Collections.Generic.List[string]
    $L.Add('=' * 70); $L.Add("$(Get-Date -Format s)  $a")
    $L.Add("PowerShell $($PSVersionTable.PSVersion)  OS $([Environment]::OSVersion.VersionString)  7-Zip: $(if ($SevenZip) { $SevenZip } else { 'not installed' })")
    $fs = $null
    try {
        $fs = [IO.File]::OpenRead($a); $br = New-Object IO.BinaryReader($fs); $len = $fs.Length
        $L.Add("File size: $len bytes ($(GB $len) GB)")
        $head = $br.ReadBytes(8); $L.Add("First bytes: $(($head | ForEach-Object { $_.ToString('X2') }) -join ' ')")
        if ($head[0] -eq 0x50 -and $head[1] -eq 0x4B) { $L.Add('Type: zip (PK)') }
        elseif ($head[0] -eq 0x37 -and $head[1] -eq 0x7A) { $L.Add('Type: actually a 7z file with a .zip name') }
        elseif ($head[0] -eq 0x52 -and $head[1] -eq 0x61) { $L.Add('Type: actually a RAR file with a .zip name') }
        elseif ($head[0] -eq 0x3C) { $L.Add('Type: looks like HTML/text (a web page saved instead of the download?)') }
        else { $L.Add('Type: unknown / not a zip') }
        # end of central directory record
        $tn = [int][math]::Min($len, 65557 + 20); $fs.Position = $len - $tn; $tail = $br.ReadBytes($tn)
        $e = -1; for ($i = $tn - 22; $i -ge 0; $i--) { if ($tail[$i] -eq 0x50 -and $tail[$i+1] -eq 0x4B -and $tail[$i+2] -eq 5 -and $tail[$i+3] -eq 6) { $e = $i; break } }
        if ($e -lt 0) { $L.Add('PROBLEM: no end-of-central-directory record. The file is cut off (incomplete download) or not a zip.'); return }
        $count = [uint64][BitConverter]::ToUInt16($tail, $e + 10); $cdSize = [uint64][BitConverter]::ToUInt32($tail, $e + 12); $cdOff = [uint64][BitConverter]::ToUInt32($tail, $e + 16)
        $L.Add("EOCD at $($len - $tn + $e): entries=$count cdSize=$cdSize cdOffset=$cdOff comment=$([BitConverter]::ToUInt16($tail, $e + 20)) bytes")
        $zip64 = $false
        if ($e -ge 20 -and $tail[$e-20] -eq 0x50 -and $tail[$e-19] -eq 0x4B -and $tail[$e-18] -eq 6 -and $tail[$e-17] -eq 7) {
            $zip64 = $true; $z64 = [BitConverter]::ToUInt64($tail, $e - 12)
            $fs.Position = [long]$z64; $zb = $br.ReadBytes(56)
            if ($zb.Length -eq 56 -and [BitConverter]::ToUInt32($zb, 0) -eq 0x06064b50) {
                $count = [BitConverter]::ToUInt64($zb, 32); $cdSize = [BitConverter]::ToUInt64($zb, 40); $cdOff = [BitConverter]::ToUInt64($zb, 48)
                $L.Add("ZIP64: yes (over 4 GB or 65535 files). entries=$count cdSize=$cdSize cdOffset=$cdOff")
            } else { $L.Add("PROBLEM: ZIP64 locator points to $z64 but there is no ZIP64 record there") }
        } else { $L.Add('ZIP64: no') }
        if ($cdOff + $cdSize -gt [uint64]$len) { $L.Add('PROBLEM: central directory ends past the end of the file (file truncated)'); return }
        $names = @{ 0='store'; 1='shrink'; 6='implode'; 8='deflate'; 9='DEFLATE64'; 12='BZIP2'; 14='LZMA'; 93='ZSTD'; 95='XZ'; 98='PPMD'; 99='AES-ENCRYPTED' }
        $stat = @{}; $enc = 0; $badLocal = 0; $total = [uint64]0; $n = 0
        $fs.Position = [long]$cdOff
        $L.Add('--- entries: method | flags | compressed | size | localOffset | path ---')
        while ($n -lt $count) {
            $h = $br.ReadBytes(46)
            if ($h.Length -lt 46 -or [BitConverter]::ToUInt32($h, 0) -ne 0x02014b50) { $L.Add("PROBLEM: central directory broken at entry $n (position $($fs.Position))"); break }
            $flags = [BitConverter]::ToUInt16($h, 8); $m = [int][BitConverter]::ToUInt16($h, 10)
            $cs = [uint64][BitConverter]::ToUInt32($h, 20); $us = [uint64][BitConverter]::ToUInt32($h, 24)
            $nl = [BitConverter]::ToUInt16($h, 28); $xl = [BitConverter]::ToUInt16($h, 30); $cl = [BitConverter]::ToUInt16($h, 32)
            $lo = [uint64][BitConverter]::ToUInt32($h, 42)
            $nb = $br.ReadBytes($nl); $x = $br.ReadBytes($xl); [void]$br.ReadBytes($cl)
            $name = if ($flags -band 0x800) { [Text.Encoding]::UTF8.GetString($nb) } else { [Text.Encoding]::GetEncoding(437).GetString($nb) }
            $p = 0
            while ($p + 4 -le $x.Length) {
                $id = [BitConverter]::ToUInt16($x, $p); $sz = [BitConverter]::ToUInt16($x, $p + 2); $q = $p + 4
                if ($id -eq 1) {
                    if ($us -eq 4294967295 -and $q + 8 -le $x.Length) { $us = [BitConverter]::ToUInt64($x, $q); $q += 8 }
                    if ($cs -eq 4294967295 -and $q + 8 -le $x.Length) { $cs = [BitConverter]::ToUInt64($x, $q); $q += 8 }
                    if ($lo -eq 4294967295 -and $q + 8 -le $x.Length) { $lo = [BitConverter]::ToUInt64($x, $q) }
                }
                $p += 4 + $sz
            }
            $mn = if ($names.ContainsKey($m)) { $names[$m] } else { "method$m" }
            $stat[$mn] = 1 + [int]$stat[$mn]; if ($flags -band 1) { $enc++ }; $total += $us
            $L.Add("$mn | 0x$($flags.ToString('X4')) | $cs | $us | $lo | $name")
            $keep = $fs.Position
            if ($lo + $cs + 4 -le [uint64]$len) { $fs.Position = [long]$lo; if ($br.ReadUInt32() -ne 0x04034b50) { $badLocal++; $L.Add("   ^ PROBLEM: no local header at offset $lo") } }
            else { $badLocal++; $L.Add("   ^ PROBLEM: data for this file runs past the end of the zip (truncated)") }
            $fs.Position = $keep; $n++
        }
        $L.Add('--- summary ---')
        $L.Add("Entries read: $n of $count   Total unpacked: $(GB $total) GB   Methods: $(($stat.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ', ')")
        $L.Add("Encrypted entries: $enc   Bad/missing local headers: $badLocal")
        if ($stat.Keys | Where-Object { $_ -notin 'store', 'deflate' }) { $L.Add('HINT: uses a compression method Windows tar/.NET may not support: install 7-Zip (7-zip.org) and run again.') }
        if ($zip64) { $L.Add('HINT: ZIP64 archive. Windows tar.exe often calls these "Damaged"; setup falls back to .NET for them.') }
    } catch { $L.Add("REPORT ERROR: $($_.Exception.Message)") }
    finally {
        if ($fs) { $fs.Dispose() }
        Log-Report $L
        Say "Zip report: $ReportFile (send this file if setup fails)" DarkGray
    }
}

function NetZip-Open($a) { Add-Type -AssemblyName System.IO.Compression.FileSystem; [IO.Compression.ZipFile]::OpenRead($a) }

# Runs tar.exe, returns @{Out; Code; Err} without letting its stderr stop the script.
function Run-Tar([string[]]$tarArgs) {
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $errF = Join-Path $env:TEMP 'openraven_tar_err.txt'
    $out = @(& tar.exe @tarArgs 2> $errF); $code = $LASTEXITCODE
    $ErrorActionPreference = $eap
    $err = if (Test-Path -LiteralPath $errF) { (Get-Content -LiteralPath $errF -Raw) } else { '' }
    @{ Out = $out; Code = $code; Err = $err }
}

# Entries as @{Path; Size} (Size = -1 when unknown: tar.exe listing).
function List-Archive($a) {
    if ($a -match '\.zip$') { Zip-Report $a }
    if ($SevenZip) {
        $cur = $null
        foreach ($l in (& $SevenZip l -slt -ba $a)) {
            if ($l -like 'Path = *') { if ($cur) { $cur }; $cur = [pscustomobject]@{ Path = ($l.Substring(7) -replace '\\', '/'); Size = -1; Dir = $false } }
            elseif ($cur -and $l -like 'Size = *') { [long]$s = 0; if ([long]::TryParse($l.Substring(7), [ref]$s)) { $cur.Size = $s } }
            elseif ($cur -and $l -like 'Folder = +') { $cur.Dir = $true }
        }
        if ($cur) { $cur }
        if ($LASTEXITCODE -ne 0) { Log-Report "7-Zip listing exit $LASTEXITCODE on $a"; Say "7-Zip had trouble reading $(Split-Path -Leaf $a) (exit $LASTEXITCODE)" Yellow }
        return
    }
    $r = Run-Tar @('-tf', $a)
    if ($r.Code -eq 0) {
        $r.Out | ForEach-Object { [pscustomobject]@{ Path = ($_ -replace '\\', '/'); Size = -1; Dir = $_.EndsWith('/') } }
        return
    }
    Log-Report "tar.exe -tf failed (exit $($r.Code)) on $a after $($r.Out.Count) entries:`r`n$($r.Err)"
    if ($a -notmatch '\.zip$') { throw "Windows tar.exe could not read $(Split-Path -Leaf $a). Install 7-Zip (7-zip.org) and run again. Details: $ReportFile" }
    Say 'Windows tar.exe could not read this zip; trying the .NET zip reader instead...' Yellow
    try { $z = NetZip-Open $a } catch {
        Log-Report ".NET could not open it either: $($_.Exception.Message)"
        throw "This zip could not be read (tar and .NET both failed). Install 7-Zip (7-zip.org) and run again; if that fails too, the download is damaged. Please send $ReportFile"
    }
    $script:NetZip[$a] = $true
    Log-Report ".NET opened it fine ($($z.Entries.Count) entries); using .NET for this zip."
    try { foreach ($en in $z.Entries) { [pscustomobject]@{ Path = ($en.FullName -replace '\\', '/'); Size = [long]$en.Length; Dir = $en.FullName.EndsWith('/') -or $en.FullName.EndsWith('\') } } } finally { $z.Dispose() }
}
function Extract($a, $dest, [string[]]$members) {
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    if ($SevenZip) {
        & $SevenZip x -y -bso0 -bsp1 "-o$dest" $a @members | Out-Host
    } elseif ($script:NetZip[$a]) {
        $want = @{}; foreach ($m in $members) { $want[$m] = $true }
        $z = NetZip-Open $a; $done = 0
        try {
            foreach ($en in $z.Entries) {
                $p = $en.FullName -replace '\\', '/'
                if ($p.EndsWith('/') -or ($want.Count -and -not $want[$p])) { continue }
                $t = Join-Path $dest ($p -replace '/', '\')
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $t) | Out-Null
                try { [IO.Compression.ZipFileExtensions]::ExtractToFile($en, $t, $true) } catch {
                    Log-Report ".NET extract failed on '$p': $($_.Exception.Message)"
                    throw "could not extract '$p' ($($_.Exception.Message)). Install 7-Zip (7-zip.org) and run again. If it still fails, please send $ReportFile"
                }
                if ((++$done % 1000) -eq 0) { Say "  $done files..." }
            }
        } finally { $z.Dispose() }
        return
    } else {
        $r = Run-Tar (@('-xf', $a, '-C', $dest) + $members)
        if ($r.Code -ne 0) { Log-Report "tar.exe -xf failed (exit $($r.Code)) on ${a}:`r`n$($r.Err)" }
        $global:LASTEXITCODE = $r.Code
    }
    if ($LASTEXITCODE -ne 0) { throw "could not extract $a (exit $LASTEXITCODE). Installing 7-Zip (7-zip.org) usually fixes this. Details: $ReportFile" }
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

# Archive entries that could be game / halo.so: by name, or (renamed) by exact size.
function Program-Candidates($entries) {
    $entries | Where-Object { $_.Path -match '(^|/)(game|halo\.so)$' -or $_.Size -eq $SIZE_GAME -or $_.Size -eq $SIZE_HALOSO } |
        Where-Object { $_.Path -notmatch '(^|/)pm/g7/halo/data/' } | ForEach-Object { $_.Path }
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
        # take game / halo.so first: they sit in pm/g7/halo, which is moved away below
        if (-not (Have-Dec)) { Take-Program @(Program-Candidates $entries | ForEach-Object { Join-Path $tmpA $_ }) }
        $src = Join-Path $tmpA ($prefix + 'pm')
        if (Test-Path -LiteralPath (Join-Path $Root 'pm')) {
            # keep anything already there, add the rest
            Get-ChildItem -LiteralPath $src | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $Root 'pm') -Recurse -Force }
        } else { Move-Item -LiteralPath $src -Destination (Join-Path $Root 'pm') }
    }

    # decrypted game / halo.so: by name, or (renamed) by exact size; then checked by hash
    if (-not (Have-Dec) -and -not $extracted) {
        $cands = @(Program-Candidates $entries)
        if ($cands) {
            if (-not $extracted) {
                if (Test-Path -LiteralPath $tmpA) { Remove-Item -LiteralPath $tmpA -Recurse -Force }
                Extract $a $tmpA @($cands)
            }
            Take-Program @($cands | ForEach-Object { Join-Path $tmpA $_ })
        }
    }
    if ($data -and -not (Have-Dec)) { $script:DataOnlyPack = Split-Path -Leaf $a }

    # archives inside this archive (for example a .zip holding the .7z)
    if (-not (Ready) -and $depth -lt 2) {
        $inner = @($entries | Where-Object { $_.Path -match '\.(zip|7z|rar)$' -and $_.Path -notmatch '(^|/)pm/g7/' })   # the game's own data has .zip files that are not archives to unpack
        foreach ($i in $inner) {
            if (Ready) { break }
            $p = Join-Path $tmpA $i.Path
            if (-not (Test-Path -LiteralPath $p)) {
                if ($i.Size -gt 0) { Need-Space $i.Size "$($i.Path) (an archive inside $(Split-Path -Leaf $a))" }
                Extract $a $tmpA @($i.Path)
            }
            try { Use-Archive $p ($depth + 1) } catch { Log-Report "Skipped inner archive $($i.Path): $($_.Exception.Message)"; Say "Skipped $($i.Path) (could not read it)" DarkGray }
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
                    Say "Got the game data from '$($script:DataOnlyPack)'. Still needed: the v3.11 game program (game + halo.so); that archive doesn't have it (the v3.11 .7z pack never has halo.so)." Yellow
                    Say 'Pick an archive that has them: a v3.11 copy that includes both files (see README).' Yellow
                } else {
                    Say 'Still needed: the v3.11 game program (game + halo.so). None of the archives checked had it: pick a v3.11 copy that includes both files (see README).' Yellow
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
    if (Test-Path -LiteralPath $ReportFile) { Say "Please send this file to the OpenRaven dev: $ReportFile" Yellow }
    exit 1
}
