$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

Push-Location (Join-Path $repoRoot "PicoSwitch")
try {
    npm ci
    if ($LASTEXITCODE -ne 0) { throw "npm ci failed" }
    node compress.js
    if ($LASTEXITCODE -ne 0) { throw "CivSwitch payload generation failed" }
} finally {
    Pop-Location
}

Push-Location $PSScriptRoot
try {
    node prepare_payload.mjs
    if ($LASTEXITCODE -ne 0) { throw "Flipper payload embedding failed" }
    ufbt build
    if ($LASTEXITCODE -ne 0) { throw "uFBT build failed" }

    $ufbtHome = if ($env:UFBT_HOME) { $env:UFBT_HOME } else { Join-Path $env:USERPROFILE ".ufbt" }
    $builtFap = Join-Path $ufbtHome "build\civswitch_flipper.fap"
    $distDir = Join-Path $PSScriptRoot "dist"
    New-Item -ItemType Directory -Force -Path $distDir | Out-Null
    Copy-Item -LiteralPath $builtFap -Destination (Join-Path $distDir "CivSwitch.fap") -Force
    Write-Host "Built $(Join-Path $distDir 'CivSwitch.fap')"
} finally {
    Pop-Location
}
