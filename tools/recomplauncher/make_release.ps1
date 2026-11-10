# Makes the release zip that Recomp Launcher installs: this project's source at HEAD, with XenonRecomp and its libraries
# (at the commits the submodules name) in thirdparty\XenonRecomp, and recomplauncher.json at the top. Only files in git go
# in, so nothing of the game can: the player's own copy is only read on their PC, by tools\recomplauncher\prepare.cmd.
# Usage: powershell -ExecutionPolicy Bypass -File tools\recomplauncher\make_release.ps1   ->  out\release\SSX-resled-v<VERSION>-Windows-x64.zip
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root

if (git status --porcelain --untracked-files=no) { throw 'There are uncommitted changes: the release is made from HEAD, so commit them first.' }
$version = (Get-Content VERSION -TotalCount 1).Trim()
$name = "SSX-resled-v$version-Windows-x64"
$out = Join-Path $root 'out\release'
$stage = Join-Path $out 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

function Export-Tree([string]$repo, [string]$commit, [string]$into) {
    New-Item -ItemType Directory -Force $into | Out-Null
    $tar = Join-Path $out 'part.tar'
    git -C $repo archive --format=tar -o $tar $commit
    if ($LASTEXITCODE -ne 0) { throw "git archive of $commit in $repo failed" }
    tar -x -f $tar -C $into
    if ($LASTEXITCODE -ne 0) { throw "unpacking $repo failed" }
    Remove-Item $tar
}

# A checkout that has this commit: the submodule's own, or (for XenonRecomp) the one next to this folder where it's worked on.
function Find-Repo([string[]]$candidates, [string]$commit) {
    foreach ($c in $candidates) {
        if ((Test-Path (Join-Path $c '.git')) -and (git -C $c cat-file -t $commit 2>$null) -eq 'commit') { return $c }
    }
    throw "No checkout has commit $commit (looked in $($candidates -join ', ')). Run git submodule update --init --recursive."
}

# The submodules of a tree, and theirs, each exported at the commit the tree names. $checkouts: every checkout of the
# repo the tree is from (a submodule's own may not have its submodules, the one it's worked on in does).
function Export-Submodules([string[]]$checkouts, [string]$commit, [string]$into) {
    $repo = Find-Repo $checkouts $commit
    git -C $repo ls-tree -r $commit | ForEach-Object {
        if ($_ -match '^160000 commit ([0-9a-f]{40})\t(.+)$') {
            $sha = $Matches[1]; $path = $Matches[2]
            $candidates = @($checkouts | ForEach-Object { Join-Path $_ $path })
            if ($path -eq 'thirdparty/XenonRecomp') { $candidates += Join-Path (Split-Path $root -Parent) 'XenonRecomp' }
            $source = Find-Repo $candidates $sha
            Write-Output "  $path at $($sha.Substring(0, 9)) from $source"
            $target = Join-Path $into $path
            Export-Tree $source $sha $target
            Export-Submodules $candidates $sha $target
        }
    }
}

Write-Output "SSX-resled $version at $(git rev-parse --short HEAD)"
Export-Tree $root 'HEAD' $stage
Export-Submodules @($root) (git rev-parse HEAD) $stage

# What must be there for Recomp Launcher and the build.
foreach ($f in 'recomplauncher.json', 'VERSION', 'tools\recomplauncher\prepare.cmd', 'thirdparty\XenonRecomp\XenonUtils\ppc_context.h', 'thirdparty\XenonRecomp\thirdparty\simde\simde\x86\sse.h') {
    if (-not (Test-Path (Join-Path $stage $f))) { throw "$f is missing from the release" }
}
# And what must never be.
$bad = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in '.xex', '.iso', '.big', '.stfs' -or $_.FullName -match '\\ARRecompLib\\ppc\\' }
if ($bad) { throw "Game files in the release: $($bad.FullName -join ', ')" }

$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
tar -a -c -f $zip -C $stage *
if ($LASTEXITCODE -ne 0) { throw 'zipping failed' }
Remove-Item $stage -Recurse -Force
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $name.zip" | Set-Content -Encoding ascii "$zip.sha256"
Write-Output ("{0} ({1:N1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
Write-Output "SHA-256 $hash"
