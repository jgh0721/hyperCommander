#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDialog>
#include <QKeySequence>
#include <QList>
#include <QSize>
#include <QString>
#include <QStringList>

class QComboBox;
class QLabel;
class QPushButton;
class QRadioButton;
class QSpinBox;
namespace fm::ui { class ChoiceCard; }

namespace hc::document
{
    /// Ctrl+N 으로 만드는 문서 종류. 순서가 대화상자의 키(1 – 5)다.
    enum class NewDocumentKind { Text, Markdown, Hwpx, Epub, Image };

    QString         NewDocumentKindId( NewDocumentKind Kind );
    NewDocumentKind NewDocumentKindFrom( const QString& Id, NewDocumentKind Fallback = NewDocumentKind::Text );

    /// 새 이미지의 크기 · 배경. preset_ 은 마지막 선택을 기억하는 이름(fhd · hd · square · a4 · custom · clipboard).
    struct NewImageSpec
    {
        int     width_      = 1920;
        int     height_     = 1080;
        QString background_ = QStringLiteral( "white" );   // white · clear · black
        QString preset_     = QStringLiteral( "fhd" );
        bool    clipboard_  = false;
        int     dotsPerInch_ = 96;
    };

    /// 가로 · 세로 1 – 16384 px(‘출력 크기’ 상한), 전체 128 MP(32bit 512 MiB). 받을 수 있으면 빈 문자열.
    QString NewImageError( int Width, int Height );

    /// ‘새 문서’ 대화상자(Ctrl+N). 제품 대화상자 틀(머리 · 본문 · 바닥)에 종류 카드 다섯 장을 둔다.
    ///
    /// - 카드를 누르면 고르기만 한다. [만들기] · Enter · 카드 두 번 누르기 · 1 – 5 · (열린 채) Ctrl+N 으로 만든다.
    /// - 고른 종류는 Chosen() 으로 알리고 닫는다. EPUB · 이미지는 받는 쪽이 이어지는 대화상자를 연다.
    /// - 호스트 펌프를 막지 않도록 open() 으로 연다. 닫히면 스스로 지워진다.
    class NewDocumentChooser final : public QDialog
    {
        Q_OBJECT
    public:
        struct Options
        {
            NewDocumentKind initial_      = NewDocumentKind::Text;
            bool            viewer_       = false;  // HyperViewer: EPUB 은 HyperEditor 로 넘긴다는 설명
            int             nextDocument_ = 1;
            int             nextImage_    = 1;
            QKeySequence    repeatKey_;             // 지금 file.new 바인딩: 열린 채 누르면 고른 종류를 만든다
        };

        NewDocumentChooser( QWidget* Parent, const Options& Value );
        /// file.new 가 대화상자가 열린 채 다시 왔을 때: 고른 종류를 만든다.
        void ActivateCurrent();

    signals:
        void Chosen( hc::document::NewDocumentKind Kind );

    public slots:
        void accept() override;

    protected:
        bool eventFilter( QObject* Object, QEvent* Event ) override;
        void keyPressEvent( QKeyEvent* Event ) override;

    private:
        int  checkedKind() const;
        void choose( int Index );
        void refreshNote();

        Options                      options_;
        QList< fm::ui::ChoiceCard* > kinds_;
        QLabel*                      note_ = nullptr;
        QPushButton*                 create_ = nullptr;
        bool                         chosen_ = false;
    };

    /// ‘새 이미지’ 대화상자. 크기(클립보드 이미지 포함) · 가로 · 세로 · 배경을 고른다.
    class NewImageDialog final : public QDialog
    {
        Q_OBJECT
    public:
        struct Options
        {
            QSize        clipboardSize_;   // 비어 있으면 ‘클립보드 이미지’ 항목이 없다
            NewImageSpec lastImage_;
            int          nextImage_ = 1;
            QKeySequence repeatKey_;
        };

        NewImageDialog( QWidget* Parent, const Options& Value );
        NewImageSpec Spec() const;
        void ActivateCurrent() { accept(); }

    signals:
        void ImageChosen( const hc::document::NewImageSpec& Spec );

    public slots:
        void accept() override;

    protected:
        void keyPressEvent( QKeyEvent* Event ) override;

    private:
        void refresh();

        Options                 options_;
        QComboBox*              size_ = nullptr;
        QSpinBox*               width_ = nullptr;
        QSpinBox*               height_ = nullptr;
        QList< QRadioButton* >  backgrounds_;
        QLabel*                 message_ = nullptr;
        QPushButton*            create_ = nullptr;
        bool                    chosen_ = false;
    };
}
