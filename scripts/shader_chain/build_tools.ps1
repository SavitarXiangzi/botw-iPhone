<#
    build_tools.ps1 -- build the two programs the offline shader chain needs.

    Both are built from source on this machine, at the versions the reference
    chain pins, into build/shader-chain/tools/:

      dmsh_xlate    the Maxwell SASS -> SPIR-V translator (a .NET 10 program
                    from nxnative, which vendors Ryujinx's shader library)
      spirv-cross   SPIR-V -> MSL, upstream commit f11ba9f
                    (tag vulkan-sdk-1.4.363.0)

    Requirements: .NET 10 SDK, CMake + Visual Studio 2022 (Windows) or
    Xcode command line tools (macOS).  Nothing is written outside
    build/shader-chain/ except NuGet's own cache.

    Usage: powershell -File scripts/shader_chain/build_tools.ps1
           -Src <nxnative checkout>   (default: ..\..\..\nxnative-git next to
                                       this repository)
#>
[CmdletBinding()]
param(
    [string]$Src = "",
    [string]$Dotnet = "",
    [string]$SpirvCommit = "f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$root = Join-Path $repo "build\shader-chain"
$tools = Join-Path $root "tools"
New-Item -ItemType Directory -Force -Path $tools | Out-Null

if (-not $Src) {
    $Src = Join-Path (Split-Path -Parent $repo) "nxnative-git"
}
if (-not (Test-Path (Join-Path $Src "dm\gfx\tools\dmsh_xlate\dmsh_xlate.csproj"))) {
    throw "no nxnative checkout at '$Src' (pass -Src <path to nxnative>)"
}
if (-not $Dotnet) {
    $Dotnet = (Get-Command dotnet -ErrorAction SilentlyContinue).Source
    if (-not $Dotnet -and $env:DOTNET_ROOT) { $Dotnet = Join-Path $env:DOTNET_ROOT "dotnet.exe" }
}
if (-not $Dotnet) { throw ".NET SDK not found: install one or pass -Dotnet <dotnet.exe>" }

Write-Host "[1/2] dmsh_xlate (needs a .NET 10 SDK)"
& $Dotnet publish (Join-Path $Src "dm\gfx\tools\dmsh_xlate\dmsh_xlate.csproj") `
    -c Release -o (Join-Path $tools "xlate") | Out-Host
if ($LASTEXITCODE) { throw "dotnet publish failed ($LASTEXITCODE)" }

Write-Host "[2/2] spirv-cross $SpirvCommit"
$sc = Join-Path $root "spirv-cross"
if (-not (Test-Path (Join-Path $sc "src\.git"))) {
    git clone --quiet https://github.com/KhronosGroup/SPIRV-Cross.git (Join-Path $sc "src")
}
Push-Location (Join-Path $sc "src")
try {
    git checkout --quiet $SpirvCommit
    if ($LASTEXITCODE) { throw "cannot check out $SpirvCommit" }
} finally { Pop-Location }
if (-not (Test-Path (Join-Path $sc "build\CMakeCache.txt"))) {
    cmake -S (Join-Path $sc "src") -B (Join-Path $sc "build") -DCMAKE_BUILD_TYPE=Release `
        -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_SKIP_INSTALL=ON | Out-Host
    if ($LASTEXITCODE) { throw "cmake configure failed ($LASTEXITCODE)" }
}
cmake --build (Join-Path $sc "build") --config Release --target spirv-cross | Out-Host
if ($LASTEXITCODE) { throw "spirv-cross build failed ($LASTEXITCODE)" }

$exe = if ($IsWindows -or $env:OS -eq "Windows_NT") { ".exe" } else { "" }
$cross = Get-ChildItem -Path (Join-Path $sc "build") -Filter "spirv-cross$exe" -Recurse |
    Select-Object -First 1
Copy-Item $cross.FullName (Join-Path $tools "spirv-cross$exe") -Force
Write-Host "built:"
Write-Host "  " (Join-Path $tools "xlate\dmsh_xlate$exe")
Write-Host "  " (Join-Path $tools "spirv-cross$exe")
