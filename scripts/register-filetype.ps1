# Associate .ocproject with the editor, so opening a project manifest opens it in Aver Engine.
#
#   ./scripts/register-filetype.ps1                        # per-user (HKCU), no admin
#   ./scripts/register-filetype.ps1 -AllUsers              # machine-wide (HKLM), NEEDS AN ADMIN SHELL
#   ./scripts/register-filetype.ps1 -Release               # point at build-release\bin
#   ./scripts/register-filetype.ps1 -Exe <path>            # ...or an explicit binary
#   ./scripts/register-filetype.ps1 -Unregister            # undo the per-user registration
#   ./scripts/register-filetype.ps1 -Unregister -AllUsers  # undo the machine-wide one
#
# TWO SCOPES, AND THE DIFFERENCE MATTERS FOR WHETHER THE PICKER APPEARS.
#
# The per-user scope writes a ProgID and an Open-with entry. That is enough for the shell to RESOLVE
# the file type -- AssocQueryString returns the right command -- but Windows still asks the user
# which app to use the first time, because a default is a user's decision and nothing a program
# writes can pre-empt it.
#
# -AllUsers additionally registers the engine as a real APPLICATION through Default Programs:
# a Capabilities key describing what it handles, and an entry in RegisteredApplications pointing at
# it. That is the documented mechanism by which an installed program presents itself as a candidate
# handler, and it is what makes Windows treat the engine as the owner of .ocproject rather than as
# something a user once browsed to.
#
# WHAT NEITHER SCOPE DOES, deliberately: write
# HKCU\...\Explorer\FileExts\.ocproject\UserChoice. That key is protected by a per-user hash
# specifically so that programs CANNOT seize file types, and forging it is circumventing an
# anti-hijacking control rather than configuring a machine. If Windows still offers the picker,
# choosing Aver Engine once with "always use this app" is the supported way to settle it -- and the
# entry is now named properly, so it is findable.
#
# NO ENGINE CHANGE WAS NEEDED for any of this. Everything the editor loads -- its icon sheets, the C#
# engine root, the Tools menu's module list -- resolves from executableDir() and by walking up from it
# for cmake/AvModule.cmake, never from the working directory. Explorer launches with the working
# directory set wherever it likes, so a build that read CWD would come up with no icons and an empty
# Content Browser; this one was verified by launching from an unrelated folder with a relative path.
[CmdletBinding()]
param(
    [switch] $Release,
    [string] $Exe,
    [switch] $Unregister,
    [switch] $AllUsers
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$appName  = 'Aver Engine'

# EVERY TYPE THE EDITOR CLAIMS, in one table, so register, unregister and the Default Programs
# declaration cannot drift apart -- adding a type is this table and nothing else.
#
# ONLY TYPES THE EDITOR CAN ACTUALLY OPEN belong here. A registration is a promise that
# double-clicking does something, and the shell cannot express "opens, but ignores it". .ocmap was
# the second half of a bug where the association was missing AND the editor would have dropped the
# file if it had one: a bare path fell through to the beam loader, which refused it and left an
# empty editor. Both halves were fixed together, so do not add an extension here before
# createApplication's positional-argument handling in SandboxApp.cpp recognises it.
#
# .ocmap and .ocworld are one format under two names -- what a project calls its levels, and what
# the same content is called outside one -- so they take distinct ProgIDs, because Windows keys the
# icon and the open verb off the ProgID, but share a human-readable type name.
$types = @(
    @{ Ext = '.ocproject'; ProgId = 'AverEngine.Project'; TypeName = 'Aver Engine Project' },
    @{ Ext = '.ocmap';     ProgId = 'AverEngine.Map';     TypeName = 'Aver Engine Level'   },
    @{ Ext = '.ocworld';   ProgId = 'AverEngine.World';   TypeName = 'Aver Engine Level'   }
)

# Every key this script owns, named once, so register and unregister cannot drift apart.
$hive     = if ($AllUsers) { 'HKLM:' } else { 'HKCU:' }
function Get-ExtKey  ($e) { "$hive\Software\Classes$([char]92)$e" }
function Get-ProgKey ($i) { "$hive\Software\Classes\$i" }
$capKey   = "$hive\Software\Aver\Engine\Capabilities"
$regAppsK = "$hive\Software\RegisteredApplications"

# CAN WE WRITE HKLM? Asked by trying, not by asking who we are.
#
# The obvious test -- WindowsPrincipal.IsInRole(Administrator) -- is a proxy, and on this machine it
# is a proxy that LIES: it returns true under a filtered UAC token while every HKLM write is denied.
# An account in the Administrators group running unelevated is the normal case, not an exotic one, so
# the check has to be for the capability rather than for the membership. Creating and removing a
# throwaway key answers the actual question and leaves nothing behind.
# The probe lives UNDER HKLM\Software\Aver, which -Unregister -AllUsers already removes, so a probe
# that cannot clean itself up is swept by the uninstall path instead of becoming litter with its own
# special case. The first version wrote HKLM\Software\AverEngine.RegistrationProbe -- outside
# everything the script owns -- and on a machine where the create half-succeeds and the delete is
# denied, that is a stray key nothing will ever collect.
function Test-CanWriteHklm {
    $probe = 'HKLM:\Software\Aver\RegistrationProbe'
    $ok = $false
    try {
        New-Item -Path $probe -Force -ErrorAction Stop | Out-Null
        $ok = $true
    } catch { $ok = $false }
    # Attempted whether or not the create reported success: on a filtered token the two do not agree,
    # which is the whole reason this function exists.
    Remove-Item -LiteralPath $probe -Recurse -Force -ErrorAction SilentlyContinue
    return $ok
}

# Refused UP FRONT rather than part way through. A machine-wide registration that fails on its third
# key leaves the extension pointing at a ProgID that does not exist yet, which is a worse state than
# either doing it or not.
if ($AllUsers -and -not (Test-CanWriteHklm)) {
    Write-Error @"
-AllUsers writes to HKLM and this shell cannot. Nothing has been changed.

Note that being in the Administrators group is not enough: an unelevated shell holds a filtered
token, and this was checked by attempting a write rather than by asking for group membership.

Open an ADMINISTRATOR PowerShell and run:
  powershell -ExecutionPolicy Bypass -File "$PSCommandPath" -AllUsers
"@
}

Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;
public static class AverShellNotify {
  [DllImport("shell32.dll", CharSet=CharSet.Unicode)]
  public static extern void SHChangeNotify(int eventId, uint flags, IntPtr a, IntPtr b);
}
'@ -ErrorAction SilentlyContinue

# Explorer serves associations from a cached table and does not re-read the registry because someone
# wrote to it. Without this the registration is correct on disk and invisible in practice, which is
# the most confusing possible failure: every key checks out and nothing happens.
function Notify-Shell { [AverShellNotify]::SHChangeNotify(0x08000000, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero) }

if ($Unregister) {
    $appLeafKey = "$hive\Software\Classes\Applications\Sandbox.exe"
    $doomed = @()
    foreach ($t in $types) { $doomed += (Get-ProgKey $t.ProgId); $doomed += (Get-ExtKey $t.Ext) }
    $doomed += @($appLeafKey, "$hive\Software\Aver")
    foreach ($k in $doomed) {
        if (Test-Path $k) { Remove-Item -LiteralPath $k -Recurse -Force; "removed $k" }
        else              { "absent  $k" }
    }
    if (Test-Path $regAppsK) {
        if ((Get-Item $regAppsK).GetValueNames() -contains $appName) {
            Remove-ItemProperty -LiteralPath $regAppsK -Name $appName -Force
            "removed RegisteredApplications\$appName"
        }
    }
    Notify-Shell
    "`n$(($types | ForEach-Object { $_.Ext }) -join ', ') no longer associated in $hive. Explorer may need a sign-out to drop the old icon."
    exit 0
}

if (-not $Exe) {
    $dir = if ($Release) { 'build-release\bin' } else { 'build\bin' }
    $Exe = Join-Path $root "$dir\Sandbox.exe"
}
if (-not (Test-Path $Exe)) { Write-Error "Sandbox.exe not found at $Exe - build first, or pass -Exe <path>." }
$Exe = (Resolve-Path $Exe).Path

# The icon is optional: a missing one costs the file type its picture and nothing else.
$icon    = Join-Path $root 'branding\icon.ico'
$iconRef = if (Test-Path $icon) { "$icon,0" } else { "$Exe,0" }

# "%1" QUOTED, and it is not decoration: the default projects root is Documents\Aver Projects, which
# contains a space, and an unquoted %1 hands the editor two arguments that are each half a path. It
# would work everywhere a tester happened to look and fail for everyone at the default location.
$command = "`"$Exe`" `"%1`""

# ---- the file types ----
foreach ($t in $types) {
    $extKey  = Get-ExtKey  $t.Ext
    $progKey = Get-ProgKey $t.ProgId

    New-Item -Path $extKey  -Force | Out-Null
    New-Item -Path $progKey -Force | Out-Null
    Set-ItemProperty -Path $extKey  -Name '(default)' -Value $t.ProgId
    Set-ItemProperty -Path $progKey -Name '(default)' -Value $t.TypeName
    Set-ItemProperty -Path $progKey -Name 'FriendlyTypeName' -Value $t.TypeName

    New-Item -Path "$progKey\DefaultIcon" -Force | Out-Null
    Set-ItemProperty -Path "$progKey\DefaultIcon" -Name '(default)' -Value $iconRef
    New-Item -Path "$progKey\shell\open\command" -Force | Out-Null
    Set-ItemProperty -Path "$progKey\shell\open\command" -Name '(default)' -Value $command

    # Advertise the ProgID on the extension: this is what puts the engine in the Open-with list.
    New-Item -Path "$extKey\OpenWithProgids" -Force | Out-Null
    New-ItemProperty -Path "$extKey\OpenWithProgids" -Name $t.ProgId -PropertyType String -Value '' -Force | Out-Null
}

# ---- the application, which is a different thing from the file type ----
# A ProgID's description names the FILE TYPE; the name of the APPLICATION comes from the exe's
# version resource and from FriendlyAppName here. Registering only the first is why the picker
# offered "Sandbox.exe" -- the binary had no VERSIONINFO at all, so the shell had nothing else to
# call it. It carries one now (sandbox/Sandbox.rc); this covers a binary built before that landed.
$appKey = "$hive\Software\Classes\Applications\$(Split-Path -Leaf $Exe)"
New-Item -Path "$appKey\shell\open\command" -Force | Out-Null
Set-ItemProperty -Path "$appKey\shell\open\command" -Name '(default)' -Value $command
Set-ItemProperty -Path $appKey -Name 'FriendlyAppName' -Value $appName
New-Item -Path "$appKey\SupportedTypes" -Force | Out-Null
foreach ($t in $types) {
    New-ItemProperty -Path "$appKey\SupportedTypes" -Name $t.Ext -PropertyType String -Value '' -Force | Out-Null
}

# ---- Default Programs, machine-wide only ----
# Capabilities + RegisteredApplications is the documented way a program declares "I handle this file
# type" to Windows. It is what makes the engine appear in Settings > Default apps as an application
# rather than as a stray executable, and it is the difference between a handler Windows knows about
# and one a user once browsed to. HKCU has no equivalent that suppresses the picker.
if ($AllUsers) {
    New-Item -Path $capKey -Force | Out-Null
    Set-ItemProperty -Path $capKey -Name 'ApplicationName'        -Value $appName
    Set-ItemProperty -Path $capKey -Name 'ApplicationDescription' -Value 'Aver Engine editor and runtime'
    New-Item -Path "$capKey\FileAssociations" -Force | Out-Null
    foreach ($t in $types) {
        New-ItemProperty -Path "$capKey\FileAssociations" -Name $t.Ext -PropertyType String -Value $t.ProgId -Force | Out-Null
    }

    New-Item -Path $regAppsK -Force | Out-Null
    New-ItemProperty -Path $regAppsK -Name $appName -PropertyType String `
                     -Value 'Software\Aver\Engine\Capabilities' -Force | Out-Null
}

Notify-Shell

"registered $(($types | ForEach-Object { $_.Ext }) -join ', ')  [$(if ($AllUsers) { 'machine-wide, HKLM' } else { 'per-user, HKCU' })]"
"  command : $command"
"  icon    : $iconRef"
"  app     : $appName"
if ($AllUsers) { "  declared through Default Programs (Capabilities + RegisteredApplications)" }
"  shell notified (SHCNE_ASSOCCHANGED)"
""
"Rebuilding does not need a re-run; the path is to the binary, not to a copy of it."
"Reverse with -Unregister$(if ($AllUsers) { ' -AllUsers' })."
""
"If Windows still asks which app to use, choose $appName once and tick 'always use this app'."
"That last step is the user's to take by design: the key that records it is hash-protected"
"precisely so a program cannot claim a file type on someone's behalf."
