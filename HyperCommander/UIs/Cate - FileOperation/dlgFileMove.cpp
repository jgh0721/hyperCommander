#include "stdafx.h"
#include "dlgFileMove.hpp"

QFileMoveUI::QFileMoveUI( QWidget* Parent, Qt::WindowFlags Flags )
    : nsHC::QBaseUI( Parent, Flags )
{
    ui.setupUi( this );
}

class CFileCopyEngine
{
    // NTFS 스트림 복사 옵션
    // 복사 후 해시 확인 옵션( xxHash3 )
    // 열 수 없는 파일 건너뛰기
    // 읽기 전용/숨김/시스템 덮어쓰기/삭제하기 
    // NTFS 권한 복사

    // SRC 와 DST 의 FileSystem 확인( NTFS 인지 확인 )
    // SRC 파일 열기
    // DST 파일 열기

};