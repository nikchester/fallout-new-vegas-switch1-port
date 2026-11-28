# Windows entry point: runs tools/owner_cycle.sh inside WSL (Docker Desktop with WSL integration enabled).
#   powershell -File tools\owner_cycle.ps1 -Exe C:\Users\me\Documents\My_Projects\fnv-private\FalloutNV.exe [-Jobs 2] [-Opt -O1] [-Extra "--no-update"]
param([string]$Exe = "", [int]$Jobs = 2, [string]$Opt = "-O1", [string]$Extra = "")
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$wrepo = (wsl wslpath -a ($repo -replace '\\','/')).Trim()
$args2 = "-j $Jobs --opt $Opt $Extra"
if ($Exe) { $wexe = (wsl wslpath -a ($Exe -replace '\\','/')).Trim(); $args2 = "--exe '$wexe' $args2" }
wsl bash -c "cd '$wrepo' && tools/owner_cycle.sh $args2"
exit $LASTEXITCODE
