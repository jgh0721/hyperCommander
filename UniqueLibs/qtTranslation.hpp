#pragma once

/// 플랫폼/프레임워크 헤더파일들( Windows, Boost, Qt, ... )
#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

namespace hc::ui
{
    /// Qt 내장 문구(메시지 상자의 '자세히 보기', 표준 대화상자 단추 등)를 한국어로 — 배포한 Libs/translations/qt_ko.qm.
    /// 제품 화면 문구가 한국어이므로 시스템 언어와 상관없이 한국어를 먼저 시도한다.
    inline void InstallQtTranslation(QCoreApplication& App)
    {
        auto* Translator = new QTranslator(&App);
        const QStringList Folders{QLibraryInfo::path(QLibraryInfo::TranslationsPath),
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Libs/translations"))};
        for (const auto& Folder : Folders) {
            if (Translator->load(QLocale(QLocale::Korean, QLocale::SouthKorea), QStringLiteral("qt"), QStringLiteral("_"), Folder)) {
                QCoreApplication::installTranslator(Translator);
                return;
            }
        }
        delete Translator;
    }
}
