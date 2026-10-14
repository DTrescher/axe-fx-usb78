[CmdletBinding()]
param([switch]$Remove)

$ErrorActionPreference = 'Stop'
$taskName = 'Axe-Fx III USB 7-8 FAS Bridge'
$oldTaskName = 'Axe-Fx III USB 7-8 Bridge'

if ($Remove) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Output 'Removed the FAS startup task. The previous bridge task was left unchanged.'
    return
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$panel = Join-Path $projectRoot 'build\Release\axe-usb78-panel.exe'
if (-not (Test-Path -LiteralPath $panel -PathType Leaf)) {
    throw 'Build the tray panel first: .\tools\build.ps1 -Test'
}

$oldTask = Get-ScheduledTask -TaskName $oldTaskName -ErrorAction SilentlyContinue
if ($oldTask -and $oldTask.State -eq 'Running') {
    throw 'Stop the previous bridge before registering the tray panel startup task.'
}
$oldWasEnabled = [bool]($oldTask -and $oldTask.Settings.Enabled)
$oldWasDisabledByThisScript = $false
$previousPanelTask = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
$previousPanelTaskXml = $null
$previousPanelTaskWasEnabled = $false
if ($previousPanelTask) {
    $previousPanelTaskXml = Export-ScheduledTask -TaskName $taskName -ErrorAction Stop
    $previousPanelTaskWasEnabled = [bool]$previousPanelTask.Settings.Enabled
}
$newTaskRegistrationAttempted = $false

try {
    if ($oldWasEnabled) {
        $oldWasDisabledByThisScript = $true
        Disable-ScheduledTask -TaskName $oldTaskName | Out-Null
    }

    $user = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
    $action = New-ScheduledTaskAction -Execute $panel -Argument '--minimized' -WorkingDirectory $projectRoot
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet `
        -Priority 4 `
        -ExecutionTimeLimit ([TimeSpan]::Zero) `
        -RestartCount 3 `
        -RestartInterval (New-TimeSpan -Minutes 1) `
        -MultipleInstances IgnoreNew `
        -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries

    $newTaskRegistrationAttempted = $true
    Register-ScheduledTask `
        -TaskName $taskName `
        -Action $action `
        -Trigger $trigger `
        -Principal $principal `
        -Settings $settings `
        -Description 'Starts the per-user FAS tray panel at logon; the saved routing toggle controls playback.' `
        -Force | Out-Null

    $registered = Get-ScheduledTask -TaskName $taskName -ErrorAction Stop
    if (-not $registered.Settings.Enabled) {
        throw 'Task Scheduler registered the FAS task in a disabled state.'
    }
}
catch {
    $registrationFailure = $_
    $rollbackErrors = [System.Collections.Generic.List[string]]::new()

    if ($newTaskRegistrationAttempted) {
        try {
            if ($previousPanelTaskXml) {
                Register-ScheduledTask -TaskName $taskName -Xml $previousPanelTaskXml -Force -ErrorAction Stop | Out-Null
                if ($previousPanelTaskWasEnabled) {
                    Enable-ScheduledTask -TaskName $taskName -ErrorAction Stop | Out-Null
                }
                else {
                    Disable-ScheduledTask -TaskName $taskName -ErrorAction Stop | Out-Null
                }
            }
            else {
                $partiallyRegistered = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
                if ($partiallyRegistered) {
                    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction Stop
                }
            }
        }
        catch {
            $rollbackErrors.Add("Restoring the previous FAS task failed: $($_.Exception.Message)")
        }
    }

    if ($oldWasDisabledByThisScript) {
        try {
            Enable-ScheduledTask -TaskName $oldTaskName -ErrorAction Stop | Out-Null
        }
        catch {
            $rollbackErrors.Add("Re-enabling the previous bridge task failed: $($_.Exception.Message)")
        }
    }

    if ($rollbackErrors.Count) {
        throw "Startup registration failed: $($registrationFailure.Exception.Message) Rollback also failed: $($rollbackErrors -join '; ')"
    }
    throw $registrationFailure.Exception
}

Write-Output 'FAS tray panel startup registered for the current user at logon. It starts minimized and honors the saved routing toggle.'
if ($oldWasEnabled) {
    Write-Output 'The previous bridge task remains registered but disabled for rollback; remove it after validating the panel startup.'
}
