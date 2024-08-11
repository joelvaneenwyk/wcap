@echo off
goto:$main

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
    set "CL=/MTd /Od /Zi /D_DEBUG /RTC1 /Fdwcap.pdb /fsanitize=address"
    set "LINK=/DEBUG"
  ) else (
    set "CL=/GL /O1 /DNDEBUG /GS-"
    set "LINK=/LTCG /OPT:REF /OPT:ICF ucrt.lib libvcruntime.lib"
  )

  fxc.exe /nologo /T cs_5_0 /E Resize  /O3 /WX /Fh wcap_resize_shader.h /Vn ResizeShaderBytes /Qstrip_reflect /Qstrip_debug /Qstrip_priv wcap_shaders.hlsl
  if errorlevel 1 goto:$main_end
  fxc.exe /nologo /T cs_5_0 /E Convert /O3 /WX /Fh wcap_convert_shader.h /Vn ConvertShaderBytes /Qstrip_reflect /Qstrip_debug /Qstrip_priv wcap_shaders.hlsl
  if errorlevel 1 goto:$main_end

  rc.exe /nologo wcap.rc
  if errorlevel 1 goto:$main_end

  cl.exe /nologo /W3 /WX /MP *.c /Fewcap.exe wcap.res /link /INCREMENTAL:NO /MANIFEST:EMBED /MANIFESTINPUT:wcap.manifest /SUBSYSTEM:WINDOWS /FIXED /merge:_RDATA=.rdata
  if errorlevel 1 goto:$main_end

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
