param([string]$Action = "Test")

Write-Host "=================================================="
Write-Host " CNC Wireless Transfer System - Validation Runner"
Write-Host "=================================================="

if ($Action -eq "Test" -or $Action -eq "All") {
    Write-Host "`n[1/3] Running Python Software Validation Suite..."
    python tools/test_transfer.py
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Software validation tests FAILED."
        exit $LASTEXITCODE
    } else {
        Write-Host "Software validation tests PASSED successfully."
    }
}

if ($Action -eq "Build" -or $Action -eq "All") {
    Write-Host "`n[2/3] Checking ESP-IDF Build Environment..."
    $env:IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.5.1"
    $env:IDF_PYTHON_ENV_PATH = "C:\Espressif\python_env\idf5.5_py3.11_env"
    
    if (-not (Test-Path $env:IDF_PATH)) {
        Write-Host "ESP-IDF path not found at $env:IDF_PATH. Skipping build."
        exit 0
    }

    Write-Host "Building Receiver..."
    Set-Location receiver
    & "$env:IDF_PATH\export.ps1"
    idf.py build
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Receiver build FAILED."
        Set-Location ..
        exit $LASTEXITCODE
    }
    Set-Location ..

    Write-Host "Building Transmitter..."
    Set-Location transmitter
    & "$env:IDF_PATH\export.ps1"
    idf.py build
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Transmitter build FAILED."
        Set-Location ..
        exit $LASTEXITCODE
    }
    Set-Location ..
    Write-Host "Both Receiver and Transmitter built successfully."
}

if ($Action -eq "DetectHardware" -or $Action -eq "All") {
    Write-Host "`n[3/3] Detecting ESP-IDF Hardware / COM Ports..."
    $ports = Get-PnpDevice -PresentOnly | Where-Object { $_.Name -like "*COM*" -or $_.Name -like "*USB Serial*" -or $_.Name -like "*JTAG*" }
    if ($ports) {
        Write-Host "Connected hardware ports detected:"
        $ports | Select-Object Status, Class, FriendlyName, InstanceId | Format-Table -AutoSize
    } else {
        Write-Host "No physical ESP32-S3 hardware ports detected. Hardware-in-the-loop validation is marked as NOT AVAILABLE / BLOCKED."
    }
}

Write-Host "`n=================================================="
Write-Host " Validation Script Completed Successfully"
Write-Host "=================================================="
