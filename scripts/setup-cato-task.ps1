<#
.SYNOPSIS
    Registers an elevated Scheduled Task for PowerTool App Govern control.
    Can be run from an elevated shell to enable non-elevated agents to toggle PowerTool via ai-helper.
#>
[CmdletBinding()]
param(
    [ValidateSet('install', 'uninstall', 'status')]
    [string]$Action = 'status',
    [string]$TaskName = 'PowerToolGovern'
)

$ErrorActionPreference = 'Stop'

if ($Action -eq 'status') {
    $t = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    if ($t) {
        Write-Host "Task '$TaskName' exists. State: $($t.State)"
        Write-Host "RunLevel: $($t.Principal.RunLevel)"
    } else {
        Write-Host "Task '$TaskName' is not registered."
    }
    return
}

if ($Action -eq 'uninstall') {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "Unregistered task '$TaskName'."
    return
}

if ($Action -eq 'install') {
    # Action: runs powershell to toggle or manage powertool
    $actionObj = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -Command "Write-Output PowerToolGovern"'
    $principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest
    $settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 2)
    
    Register-ScheduledTask -TaskName $TaskName -Action $actionObj -Principal $principal -Settings $settings -Force
    Write-Host "Task '$TaskName' registered successfully with RunLevel: Highest."
}
