# Clones the upstream reference projects into refs\ (ignored by git), read-only, for study.
# Nothing from refs\ is built or run by DKRecomp. Copying code from them requires an entry in THIRD_PARTY_NOTICES.md.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$refs = Join-Path $root "refs"
New-Item -ItemType Directory -Force $refs | Out-Null
$repos = @(
    "https://github.com/valentderah/totk-pc-recomp.git",
    "https://github.com/bgyss/SwitchRecomp.git",
    "https://github.com/peiikai/LASRecomp.git",
    "https://github.com/N64Recomp/N64Recomp.git"
)
foreach ($url in $repos) {
    $name = [IO.Path]::GetFileNameWithoutExtension($url)
    $dest = Join-Path $refs $name
    if (Test-Path $dest) { Write-Host "$name already present"; continue }
    git clone --depth 1 $url $dest
}
