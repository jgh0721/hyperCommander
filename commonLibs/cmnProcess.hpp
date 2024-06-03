#pragma once

#include <QtCore>

namespace nsCmn
{
    namespace nsProcess
    {
        /*!
            주어진 경로의 파일을 실행한다.

            @param sFilePath    실행할 파일 및 경로
            @param isHide       프로세스의 실행을 화면에 표시할 지 여부
            @param isWait       프로세스의 종료를 기다릴 지 여부
            @param dwProcessId  프로세스의 종료를 기다리지 않고, 프로세스의 PID 가 필요할 때 사용, isWait == false 일 때만 반환
            @return : 성공 / 실패
        */
        bool                                            CreateProcessAsNormal( const QString& sFilePath, const QString& sCMDLine, bool isHide, bool isWait, uint32_t* dwProcessId = nullptr );

    } // nsProcess

} // nsCmn