param([string]$Version = "")
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# 版本的单一事实来源：extension/package.json（build.cmd 仍可用 -Version 覆盖）。
# manifest 里的 Identity Version 必须与它同步——本机安装读 package.json，Marketplace 发布读 manifest。
if (-not $Version) {
    $Version = (Get-Content -LiteralPath (Join-Path $root 'extension\package.json') -Raw | ConvertFrom-Json).version
}

function Add-ZipEntry($zip, $filePath, $entryName) {
    $entry = $zip.CreateEntry($entryName, [System.IO.Compression.CompressionLevel]::Optimal)
    $fs = [IO.File]::OpenRead($filePath)
    try {
        $es = $entry.Open()
        try { $fs.CopyTo($es) } finally { $es.Dispose() }
    } finally { $fs.Dispose() }
}

# 1. Stage files (manifest includes README/icon Assets required by the Marketplace gallery)
$buildDir = Join-Path $root 'vsix-build'
if (Test-Path $buildDir) { Remove-Item -Recurse -Force $buildDir }
New-Item -ItemType Directory $buildDir | Out-Null
Copy-Item -Recurse -Force (Join-Path $root 'extension') (Join-Path $buildDir 'extension')
Copy-Item -LiteralPath (Join-Path $root 'vsix\[Content_Types].xml') -Destination $buildDir -Force
Copy-Item (Join-Path $root 'vsix\extension.vsixmanifest') $buildDir -Force

# 把版本写进 manifest：原样复制会让它停在仓库里的旧值，与 package.json 错位
$mf = Join-Path $buildDir 'extension.vsixmanifest'
# 排除备份/临时文件：这里是手工建 zip，不走 .vscodeignore，不排除会把 *.bak* 一并打进包
Get-ChildItem -Recurse -File (Join-Path $buildDir 'extension') | Where-Object { $_.Name -match '\.bak($|[-.])' } | Remove-Item -Force

$xml = [IO.File]::ReadAllText($mf)
$xml = [regex]::Replace($xml, '(<Identity\b[^>]*\bVersion=")[^"]*(")', '${1}' + $Version + '${2}')
[IO.File]::WriteAllText($mf, $xml, (New-Object Text.UTF8Encoding($false)))

# 2. Build zip with [Content_Types].xml as the FIRST entry (required by Marketplace OPC validator)
$vsix = Join-Path $root "dist\win-exec-mcp-$Version.vsix"
New-Item -ItemType Directory -Force (Join-Path $root 'dist') | Out-Null
if (Test-Path $vsix) { Remove-Item $vsix -Force }

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($vsix, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    Add-ZipEntry $zip (Join-Path $buildDir '[Content_Types].xml') '[Content_Types].xml'
    Add-ZipEntry $zip (Join-Path $buildDir 'extension.vsixmanifest') 'extension.vsixmanifest'
    Get-ChildItem -Recurse -File (Join-Path $buildDir 'extension') | ForEach-Object {
        $rel = $_.FullName.Substring($buildDir.Length + 1).Replace('\', '/')
        Add-ZipEntry $zip $_.FullName $rel
    }
} finally { $zip.Dispose() }

Write-Host "OK: $vsix"
