#include "stdafx.h"
#include "rstCompletionDetailPopup.hpp"

/// 모듈
#include "core/ThemeManager.h"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QGuiApplication>
#include <QLabel>
#include <QPainter>
#include <QScreen>
#include <QVBoxLayout>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {

namespace {
/// 패널 고정 폭. 목록 팝업(280~720)과 나란히 놓아도 화면을 잡아먹지 않는 값.
constexpr int kPanelWidth = 340;
/// 텍스트 모드에서 본문이 길면 여기서 자른다.
constexpr int kMaxPanelHeight = 320;
/// 목록 팝업과의 간격.
constexpr int kAnchorGap = 6;
/// 프리뷰 상자. 이미지 종횡비와 무관하게 이 크기를 지킨다.
constexpr int kPreviewWidth = 320;
constexpr int kPreviewHeight = 180;
/// 파일 모드의 고정 높이. 방향키로 훑을 때 패널이 위아래로 펄떡이는 것이
/// 이 기능에서 가장 먼저 거슬리는 결함이라 내용과 무관하게 고정한다.
constexpr int kFilePanelHeight = 330;
/// 알파 배경 체커보드 한 칸.
constexpr int kCheckerSize = 8;
}  // namespace

/// 프리뷰 상자. 체커보드 + 중앙 정렬 + 테두리 + 안내 문구를 직접 그린다.
///
/// QLabel 로 흉내 내면 스타일시트와 싸우게 된다. 그리고 체커보드는 선택이
/// 아니다 — 투명 로고를 패널 배경색 위에 그리면 한쪽 테마에서 아예 안 보인다.
/// logo-dark.png / logo-light.png 가 정확히 그 경우다.
class CompletionPreviewCanvas final : public QWidget
{
public:
    explicit CompletionPreviewCanvas( QWidget* parent = nullptr )
        : QWidget( parent )
    {
        setFixedSize( kPreviewWidth, kPreviewHeight );
    }

    void setContent( const QPixmap& pixmap, const bool hasAlpha, const QString& note )
    {
        pixmap_ = pixmap;
        hasAlpha_ = hasAlpha;
        note_ = note;
        update();
    }

protected:
    void paintEvent( QPaintEvent* ) override
    {
        QPainter painter( this );
        const QRect box = rect().adjusted( 0, 0, -1, -1 );

        if( hasAlpha_ && !pixmap_.isNull() )
        {
            // 테마와 무관한 중립 회색 두 톤. 어느 배경에서도 알파가 보인다.
            painter.fillRect( box, QColor( 0x9a, 0x9a, 0x9a ) );
            painter.setPen( Qt::NoPen );
            painter.setBrush( QColor( 0xc4, 0xc4, 0xc4 ) );
            for( int y = box.top(); y <= box.bottom(); y += kCheckerSize )
            {
                for( int x = box.left(); x <= box.right(); x += kCheckerSize )
                {
                    const bool even = ( ( x - box.left() ) / kCheckerSize
                                        + ( y - box.top() ) / kCheckerSize )
                                      % 2 == 0;
                    if( even )
                        painter.drawRect( QRect( x, y, kCheckerSize, kCheckerSize ) & box );
                }
            }
        }
        else
        {
            painter.fillRect( box,
                             ThemeManager::instance().color( QStringLiteral( "common.surfaceAlt" ) ) );
        }

        if( !pixmap_.isNull() )
        {
            const QSize logical = pixmap_.deviceIndependentSize().toSize();
            const QPoint topLeft( box.left() + ( box.width() - logical.width() ) / 2,
                                 box.top() + ( box.height() - logical.height() ) / 2 );
            painter.drawPixmap( topLeft, pixmap_ );
        }
        else if( !note_.isEmpty() )
        {
            painter.setPen(
                ThemeManager::instance().color( QStringLiteral( "common.foregroundMuted" ) ) );
            painter.drawText( box, Qt::AlignCenter | Qt::TextWordWrap, note_ );
        }

        painter.setPen( ThemeManager::instance().color( QStringLiteral( "common.border" ) ) );
        painter.setBrush( Qt::NoBrush );
        painter.drawRect( box );
    }

private:
    QPixmap                             pixmap_;
    bool                                hasAlpha_ = false;
    QString                             note_;
};

