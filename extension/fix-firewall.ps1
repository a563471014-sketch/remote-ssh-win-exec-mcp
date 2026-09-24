# WinExec MCP - remove Windows Firewall BLOCK rules for win-exec and ensure ALLOW rules.
# Must run AS ADMIN (the extension command "WinExec MCP: 修复防火墙权限（管理员）"
# or fix-firewall.cmd self-elevates).
# Log: %TEMP%\win-exec-mcp-fix.log
$ErrorActionPreference = 'Continue'
$log = Join-Path $env:TEMP 'win-exec-mcp-fix.log'
function L([string]$m) { $m | Tee-Object -FilePath $log -Append }

L ''
L ('=== win-exec-mcp fix-firewall ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + ' ===')
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
L ('admin = ' + $admin)
if (-not $admin) {
  L 'ERROR: not elevated - run fix-firewall.cmd (it will ask for UAC).'
  Write-Host ''
  Read-Host 'Press Enter to close'
  exit 1
}

# collect every win-exec exe path: stable copy + all installed extension folders
$paths = @()
$paths += (Join-Path $env:LOCALAPPDATA 'win-exec-mcp\bin\win-exec-mcp.exe')
$roots = @(
  (Join-Path $env:USERPROFILE '.vscode\extensions'),
  (Join-Path $env:USERPROFILE '.vscode-insiders\extensions'),
  (Join-Path $env:USERPROFILE '.trae\extensions'),
  (Join-Path $env:USERPROFILE '.trae-cn\extensions')
)
foreach ($root in $roots) {
  Get-ChildItem $root -Directory -Filter '*win-exec-mcp*' -ErrorAction SilentlyContinue |
    ForEach-Object { $paths += (Join-Path $_.FullName 'bin\win-exec-mcp.exe') }
}
$curs = @($paths | Sort-Object -Unique | Where-Object { Test-Path $_ })
L ('target exe paths: ' + $curs.Count)
foreach ($c in $curs) { L ('  ' + $c) }

# 1) remove ALL inbound BLOCK rules for win-exec
try {
  $rules = @(Get-NetFirewallRule -ErrorAction Stop | Where-Object { $_.DisplayName -like '*win-exec*' })
} catch {
  L ('Get-NetFirewallRule failed: ' + $_.Exception.Message)
  $rules = @()
}
L ('win-exec rules found: ' + $rules.Count)
$blocks = @($rules | Where-Object { $_.Action -eq 'Block' })
L ('BLOCK rules to remove: ' + $blocks.Count)
foreach ($r in $blocks) {
  try {
    Remove-NetFirewallRule -Name $r.Name -ErrorAction Stop
    L ('removed BLOCK: ' + $r.DisplayName + ' [' + $r.Name + ']')
  } catch {
    L ('FAILED to remove ' + $r.Name + ' : ' + $_.Exception.Message)
  }
}

# 2) refresh explicit ALLOW rules for current paths
foreach ($r in @(Get-NetFirewallRule -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -like 'win-exec-mcp.exe allow*' })) {
  try { Remove-NetFirewallRule -Name $r.Name -ErrorAction Stop; L ('removed old allow: ' + $r.DisplayName) }
  catch { L ('failed removing old allow ' + $r.Name + ': ' + $_.Exception.Message) }
}
foreach ($cur in $curs) {
  try {
    New-NetFirewallRule -DisplayName 'win-exec-mcp.exe allow' -Direction Inbound -Action Allow -Program $cur -Profile Any -Enabled True -ErrorAction Stop | Out-Null
    L ('allow rule ensured: ' + $cur)
  } catch {
    L ('allow rule failed for ' + $cur + ': ' + $_.Exception.Message)
  }
}

# 3) stop running helper instances (the extension respawns them after window reload)
taskkill.exe /IM win-exec-mcp.exe /F 2>$null | Out-Null
L 'stray win-exec-mcp.exe processes killed'

# 4) verify
try {
  $after = @(Get-NetFirewallRule -ErrorAction Stop | Where-Object { $_.DisplayName -like '*win-exec*' -and $_.Action -eq 'Block' })
  L ('BLOCK rules remaining: ' + $after.Count)
  if ($after.Count -eq 0) { L 'RESULT: SUCCESS - no win-exec block rules remain.' }
  else { L 'RESULT: STILL BLOCKED - ' + $after.Count + ' block rules remain!'; $after | ForEach-Object { L ('  still blocked: ' + $_.DisplayName) } }
} catch {
  L ('verify failed: ' + $_.Exception.Message)
}
L 'Next: reload the VS Code window (Developer: Reload Window).'
L ('log file: ' + $log)

Write-Host ''
Read-Host 'Press Enter to close'
