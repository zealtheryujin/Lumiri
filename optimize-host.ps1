param(
    [string]$Adapter = "Ethernet",
    [switch]$Apply
)

$ErrorActionPreference = "Stop"
$nic = Get-NetAdapter -Name $Adapter
$admin = ([Security.Principal.WindowsPrincipal](
    [Security.Principal.WindowsIdentity]::GetCurrent()
)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

Write-Host "Lumiri host performance check"
Write-Host "Adapter : $($nic.InterfaceDescription)"
Write-Host "Link    : $($nic.LinkSpeed)"
Write-Host "Status  : $($nic.Status)"
powercfg /getactivescheme

if ($nic.LinkSpeed -match "^100 Mbps") {
    Write-Warning "Link 100 Mbps. Replace/check the Ethernet cable and router/switch port until it negotiates at 1 Gbps."
}

$keywords = @(
    "AdvancedEEE",
    "EEE",
    "EnableGreenEthernet",
    "GigaLite",
    "*InterruptModeration"
)

Get-NetAdapterAdvancedProperty -Name $Adapter |
    Where-Object RegistryKeyword -in $keywords |
    Select-Object DisplayName, DisplayValue, RegistryKeyword |
    Format-Table -AutoSize

if (-not $Apply) {
    Write-Host "No settings changed. Run as Administrator with -Apply to disable latency-oriented power/coalescing features."
    exit 0
}
if (-not $admin) {
    throw "Administrator privileges are required for -Apply."
}

foreach ($keyword in $keywords) {
    $property = Get-NetAdapterAdvancedProperty -Name $Adapter -RegistryKeyword $keyword -ErrorAction SilentlyContinue
    if ($property) {
        Set-NetAdapterAdvancedProperty -Name $Adapter -RegistryKeyword $keyword -RegistryValue 0 -NoRestart
        Write-Host "Disabled: $($property.DisplayName)"
    }
}

powercfg /setactive SCHEME_MIN
Restart-NetAdapter -Name $Adapter
Write-Host "Applied. The adapter was restarted; verify that Link is now 1 Gbps."

