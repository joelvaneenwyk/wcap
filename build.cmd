@echo off
goto:$main

:fxc
    if not exist shaders mkdir shaders
    fxc.exe /nologo %FXC% /WX /Ges /T cs_5_0 /E %1 /Fo shaders\%1.dxbc /Fc shaders\%1.asm wcap_shaders.hlsl || exit /b 1
    fxc.exe /nologo /compress /Vn %1ShaderBytes /Fo shaders\%1.dcs /Fh shaders\%1.h shaders\%1.dxbc || exit /b 1
goto :eof

:$main
setlocal enabledelayedexpansion
  cd /d "%~dp0"
  set "WCAP_RETURN_CODE=XX"

  where /q cl.exe && goto:$main_found_cl

  set __VSCMD_ARG_NO_LOGO=1
  for /f "tokens=*" %%i in ('"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath') do (
    set "VS=%%i\VC\Auxiliary\Build\vcvarsall.bat"
  )
  if not exist "!VS!" (
    echo ERROR: Path not found: "!VS!"
    set "WCAP_RETURN_CODE=2"
    goto:$main_end
  )
  echo ##[cmd] "!VS!" amd64
  call "!VS!" amd64
  if errorlevel 1 goto:$main_end

  :$main_found_cl
  if "%VSCMD_ARG_TGT_ARCH%" neq "x64" (
    echo ERROR: please run this from MSVC x64 native tools command prompt, 32-bit target is not supported!
    set "WCAP_RETURN_CODE=3"
    goto:$main_end
  )

  if "%~1" equ "debug" (
    set CL=/MTd /Od /Zi /D_DEBUG /RTC1 /Fdwcap.pdb /fsanitize=address
    set LINK=/DEBUG
    set FXC=/Od /Zi
  ) else (
    set CL=/GL /O1 /Oi /DNDEBUG /GS-
    set LINK=/LTCG /OPT:REF /OPT:ICF ucrt.lib libvcruntime.lib
    set FXC=/O3 /Qstrip_reflect /Qstrip_debug /Qstrip_priv
  )

  call :fxc ResizePassH            || exit /b 1
  call :fxc ResizePassV            || exit /b 1
  call :fxc ResizeLinearPassH      || exit /b 1
  call :fxc ResizeLinearPassV      || exit /b 1
  call :fxc ConvertSinglePass      || exit /b 1
  call :fxc ConvertPass1           || exit /b 1
  call :fxc ConvertPass2           || exit /b 1

  for /f %%i in ('call git describe --always --dirty') do set CL=%CL% -DWCAP_GIT_INFO=\"%%i\"

  rc.exe /nologo wcap.rc || exit /b 1
  cl.exe /nologo /W3 /WX wcap.c wcap.res /link /INCREMENTAL:NO /MANIFEST:EMBED /MANIFESTINPUT:wcap.manifest /SUBSYSTEM:WINDOWS /FIXED /merge:_RDATA=.rdata || exit /b 1
  del *.obj *.res >nul
  goto:$main_end

  :$main_end
  if "!WCAP_RETURN_CODE!"=="XX" set "WCAP_RETURN_CODE=%ERRORLEVEL%"
  if "!WCAP_RETURN_CODE!"=="0" (
    echo Build succeeded.
  ) else (
    echo Build failed. Return code: !WCAP_RETURN_CODE!
  )
endlocal & exit /b %WCAP_RETURN_CODE%
