@ECHO OFF

PUSHD %~dp0

REM Usage
REM BuildTools VC2022 X86
REM BuildTools VC2022 X64

SET COMPILER=
SET PLATFORM=

IF "%1" == "/?" GOTO HELP
IF "%1" == "" GOTO HELP
IF "%2" == "" GOTO HELP

SET COMPILER=%1
SET PLATFORM=%2

IF /I "%COMPILER%" == "VC2022" (
  for /f "usebackq tokens=*" %%i in (`vswhere.exe -version "[17, 18)" -property installationPath`) do (
      Call "%%i\VC\Auxiliary\Build\vcvarsall.bat" %2
  )
)

IF /I "%COMPILER%" == "VC2019" (
  for /f "usebackq tokens=*" %%i in (`vswhere.exe -version "[16, 17)" -property installationPath`) do (
      Call "%%i\VC\Auxiliary\Build\vcvarsall.bat" %2
  )
)

IF /I "%COMPILER%" == "VC2017" (
  for /f "usebackq tokens=*" %%i in (`vswhere.exe -version "[15, 16)" -property installationPath`) do (
      Call "%%i\VC\Auxiliary\Build\vcvarsall.bat" %2
  )
)

IF /I "%COMPILER%" == "VC2017_XP" (
  for /f "usebackq tokens=*" %%i in (`vswhere.exe -version "[15, 16)" -property installationPath`) do (
      Call "%%i\VC\Auxiliary\Build\vcvarsall.bat" %2
  )
  
  Call setVC_TargetXP.cmd %2
)

GOTO END

:HELP
ECHO GYTNI QT Build Script(Compiler Setup) - 2024
ECHO BuildTools Compiler Platform
ECHO .
ECHO .
ECHO Compiler = VC2017 VC2017_XP VC2019 VC2022
ECHO Platform = X86 X64

:END

POPD
