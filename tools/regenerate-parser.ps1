param(
    [Parameter(Mandatory = $true)]
    [string]$Packcc
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$grammar = Join-Path $projectRoot 'src/dicescript_parser.peg'
$output = Join-Path $projectRoot 'src/dicescript_parser'

if (-not (Test-Path -LiteralPath $Packcc -PathType Leaf)) {
    throw "PackCC executable not found: $Packcc"
}

& $Packcc -o $output $grammar
if ($LASTEXITCODE -ne 0) {
    throw "PackCC failed with exit code $LASTEXITCODE"
}

Write-Host 'Regenerated src/dicescript_parser.c and src/dicescript_parser.h'
