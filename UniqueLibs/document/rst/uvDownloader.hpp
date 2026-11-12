#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

namespace hc::rst {

/// 제품이 검증한 uv 릴리스. 내려받은 압축 파일의 SHA-256 이 여기 적힌 값과 같아야 쓴다.
///
/// uv 는 제품에 넣지 않는다( 압축 파일 15 MB, 실행 파일 60 MB 남짓 ). reStructuredText 미리보기를 처음 쓸 때
/// astral.sh 의 공식 배포처에서 받는다. 버전을 올릴 때는 세 값을 함께 바꾼다 — 해시는 릴리스의
/// `uv-x86_64-pc-windows-msvc.zip.sha256` 에 있다.
struct UvRelease
{
    static constexpr const char* Version = "0.13.7";
    static constexpr const char* ArchiveName = "uv-x86_64-pc-windows-msvc.zip";
    static constexpr const char* ArchiveSha256 = "0cdc5b38fe5a8168ed6a25814f5ba43c21d313dd3187314fccb37b7cec46e021";
    /// 받는 순서. 공식 설치 스크립트( uv-installer.ps1 )와 같다: astral.sh 의 배포처, 실패하면 GitHub 릴리스.
    [[nodiscard]] static QStringList DownloadUrls();
};

/// ZIP 보관 파일에서 이름이 EntryName 인 항목( 폴더 안에 있어도 된다 ) 하나를 꺼낸다.
/// 저장( 0 )과 deflate( 8 )만 받고 CRC-32 를 확인한다. 실패하면 빈 값을 돌려주고 Error 에 이유를 적는다.
[[nodiscard]] QByteArray ExtractZipEntry( const QByteArray& Archive, const QString& EntryName, QString* Error = nullptr );

/// uv 실행 파일을 내려받아 Target 에 놓는다. 한 번에 한 건만 돈다.
class UvDownloader final : public QObject
{
    Q_OBJECT

public:
    explicit UvDownloader( QObject* Parent = nullptr );
    ~UvDownloader() override;

    /// Target 옆의 표식( uv.version )이 UvRelease::Version 이고 Target 이 있으면 참.
    [[nodiscard]] static bool IsInstalled( const QString& Target );
    /// 표식에 적힌 버전. 없으면 빈 문자열.
    [[nodiscard]] static QString InstalledVersion( const QString& Target );

    void Start( const QString& Target );
    void Cancel();
    [[nodiscard]] bool IsRunning() const;

signals:
    void Progress( qint64 Received, qint64 Total );
    void Log( const QString& Text );
    void Finished( bool Ok, const QString& ErrorMessage );

private:
    void tryNext();
    void onFinished();
    void fail( const QString& Message );

    QNetworkAccessManager*      network_ = nullptr;
    QPointer< QNetworkReply >   reply_;
    QStringList                 urls_;
    QString                     target_;
    QString                     lastError_;
    bool                        cancelled_ = false;
};

}  // namespace hc::rst
