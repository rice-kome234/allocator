param(
    [ValidateSet('Debug', 'Release', 'Both')][string]$Configuration = 'Both',
    [switch]$Run,
    [switch]$Sanitize
)

$ErrorActionPreference = 'Stop'
# 日本語の実行結果を、外部プログラムから UTF-8 として受け取る。
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$projectRoot = Split-Path -Parent $PSScriptRoot
$locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = $null
if (Test-Path -LiteralPath $locator) {
    $installation = & $locator -latest -prerelease -version '[18.0,19.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $installation) {
    $vsRoot = Join-Path $env:ProgramFiles 'Microsoft Visual Studio\18'
    if (Test-Path -LiteralPath $vsRoot) {
        $installation = Get-ChildItem -LiteralPath $vsRoot -Directory |
            Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'MSBuild\Current\Bin\MSBuild.exe') } |
            Select-Object -First 1 -ExpandProperty FullName
    }
}
if (-not $installation) {
    Write-Error 'C++ x64 ツールを備えた Visual Studio 2026 が見つかりません。'
    exit 1
}
$msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
$configurations = @($Configuration)
if ($Configuration -eq 'Both') { $configurations = @('Debug', 'Release') }
foreach ($buildConfiguration in $configurations) {
    $arguments = @((Join-Path $projectRoot 'slabPool.slnx'), '/m', '/nologo', '/verbosity:minimal',
        "/p:Configuration=$buildConfiguration", '/p:Platform=x64')
    if ($Sanitize) { $arguments += '/p:Sanitize=true' }
    & $msbuild @arguments
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $outputConfiguration = $buildConfiguration
    if ($Sanitize) { $outputConfiguration += '-ASan' }
    $executable = Join-Path $projectRoot "bin\x64\$outputConfiguration\slabPool.exe"
    if ($Run) {
        # main.cppの最小使用例を実行する。実行時引数は不要。
        & $executable
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
}
exit 0
