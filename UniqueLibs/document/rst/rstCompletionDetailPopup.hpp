#pragma once

#include <QFrame>
#include <QPixmap>
#include <QSize>
#include <QString>

class QLabel;

namespace hc::rst {

class CompletionPreviewCanvas;

/// 상세 패널의 **파일 모드**에 넣을 것. 값 타입이다.
struct CompletionFileDetail
{
    QString                             fileName;        ///< 제목 줄
    QString                             directoryText;   ///< 경로 블록 첫 줄 ("../images/ui/")
    QString                             metaLine;        ///< "PNG · 512×512 · 34.2KB"
    QString                             note;            ///< 프리뷰 자리에 대신 보일 안내
    QPixmap                             preview;         ///< 비면 안내나 빈 상자
    bool                                hasAlpha = false;///< true 면 체커보드를 깐다
    bool                                showPreviewBox = true;
};

/// 자동완성 목록 옆에 붙는 상세 패널.
///
/// 목록 행 안에 오른쪽 정렬로 그리던 `detail` 은 행 폭의 2/5 로 잘려서
/// (`CompletionItemDelegate::paint`) 용어집 정의처럼 긴 글을 보여 줄 수 없다.
/// 이 패널은 그 내용을 별도 창에 줄바꿈해서 담는다.
///
/// 모드가 둘이다. **텍스트 모드**(용어집 정의, 본문 호버)와 **파일 모드**
/// (경로 후보). 위젯 구성도 크기 규칙도 비동기 여부도 달라서 한 함수에 인자를
/// 더하면 두 규칙이 서로를 밟는다.
///
/// **포커스를 가져가지 않는다.** 캐럿은 편집기에 남아 있어야 한다.
class CompletionDetailPopup final : public QFrame
{
    Q_OBJECT

public:
    explicit CompletionDetailPopup( QWidget* parent = nullptr );

    /// 표시할 내용을 채운다. title 과 body 가 모두 비면 false 를 돌려주고
    /// 아무것도 표시하지 않는다 (호출 측은 이때 숨기면 된다).
    bool                                setContent( const QString& title, const QString& body,
                                                    const QString& source = {} );

    /// 파일/디렉터리 모드. 프리뷰 없이 뼈대(이름·경로)만 먼저 채워도 된다.
    ///
    /// 돌려주는 값은 "지금 무엇을 보여 주고 있는가" 의 세대 번호다. 뒤늦게
    /// 도착한 프리뷰는 이 번호로 걸러진다 — 그 판단을 할 수 있는 객체가
    /// 여기뿐이라 가드를 여기에 둔다. QPointer 만으로는 부족하다. 패널은
    /// 살아 있고 내용만 바뀐 경우가 정확히 그 버그이기 때문이다.
    quint64                             setFileContent( const CompletionFileDetail& detail );

    /// 뒤늦게 도착한 프리뷰/메타를 붙인다. token 이 현재 세대가 아니면 버린다.
    void                                applyPreview( quint64 token, const QPixmap& preview,
                                                      bool hasAlpha, const QString& metaLine,
                                                      const QString& note );

    /// 프리뷰 상자의 논리 픽셀 크기. 로더가 축소 목표를 정할 때 쓴다.
    [[nodiscard]] QSize                 previewBoxSize() const;

    /// anchor 오른쪽 바깥에 붙여 띄운다. 화면을 벗어나면 왼쪽/아래/위로 옮긴다.
    /// anchor 는 전역 좌표 (보통 자동완성 팝업의 geometry()).
    void                                showBesideAnchor( const QRect& anchor );
    /// 마우스 커서 근처에 띄운다 (본문 호버용).
    void                                showNearPoint( const QPoint& globalPos );

    [[nodiscard]] bool                  hasContent() const;

private:
    enum class Mode
    {
        Text,
        File,
    };

    /// 내용에 맞춰 크기를 잡는다. 폭은 고정, 높이는 본문 길이에 따른다.
    void                                resizeToContent();
    /// 화면 경계 안으로 밀어 넣는다.
    [[nodiscard]] QPoint                clampToScreen( QPoint target, const QPoint& reference ) const;
    void                                applyMode( Mode mode );

    QLabel*                             titleLabel_ = nullptr;
    QLabel*                             bodyLabel_ = nullptr;
    QLabel*                             sourceLabel_ = nullptr;

    // ── 파일 모드 ──
    QLabel*                             pathLabel_ = nullptr;
    CompletionPreviewCanvas*            preview_ = nullptr;
    QLabel*                             metaLabel_ = nullptr;

    Mode                                mode_ = Mode::Text;
    /// setContent / setFileContent 마다 증가한다.
    quint64                             contentToken_ = 0;
};

}  // namespace hc::rst
