param(
    [string]$RomPath = "H:\Assets\analoguegen1\common\pokered.gb"
)

$ErrorActionPreference = "Stop"

$RepoRoot    = (Resolve-Path "$PSScriptRoot\..").Path
$ToolsDir    = Join-Path $RepoRoot "tools\gbrecomp"
$OutDir      = Join-Path $RepoRoot "generated\pokered"
$ZipUrl      = "https://github.com/arcanite24/gb-recompiled/releases/download/v0.1.0/gb-recompiled-windows-x64.zip"
$ZipPath     = Join-Path $ToolsDir "gb-recompiled-windows-x64.zip"

if (-not (Test-Path $RomPath)) {
    throw "ROM not found at '$RomPath'. Pass -RomPath <path-to-pokered.gb>."
}

$GbRecompExe = Get-ChildItem -Path $ToolsDir -Filter "gbrecomp.exe" -Recurse -ErrorAction SilentlyContinue |
               Select-Object -First 1 -ExpandProperty FullName

if (-not $GbRecompExe) {
    Write-Host "[recomp_rom] Downloading gb-recompiled v0.1.0..."
    New-Item -ItemType Directory -Force -Path $ToolsDir | Out-Null
    Invoke-WebRequest -Uri $ZipUrl -OutFile $ZipPath -UseBasicParsing
    Expand-Archive -Path $ZipPath -DestinationPath $ToolsDir -Force
    Remove-Item -Path $ZipPath -Force -ErrorAction SilentlyContinue

    $GbRecompExe = Get-ChildItem -Path $ToolsDir -Filter "gbrecomp.exe" -Recurse -ErrorAction SilentlyContinue |
                   Select-Object -First 1 -ExpandProperty FullName
    if (-not $GbRecompExe) {
        throw "Failed to locate gbrecomp.exe inside '$ToolsDir' after extraction."
    }
}

Write-Host "[recomp_rom] Using recompiler: $GbRecompExe"
Write-Host "[recomp_rom] Input ROM:        $RomPath"
Write-Host "[recomp_rom] Output directory: $OutDir"

if (Test-Path $OutDir) {
    Get-ChildItem -Path $OutDir -Filter "*.c" -ErrorAction SilentlyContinue | Remove-Item -Force
    Get-ChildItem -Path $OutDir -Filter "*.h" -ErrorAction SilentlyContinue | Remove-Item -Force
} else {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
}

& $GbRecompExe $RomPath -o $OutDir --output-prefix pokered
if ($LASTEXITCODE -ne 0) {
    throw "gbrecomp failed with exit code $LASTEXITCODE."
}

Write-Host ""
Write-Host "[recomp_rom] Static recompilation complete!"
Write-Host "Next steps:"
Write-Host "  Pocket (via Docker):  .\tools\docker_run.ps1 `"BACKEND=recomp pocket`""
Write-Host "  Pocket (Linux / CI):  make -C src BACKEND=recomp pocket"
Write-Host "  PC SDL2 (Linux/WSL):  make -C src BACKEND=recomp pc"
