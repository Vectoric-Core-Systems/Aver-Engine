# Associate .ocproject with the editor, so double-clicking a project manifest opens it.
#
#   ./scripts/register-filetype.ps1              # register, pointing at build\bin\Sandbox.exe
#   ./scripts/register-filetype.ps1 -Release     # ...at build-release\bin instead
#   ./scripts/register-filetype.ps1 -Exe <path>  # ...at an explicit binary
#   ./scripts/register-filetype.ps1 -Unregister  # put it back exactly as it was
#
# PER-USER, under HKCU\Software\Classes. Nothing here touches HKLM, needs administrator, or changes a
# setting outside this file type: the whole footprint is two keys named below, and -Unregister
# deletes both. That matters because a file association is a change to someone's machine rather than
# to this repository, and the polite version of that is one you can read first and reverse after.
#
# NO ENGINE CHANGE WAS NEEDED for this. Everything the editor loads -- its icon sheets, the C#
# engine root, the Tools menu's module list -- is resolved from executableDir() and by walking up
# from it for cmake/AvModule.cmake, never from the working directory. Explorer launches with the
# working directory set wherever it likes, so a build that read CWD would come up with no icons and
# an empty Content Browser; this one was verified by launching from an unrelated folder with a
# relative path and watching it resolve all four.
[CmdletBinding()]
param(
    [switch] $Release,
    [string] $Exe,
    [switch] $Unregister
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# The two keys, named once so register and unregister cannot drift apart.
$progId  = 'AverEngine.Project'
$extKey  = 'HKCU:\Software\Classes\.ocproject'
$progKey = "HKCU:\Software\Classes\$progId"

if ($Unregister) {
    foreach ($k in @($progKey, $extKey)) {
        if (Test-Path $k) { Remove-Item $k -Recurse -Force; "removed $k" }
        else              { "absent  $k" }
    }
    "`n.ocproject is no longer associated. Explorer may need a sign-out to stop showing the old icon."
    exit 0
}

if (-not $Exe) {
    $dir = if ($Release) { 'build-release\bin' } else { 'build\bin' }
    $Exe = Join-Path $root "$dir\Sandbox.exe"
}
if (-not (Test-Path $Exe)) {
    Write-Error "Sandbox.exe not found at $Exe - build first, or pass -Exe <path>."
}
$Exe = (Resolve-Path $Exe).Path

# The icon is optional: a missing one costs the file type its picture and nothing else.
$icon = Join-Path $root 'branding\icon.ico'
$iconRef = if (Test-Path $icon) { "$icon,0" } else { "$Exe,0" }

New-Item -Path $extKey  -Force | Out-Null
New-Item -Path $progKey -Force | Out-Null
Set-ItemProperty -Path $extKey  -Name '(default)' -Value $progId
Set-ItemProperty -Path $progKey -Name '(default)' -Value 'Aver Engine Project'

New-Item -Path "$progKey\DefaultIcon" -Force | Out-Null
Set-ItemProperty -Path "$progKey\DefaultIcon" -Name '(default)' -Value $iconRef

# "%1" QUOTED, and it is not decoration: the default projects root is under Documents\Aver Projects,
# which contains a space, and an unquoted %1 hands the editor two arguments that are each half a
# path. It would work everywhere the tester happened to look and fail for everyone else.
New-Item -Path "$progKey\shell\open\command" -Force | Out-Null
Set-ItemProperty -Path "$progKey\shell\open\command" -Name '(default)' -Value "`"$Exe`" `"%1`""

# Advertise the ProgID ON the extension, which is what puts "Aver Engine" in the Open-with list.
# Without it the picker offers a browse button and nothing else, and the entry you want is not there
# to choose -- which reads as the association not having worked at all.
New-Item -Path "$extKey\OpenWithProgids" -Force | Out-Null
New-ItemProperty -Path "$extKey\OpenWithProgids" -Name $progId -PropertyType String -Value '' -Force | Out-Null

# TELL THE SHELL. Explorer serves associations from a cached table and does not re-read the registry
# because someone wrote to it; a registration without this notification is correct on disk and
# invisible in practice, which is the most confusing possible failure -- every key checks out and
# double-clicking still does nothing.
Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;
public static class AverShellNotify {
  [DllImport("shell32.dll", CharSet=CharSet.Unicode)]
  public static extern void SHChangeNotify(int eventId, uint flags, IntPtr a, IntPtr b);
}
'@ -ErrorAction SilentlyContinue
[AverShellNotify]::SHChangeNotify(0x08000000, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero)  # SHCNE_ASSOCCHANGED

"registered .ocproject -> $progId"
"  command : `"$Exe`" `"%1`""
"  icon    : $iconRef"
"  shell notified (SHCNE_ASSOCCHANGED)"
""
"Rebuilding does not need a re-run; the path is to the binary, not to a copy of it."
"Reverse with -Unregister."
""
"IF DOUBLE-CLICKING STILL SHOWS THE 'How do you want to open this file?' PICKER:"
"  choose Aver Engine and tick 'Always use this app'. That is not a defect in this script."
"  Windows 8 and later record the chosen default in"
"    HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.ocproject\UserChoice"
"  and protect it with a per-user hash specifically so that an installer CANNOT set it. Only the"
"  picker can, by design, to stop programs seizing file types. Everything this script writes is the"
"  half a program is allowed to write: the ProgID, its command, its icon, and the Open-with entry"
"  that makes 'Aver Engine' appear in that list in the first place."
