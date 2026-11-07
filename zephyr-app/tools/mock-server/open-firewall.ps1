# Opens the TCP port in the Windows firewall for the mock server. Run PowerShell AS ADMINISTRATOR.
#   .\open-firewall.ps1 -Port 8443            (add -Ntp to also open UDP 123)
# Remove it again after the test:  .\open-firewall.ps1 -Remove
param([int]$Port = 8443, [switch]$Ntp, [switch]$Remove)
$ErrorActionPreference = "Stop"
$name = "taxi mock server"
Get-NetFirewallRule -DisplayName "$name*" -ErrorAction SilentlyContinue | Remove-NetFirewallRule
if ($Remove) { Write-Host "firewall rules removed"; return }
New-NetFirewallRule -DisplayName "$name TCP $Port" -Direction Inbound -Protocol TCP -LocalPort $Port -Action Allow | Out-Null
if ($Ntp) { New-NetFirewallRule -DisplayName "$name NTP" -Direction Inbound -Protocol UDP -LocalPort 123 -Action Allow | Out-Null }
Write-Host "opened TCP $Port" $(if ($Ntp) { "and UDP 123" })
