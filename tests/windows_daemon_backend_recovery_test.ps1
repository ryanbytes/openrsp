# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [Parameter(Mandatory = $true)]
    [string]$Client
)

$ErrorActionPreference = "Stop"
$env:OPENRSPD_PORT = "50157"
& $Client
if ($LASTEXITCODE -ne 0) {
    throw "windows-daemon-backend-recovery-test failed with exit code $LASTEXITCODE"
}
