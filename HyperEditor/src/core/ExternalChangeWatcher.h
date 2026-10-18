#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class QFileSystemWatcher;
class QTimer;

namespace mrst {

/// 열어 둔 파일이 **다른 프로그램에 의해** 바뀌었는지 지켜본다.
///
/// 발견 수단이 두 가지다.
///
/// * `Detection::Notify` — `QFileSystemWatcher`. Windows 구현은
///   `FindFirstChangeNotification` 으로 **디렉터리 변경 알림**을 받는 전용
///   스레드다(qtbase/qfilesystemwatcher_win.cpp). 즉 우리가 주기적으로 파일을
///   찍어 보는 것이 아니라 커널이 깨워 준다 — `ReadDirectoryChangesW` 와 같은
///   커널 통보 계열이고, 대기 중에는 비용이 0 이다.
/// * `Detection::Poll` — 타이머로 파일 정보를 다시 읽는다. 네트워크 드라이브나
///   가상 파일 시스템처럼 알림이 오지 않는 곳에서도 반드시 발견된다.
///
/// **NTFS 변경 저널(USN journal)은 후보에서 뺐다.** 저널을 읽으려면 볼륨
/// 핸들(`\.\C:`)을 열어야 하고 그것은 관리자 권한을 요구한다. 편집기를 관리자로
/// 띄우게 만드는 값은 아니고, 애초에 저널은 볼륨 전체의 변경을 훑는 도구라
/// "열려 있는 파일 몇 개" 를 보는 데는 과하다.
///
/// ── 오탐을 막는 두 장치 ──
///
/// 1. **안정화 대기(settle)**. 알림 한 번에 곧바로 알리지 않고, 간격을 두고 두 번
///    잰 값이 같을 때만 알린다. 큰 파일을 쓰는 중에는 크기가 계속 자라므로 이
///    조건이 성립하지 않는다. 이것이 없으면 **반쯤 쓰인 파일을 불러온다.**
///    같은 장치가 "지우고 다시 만드는" 저장 방식(임시 파일 + rename, 다른
///    편집기의 기본 동작)을 삭제가 아니라 변경으로 보게 만든다.
/// 2. **자기 쓰기 구간(self write)**. 우리가 저장하는 동안 온 알림은 버린다.
///    저장 직후에 "밖에서 바뀌었습니다" 를 띄우는 것만큼 신뢰를 깎는 것이 없다.
class ExternalChangeWatcher final : public QObject
{
    Q_OBJECT

public:
    /// 변경을 발견했을 때 무엇을 할지. 판단은 이 클래스가 하지 않는다 —
    /// 저장하지 않은 편집이 있는지 아는 것은 뷰를 가진 쪽이다.
    enum class Action
    {
        Ignore = 0,     ///< 감시 자체를 하지 않는다
        Reload = 1,     ///< 기본값
        Ask    = 2,
    };
    Q_ENUM( Action )

    enum class Detection
    {
        Notify = 0,     ///< OS 변경 알림 (기본값)
        Poll   = 1,
    };
    Q_ENUM( Detection )

    /// "그때 우리가 읽은 그 파일인가" 를 판정하는 값.
    ///
    /// 내용 해시가 아니라 크기+수정시각이다. 해시는 대용량 파일에서 감시
    /// 자체가 부담이 되고, 이 값은 TextShadowBackupStore 가 핫 엑시트 백업의
    /// 유효성을 판정할 때 이미 쓰고 있는 것과 같은 기준이다.
    struct Fingerprint
    {
        bool   exists = false;
        qint64 size = -1;
        qint64 lastModifiedUtcMs = 0;

        bool operator==( const Fingerprint& other ) const = default;
    };

    explicit ExternalChangeWatcher( QObject* parent = nullptr );
    ~ExternalChangeWatcher() override;

    // ── 설정 (AppSettings) ──
    static Action      configuredAction();
    static Detection   configuredDetection();
    static int         configuredPollSeconds();
    static int         defaultPollSeconds();
    static int         minPollSeconds();
    static int         maxPollSeconds();
    static Fingerprint fingerprintOf( const QString& filePath );

    /// 설정을 다시 읽어 즉시 반영한다. 감시를 켜는 전환이면 지금 디스크 상태를
    /// 새 기준으로 삼는다 — 꺼 두었던 동안의 변경까지 몰아서 알리면, 설정을
    /// 켠 순간 대화상자가 줄줄이 뜬다.
    void reloadSettings();

    void setAction( Action action );
    void setDetection( Detection detection );
    void setPollSeconds( int seconds );
    /// 안정화 대기 시간. 테스트에서 줄이기 위한 손잡이다.
    void setSettleInterval( int milliseconds );

