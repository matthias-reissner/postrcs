param(
    [Parameter(Mandatory = $true)]
    [string] $PgRoot,
    [string] $Version = "1.2.1"
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
    $extensionScripts = Get-ChildItem -LiteralPath $repositoryRoot `
        -Filter "postrcs--*.sql" -File
    if (-not ($extensionScripts.Name -contains "postrcs--$Version.sql")) {
        throw "Install script for PostRCS $Version was not found."
    }
    $extensionScripts | Copy-Item -Destination $extensionPath
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
