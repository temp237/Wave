@echo off
setlocal
rem ==========================================================================
rem  Wave builder
rem   1. Builds backend\WaveBackend.slnx (Release x64) with MSBuild
rem      (falls back to the existing backend\x64\Release\WaveBackend.exe if
rem      Visual Studio / MSBuild can't be found)
rem   2. Runs "cargo tauri build" on frontend and waits for it
rem   3. Copies WaveBackend.exe and the app (as Wave.exe) into .\Wave
rem ==========================================================================

set "ROOT=%~dp0"
set "BACKEND_DIR=%ROOT%backend"
set "BACKEND_EXE=%BACKEND_DIR%\x64\Release\WaveBackend.exe"
set "FRONTEND_DIR=%ROOT%frontend"
set "FRONTEND_EXE=%FRONTEND_DIR%\src-tauri\target\release\wavefrontend.exe"
set "OUT=%ROOT%Wave"
set "DEPS_BIN=%ROOT%Dependencies\onnxruntime-directml\bin"

echo.
echo [1/3] Building backend...
set "MSBUILD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
)

if not defined MSBUILD goto :no_msbuild
echo Using "%MSBUILD%"
"%MSBUILD%" "%BACKEND_DIR%\WaveBackend.slnx" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
if errorlevel 1 (
    echo.
    echo [ERROR] Backend build failed.
    goto :fail
)
goto :backend_done

:no_msbuild
echo MSBuild not found - using the existing WaveBackend.exe instead.

:backend_done
if not exist "%BACKEND_EXE%" (
    echo [ERROR] Could not find "%BACKEND_EXE%"
    goto :fail
)

echo.
echo [2/3] Building frontend (cargo tauri build)...
if exist "%USERPROFILE%\.cargo\bin\cargo.exe" set "PATH=%USERPROFILE%\.cargo\bin;%PATH%"
pushd "%FRONTEND_DIR%"
set "RUSTFLAGS=--remap-path-prefix=%USERPROFILE%=~"
call cargo tauri build --no-bundle
set "TAURI_ERR=%errorlevel%"
popd
if not "%TAURI_ERR%"=="0" (
    echo.
    echo [ERROR] cargo tauri build failed.
    goto :fail
)
if not exist "%FRONTEND_EXE%" (
    echo [ERROR] Could not find "%FRONTEND_EXE%"
    goto :fail
)

echo.
echo [3/3] Packaging into "%OUT%"...
rem Close running copies so the old exes aren't locked
taskkill /im Wave.exe /f >nul 2>&1
taskkill /im wavefrontend.exe /f >nul 2>&1
taskkill /im WaveBackend.exe /f >nul 2>&1
ping -n 3 127.0.0.1 >nul

if not exist "%OUT%" mkdir "%OUT%"
rem Delete first instead of overwriting in place: Explorer caches exe icons per file,
rem so an overwritten exe can keep showing its old icon.
del /f /q "%OUT%\WaveBackend.exe" "%OUT%\WaveMicApo.dll" "%OUT%\Wave.exe" "%OUT%\wavefrontend.exe" "%OUT%\execute.bat" >nul 2>&1
copy /y "%BACKEND_EXE%" "%OUT%\WaveBackend.exe" >nul || goto :fail
copy /y "%BACKEND_DIR%\x64\Release\WaveMicApo.dll" "%OUT%\WaveMicApo.dll" >nul || goto :fail
copy /y "%FRONTEND_EXE%" "%OUT%\Wave.exe" >nul || goto :fail
copy /y "%DEPS_BIN%\*.dll" "%OUT%\" >nul || goto :fail
copy /y "%DEPS_BIN%\*.dll" "%BACKEND_DIR%\x64\Release\" >nul
rem Ask Explorer to refresh its icon cache
"%SystemRoot%\System32\ie4uinit.exe" -show >nul 2>&1


echo.
echo Done! Output:
dir /b "%OUT%"
echo.
pause
exit /b 0

:fail
echo.
echo Build failed.
pause
exit /b 1