    /// 감시 대상을 이 목록으로 맞춘다. 없어진 것은 빼고 새것은 넣는다.
    ///
    /// 탭이 열리고 닫히고 "다른 이름으로 저장" 으로 경로가 바뀌는 것을 각각
    /// 추적하는 대신, 탭 목록에서 이 목록을 만들어 통째로 넘기는 쪽이 어긋날
    /// 여지가 없다.
    void setWatchedFiles( const QStringList& filePaths );

    /// 지금 디스크 상태를 새 기준으로 삼는다. 우리가 저장하거나 다시 불러온 뒤.
    void markSynchronized( const QString& filePath );

    /// 우리가 이 파일을 쓰기 시작한다 / 다 썼다. 사이에 온 알림은 버린다.
    void beginSelfWrite( const QString& filePath );
    void endSelfWrite( const QString& filePath, bool synchronized = true );

    /// 전부 다시 확인한다. 창이 다시 활성화될 때 부른다 — 알림이 오지 않는
    /// 경로(네트워크 드라이브, 가상 파일 시스템)에서 마지막 그물이 된다.
    void recheckAll();

    [[nodiscard]] bool        isEnabled() const { return action_ != Action::Ignore; }
    [[nodiscard]] Action      action() const { return action_; }
    [[nodiscard]] Detection   detection() const { return detection_; }
    [[nodiscard]] int         pollSeconds() const { return pollSeconds_; }
    [[nodiscard]] QStringList watchedFiles() const;

signals:
    void sigFileChanged( const QString& filePath );
    void sigFileVanished( const QString& filePath );

private:
    struct Entry
    {
        QString     path;               ///< 신호에 실어 보낼 표기 (절대 경로)
        Fingerprint baseline;           ///< 우리가 아는 상태
        Fingerprint probe;              ///< 직전 확인에서 본 상태
        bool        probing = false;
        int         selfWrites = 0;
        /// 자기 쓰기 구간이 언제까지 유효한가 (단조 시계, ms).
        ///
        /// endSelfWrite() 를 못 받으면 그 파일은 **영원히** 감시에서 빠진다. 짝이
        /// 어긋날 수 있는 경로가 실재하므로(저장 완료 신호가 뷰 파괴와 겹치는 등)
        /// 시한을 둔다. 사람이 저장 한 번을 기다릴 수 있는 시간보다 넉넉하다.
        qint64      selfWriteDeadlineMs = 0;
        /// OS 알림을 걸지 못했다. 이 항목만 폴링으로 본다.
        bool        pollFallback = false;
        /// 이 항목이 상위 디렉터리 감시의 참조를 하나 들고 있는가.
        ///
        /// attachOsWatch() 는 여러 경로에서 되풀이 호출된다(기준 갱신, 방식 전환,
        /// 안정화 확인). 그때마다 참조를 더하면 수가 절대 0 으로 돌아오지 않아
        /// 디렉터리 감시가 영원히 남는다. 그래서 항목당 한 번만 센다.
        bool        holdsDirRef = false;
    };

    /// Windows 는 경로 대소문자를 구분하지 않는다. 같은 파일이 두 항목이 되면
    /// 알림은 한쪽에만 오고 다른 쪽은 영원히 낡은 기준을 들고 있게 된다.
    static QString keyFor( const QString& filePath );
    /// 자기 쓰기 구간이 아직 유효한가. 시한이 지났으면 카운터를 되돌린다.
    static bool consumeSelfWrite( Entry& entry );

    void onWatchedFileChanged( const QString& filePath );
    void onWatchedDirectoryChanged( const QString& dirPath );
    void onSettleTick();
    void onPollTick();

    void scheduleProbe( const QString& key );
    void rebaselineAll();
    void applyDetection();
    void attachOsWatch( Entry& entry );
    void detachOsWatch( Entry& entry );
    void retainDirectory( const QString& dirPath );
    void releaseDirectory( const QString& dirPath );
    void updatePollTimerState();

    QFileSystemWatcher*     osWatcher_ = nullptr;
    QTimer*                 settleTimer_ = nullptr;
    QTimer*                 pollTimer_ = nullptr;
    QHash< QString, Entry > entries_;        ///< keyFor() -> 항목
    QHash< QString, int >   dirRefs_;        ///< keyFor(디렉터리) -> 참조 수
    QSet< QString >         pending_;        ///< 안정화 확인을 기다리는 키
    Action                  action_ = Action::Reload;
    Detection               detection_ = Detection::Notify;
    int                     pollSeconds_ = 3;
    int                     settleMs_ = 250;
    /// onSettleTick() 안에서 신호를 내면 받는 쪽이 모달 대화상자를 띄울 수 있고,
    /// 그 이벤트 루프에서 이 타이머가 다시 발화한다. 재진입을 막지 않으면 같은
    /// 항목을 두 번 알린다.
    bool                    inSettleTick_ = false;
};

} // namespace mrst
