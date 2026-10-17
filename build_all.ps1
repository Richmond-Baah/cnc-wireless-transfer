param(
    [bool]$FullClean = $false
)

$ErrorActionPreference = "Stop"

Write-Host "=== Setting up ESP-IDF Environment ==="
$env:IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.5.1"
$env:IDF_TOOLS_PATH = "C:\Espressif"
$env:PATH = "C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;C:\Espressif\tools\bin;$env:PATH"

& "$env:IDF_PATH\export.ps1"

Write-Host "`n=== ESP-IDF Environment Verified ==="
idf.py --version

$origDir = Get-Location

# Build Receiver
Write-Host "`n=========================================="
Write-Host " Building Receiver (cnc_receiver)"
Write-Host "=========================================="
Set-Location "$origDir\receiver"
if ($FullClean -and (Test-Path "build")) {
    Write-Host "Cleaning build directory..."
    Remove-Item -Path "build" -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "Running idf.py build..."
idf.py build
if ($LASTEXITCODE -ne 0) {
    Set-Location $origDir
    throw "Receiver build failed with exit code $LASTEXITCODE"
}

# Build Transmitter
Write-Host "`n=========================================="
Write-Host " Building Transmitter (cnc_transmitter)"
Write-Host "=========================================="
Set-Location "$origDir\transmitter"
if ($FullClean -and (Test-Path "build")) {
    Write-Host "Cleaning build directory..."
    Remove-Item -Path "build" -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "Running idf.py build..."
idf.py build
if ($LASTEXITCODE -ne 0) {
    Set-Location $origDir
    throw "Transmitter build failed with exit code $LASTEXITCODE"
}

Set-Location $origDir
Write-Host "`n=========================================="
Write-Host " BOTH TARGETS COMPILED CLEANLY AND SUCCESSFULLY! "
Write-Host "=========================================="
