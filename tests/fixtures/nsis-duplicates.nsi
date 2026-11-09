; Regression fixture: never execute the generated installer.
; Build with /DINPUT=<payload directory> /DOUTPUT=<exe path>
; /DCOMPRESSOR=zlib|bzip2|lzma /DSOLID=on|off.
Unicode true
Name "HyperCommander NSIS duplicate fixture"
OutFile "${OUTPUT}"
RequestExecutionLevel user
!if "${SOLID}" == "on"
SetCompressor /SOLID ${COMPRESSOR}
!else
SetCompressor ${COMPRESSOR}
!endif
Section
!ifdef STORE_FILES
SetCompress off
!endif
SetOutPath "$INSTDIR"
; Two equal-size payloads target exactly the same installed filename.
StrCmp $0 "x64" native64 native32
native32:
File /oname=app.bin "${INPUT}\x86.bin"
Goto done
native64:
File /oname=app.bin "${INPUT}\x64.bin"
done:
; A real filename deliberately collides with the first synthetic alias.
File "/oname=app [NSIS 0].bin" "${INPUT}\collision.bin"
File /oname=empty.bin "${INPUT}\empty.bin"
SectionEnd
