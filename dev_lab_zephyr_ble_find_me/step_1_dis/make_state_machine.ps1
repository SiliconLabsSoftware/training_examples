# .\make_state_machine.ps1 state_machine.md

param(
    [Parameter(Mandatory=$true, Position=0)]
    [string]$OutputFileName
)

$InputPath = "src\main.c"
$OutputFolder = "state_machine"
$OutputPath = Join-Path $OutputFolder $OutputFileName

if (-not (Test-Path -LiteralPath $InputPath)) {
    throw "Input file not found: $InputPath"
}

# Create output folder if it doesn't exist
if (-not (Test-Path -LiteralPath $OutputFolder)) {
    New-Item -ItemType Directory -Path $OutputFolder | Out-Null
}

$pattern = '^.*//!\s*'

Get-Content -LiteralPath $InputPath |
    Where-Object { $_ -match '//!' } |
    ForEach-Object { $_ -replace $pattern, '' } |
    Set-Content -LiteralPath $OutputPath -Encoding UTF8

Write-Host "State machine file created at: $OutputPath"