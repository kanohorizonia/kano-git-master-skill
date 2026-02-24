# Build kano-git CLI (Windows PowerShell)
# Usage: .\build.ps1 [-Config debug|release] [-Clean]
param(
    [ValidateSet("debug", "release")]
    [string]$Config = "release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

Push-Location $PSScriptRoot
try {
    $BuildDir = "build\$Config"

    if ($Clean -and (Test-Path $BuildDir)) {
        Write-Host "Cleaning $BuildDir..."
        Remove-Item -Recurse -Force $BuildDir
    }

    $BuildType = (Get-Culture).TextInfo.ToTitleCase($Config)

    Write-Host "Configuring ($Config)..."
    if ($env:VCPKG_ROOT) {
        cmake --preset $Config
    }
    else {
        # Fallback if VCPKG_ROOT is not set but vcpkg is installed globally or in common paths
        $vcpkgPaths = @("C:\src\vcpkg", "D:\vcpkg", "C:\vcpkg")
        $toolchain = $null
        foreach ($p in $vcpkgPaths) {
            if (Test-Path "$p\scripts\buildsystems\vcpkg.cmake") {
                $toolchain = "$p\scripts\buildsystems\vcpkg.cmake"
                break
            }
        }
        if ($toolchain) {
            cmake -B $BuildDir -DCMAKE_BUILD_TYPE=$BuildType -DCMAKE_TOOLCHAIN_FILE=$toolchain
        }
        else {
            cmake -B $BuildDir -DCMAKE_BUILD_TYPE=$BuildType
        }
    }

    Write-Host "Building..."
    cmake --build $BuildDir --config $BuildType

    Write-Host ""
    Write-Host "Build complete: $BuildDir\kano-git.exe"
    Write-Host "Run: .\$BuildDir\kano-git.exe version"
}
finally {
    Pop-Location
}
