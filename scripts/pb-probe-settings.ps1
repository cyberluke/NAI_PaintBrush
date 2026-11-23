# Probe the NAI PaintBrush Settings dialog through Windows UI Automation.
#
# Regression gate for the "Settings crashes on open" class of bugs: the dialog
# resolves every controller property synchronously while opening, so an
# unregistered config key (assert(param != NULL) in src/config/core.c) kills
# the process at the moment the gear is invoked.
#
# Also reports the gear button's accessible name, which the i18n startup
# bootstrap changes ("NAI Settings" vs a translated name such as
# "NAI-Einstellungen"), and optionally dumps the visible UIA tree so popup
# contents can be inspected.
#
# Usage:
#   .\scripts\pb-probe-settings.ps1
#   .\scripts\pb-probe-settings.ps1 -ExpectGearName '^NAI-Einstellungen$' -Dump
#
# Exit codes: 0 = PASS, 1 = FAIL (dialog/process), 2 = no running app.

[CmdletBinding()]
param(
    [string]$ExpectGearName = "",
    [string]$PickLanguage = "",
    [switch]$ClickApply,
    [int]$TimeoutSec = 60,
    [int]$SettleMs = 3000,
    [switch]$Dump,
    [switch]$KeepOpen
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE  = [System.Windows.Automation.AutomationElement]
$TS  = [System.Windows.Automation.TreeScope]

function Fail([string]$message) {
    Write-Output "PROBE=FAIL"
    Write-Output "REASON=$message"
    exit 1
}

$proc = Get-Process nai-paintbrush -ErrorAction SilentlyContinue |
        Sort-Object StartTime -Descending | Select-Object -First 1
if (-not $proc) { Write-Output "PROBE=NOAPP"; exit 2 }

$pidCond = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $proc.Id)
$win = $null
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
    $win = $AE::RootElement.FindFirst($TS::Children, $pidCond)
    if ($win) { break }
    Start-Sleep -Milliseconds 400
}
if (-not $win) { Fail "main window not found within ${TimeoutSec}s (pid $($proc.Id))" }
Write-Output ("WINDOW=" + $win.Current.Name)

# The gear is an IconToolButton: Accessible.name is its `description`
# (qsTr("NAI Settings")), so the localized catalog changes the UIA Name.
$buttons = $win.FindAll($TS::Descendants,
    (New-Object System.Windows.Automation.PropertyCondition(
        $AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)))
$gear = $null
foreach ($b in $buttons) {
    $name = $b.Current.Name
    if ([string]::IsNullOrWhiteSpace($name)) { continue }
    if ($name -match 'Settings|Einstellungen|Nastavení|Nastavenia|Paramètres|Impostazioni|Configuración|Cài đặt') {
        $gear = $b
        Write-Output "GEAR_NAME=$name"
        break
    }
}
if (-not $gear) { Fail "settings gear button not found (buttons: $($buttons.Count))" }

if ($ExpectGearName -ne "") {
    if ($gear.Current.Name -notmatch $ExpectGearName) {
        Fail "gear name '$($gear.Current.Name)' does not match '$ExpectGearName'"
    }
    Write-Output "LANGUAGE_APPLIED=yes"
}

$invoke = $gear.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
$invoke.Invoke()
Start-Sleep -Milliseconds $SettleMs

$proc.Refresh()
if ($proc.HasExited) {
    Fail "process exited after opening Settings (crash reproduced); exit code $($proc.ExitCode)"
}
Write-Output "SETTINGS_ALIVE=yes"

# The Dialog is a QQuickPopup rendered inside the same native window; Qt still
# exposes it as a UIA Window whose AutomationId ends with the QML type name.
function Find-SettingsDialog {
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        $AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::Window)
    foreach ($w in $win.FindAll($TS::Descendants, $cond)) {
        if ($w.Current.AutomationId -like '*PaintbrushSettingsDialog*') { return $w }
    }
    return $null
}
$dlg = Find-SettingsDialog
if (-not $dlg) { Fail "settings dialog did not appear in the UIA tree" }
Write-Output ("DIALOG=" + $dlg.Current.Name)

