#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFrame>
#include <QKeySequence>
#include <QList>
#include <QPoint>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QStringList>

class QAbstractButton;
class QLabel;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QStackedWidget;
namespace fm::ui { class ChoiceCard; }

namespace hc::document
{
    /// Ctrl+N 으로 만드는 문서 종류. 순서가 팝업의 키(1 – 4)다.
    enum class NewDocumentKind { Text, Markdown, Epub, Image };

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

    /// Ctrl+N 새 문서 팝업(시안 ① · ③). 종류 카드 네 장과 이미지 크기 단계를 한 팝업에 둔다.
    ///
    /// - 카드를 누르거나 Enter · 1 – 4 · (열린 채) Ctrl+N 으로 고르면 Chosen() 을 내고 닫는다.
    ///   이미지는 같은 팝업의 크기 단계로 넘어가고, 거기서 만들면 ImageChosen() 을 낸다.
    /// - Esc 는 크기 단계에서 종류 선택으로, 종류 선택에서 닫기로 간다. 닫히면 포커스를 SetFocusReturn() 위젯으로 돌린다.
    /// - 닫히면 스스로 지워진다(WA_DeleteOnClose).
    class NewDocumentChooser final : public QFrame
    {
        Q_OBJECT
    public:
        struct Options
        {
            NewDocumentKind initial_      = NewDocumentKind::Text;
            bool            viewer_       = false;  // HyperViewer: EPUB 은 HyperEditor 로 넘긴다는 설명
            bool            imagePage_    = false;  // 크기 단계로 바로 연다(file.newImage)
            QSize           clipboardSize_;         // 비어 있으면 ‘클립보드 이미지’ 행이 없다
            NewImageSpec    lastImage_;
            int             nextDocument_ = 1;
            int             nextImage_    = 1;
            QKeySequence    repeatKey_;             // 지금 file.new 바인딩: 열린 채 누르면 선택 항목을 만든다
            QPoint          anchorBelow_;           // 전역 좌표. 카드의 왼쪽 위(단추 아래) 또는
            QPoint          topCenter_;             // 전역 좌표. 카드의 위 가운데(키보드 · 메뉴)
            bool            useAnchor_    = false;
        };

        static constexpr int PanelWidth = 360;

        static NewDocumentChooser* Open( QWidget* Parent, const Options& Value );
        void SetFocusReturn( QWidget* Widget ) { focusReturn_ = Widget; }
        /// file.new 가 팝업이 열린 채 다시 왔을 때: 고른 종류(또는 크기 단계의 이미지)를 만든다.
        void ActivateCurrent();

    signals:
        void Chosen( hc::document::NewDocumentKind Kind );
        void ImageChosen( const hc::document::NewImageSpec& Spec );

    protected:
        void paintEvent( QPaintEvent* Event ) override;
        void keyPressEvent( QKeyEvent* Event ) override;
        bool eventFilter( QObject* Object, QEvent* Event ) override;
        void closeEvent( QCloseEvent* Event ) override;

    private:
        NewDocumentChooser( QWidget* Parent, const Options& Value );
        QWidget* buildKinds();
        QWidget* buildImage();
        void     place();
        void     showPage( int Page );
        void     choose( int Index );
        void     createImage();
        void     refreshKindNote();
        void     refreshImageNote();
        int      checkedKind() const;
        int      checkedPreset() const;
        NewImageSpec imageSpec() const;
        bool     handleKey( QKeyEvent* Key );

        Options                         options_;
        QPointer< QWidget >             focusReturn_;
        QStackedWidget*                 pages_ = nullptr;
        QList< fm::ui::ChoiceCard* >    kinds_;
        QLabel*                         kindNote_ = nullptr;
        QList< fm::ui::ChoiceCard* >    presets_;
        QList< QSize >                  presetSizes_;
        QStringList                     presetIds_;
        QSpinBox*                       width_ = nullptr;
        QSpinBox*                       height_ = nullptr;
        QList< QRadioButton* >          backgrounds_;
        QLabel*                         imageNote_ = nullptr;
        QPushButton*                    create_ = nullptr;
        bool                            chosen_ = false;
    };
}
