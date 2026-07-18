@echo off
setlocal enabledelayedexpansion

rem ===========================================================================
rem  build_arm64x.cmd - build the ARM64X Neptune UMD package.
rem
rem  Windows cannot load a single DLL that satisfies both native-arm64 and
rem  arm64ec (x64-emulated) Direct3D callers, so the Neptune UMD is built twice
rem  and stitched together behind a code-less ARM64X forwarder:
rem
rem    1. build.cmd <mesa> arm64     -> native arm64 neptune_umd.dll
rem    2. build.cmd <mesa> arm64ec   -> arm64ec   neptune_umd.dll
rem    3. make-arm64x.bat (from the mesa source) forwards the two views into
rem       mesa-install-arm64x\{neptune_umd.dll,neptune_umd_arm64.dll,neptune_umd_ec.dll}
rem
rem  Point the viogpu3d KMD build's MESA_ARM64X_PREFIX at mesa-install-arm64x to
rem  package the result.  See BUILDING.md (ARM64X) in the viogpu3d driver.
rem ===========================================================================

if "%~1" neq "" (
  set MESA_SOURCE=%~1
) else (
  echo usage: build_arm64x.cmd path\to\mesa
  exit /b 1
)

rem *** fail early if the mesa source lacks the ARM64X forwarder tool ***
set MAKE_ARM64X=%MESA_SOURCE%\src\virtio\neptune\triton\tools\make-arm64x.bat
if not exist "%MAKE_ARM64X%" (
  echo ERROR: "%MAKE_ARM64X%" not found^^!
  echo The mesa source does not provide the ARM64X forwarder tool.
  exit /b 1
)

rem *** build both views (native arm64 first, then the arm64ec EC view) ***
call "%~dp0build.cmd" "%MESA_SOURCE%" arm64   || exit /b 1
call "%~dp0build.cmd" "%MESA_SOURCE%" arm64ec || exit /b 1

rem *** stitch the two views into an ARM64X forwarder package ***
set ARM64_UMD=%CD%\mesa-install-arm64\bin\neptune_umd.dll
set EC_UMD=%CD%\mesa-install-arm64ec\bin\neptune_umd.dll
set ARM64X_DIR=%CD%\mesa-install-arm64x

if not exist "%ARM64_UMD%" ( echo ERROR: "%ARM64_UMD%" not found^^! & exit /b 1 )
if not exist "%EC_UMD%"    ( echo ERROR: "%EC_UMD%" not found^^!    & exit /b 1 )

rd /s /q "%ARM64X_DIR%" 1>nul 2>nul
call "%MAKE_ARM64X%" "%ARM64_UMD%" "%EC_UMD%" "%ARM64X_DIR%" || exit /b 1

echo ARM64X package written to %ARM64X_DIR%
echo Done^^!
