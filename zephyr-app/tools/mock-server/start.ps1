# Starts the mock server on Windows (PowerShell).
#   .\start.ps1 -Ip 203.0.113.7 -Port 8443          # your static IP and the open TCP port
#   .\start.ps1 -Ip 192.168.1.20 -Port 8443 -Ntp    # also NTP on UDP 123 (meter on the same network; run PowerShell as administrator)
# First run: makes a virtual environment (.venv) and installs the packages. Needs Python 3.11+ from python.org.
param(
    [string]$Ip = "",
    [int]$Port = 8443,
    [switch]$Ntp
)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

if (-not (Get-Command py -ErrorAction SilentlyContinue)) {
    throw "Python is not installed. Get Python 3.11 or newer from https://www.python.org/downloads/ (tick 'Add python.exe to PATH' and 'py launcher')."
}
$ver = & py -3 -c "import sys; print('%d.%d' % sys.version_info[:2])"
if ([version]$ver -lt [version]"3.11") { throw "Python $ver is too old: 3.11 or newer is needed." }
if (-not (Test-Path ".venv\Scripts\python.exe")) {
    Write-Host "Creating .venv ..."
    py -3 -m venv .venv
}
& .\.venv\Scripts\python.exe -m pip install -q -r requirements.txt

$serverArgs = @("server.py", "--port", $Port)
if ($Ip)  { $serverArgs += @("--ip", $Ip) }
if ($Ntp) { $serverArgs += "--ntp" }
& .\.venv\Scripts\python.exe @serverArgs
