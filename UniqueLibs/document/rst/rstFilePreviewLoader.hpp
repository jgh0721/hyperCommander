#pragma once

#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QSize>
#include <QString>

#include <atomic>

class QTimer;

namespace hc::rst {

/// 상세 패널에 붙일 파일 미리보기를 만든다.
///
/// **디스크를 만지는 모든 일**(헤더 읽기, 디코딩, stat)을 워커로 보낸다.
/// 죽은 네트워크 드라이브 경로에서 GUI 가 통째로 멈추지 않게 하려는 것이 첫째
/// 이유고, 큰 PNG 를 방향키마다 디코딩하지 않으려는 것이 둘째다.
///
/// 늦게 도착한 결과가 다른 항목 위에 붙는 것을 3중으로 막는다:
/// 여기 원자 세대 번호(디코딩 전·송신 전 2회 검사), 상세 패널의 contentToken_,
/// 그리고 QPointer. **QPointer 만으로는 부족하다** — 패널은 살아 있고 내용만
/// 바뀐 경우가 정확히 그 버그다.
class FilePreviewLoader final : public QObject
{
    Q_OBJECT

public:
    struct Result
    {
        quint64                         token = 0;
        QPixmap                         pixmap;      ///< 비면 프리뷰 없음
        bool                            hasAlpha = false;
        QString                         metaLine;
        QString                         note;        ///< 실패/거부 사유 (이미 번역됨)
    };

    explicit FilePreviewLoader( QObject* parent = nullptr );

    /// token 은 CompletionDetailPopup::setFileContent() 가 돌려준 값 그대로.
    /// 캐시에 있으면 그 자리에서 previewReady 를 낸다(디스크를 만지지 않는다).
    void                                request( quint64 token, const QString& absolutePath,
                                                 const QSize& boxLogical, qreal devicePixelRatio,
                                                 bool isDirectory );
    /// 팝업이 닫혔다. 대기 중인 요청을 무효화한다.
    void                                cancel();

signals:
    void                                previewReady( const hc::rst::FilePreviewLoader::Result& result );

private:
    /// 워커가 돌려주는 것. **문자열이 없다** — tr() 을 워커에서 부르지 않는다.
    /// 이 앱은 재시작 없이 언어를 바꾸므로 installTranslator 와 겹칠 수 있다.
    struct Probe
    {
        QImage                          image;
        QByteArray                      format;
        QSize                           sourceSize;
        int                             frameCount = 1;
        qint64                          bytes = 0;
        bool                            exists = false;
        bool                            tooBig = false;
        bool                            unreadable = false;
        bool                            isDirectory = false;
    };

    /// 캐시에 남길 값싼 부분. 픽스맵은 QPixmapCache 가 따로 들고 있다.
    struct Meta
    {
        bool                            hasAlpha = false;
        QString                         metaLine;
        QString                         note;
    };

    void                                fire();
    void                                deliver( quint64 token, const QString& cacheKey,
                                                 const Probe& probe, qreal devicePixelRatio );
    [[nodiscard]] static QString        cacheKeyFor( const QString& path, const QSize& box,
                                                     qreal devicePixelRatio );

    QTimer*                             debounce_ = nullptr;
    quint64                             pendingToken_ = 0;
    QString                             pendingPath_;
    QSize                               pendingBox_;
    qreal                               pendingDpr_ = 1.0;
    bool                                pendingIsDirectory_ = false;

    /// 워커가 협조적으로 물러나기 위한 표식. QThreadPool 은 실행 중 취소가 없다.
    std::atomic< quint64 >              latest_{ 0 };
    /// 경로+상자+dpr -> 값싼 메타. 픽스맵은 QPixmapCache 에 있다.
    QHash< QString, Meta >              metaCache_;
};

}  // namespace hc::rst
