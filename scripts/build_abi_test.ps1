<#
    build_abi_test.ps1 -- build and run the portable NVN/core tests on Windows.

    This is the closest thing to an "iOS build" available without a Mac.  It
    compiles every file that does not need Apple frameworks (the generated
    dispatch, the null backend, botw_core) with /W4 /WX and runs two tests:

      nvn_abi_test      registry <-> header <-> dispatch consistency
      core_smoke_test   the C ABI the Swift shell calls

    Usage:  powershell -File scripts/build_abi_test.ps1
    Exit code 0 means both tests passed.
#>
[CmdletBinding()]
param(
    [string]$VcVars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    if (-not (Test-Path $VcVars)) {
        Write-Error "vcvars64.bat not found at '$VcVars'. Pass -VcVars <path>."
    }
    New-Item -ItemType Directory -Force -Path build\nvn\abi, build\nvn\core | Out-Null

    $inc = "/I core\include /I ios\nvn\include"
    $lib = "ios\nvn\src\nvn_dispatch.cpp ios\nvn\src\nvn_null.cpp core\src\botw_core.cpp"
    $flags = "/nologo /std:c++20 /EHsc /W4 /WX"

    $jobs = @(
        @{ name = "nvn_abi_test";    src = "tests\nvn_abi_test.cpp";    obj = "build\nvn\abi\" },
        @{ name = "core_smoke_test"; src = "tests\core_smoke_test.cpp"; obj = "build\nvn\core\" }
    )

    foreach ($j in $jobs) {
        $cmd = "call `"$VcVars`" >nul && cl $flags $inc $($j.src) $lib /Fo:$($j.obj) /Fe:build\nvn\$($j.name).exe"
        cmd /c $cmd 2>&1 | ForEach-Object { if ("$_".Trim()) { Write-Host "  $_" } }
        if ($LASTEXITCODE -ne 0) { Write-Error "$($j.name): build failed"; exit 1 }
        # Native tools write to stderr; with ErrorActionPreference=Stop that
        # becomes a terminating error, so relax it just for this call.
        $prev = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        & ".\build\nvn\$($j.name).exe" 2>$null
        $rc = $LASTEXITCODE
        $ErrorActionPreference = $prev
        if ($rc -ne 0) { Write-Error "$($j.name): test failed (exit $rc)"; exit 1 }
    }
    # The one Apple-only translation unit, type-checked against the stub
    # headers in tests/stubs/.  Skips (exit 0) if no clang is installed;
    # pass -RequireClang to make that a failure instead.
    $syntax = Join-Path $PSScriptRoot "syntax_check_metal.py"
    $py = "C:\Python314\python.exe"
    if (-not (Test-Path $py)) { $py = "python" }
    & $py $syntax
    if ($LASTEXITCODE -ne 0) { Write-Error "nvn_metal.mm: syntax check failed"; exit 1 }

    Write-Host "build_abi_test: all tests passed"
} finally {
    Pop-Location
}

