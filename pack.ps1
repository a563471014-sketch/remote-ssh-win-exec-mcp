param([string]$Version = "0.3.1")
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

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
