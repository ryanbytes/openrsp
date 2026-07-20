# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [Parameter(Mandatory = $true)]
    [string]$Daemon,
    [Parameter(Mandatory = $true)]
    [string]$Client
)

$ErrorActionPreference = "Stop"
$env:OPENRSPD_PORT = "50156"
$log = Join-Path $env:TEMP "openrsp-windows-daemon-lock-$PID.log"
$driver = Start-Process -FilePath $Daemon -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput $log -RedirectStandardError "$log.error"
try {
    Start-Sleep -Milliseconds 500
    if ($driver.HasExited) {
        throw "openrspd exited before the daemon lock test"
    }
    & $Client
    if ($LASTEXITCODE -ne 0) {
        throw "windows-daemon-lock-test failed with exit code $LASTEXITCODE"
    }
}
finally {
    if (-not $driver.HasExited) {
        Stop-Process -Id $driver.Id -Force
        $driver.WaitForExit()
    }
    Remove-Item -LiteralPath $log, "$log.error" -Force -ErrorAction SilentlyContinue
}