CompletionDetailPopup::CompletionDetailPopup( QWidget* parent )
    : QFrame( parent, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint )
{
    setAttribute( Qt::WA_ShowWithoutActivating );
    setFocusPolicy( Qt::NoFocus );
    setFrameShape( QFrame::StyledPanel );
    setFrameShadow( QFrame::Plain );
    setAutoFillBackground( true );

    QPalette framePalette = palette();
    framePalette.setColor( QPalette::Window, framePalette.color( QPalette::Base ) );
    setPalette( framePalette );

    auto* layout = new QVBoxLayout( this );
    layout->setContentsMargins( 10, 8, 10, 8 );
    layout->setSpacing( 6 );

    titleLabel_ = new QLabel( this );
    titleLabel_->setWordWrap( true );
    titleLabel_->setTextInteractionFlags( Qt::NoTextInteraction );
    QFont titleFont = titleLabel_->font();
    titleFont.setBold( true );
    titleLabel_->setFont( titleFont );
    layout->addWidget( titleLabel_ );

    // ── 파일 모드 위젯 ──
    pathLabel_ = new QLabel( this );
    pathLabel_->setTextInteractionFlags( Qt::NoTextInteraction );
    pathLabel_->setAlignment( Qt::AlignTop | Qt::AlignLeft );
    // 워드랩 대신 줄마다 가운데를 생략한다. 줄 수가 내용에 따라 변하지 않아야
    // 파일 모드의 높이 고정이 성립한다.
    pathLabel_->setWordWrap( false );
    QFont smallFont = pathLabel_->font();
    smallFont.setPointSize( qMax( smallFont.pointSize() - 1, 7 ) );
    pathLabel_->setFont( smallFont );
    layout->addWidget( pathLabel_ );

    preview_ = new CompletionPreviewCanvas( this );
    layout->addWidget( preview_, 0, Qt::AlignHCenter );

    metaLabel_ = new QLabel( this );
    metaLabel_->setTextInteractionFlags( Qt::NoTextInteraction );
    metaLabel_->setFont( smallFont );
    layout->addWidget( metaLabel_ );

    // ── 텍스트 모드 위젯 ──
    bodyLabel_ = new QLabel( this );
    bodyLabel_->setWordWrap( true );
    bodyLabel_->setAlignment( Qt::AlignTop | Qt::AlignLeft );
    bodyLabel_->setTextInteractionFlags( Qt::NoTextInteraction );
    layout->addWidget( bodyLabel_, 1 );

    sourceLabel_ = new QLabel( this );
    sourceLabel_->setWordWrap( true );
    sourceLabel_->setTextInteractionFlags( Qt::NoTextInteraction );
    sourceLabel_->setFont( smallFont );
    QPalette dimmed = sourceLabel_->palette();
    QColor dimColor = dimmed.color( QPalette::Active, QPalette::WindowText );
    dimColor.setAlpha( 150 );
    dimmed.setColor( QPalette::All, QPalette::WindowText, dimColor );
    sourceLabel_->setPalette( dimmed );
    pathLabel_->setPalette( dimmed );
    metaLabel_->setPalette( dimmed );
    layout->addWidget( sourceLabel_ );
    // 파일 모드는 높이가 고정이라 남는 공간이 생긴다. 스트레치가 없으면 라벨들이
    // 그 공간을 나눠 가져 경로 블록과 프리뷰 상자 사이가 벌어진다.
    layout->addStretch( 0 );

    applyMode( Mode::Text );
}

void CompletionDetailPopup::applyMode( const Mode mode )
{
    mode_ = mode;
    const bool file = mode == Mode::File;

    pathLabel_->setVisible( file );
    preview_->setVisible( file );
    metaLabel_->setVisible( file );
    bodyLabel_->setVisible( !file );
    sourceLabel_->setVisible( !file );
}

QSize CompletionDetailPopup::previewBoxSize() const
{
    return { kPreviewWidth, kPreviewHeight };
}

bool CompletionDetailPopup::setContent( const QString& title, const QString& body,
                                        const QString& source )
{
    const QString trimmedTitle = title.trimmed();
    const QString trimmedBody = body.trimmed();
    if( trimmedTitle.isEmpty() && trimmedBody.isEmpty() )
    {
        titleLabel_->clear();
        bodyLabel_->clear();
        sourceLabel_->clear();
        ++contentToken_;
        return false;
    }

    ++contentToken_;
    applyMode( Mode::Text );

    titleLabel_->setText( trimmedTitle );
    titleLabel_->setVisible( !trimmedTitle.isEmpty() );
    bodyLabel_->setText( trimmedBody );
    bodyLabel_->setVisible( !trimmedBody.isEmpty() );
    sourceLabel_->setText( source.trimmed() );
    sourceLabel_->setVisible( !source.trimmed().isEmpty() );

    resizeToContent();
    return true;
}

quint64 CompletionDetailPopup::setFileContent( const CompletionFileDetail& detail )
{
    ++contentToken_;
    applyMode( Mode::File );

    titleLabel_->setText( detail.fileName );
    titleLabel_->setVisible( !detail.fileName.isEmpty() );

    // 경로 블록은 두 줄 고정이다. 줄마다 가운데를 생략해 높이가 변하지 않게 한다.
    const int textWidth = kPanelWidth - 20;
    const QFontMetrics pathMetrics( pathLabel_->font() );
    const QString directory =
        pathMetrics.elidedText( detail.directoryText, Qt::ElideMiddle, textWidth );
    const QString name =
        pathMetrics.elidedText( QStringLiteral( "  " ) + detail.fileName, Qt::ElideMiddle, textWidth );
    pathLabel_->setText( directory + QLatin1Char( '\n' ) + name );

    preview_->setVisible( detail.showPreviewBox );
    preview_->setContent( detail.preview, detail.hasAlpha, detail.note );
    metaLabel_->setText( detail.metaLine );

    setFixedWidth( kPanelWidth );
    setFixedHeight( kFilePanelHeight );
    return contentToken_;
}

