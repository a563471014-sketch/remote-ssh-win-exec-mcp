Add-Type -AssemblyName System.IO.Compression.FileSystem
# pick the newest .vsix under dist (no hard-coded absolute path)
$vsix = Get-ChildItem (Join-Path $PSScriptRoot 'dist\*.vsix') | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $vsix) { throw 'no .vsix found under dist' }
Write-Host "verifying: $($vsix.FullName)"
$z = [System.IO.Compression.ZipFile]::OpenRead($vsix.FullName)
Write-Host '--- entries ---'
$z.Entries | ForEach-Object { Write-Host $_.FullName }
Write-Host '--- [Content_Types].xml ---'
$ct = $z.GetEntry('[Content_Types].xml')
Write-Host (New-Object IO.StreamReader($ct.Open())).ReadToEnd()
$z.Dispose()
