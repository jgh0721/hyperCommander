#include "rstTestRunner.hpp"

#include <QCoreApplication>
#include <QTest>
#include <QTimer>

#include <cstdio>

namespace hc::rst::tests {

QList< QObject* >& registry()
{
    static QList< QObject* > instances;
    return instances;
}

}  // namespace hc::rst::tests

int main( int argc, char** argv )
{
    QCoreApplication app( argc, argv );
    if (app.arguments().contains(QStringLiteral("--process-reaper-child")))
    {
        QTimer::singleShot(30'000, &app, &QCoreApplication::quit);
        return app.exec();
    }

    const QByteArray selectedClass = qgetenv("HC_RST_TEST_CLASS");
    int executed = 0;
    int failures = 0;
    for( QObject* testObject : hc::rst::tests::registry() )
    {
        if (!selectedClass.isEmpty() && selectedClass != testObject->metaObject()->className())
            continue;
        ++executed;
        const int result = QTest::qExec( testObject, argc, argv );
        // Windows 에서 QTest 의 기본 로거는 stdout 이 리다이렉트되면 출력을
        // OutputDebugString 으로 보낸다. 셸에서 돌리면 어느 클래스가 깨졌는지
        // 알 길이 없으므로 여기서 stderr 로 한 줄 남긴다.
        if( result != 0 )
            std::fprintf( stderr, "FAILED: %s (%d)\n", testObject->metaObject()->className(), result );
        failures += result;
    }

    if (executed == 0)
    {
        std::fprintf(stderr, "No test class matched HC_RST_TEST_CLASS\n");
        return 1;
    }
    return failures;
}
