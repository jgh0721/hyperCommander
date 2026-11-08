#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리
#include "commonLibs/document/epub/epubTemplate.hpp"

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace hc::document
{
    /// 새 EPUB 대화상자의 결과. path_ 는 만들 파일의 절대 경로.
    struct NewEpubRequest
    {
        epub::NewBook book_;
        QString       path_;
        QString       coverPath_;
    };

    /// 제목에서 파일 이름을 만든다. 금지 문자 → 공백, 끝의 점 · 공백 제거, 120자, 예약 이름에는 ‘_’, 끝에 .epub.
    QString NewEpubFileName( const QString& Title );
    /// 폴더 안에서 겹치지 않는 ‘이름 (2).epub’ 형식의 이름.
    QString UniqueEpubFileName( const QString& Folder, const QString& FileName );

    /// ‘새 EPUB 만들기’ 대화상자(시안 ④). [만들고 편집]으로 닫으면 Requested() 를 낸다(D5).
    /// 같은 이름 · 폴더 쓰기 가능 여부는 닫기 전에 검사한다. 호스트 펌프를 막지 않도록 open() 으로 연다.
    class NewEpubDialog final : public QDialog
    {
        Q_OBJECT
    public:
        NewEpubDialog( QWidget* Parent, const QString& Folder, const QString& Creator, const QString& Language );
        /// 닫은 뒤 쓰기에 실패했을 때: 입력값을 되돌리고 오류를 보인다.
        void Restore( const NewEpubRequest& Request, const QString& Error, bool Exists );
        NewEpubRequest Request() const;

    signals:
        void Requested( const hc::document::NewEpubRequest& Request );

    public slots:
        void accept() override;

    private:
        void validate();
        void pickCover();
        void setCover( const QString& Path );
        QString language() const;
        QString folder() const;

        QLineEdit*   title_ = nullptr;
        QLineEdit*   creator_ = nullptr;
        QComboBox*   language_ = nullptr;
        QLineEdit*   chapter_ = nullptr;
        QLabel*      cover_ = nullptr;
        QPushButton* coverClear_ = nullptr;
        QCheckBox*   stylesheet_ = nullptr;
        QLineEdit*   folder_ = nullptr;
        QLineEdit*   file_ = nullptr;
        QLabel*      message_ = nullptr;
        QPushButton* suggest_ = nullptr;
        QPushButton* create_ = nullptr;
        QByteArray   coverBytes_;
        QString      coverPath_, coverError_, restoredError_;
        bool         fileEdited_ = false, restoredExists_ = false;
    };
}
