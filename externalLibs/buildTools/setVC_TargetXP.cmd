SET VC_PLATFORM=%1

ECHO TargetXP %1%

SET INCLUDE=%ProgramFiles(x86)%\Microsoft SDKs\Windows\7.1A\Include;%INCLUDE%
SET PATH=%ProgramFiles(x86)%\Microsoft SDKs\Windows\7.1A\Bin;%PATH%
SET CL=/D_USING_V110_SDK71_;/D_USING_V120_SDK71_;/D_USING_V140_SDK71_;/D_USING_V141_SDK71_;%CL%
SET LIB=%ProgramFiles(x86)%\Microsoft SDKs\Windows\7.1A\Lib;%LIB%

IF /I "%1" == "" (
    SET LINK=/SUBSYSTEM:CONSOLE,5.01 %LINK%
)

IF /I "%1" == "X86" (
    SET LINK=/SUBSYSTEM:CONSOLE,5.01 %LINK%
)

IF /I "%1" == "X64" (
    SET LINK=/SUBSYSTEM:CONSOLE,5.02 %LINK%
)