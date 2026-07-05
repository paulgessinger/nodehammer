param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("configure", "configure-incremental", "build", "build-nodehammer")]
    [string]$Command
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe was not found. Install Visual Studio Build Tools with the C++ toolchain."
}

$env:PATH = "$(Split-Path -Parent $vswhere);$env:PATH"

$vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) {
    throw "No Visual Studio installation with the C++ toolchain was found."
}

$vsDevCmd = Join-Path $vsInstall "Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path -LiteralPath $vsDevCmd)) {
    throw "VsDevCmd.bat was not found under '$vsInstall'."
}

$conanPackageRoot = Join-Path $env:USERPROFILE ".conan2\p\b"
$sokolShdc = Get-ChildItem -Path $conanPackageRoot -Filter "sokol-shdc.exe" -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -like "*\p\bin\sokol-shdc.exe" } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if ($sokolShdc) {
    $env:PATH = "$($sokolShdc.Directory.FullName);$env:PATH"
}

switch ($Command) {
    "configure" {
        $cmakeArgs = @(
            "--preset", "conan-relwithdebinfo",
            "--fresh",
            "-GNinja",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DNODEHAMMER_BUILD_TESTS=ON",
            "-DNODEHAMMER_WITH_VIEWER=ON"
        )
    }
    "configure-incremental" {
        $cmakeArgs = @(
            "--preset", "conan-relwithdebinfo",
            "-GNinja",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DNODEHAMMER_BUILD_TESTS=ON",
            "-DNODEHAMMER_WITH_VIEWER=ON"
        )
    }
    "build" {
        $cmakeArgs = @("--build", "--preset", "conan-relwithdebinfo")
    }
    "build-nodehammer" {
        $cmakeArgs = @("--build", "--preset", "conan-relwithdebinfo", "--target", "nodehammer")
    }
}

$escapedVsDevCmd = $vsDevCmd.Replace('"', '""')
$escapedRepo = $repo.Replace('"', '""')
$conanBuild = Join-Path $repo "build\RelWithDebInfo\generators\conanbuild.bat"
$escapedConanBuild = $conanBuild.Replace('"', '""')
$escapedCmakeArgs = ($cmakeArgs | ForEach-Object { '"' + $_.Replace('"', '""') + '"' }) -join " "
$cmdLine = "`"$escapedVsDevCmd`" -arch=x64 -host_arch=x64 && cd /d `"$escapedRepo`""

if (Test-Path -LiteralPath $conanBuild) {
    $cmdLine += " && call `"$escapedConanBuild`""
}

$cmdLine += " && cmake $escapedCmakeArgs"

& cmd.exe /S /C $cmdLine
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
