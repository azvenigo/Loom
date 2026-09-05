# Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#
# Install, update or remove loom as a Windows service. Run from an elevated PowerShell.
#
#   .\loom-service.ps1 install [-Binary ..\..\build\Release\loom.exe]
#   .\loom-service.ps1 update  [-Binary ...]
#   .\loom-service.ps1 uninstall
#   .\loom-service.ps1 status
#
# SAME SHAPE AS THE LINUX SCRIPTS, deliberately - config preserved across updates, the old binary
# kept as loom.prev, a health check that reads /stats rather than trusting the service state, and a
# rollback when that check fails. Two platforms with different update semantics is how one of them
# quietly stops being tested.
#
# sc.exe rather than NSSM or a wrapper: loom runs in the foreground, writes to stdout and exits on
# a stop signal, which is all a plain service needs. The one thing it does NOT get from sc.exe is
# graceful SIGTERM semantics - Windows sends SERVICE_CONTROL_STOP and loom's console handler takes
# it, which is why Stop-Service is given time to let the final snapshot land before anything else
# happens.

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('install', 'update', 'uninstall', 'status')]
    [string] $Action = 'status',

    [string] $Binary,
    [string] $InstallDir = 'C:\Program Files\Loom',
    [string] $DataDir    = 'C:\ProgramData\Loom\data',
    [string] $ConfigPath = 'C:\ProgramData\Loom\loom.conf'
)

$ErrorActionPreference = 'Stop'
$ServiceName = 'loom'
$Repo    = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Target  = Join-Path $InstallDir 'loom.exe'
$Prev    = Join-Path $InstallDir 'loom.prev.exe'

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not (New-Object Security.Principal.WindowsPrincipal $id).IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this from an elevated PowerShell.'
    }
}

# The config is a flat KEY=VALUE file, the same shape as the systemd EnvironmentFile, so the two
# platforms are configured identically and neither grows settings the other cannot express.
function Read-Config {
    $cfg = @{ LOOM_BIND = '127.0.0.1'; LOOM_PORT = '7700'; LOOM_DATA = $DataDir; LOOM_EXTRA = '' }
    if (Test-Path $ConfigPath) {
        foreach ($line in Get-Content $ConfigPath) {
            if ($line -match '^\s*([A-Z_]+)\s*=\s*(.*)$') { $cfg[$Matches[1]] = $Matches[2].Trim() }
        }
    }
    return $cfg
}

function Write-DefaultConfig {
    # NEVER overwrites. This file is the one thing here holding decisions somebody made.
    if (Test-Path $ConfigPath) { Write-Host "keeping existing $ConfigPath"; return }
    New-Item -ItemType Directory -Force -Path (Split-Path $ConfigPath) | Out-Null
    @(
        '# Loom service configuration. Preserved across updates.',
        'LOOM_BIND=127.0.0.1',
        'LOOM_PORT=7700',
        "LOOM_DATA=$DataDir",
        '# Anything else you would pass on the command line: --token=SECRET --sync=always',
        'LOOM_EXTRA='
    ) | Set-Content -Path $ConfigPath -Encoding ASCII
    Write-Host "wrote $ConfigPath"
}

function Get-Origin {
    $cfg  = Read-Config
    $bind = $cfg.LOOM_BIND
    if ([string]::IsNullOrWhiteSpace($bind) -or $bind -eq '0.0.0.0' -or $bind -eq '::') {
        $bind = '127.0.0.1'
    }
    return "http://${bind}:$($cfg.LOOM_PORT)"
}

# HEALTHY MEANS SERVING, not listening. /stats only answers once the snapshot is loaded and the
# store is up, and the jot count is a number a half-started process cannot produce.
function Wait-Healthy([int] $Seconds = 30) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    $origin   = Get-Origin
    while ((Get-Date) -lt $deadline) {
        try {
            $r = Invoke-RestMethod -Uri "$origin/stats" -TimeoutSec 3
            if ($null -ne $r.jots) { Write-Host "healthy: serving $($r.jots) jots"; return $true }
        } catch { }
        Start-Sleep -Seconds 1
    }
    return $false
}

