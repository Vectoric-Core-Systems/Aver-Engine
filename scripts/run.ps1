# Build then run the sandbox. Extra args go to Sandbox.exe (e.g. --frames 5 --headless).
& "$PSScriptRoot\build.bat"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$exe = Join-Path $PSScriptRoot "..\build\bin\Sandbox.exe"
& $exe @args
exit $LASTEXITCODE
