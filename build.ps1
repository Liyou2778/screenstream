# build.ps1 - builds screenstream with MSVC + Ninja using absolute tool paths.
$ErrorActionPreference = 'Stop'

$vcvars  = 'C:\develop\vs2022\community\VC\Auxiliary\Build\vcvars64.bat'
$cmake   = 'C:\develop\vs2022\community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja   = 'C:\develop\vs2022\community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'

if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }
if (-not (Test-Path $cmake))  { throw "cmake.exe not found: $cmake" }
if (-not (Test-Path $ninja))  { throw "ninja.exe not found: $ninja" }

# chcp 65001: MSVC on a Chinese Windows emits its /showIncludes prefix in the console
# code page. CMake stores the detected prefix as UTF-8, so without UTF-8 output Ninja
# cannot filter the dependency lines (they get echoed for every header).
$cmd = 'chcp 65001 >nul && call "' + $vcvars + '" && "' + $cmake + '" -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="' + $ninja + '" && "' + $cmake + '" --build build'

Write-Host "== build command =="
Write-Host $cmd
cmd /c $cmd
$code = $LASTEXITCODE
if ($code -ne 0) { throw "build failed with exit code $code" }
Write-Host "== build OK =="
