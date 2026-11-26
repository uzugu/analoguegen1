param(
    [Parameter(Position = 0)]
    [string]$Target = "pocket"
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path
$Image = "analoguegen1-dev"

if ($Target -eq "image") {
    docker build -t $Image $RepoRoot
    exit $LASTEXITCODE
}

docker run --rm -v "${RepoRoot}:/workspace" -w /workspace $Image make $Target
exit $LASTEXITCODE
