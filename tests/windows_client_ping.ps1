# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [Parameter(Mandatory = $true)]
    [string]$Daemon,
    [Parameter(Mandatory = $true)]
    [string]$Client
)

$ErrorActionPreference = "Stop"
$env:OPENRSPD_PORT = "50155"
$log = Join-Path $env:TEMP "openrsp-windows-ping-$PID.log"
$driver = Start-Process -FilePath $Daemon -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput $log -RedirectStandardError "$log.error"
try {
    Start-Sleep -Milliseconds 500
    if ($driver.HasExited) {
        throw "openrspd exited before the client ping"
    }
    & $Client
    if ($LASTEXITCODE -ne 0) {
        throw "openrsp-client-test failed with exit code $LASTEXITCODE"
    }
}
finally {
    if (-not $driver.HasExited) {
        Stop-Process -Id $driver.Id -Force
        $driver.WaitForExit()
    }
    Remove-Item -LiteralPath $log, "$log.error" -Force -ErrorAction SilentlyContinue
}
