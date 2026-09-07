# Build then run the sandbox. Extra args go to Sandbox.exe (e.g. --frames 5 --headless).
& "$PSScriptRoot\build.bat"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$exe = Join-Path $PSScriptRoot "..\build\bin\Sandbox.exe"
# Start-Process -Wait, not `& $exe`: Sandbox.exe is /SUBSYSTEM:WINDOWS since 0.5.0, and Windows
# PowerShell does not wait for a GUI-subsystem process -- this script would return instantly with a
# BLANK $LASTEXITCODE while the editor was still starting, so `exit $LASTEXITCODE` reported nothing
# about the run. -NoNewWindow keeps the output in this console, which attachParentConsole() attaches
# the engine to. See gates.ps1's Invoke-Gate for the measured behaviour.
$p = Start-Process -FilePath $exe -ArgumentList $args -NoNewWindow -Wait -PassThru
exit $p.ExitCode
