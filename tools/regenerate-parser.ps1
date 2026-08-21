param(
    [Parameter(Mandatory = $true)]
    [string]$Packcc
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$parsers = @(
    @{ Grammar = 'src/dicescript_parser.peg'; Output = 'src/dicescript_parser' },
    @{ Grammar = 'src/dicescript_vm_parser.peg'; Output = 'src/dicescript_vm_parser' }
)

if (-not (Test-Path -LiteralPath $Packcc -PathType Leaf)) {
    throw "PackCC executable not found: $Packcc"
}

foreach ($parser in $parsers) {
    $grammar = Join-Path $projectRoot $parser.Grammar
    $output = Join-Path $projectRoot $parser.Output
    & $Packcc -o $output $grammar
    if ($LASTEXITCODE -ne 0) {
        throw "PackCC failed for $($parser.Grammar) with exit code $LASTEXITCODE"
    }
    Write-Host "Regenerated $($parser.Output).c and $($parser.Output).h"
}
