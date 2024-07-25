@ECHO OFF
PUSHD %~dp0

SET COMPILER=%1
SET PLATFORM=%2
SET GENERATOR=
SET CMAKE_COMPILER=

IF /I "%COMPILER%" == "" (
	CHOICE /C 1234 /M "Select MSVC Version 1 = VC2022, 2 = VC2019, 3 = VC2017_XP, 4 = VC2017"

	IF ERRORLEVEL 4 (
		SET COMPILER=VC2017
		SET GENERATOR=Visual Studio 15 2017
		SET CMAKE_COMPILER=v141
	)
	
	IF ERRORLEVEL 3 (
		SET COMPILER=VC2017_XP
		SET GENERATOR=Visual Studio 15 2017
		SET CMAKE_COMPILER=v141_xp
	)
	
	IF ERRORLEVEL 2 (
		SET COMPILER=VC2019
		SET GENERATOR=Visual Studio 16 2019
		SET CMAKE_COMPILER=v142
	)
	
	IF ERRORLEVEL 1 (
		SET COMPILER=VC2022
		SET GENERATOR=Visual Studio 17 2022
		SET CMAKE_COMPILER=v143
	)
)

IF /I "%PLATFORM%" == "" (
	CHOICE /C 12 /M "Select Platform 1 = X86, 2 = X64"
	
	IF ERRORLEVEL 2 (
		SET PLATFORM=X64
	)
	
	IF ERRORLEVEL 1 (
		SET PLATFORM=X86
	)
)

ECHO.
ECHO Selected Compiler = %COMPILER%, Platform = %PLATFORM%, Generator = %GENERATOR%
SET PATH=%~dp0\buildTools;%PATH%

Call buildTools\setBuildTools.cmd %COMPILER% %PLATFORM%
Call buildTools\setQTDIROnENVs.cmd
SET QT_PATH=

IF /I "%PLATFORM%" == "X86" (
	SET QT_PATH=%QT_PATH_X86%
)

IF /I "%PLATFORM%" == "X64" (
	SET QT_PATH=%QT_PATH_X64%
)

ECHO.
ECHO QT DIR : %QT_PATH%
ECHO.

ECHO Building Google ProtocolBuffer...
PUSHD %~dp0
IF EXIST "GoogleProtocolBuffer\Script\buildWithMSVC.cmd" (
	Call GoogleProtocolBuffer\script\buildWithMSVC.cmd %COMPILER% %PLATFORM% Static
)
POPD

ECHO Building bit7z...
PUSHD %~dp0
IF EXIST "bit7z\script\buildWithMSVC.cmd" (
	Call bit7z\script\buildWithMSVC.cmd %COMPILER% %PLATFORM% Static
)
POPD

ECHO Building CryptoPP...
PUSHD %~dp0
IF EXIST "CryptoPP\script\buildWithMSVC.cmd" (
	Call CryptoPP\script\buildWithMSVC.cmd %COMPILER% %PLATFORM% Static
)
POPD

ECHO Building QtitanDataGrid...
PUSHD %~dp0
IF EXIST "QtitanDataGrid\script\buildWithMSVC.cmd" (
	Call QtitanDataGrid\script\buildWithMSVC.cmd %COMPILER% %PLATFORM% Static
)
POPD

ECHO Building QtitanChart...
PUSHD %~dp0
IF EXIST "QtitanChart\script\buildWithMSVC.cmd" (
	Call QtitanChart\script\buildWithMSVC.cmd %COMPILER% %PLATFORM% Static
)
POPD

ECHO Building QtnProperty...
PUSHD %~dp0
IF EXIST "QtnProperty\script\buildWithMSVC.cmd" (
	Call QtnProperty\script\buildWithMSVC.cmd %COMPILER% %PLATFORM% Static
)
POPD

ECHO Building QtPropertyBrowser...
PUSHD %~dp0
IF EXIST "QtPropertyBrowser\script\buildWithMSVC.cmd" (
	Call QtPropertyBrowser\script\buildWithMSVC.cmd %COMPILER% %PLATFORM% Static 
)
POPD

GOTO END

:HELP
ECHO GYTNI ExternalLibs Builder Ver 0.3
ECHO Usage: buildExternalLibs Compiler Platform
ECHO.
ECHO.
ECHO Compiler = VC2017 VC2017_XP VC2019 VC2022
ECHO Platform = X86 X64
ECHO.
ECHO Example : buildExternalLibs VC2022 X64
 
:END

POPD