function Stop-LoomAndWait {
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if (-not $svc -or $svc.Status -eq 'Stopped') { return }
    Stop-Service -Name $ServiceName -Force
    # The data lock makes one instance per directory an OS-enforced fact, so a replacement started
    # while the old process still holds it exits rather than interleaving writes. Wait it out.
    $svc.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
}

function Resolve-NewBinary {
    if ($Binary) { return (Resolve-Path $Binary).Path }
    foreach ($try in @('build\Release\loom.exe', 'build\loom.exe')) {
        $p = Join-Path $Repo $try
        if (Test-Path $p) { return $p }
    }
    throw "No loom.exe found - build it or pass -Binary."
}

switch ($Action) {

'install' {
    Assert-Admin
    $new = Resolve-NewBinary
    New-Item -ItemType Directory -Force -Path $InstallDir, $DataDir | Out-Null
    Write-DefaultConfig
    Stop-LoomAndWait
    Copy-Item $new $Target -Force

    $cfg = Read-Config
    $bin = "`"$Target`" --bind=$($cfg.LOOM_BIND) --port=$($cfg.LOOM_PORT) " +
           "--data=`"$($cfg.LOOM_DATA)`" $($cfg.LOOM_EXTRA)"

    if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
        sc.exe config $ServiceName binPath= $bin start= auto | Out-Null
    } else {
        sc.exe create $ServiceName binPath= $bin start= auto DisplayName= 'Loom jot service' | Out-Null
        sc.exe description $ServiceName 'In-RAM jot service with REST and MCP front ends.' | Out-Null
    }

    Start-Service -Name $ServiceName
    if (Wait-Healthy 30) {
        Write-Host ''
        Write-Host "loom is running on $(Get-Origin)"
        Write-Host "  config  $ConfigPath"
        Write-Host "  data    $($cfg.LOOM_DATA)"
    } else {
        throw 'loom did not come up healthy - check the Windows event log.'
    }
}

'update' {
    Assert-Admin
    $new = Resolve-NewBinary
    if (-not (Test-Path $Target)) { throw "Not installed - run 'install' first." }

    # Prove the new binary runs before stopping anything that currently works.
    & $new --help | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'The new binary does not run - nothing has been changed.' }

    if ((Get-FileHash $new).Hash -eq (Get-FileHash $Target).Hash) {
        Write-Host 'already running this exact binary - nothing to do'; return
    }

    Stop-LoomAndWait
    Copy-Item $Target $Prev -Force      # keep the old one BEFORE overwriting
    Copy-Item $new $Target -Force

    # The service command line is rebuilt from the config, so a setting added since the install is
    # picked up without anyone re-registering the service - and nothing in this path writes it.
    $cfg = Read-Config
    sc.exe config $ServiceName binPath= ("`"$Target`" --bind=$($cfg.LOOM_BIND) " +
        "--port=$($cfg.LOOM_PORT) --data=`"$($cfg.LOOM_DATA)`" $($cfg.LOOM_EXTRA)") | Out-Null

    Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if (Wait-Healthy 30) {
        Write-Host "update complete - the previous binary is at $Prev"
        return
    }

    Write-Warning 'The new build did not come up healthy - ROLLING BACK'
    Stop-LoomAndWait
    Copy-Item $Prev $Target -Force
    Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if (Wait-Healthy 30) { throw 'Rolled back; the previous build is running again.' }
    throw 'ROLLBACK ALSO FAILED - loom is down.'
}

'uninstall' {
    Assert-Admin
    Stop-LoomAndWait
    sc.exe delete $ServiceName | Out-Null
    # Data and config are deliberately left in place - see the Linux uninstaller for why.
    Write-Host "service removed. Data is still in $DataDir and settings in $ConfigPath."
}

'status' {
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if (-not $svc) { Write-Host 'loom is not installed as a service.'; return }
    Write-Host "service: $($svc.Status)"
    Write-Host "origin:  $(Get-Origin)"
    if (Wait-Healthy 3) { } else { Write-Host 'not answering /stats' }
}

}
