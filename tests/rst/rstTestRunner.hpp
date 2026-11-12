#pragma once

#include <QList>
#include <QObject>

namespace hc::rst::tests {

/// 테스트 클래스 자동 등록. 각 tst_*.cpp 가 파일 끝에서 HC_RST_REGISTER_TEST 로
/// 자기 자신을 등록하면 main.cpp 가 전부 순회하며 실행한다.
QList< QObject* >& registry();

struct Registrar
{
    explicit Registrar( QObject* instance ) { registry().append( instance ); }
};

}  // namespace hc::rst::tests

#define HC_RST_REGISTER_TEST( Class ) \
    static const hc::rst::tests::Registrar s_rstRegistrar_##Class{ new Class }