function Find-ButtonByRegex([object]$root, [string]$pattern) {
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        $AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)
    foreach ($b in $root.FindAll($TS::Descendants, $cond)) {
        if ($b.Current.Name -match $pattern) { return $b }
    }
    return $null
}

function Get-FirstCombo([object]$root) {
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        $AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::ComboBox)
    return $root.FindFirst($TS::Descendants, $cond)
}

$combo = Get-FirstCombo $dlg
if (-not $combo) { Fail "language combo box not found in the dialog" }
Write-Output ("LANGUAGE_COMBO=" + $combo.Current.Name)

if ($PickLanguage -ne "") {
    try {
        $combo.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
    } catch {
        try {
            $combo.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
        } catch {
            Fail "language combo does not expose expand/invoke: $($_.Exception.Message)"
        }
    }
    Start-Sleep -Milliseconds 900

    $item = $null
    foreach ($t in @([System.Windows.Automation.ControlType]::ListItem,
                     [System.Windows.Automation.ControlType]::Button,
                     [System.Windows.Automation.ControlType]::Text)) {
        $cond = New-Object System.Windows.Automation.AndCondition(
            (New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $proc.Id)),
            (New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $t)),
            (New-Object System.Windows.Automation.PropertyCondition($AE::NameProperty, $PickLanguage)))
        $item = $AE::RootElement.FindFirst($TS::Descendants, $cond)  # popup may be a separate scope
        if (-not $item) { $item = $dlg.FindFirst($TS::Descendants, $cond) }
        if ($item) { break }
    }
    if (-not $item) { Fail "combo item '$PickLanguage' not found after expanding" }

    $picked = $false
    try {
        $item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
        $picked = $true
    } catch {
        try {
            $item.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
            $picked = $true
        } catch { }
    }
    if (-not $picked) { Fail "combo item '$PickLanguage' exposes no selectable pattern" }
    Start-Sleep -Milliseconds 700
    Write-Output ("LANGUAGE_PICKED=" + $PickLanguage)
    Write-Output ("LANGUAGE_COMBO_AFTER=" + (Get-FirstCombo $dlg).Current.Name)
}

if ($ClickApply) {
    $apply = Find-ButtonByRegex $dlg '^(Apply|Anwenden|Použít|Použiť|Áp dụng|Appliquer|Applica|Aplicar)$'
    if (-not $apply) { Fail "Apply button not found in the dialog" }
    $apply.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Start-Sleep -Milliseconds 1200
    $proc.Refresh()
    if ($proc.HasExited) { Fail "process exited after Apply" }
    if (Find-SettingsDialog) {
        Write-Output "DIALOG_AFTER_APPLY=still-open"
    } else {
        Write-Output "DIALOG_AFTER_APPLY=closed"
    }
    Write-Output "SETTINGS_ALIVE=after-apply"
}

if ($Dump) {
    Write-Output "---- UIA TREE ----"
    $all = $win.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    $n = 0
    foreach ($e in $all) {
        $name = $e.Current.Name
        $type = $e.Current.ControlType.ProgrammaticName
        $id   = $e.Current.AutomationId
        if ([string]::IsNullOrWhiteSpace($name) -and [string]::IsNullOrWhiteSpace($id)) { continue }
        Write-Output ("{0} | name='{1}' | id='{2}'" -f $type, $name, $id)
        $n++
        if ($n -ge 200) { Write-Output "... (capped)"; break }
    }
    Write-Output "---- END TREE ----"
}

if (-not $KeepOpen) {
    $wp = $win.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
    $wp.Close()
    Start-Sleep -Milliseconds 1500
    $proc.Refresh()
    Write-Output ("APP_CLOSED=" + $(if ($proc.HasExited) { "yes" } else { "no" }))
}

Write-Output "PROBE=PASS"
exit 0
