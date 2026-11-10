# FFmpeg decodes the game's XMA audio. The build needs a Windows x64 LGPL shared build of it (include, lib and bin folders).
# When there's none where the build looks (FFMPEG_ROOT, thirdparty\ffmpeg, an ffmpeg* folder next to the project), this gets
# BtbN's FFmpeg 8.1 LGPL shared build from GitHub into thirdparty\ffmpeg, checked against the release's SHA-256 list.
# Prints "progress" lines for Recomp Launcher; exits 0 when FFmpeg is there, 1 when it couldn't be had.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$target = Join-Path $root 'thirdparty\ffmpeg'

function Test-FFmpeg([string]$folder) {
    if (-not $folder) { return $false }
    if (-not (Test-Path (Join-Path $folder 'include\libavcodec\avcodec.h'))) { return $false }
    if (-not (Test-Path (Join-Path $folder 'lib\avcodec.lib'))) { return $false }
    return [bool](Get-ChildItem (Join-Path $folder 'bin') -Filter 'avcodec-*.dll' -ErrorAction SilentlyContinue)
}

$candidates = @($env:FFMPEG_ROOT, $target)
$parent = Split-Path $root -Parent
foreach ($p in @($parent, (Join-Path $parent '_tools'))) {
    $candidates += Get-ChildItem $p -Directory -Filter 'ffmpeg*' -ErrorAction SilentlyContinue | ForEach-Object FullName
}
foreach ($c in $candidates) {
    if (Test-FFmpeg $c) {
        Write-Output "FFmpeg: $c"
        exit 0
    }
}

$release = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest'
$name = 'ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip'
$work = Join-Path $root 'out\setup\ffmpeg'
New-Item -ItemType Directory -Force $work | Out-Null
$zip = Join-Path $work $name

try {
    Write-Output 'progress 0.002 Getting FFmpeg (the audio decoder, about 40 MB)'
    $sums = (Invoke-WebRequest -UseBasicParsing "$release/checksums.sha256").Content
    if ($sums -is [byte[]]) { $sums = [Text.Encoding]::ASCII.GetString($sums) }
    $line = ($sums -split "`n") | Where-Object { $_ -match ([regex]::Escape($name) + '\s*$') } | Select-Object -First 1
    if (-not $line) {
        # A newer FFmpeg took 8.1's place in the latest release: its newest 8.x LGPL shared build.
        $names = ($sums -split "`n") | ForEach-Object { if ($_ -match '(ffmpeg-n8\.(\d+)-latest-win64-lgpl-shared-8\.\d+\.zip)\s*$') { [pscustomobject]@{ Name = $Matches[1]; Minor = [int]$Matches[2] } } }
        $pick = $names | Sort-Object Minor -Descending | Select-Object -First 1
        if (-not $pick) { throw "no FFmpeg 8 LGPL shared build for Windows in $release" }
        $name = $pick.Name
        $zip = Join-Path $work $name
        $line = ($sums -split "`n") | Where-Object { $_ -match ([regex]::Escape($name) + '\s*$') } | Select-Object -First 1
    }
    $expected = ($line.Trim() -split '\s+')[0].ToLowerInvariant()

    Invoke-WebRequest -UseBasicParsing "$release/$name" -OutFile $zip
    $actual = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $expected) { throw "the download of $name doesn't match its SHA-256" }

    Write-Output 'progress 0.004 Unpacking FFmpeg'
    $unpacked = Join-Path $work 'unpacked'
    if (Test-Path $unpacked) { Remove-Item $unpacked -Recurse -Force }
    Expand-Archive $zip -DestinationPath $unpacked
    $inner = Get-ChildItem $unpacked -Directory | Select-Object -First 1
    if (-not $inner -or -not (Test-FFmpeg $inner.FullName)) { throw "$name doesn't have the include, lib and bin folders" }
    if (Test-Path $target) { Remove-Item $target -Recurse -Force }
    Move-Item $inner.FullName $target
    Remove-Item $work -Recurse -Force
    Write-Output "FFmpeg: $name, into thirdparty\ffmpeg"
    exit 0
}
catch {
    Write-Output "progress 0 Stopped: FFmpeg (the audio decoder) couldn't be downloaded: $($_.Exception.Message). Unzip a win64 LGPL shared build of FFmpeg into thirdparty\ffmpeg and set the game up again."
    exit 1
}
