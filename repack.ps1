$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
$taskRelease = Join-Path $taskRoot 'Lumiri'
$taskFiles = [ordered]@{
    'pc-host/out/lumiri-host.exe' = 'lumiri-host.exe'
    'switch-client/lumiri.nro' = 'switch/lumiri.nro'
    'README.md' = 'README.md'
    'pc-host/firewall-onar.bat' = 'firewall-onar.bat'
    'pc-host/firewall-onar.ps1' = 'firewall-onar.ps1'
    'pc-host/host-baslat.bat' = 'host-baslat.bat'
    'assets/mascot/lumiri-anime.png' = 'assets/mascot/lumiri-anime.png'
    'third_party/AMF/LICENSE.txt' = 'licenses/AMF-LICENSE.txt'
    'third_party/AMF/UPSTREAM.txt' = 'licenses/AMF-UPSTREAM.txt'
}
foreach ($taskSource in $taskFiles.Keys) {
    if (!(Test-Path -LiteralPath (Join-Path $taskRoot $taskSource) -PathType Leaf)) {
        throw "Missing build or source file: $taskSource"
    }
}
foreach ($taskSource in $taskFiles.Keys) {
    $taskDestination = Join-Path $taskRelease $taskFiles[$taskSource]
    New-Item -ItemType Directory -Path (Split-Path $taskDestination) -Force | Out-Null
    $taskSourcePath = Join-Path $taskRoot $taskSource
    if ((Test-Path -LiteralPath $taskDestination -PathType Leaf) -and
        (Get-FileHash -LiteralPath $taskSourcePath).Hash -eq (Get-FileHash -LiteralPath $taskDestination).Hash) { continue }
    Copy-Item -LiteralPath $taskSourcePath -Destination $taskDestination -Force
}
$taskNames = @($taskFiles.Values | Sort-Object)
$taskHashes = foreach ($taskName in $taskNames) {
    (Get-FileHash -LiteralPath (Join-Path $taskRelease $taskName)).Hash.ToLowerInvariant() + '  ' + $taskName
}
[IO.File]::WriteAllLines((Join-Path $taskRelease 'SHA256SUMS.txt'), [string[]]$taskHashes, [Text.UTF8Encoding]::new($false))
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$taskStream = [IO.File]::Open((Join-Path $taskRoot 'Lumiri.zip'), [IO.FileMode]::Create, [IO.FileAccess]::Write)
$taskArchive = [IO.Compression.ZipArchive]::new($taskStream, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($taskName in ($taskNames + 'SHA256SUMS.txt')) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($taskArchive,
            (Join-Path $taskRelease $taskName), $taskName, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $taskArchive.Dispose() }
Write-Output 'Updated Lumiri/ and Lumiri.zip. Local settings and Git metadata were preserved.'
