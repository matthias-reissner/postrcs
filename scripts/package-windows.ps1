param(
    [Parameter(Mandatory = $true)]
    [string] $PgRoot,
    [string] $Version = "1.2"
)

$ErrorActionPreference = "Stop"
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$distributionPath = Join-Path $repositoryRoot "dist"
$packageName = "postrcs-$Version-pg18-windows-x64"
$zipPath = Join-Path $distributionPath "$packageName.zip"
$checksumPath = Join-Path $distributionPath "$packageName.sha256"
$stagePath = Join-Path $env:TEMP ("postrcs-package-" + [guid]::NewGuid().ToString("N"))
$extensionPath = Join-Path $stagePath "share\extension"

if (-not (Test-Path -LiteralPath (Join-Path $PgRoot "lib\postrcs.dll"))) {
    throw "PostRCS DLL not found under '$PgRoot\lib'. Build the extension first."
}

New-Item -ItemType Directory -Path $extensionPath -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stagePath "lib") -Force | Out-Null
New-Item -ItemType Directory -Path $distributionPath -Force | Out-Null

try {
    Copy-Item -LiteralPath (Join-Path $PgRoot "lib\postrcs.dll") `
        -Destination (Join-Path $stagePath "lib\postrcs.dll")
    Copy-Item -LiteralPath (Join-Path $repositoryRoot "postrcs.control") `
        -Destination $extensionPath
    Copy-Item -LiteralPath (Join-Path $repositoryRoot "postrcs--1.0.sql") `
        -Destination $extensionPath
    Copy-Item -LiteralPath (Join-Path $repositoryRoot "postrcs--1.1.sql") `
        -Destination $extensionPath
    Copy-Item -LiteralPath (Join-Path $repositoryRoot "postrcs--1.2.sql") `
        -Destination $extensionPath
    Copy-Item -LiteralPath (Join-Path $repositoryRoot "postrcs--1.0--1.1.sql") `
        -Destination $extensionPath
    Copy-Item -LiteralPath (Join-Path $repositoryRoot "postrcs--1.1--1.2.sql") `
        -Destination $extensionPath
    Copy-Item -LiteralPath (Join-Path $repositoryRoot "USAGE.md") `
        -Destination $stagePath
    Copy-Item -LiteralPath (Join-Path $distributionPath "README.txt") `
        -Destination $stagePath

    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Compress-Archive -Path (Join-Path $stagePath "*") `
        -DestinationPath $zipPath -CompressionLevel Optimal
    $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath $checksumPath `
        -Value "$hash  $packageName.zip" -Encoding ASCII
    Write-Output "Created $zipPath"
    Write-Output "SHA256 $hash"
}
finally {
    if (Test-Path -LiteralPath $stagePath) {
        Remove-Item -LiteralPath $stagePath -Recurse -Force
    }
}
