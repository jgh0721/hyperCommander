#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QString>

namespace hc::sqlite
{
    // 제품이 쓰는 SQLite 는 Qt 키트의 sqlite3.dll 하나다(설계 8.4). 이 DLL 은 exe 옆이 아니라 Libs\sqldrivers 에 있고
    // 제품 코드는 지연 로드로 링크하므로, 처음 쓰기 전에 그 경로로 먼저 불러 둔다. 문서 검색(QSQLITE)도 같은 함수를 쓴다.
    //
    // SEE: 키트 DLL 은 SQLITE_DLL=1 로 빌드되어 sqlite3_activate_see 를 부르기 전에는 키 함수가 SQLITE_MISUSE 를 돌려준다.
    // 활성화 문구는 로컬 전용 UniqueLibs/sqlite_see_activate.hpp 에만 있다(.gitignore). 그 파일이 없는 빌드는 SEE 없이 동작한다.
    enum class SeeState
    {
        Available,          // 활성화까지 끝나 암호화 파일을 열 수 있다.
        LibraryMissing,     // sqlite3.dll 을 불러오지 못했다.
        NotInBuild,         // 이 빌드에 활성화 문구가 없다(sqlite_see_activate.hpp 없음).
        NotInLibrary,       // 불러온 DLL 에 SEE 함수가 없다(제품 DLL 이 바뀐 경우).
        ActivationFailed    // 활성화 뒤에도 키 함수가 SQLITE_MISUSE 다(문구와 DLL 이 맞지 않음).
    };

    class Runtime final
    {
    public:
        // Libs\sqldrivers\sqlite3(d).dll 을 경로로 불러 둔다. 한 번만 시도하고 결과를 기억한다. 스레드에 안전하다.
        static bool Load();
        // Load() 뒤 SEE 를 활성화하고 결과를 돌려준다. 처음 부를 때 한 번만 한다.
        static SeeState See();
        // 화면에 보일 SEE 상태 한 줄(설정 › SQLite › 암호화의 안내 띠, 키 대화).
        static QString SeeText(SeeState State);
        // 불러온 DLL 의 경로 · 버전(정보 표시용). Load() 가 실패하면 빈 문자열.
        static QString LibraryPath();
        static QString Version();
    };
}
