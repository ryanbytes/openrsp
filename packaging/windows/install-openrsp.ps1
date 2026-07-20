# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageDirectory,

    [Parameter(Mandatory = $true)]
    [string]$FirmwarePath,

    [string]$InstallRoot = "$env:ProgramFiles\OpenRSP",
    [string]$ApiDirectory = "$env:ProgramFiles\SDRplay\API\x64",
    [ValidateRange(1, 65535)]
    [int]$Port = 50151,
    [switch]$StartService
)

$ErrorActionPreference = "Stop"

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [System.IO.File]::OpenRead($Path)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString(
            $sha256.ComputeHash($stream))).Replace("-", "")
    }
    finally {
        $sha256.Dispose()
        $stream.Dispose()
    }
}

function Invoke-ServiceControl {
    param([Parameter(Mandatory = $true)][string[]]$ArgumentList)

    $output = & sc.exe @ArgumentList 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "sc.exe $($ArgumentList -join ' ') failed: $($output -join ' ')"
    }
    return $output
}

function Wait-ServiceDeletion {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [int]$TimeoutSeconds = 15
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $service = Get-Service -Name $Name -ErrorAction SilentlyContinue
        if (-not $service) {
            return
        }
        $service.Dispose()
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $deadline)
    throw "Service $Name was not deleted within $TimeoutSeconds seconds."
}

$administrator = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $administrator.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this installer from an elevated Windows PowerShell session."
}

$packageBin = Join-Path (Resolve-Path $PackageDirectory) "bin"
$daemonSource = Join-Path $packageBin "openrspd.exe"
$apiSource = Join-Path $packageBin "sdrplay_api.dll"
$pthreadSource = Join-Path $packageBin "libwinpthread-1.dll"
$usbSource = Join-Path $packageBin "libusb-1.0.dll"
foreach ($required in @($daemonSource, $apiSource, $pthreadSource, $usbSource)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing packaged runtime file: $required"
    }
}

$firmware = Get-Item -LiteralPath $FirmwarePath
if ($firmware.Length -ne 6115) {
    throw "RSPduo firmware must be exactly 6115 bytes: $FirmwarePath"
}
$firmwareHash = Get-Sha256 -Path $firmware.FullName
if ($firmwareHash -ne
    "F2A9451ACD81FD8F09C5A0E506335D5D1EC9AB2C4ED53DD98DE5CFFF2A249387") {
    throw "RSPduo firmware hash does not match the verified API 3.15.1 image."
}

