#pragma once

// 키트의 sqlite3.h 를 SEE 선언과 함께 쓴다. sqlite3_key · sqlite3_rekey · sqlite3_activate_see 는
// SQLITE_HAS_CODEC 이 정의되어 있을 때만 선언된다. DLL 은 지연 로드로 링크하므로(설계 8.4) 이 헤더의 함수를
// 부르기 전에 반드시 hc::sqlite::Runtime::Load() 가 성공해야 한다.

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#ifndef SQLITE_HAS_CODEC
#define SQLITE_HAS_CODEC 1
#endif
#include <sqlite3.h>
