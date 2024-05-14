@ECHO OFF
PUSHD %~dp0

IF EXIST ".\bit7z\" (
	CD "bit7z"
	RD /S /Q X64
	RD /S /Q X86
	CD ..
)

IF EXIST ".\CryptoPP\" (
	CD "CryptoPP"
	RD /S /Q X64
	RD /S /Q X86
	CD ..
)

IF EXIST ".\GoogleProtocolBuffer\" (
	CD "GoogleProtocolBuffer"
	RD /S /Q X64
	RD /S /Q X86
	CD ..
)

IF EXIST ".\QtitanChart\" (
	CD "QtitanChart"
	RD /S /Q X64
	RD /S /Q X86
	CD ..
)

IF EXIST ".\QtitanDataGrid\" (
	CD "QtitanDataGrid"
	RD /S /Q X64
	RD /S /Q X86
	CD ..
)

IF EXIST ".\QtnProperty\" (
	CD "QtnProperty"
	RD /S /Q X64
	RD /S /Q X86
	CD ..
)

IF EXIST ".\QtPropertyBrowser\" (
	CD "QtPropertyBrowser"
	RD /S /Q X64
	RD /S /Q X86
	CD ..
)

POPD
