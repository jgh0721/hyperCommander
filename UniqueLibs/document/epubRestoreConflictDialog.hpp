#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDateTime>
#include <QDialog>
#include <QString>
#include <QStringList>

namespace hc::document
{
    /// 비교 창 한쪽 열에 보이는 책 정보.
    struct EpubBookFacts
    {
        bool      readable_ = false;
        QString   title_, creator_, publisher_, language_, modified_;
        int       chapters_ = 0;
        qint64    size_ = -1;
        QDateTime fileModified_;
    };

    /// HotExit 로 되살릴 EPUB 초안과 지금 파일의 비교(D9, 설계 10.3).
    struct EpubRestoreFacts
    {
        QString         bookPath_, draftPath_, error_;
        bool            missing_ = false;        // 지금 파일이 없다
        bool            sourceChanged_ = false;  // 편집을 시작한 뒤 디스크의 내용이 달라졌다
        EpubBookFacts   original_, current_;
        QStringList     changed_, alsoChanged_;  // 초안이 바꾼 리소스, 그중 지금 파일에서도 바뀐 리소스
        QDateTime       lastEdit_;
    };

    /// 작업 스레드에서 부른다. 초안(편집을 시작한 원본 전체 포함)과 지금 파일을 읽어 비교한다.
    EpubRestoreFacts GatherEpubRestoreFacts( const QString& DraftRoot, const QString& DraftPath, const QString& BookPath );

    /// 원본이 바뀌었을 때의 비교 확인 창(시안 ⑧). 결과는 Chosen() 으로 알린다. open() 으로 연다.
    class EpubRestoreConflictDialog final : public QDialog
    {
        Q_OBJECT
    public:
        enum Choice { OpenEditsAsCopy, OpenCurrent, DiscardDraft, Later };
        EpubRestoreConflictDialog( const EpubRestoreFacts& Facts, QWidget* Parent );
    signals:
        void Chosen( int Choice );
    private:
        void finish( Choice Value );
        bool chosen_ = false;
    };
}