void CompletionDetailPopup::applyPreview( const quint64 token, const QPixmap& preview,
                                          const bool hasAlpha, const QString& metaLine,
                                          const QString& note )
{
    // 뒤늦게 도착한 프리뷰가 다른 항목 위에 붙는 것을 막는 마지막 방어선이다.
    if( token != contentToken_ || mode_ != Mode::File )
        return;

    preview_->setContent( preview, hasAlpha, note );
    if( !metaLine.isEmpty() )
        metaLabel_->setText( metaLine );
}

bool CompletionDetailPopup::hasContent() const
{
    if( mode_ == Mode::File )
        return !titleLabel_->text().isEmpty();
    return !titleLabel_->text().isEmpty() || !bodyLabel_->text().isEmpty();
}

void CompletionDetailPopup::resizeToContent()
{
    setFixedWidth( kPanelWidth );
    // heightForWidth 를 쓰려면 폭이 먼저 확정돼야 한다.
    const int hint = layout() != nullptr ? layout()->totalHeightForWidth( kPanelWidth ) : sizeHint().height();
    const int height = hint > 0 ? hint : sizeHint().height();
    setFixedHeight( qBound( 48, height, kMaxPanelHeight ) );
}

QPoint CompletionDetailPopup::clampToScreen( QPoint target, const QPoint& reference ) const
{
    const QScreen* screen = QGuiApplication::screenAt( reference );
    if( screen == nullptr )
        screen = QGuiApplication::primaryScreen();
    if( screen == nullptr )
        return target;

    const QRect available = screen->availableGeometry();
    target.setX( qBound( available.left(), target.x(), qMax( available.left(), available.right() - width() ) ) );
    target.setY( qBound( available.top(), target.y(), qMax( available.top(), available.bottom() - height() ) ) );
    return target;
}

void CompletionDetailPopup::showBesideAnchor( const QRect& anchor )
{
    if( !hasContent() )
    {
        hide();
        return;
    }

    const QScreen* screen = QGuiApplication::screenAt( anchor.topRight() );
    if( screen == nullptr )
        screen = QGuiApplication::primaryScreen();
    const QRect available = screen != nullptr ? screen->availableGeometry() : QRect{};

    // 오른쪽 → 왼쪽 → 아래 → 위. 앞의 둘만 보고 실패하면 clampToScreen 이
    // 화면 안으로 밀어 넣는데, 목록은 720px 까지 넓어질 수 있어서 그때
    // 상세 패널이 목록 위에 겹친다 — 둘 다 못 읽게 된다.
    const QPoint candidates[] = {
        { anchor.right() + kAnchorGap, anchor.top() },
        { anchor.left() - kAnchorGap - width(), anchor.top() },
        { anchor.left(), anchor.bottom() + kAnchorGap },
        { anchor.left(), anchor.top() - kAnchorGap - height() },
    };

    QPoint target = clampToScreen( candidates[ 0 ], anchor.topRight() );
    for( const QPoint& candidate : candidates )
    {
        const QRect box( candidate, size() );
        if( !available.isNull() && !available.contains( box ) )
            continue;
        if( box.intersects( anchor ) )
            continue;
        target = candidate;
        break;
    }

    // 어디에도 온전히 들어가지 않았다. 밀어 넣되 목록을 덮지 않는 쪽을 고른다.
    if( QRect( target, size() ).intersects( anchor ) )
    {
        for( const QPoint& candidate : candidates )
        {
            const QPoint clamped = clampToScreen( candidate, anchor.center() );
            if( !QRect( clamped, size() ).intersects( anchor ) )
            {
                target = clamped;
                break;
            }
        }
    }

    // 위치가 그대로면 건드리지 않는다. Windows 에서 move+show+raise 를 반복하면
    // 방향키를 훑을 때마다 패널이 깜빡인다.
    if( isVisible() && pos() == target )
        return;

    move( target );
    show();
    raise();
}

void CompletionDetailPopup::showNearPoint( const QPoint& globalPos )
{
    if( !hasContent() )
    {
        hide();
        return;
    }

    // 커서 바로 아래 오른쪽. 마우스 포인터가 패널을 가리지 않게 조금 띄운다.
    move( clampToScreen( globalPos + QPoint( 16, 20 ), globalPos ) );
    show();
    raise();
}

}  // namespace hc::rst
