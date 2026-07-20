# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [Parameter(Mandatory = $true)]
    [string]$Installer
)

$ErrorActionPreference = "Stop"
$source = Get-Content -LiteralPath $Installer -Raw
$tokens = $null
$parseErrors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile(
    $Installer,
    [ref]$tokens,
    [ref]$parseErrors)
if ($parseErrors.Count -ne 0) {
    throw "Installer has PowerShell parse errors: $($parseErrors -join '; ')"
}

$requirements = [ordered]@{
    "bounded service deletion" =
        'function\s+Wait-ServiceDeletion[\s\S]+Get-Service[\s\S]+TimeoutSeconds'
    "vendor service cannot auto-start" =
        'Set-Service\s+-Name\s+"SDRplayAPIService"\s+-StartupType\s+Manual'
    "OpenRSP automatic startup" =
        'New-Service[\s\S]+-Name\s+OpenRSP[\s\S]+-StartupType\s+Automatic'
    "OpenRSP restart recovery" =
        '"failure"\s*,\s*"OpenRSP"[\s\S]+"reset="\s*,\s*"60"[\s\S]+restart/5000/restart/15000/restart/30000/restart/30000/restart/30000'
    "non-crash failure recovery" =
        '"failureflag"\s*,\s*"OpenRSP"\s*,\s*"1"'
    "daemon port service environment" =
        '"OPENRSPD_PORT=\$Port"[\s\S]+Services\\OpenRSP'
    "client port survives reboot" =
        'SetEnvironmentVariable\([\s\S]+"OPENRSPD_PORT"[\s\S]+"Machine"\)'
}

foreach ($requirement in $requirements.GetEnumerator()) {
    if ($source -notmatch $requirement.Value) {
        throw "Installer contract missing: $($requirement.Key)"
    }
}

"WINDOWS_INSTALLER_CONTRACT_OK requirements=$($requirements.Count)"
