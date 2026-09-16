$ErrorActionPreference = 'Stop'
try {
    $taskIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $taskPrincipal = [Security.Principal.WindowsPrincipal]::new($taskIdentity)
    if (-not $taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'firewall-onar.bat dosyasini sag tik > Yonetici olarak calistirin.'
    }
    $taskExe = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'lumiri-host.exe')).Path
    
    $taskRules = @(Get-NetFirewallApplicationFilter -Program $taskExe -ErrorAction SilentlyContinue |
        Get-NetFirewallRule | Where-Object { $_.Direction -eq 'Inbound' -and $_.Action -eq 'Block' -and $_.Enabled -eq 'True' })
    if ($taskRules.Count) {
        $taskBackupPath = Join-Path $PSScriptRoot ('firewall-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.json')
        $taskRules | Select-Object Name, DisplayName, Enabled, Profile, Action |
            ConvertTo-Json | Set-Content -LiteralPath $taskBackupPath -Encoding UTF8
        $taskRules | Disable-NetFirewallRule
    }
    $taskHash = [Security.Cryptography.SHA256]::Create()
    try { $taskKey = ([BitConverter]::ToString($taskHash.ComputeHash([Text.Encoding]::UTF8.GetBytes($taskExe.ToLowerInvariant())))).Replace('-', '').Substring(0,16) }
    finally { $taskHash.Dispose() }
    foreach ($taskProtocol in @('TCP', 'UDP')) {
        $taskName = 'Lumiri-LAN-' + $taskKey + '-' + $taskProtocol
        $taskPorts = if ($taskProtocol -eq 'TCP') { @('47800') } else { @('47799','47802') }
        $taskExisting = Get-NetFirewallRule -Name $taskName -ErrorAction SilentlyContinue
        if ($taskExisting) { $taskExisting | Remove-NetFirewallRule }
        New-NetFirewallRule -Name $taskName -DisplayName ('Lumiri LAN ' + $taskProtocol + ' ' + $taskExe) `
            -Direction Inbound -Action Allow -Enabled True -Profile Any -Program $taskExe `
            -Protocol $taskProtocol -LocalPort $taskPorts -RemoteAddress LocalSubnet | Out-Null
    }
    Write-Host 'Tamam. Bu klasordeki host icin yerel ag erisimi ayarlandi.'
    Write-Host 'Eski host pencerelerini kapatin; bu klasordeki lumiri-host.exe dosyasini bir kez acin.'
    exit 0
} catch {
    Write-Host ('HATA: ' + $_.Exception.Message) -ForegroundColor Red
    exit 1
}