$vendorService = Get-Service -Name "SDRplayAPIService" -ErrorAction SilentlyContinue
if ($vendorService -and $vendorService.Status -ne "Stopped") {
    throw "Stop SDRplayAPIService before installing OpenRSP."
}
$vendorServiceConfiguration = Get-CimInstance Win32_Service `
    -Filter "Name = 'SDRplayAPIService'" -ErrorAction SilentlyContinue
$openrspService = Get-Service -Name "OpenRSP" -ErrorAction SilentlyContinue
if ($openrspService -and $openrspService.Status -ne "Stopped") {
    throw "Stop the existing OpenRSP service before upgrading it."
}

$installBin = Join-Path $InstallRoot "bin"
$installFirmwareDirectory = Join-Path $InstallRoot "share\openrsp\firmware"
$installedFirmware = Join-Path $installFirmwareDirectory "rspduo-3020.bin"
$installedApi = Join-Path $ApiDirectory "sdrplay_api.dll"
$installedPthread = Join-Path $ApiDirectory "libwinpthread-1.dll"
$statePath = Join-Path $InstallRoot "install-state.json"
$preservedVendorBackup = $null
$preservedVendorServiceStartMode = $null
$preservedMachinePort = $null
if (Test-Path -LiteralPath $statePath) {
    $existingState = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    if ($existingState.vendorBackup -and
        (Test-Path -LiteralPath $existingState.vendorBackup -PathType Leaf)) {
        $preservedVendorBackup = $existingState.vendorBackup
    }
    $preservedVendorServiceStartMode =
        $existingState.vendorServiceStartModeBeforeInstall
    $preservedMachinePort = $existingState.machinePortBeforeInstall
}
$vendorServiceStartModeBeforeInstall = if ($preservedVendorServiceStartMode) {
    $preservedVendorServiceStartMode
} elseif ($vendorServiceConfiguration) {
    $vendorServiceConfiguration.StartMode
} else {
    $null
}
$machinePortBeforeInstall = if ($preservedMachinePort) {
    $preservedMachinePort
} else {
    [Environment]::GetEnvironmentVariable("OPENRSPD_PORT", "Machine")
}
$timestamp = Get-Date -Format "yyyyMMddTHHmmss"
$backupDirectory = Join-Path $InstallRoot "vendor-backup-$timestamp"
$backupApi = Join-Path $backupDirectory "sdrplay_api.dll"

if ($PSCmdlet.ShouldProcess($InstallRoot, "Install OpenRSP Windows runtime")) {
    New-Item -ItemType Directory -Force -Path $installBin,
        $installFirmwareDirectory, $ApiDirectory | Out-Null
    Get-ChildItem -LiteralPath $packageBin -File | Copy-Item -Destination $installBin -Force
    Copy-Item -LiteralPath $firmware.FullName -Destination $installedFirmware -Force

    if ((Test-Path -LiteralPath $installedApi) -and
        -not $preservedVendorBackup) {
        New-Item -ItemType Directory -Force -Path $backupDirectory | Out-Null
        Copy-Item -LiteralPath $installedApi -Destination $backupApi
    }
    Copy-Item -LiteralPath $apiSource -Destination $installedApi -Force
    Copy-Item -LiteralPath $pthreadSource -Destination $installedPthread -Force

    if ($vendorServiceConfiguration -and
        $vendorServiceConfiguration.StartMode -eq "Auto") {
        Set-Service -Name "SDRplayAPIService" -StartupType Manual
    }

    if ($openrspService) {
        $openrspService.Dispose()
        Invoke-ServiceControl -ArgumentList @("delete", "OpenRSP") | Out-Null
        Wait-ServiceDeletion -Name "OpenRSP"
    }
    $daemon = Join-Path $installBin "openrspd.exe"
    New-Service -Name OpenRSP -BinaryPathName "`"$daemon`" --service" `
        -DisplayName "OpenRSP RSPduo Driver" -StartupType Automatic | Out-Null
    Invoke-ServiceControl -ArgumentList @(
        "description",
        "OpenRSP",
        "Open-source RSPduo hardware daemon for the SDRplay API compatibility library."
    ) | Out-Null
    Invoke-ServiceControl -ArgumentList @(
        "failure", "OpenRSP",
        "reset=", "60",
        "actions=", "restart/5000/restart/15000/restart/30000/restart/30000/restart/30000"
    ) | Out-Null
    Invoke-ServiceControl -ArgumentList @(
        "failureflag", "OpenRSP", "1"
    ) | Out-Null

    $logDirectory = Join-Path $env:ProgramData "OpenRSP"
    New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
    $serviceEnvironment = @(
        "OPENRSPD_PORT=$Port",
        "OPENRSP_RSPDUO_FIRMWARE=$installedFirmware",
        "OPENRSPD_LOG=$(Join-Path $logDirectory 'openrspd.log')"
    )
    New-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Services\OpenRSP" `
        -Name Environment -PropertyType MultiString -Value $serviceEnvironment `
        -Force | Out-Null
    [Environment]::SetEnvironmentVariable(
        "OPENRSPD_PORT", [string]$Port, "Machine")
    if ([Environment]::GetEnvironmentVariable("OPENRSPD_PORT", "Machine") -ne
        [string]$Port) {
        throw "Failed to persist OPENRSPD_PORT for Windows API clients."
    }
    $serviceRegistry = Get-ItemProperty -Path `
        "HKLM:\SYSTEM\CurrentControlSet\Services\OpenRSP"
    if (-not $serviceRegistry.FailureActions) {
        throw "OpenRSP service recovery actions were not persisted."
    }

    $state = [ordered]@{
        installedApi = $installedApi
        candidateApiSha256 = Get-Sha256 -Path $installedApi
        vendorBackup = if ($preservedVendorBackup) {
            $preservedVendorBackup
        } elseif (Test-Path $backupApi) {
            $backupApi
        } else {
            $null
        }
        vendorServiceStartModeBeforeInstall =
            $vendorServiceStartModeBeforeInstall
        machinePortBeforeInstall = $machinePortBeforeInstall
        installedAt = (Get-Date).ToUniversalTime().ToString("o")
    }
    $state | ConvertTo-Json | Set-Content -LiteralPath `
        $statePath -Encoding UTF8

    if ($StartService) {
        Start-Service -Name OpenRSP
        (Get-Service -Name OpenRSP).WaitForStatus("Running", (New-TimeSpan -Seconds 10))
    }
}
