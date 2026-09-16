$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath $PSScriptRoot).Path
foreach ($taskPair in @(@('pc-host/out/lumiri-host.exe','Lumiri/lumiri-host.exe'), @('switch-client/lumiri.nro','Lumiri/switch/lumiri.nro'))) {
    if ((Get-FileHash -LiteralPath (Join-Path $taskRoot $taskPair[0])).Hash -ne
        (Get-FileHash -LiteralPath (Join-Path $taskRoot $taskPair[1])).Hash) { throw 'Current distribution does not match build outputs' }
}
if (!(Test-Path -LiteralPath (Join-Path $taskRoot 'Lumiri.zip'))) { throw 'Current archive is missing' }
$taskNames = @(
    'backups', 'dist', 'research-render',
    'release-host-v1.1.3', 'release-lumiri-v1.3.0', 'release-lumiri-v1.3.1', 'release-lumiri-v1.3.2',
    'release-v1.1.1', 'release-v1.1.2', 'release-v1.2.0', 'release-v1.2.1', 'release-v1.2.2',
    'release-v1.2.3', 'release-v1.2.4', 'release-v1.2.5', 'release-v1.2.6', 'release-v1.2.7', 'release-v1.2.8',
    'Lumiri-v1.3.0.zip', 'Lumiri-v1.3.1.zip', 'Lumiri-v1.3.2.zip',
    'RemotePlay-host-v1.1.3.zip', 'RemotePlay-v1.1.zip', 'RemotePlay-v1.1.2.zip', 'RemotePlay-v1.2.0.zip',
    'RemotePlay-v1.2.1-recovery.zip', 'RemotePlay-v1.2.2.zip', 'RemotePlay-v1.2.3-switch.zip',
    'RemotePlay-v1.2.4.zip', 'RemotePlay-v1.2.5-switch.zip', 'RemotePlay-v1.2.8-switch.zip',
    'RELEASE-host-v1.1.3.md', 'RELEASE-v1.1.2.md', 'RELEASE-v1.2.0.md', 'RELEASE-v1.2.1.md',
    'RELEASE-v1.2.2.md', 'RELEASE-v1.2.3.md', 'RELEASE-v1.2.4.md', 'RELEASE-v1.2.5.md', 'RELEASE-v1.2.8.md',
    'remoteplay-host.exe', 'lumiri-host.exe', 'rnul',
    'switch-client/build', 'switch-client/build-anime-127', 'switch-client/build-clock-readback-123',
    'switch-client/build-clock-ui-125', 'switch-client/build-deko-check', 'switch-client/build-handshake-122',
    'switch-client/build-language-128', 'switch-client/build-lumiri-130', 'switch-client/build-lumiri-132',
    'switch-client/build-mascot-126', 'switch-client/build-monitor-settings-124', 'switch-client/build-recovery-121',
    'switch-client/remoteplay.elf', 'switch-client/remoteplay.nacp', 'switch-client/remoteplay.nro',
    'switch-client/lumiri.elf', 'switch-client/lumiri.nacp', 'switch-client/icon.jpg', 'switch-client/mascot-icon.jpg',
    'tests/out', 'assets/mascot/ANIME-PROMPT.md', 'assets/mascot/remoteplay-anime.png',
    'assets/mascot/remoteplay-manta.png', 'assets/mascot/README.md'
)
$taskPaths = foreach ($taskName in $taskNames) {
    $taskPath = Join-Path $taskRoot $taskName
    if (!(Test-Path -LiteralPath $taskPath)) { continue }
    $taskResolved = (Resolve-Path -LiteralPath $taskPath).Path
    if (!$taskResolved.StartsWith($taskRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Outside workspace: $taskResolved" }
    $taskItem = Get-Item -LiteralPath $taskResolved -Force
    if ($taskItem.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked path: $taskResolved" }
    if ($taskItem.PSIsContainer -and (Get-ChildItem -LiteralPath $taskResolved -Recurse -Force -Attributes ReparsePoint)) { throw "Nested linked path: $taskResolved" }
    $taskResolved
}
foreach ($taskPath in $taskPaths) {
    Remove-Item -LiteralPath $taskPath -Recurse -Force
    Write-Output "Removed: $taskPath"
}
Write-Output 'Cleanup complete. Current distribution: Lumiri/ and Lumiri.zip. Sources, dependencies, tests, settings and Git metadata are preserved.'
