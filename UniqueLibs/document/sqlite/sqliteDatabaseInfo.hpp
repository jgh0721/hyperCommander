#pragma once

/// 고유 라이브러리
#include "sqliteConnection.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <functional>
#include <memory>
#include <QDialog>
#include <QFutureWatcher>

class QFormLayout;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace hc::sqlite
{
    // 데이터베이스 정보 · 유지 관리(설계 7절, 시안 ‘테이블 수정 · 데이터베이스 정보’ 의 오른쪽 창).
    // 파일(경로 · 크기 · 페이지 · 여유 페이지), 형식 · PRAGMA(인코딩 · 저널 모드 · 동기화 · 자동 VACUUM · user_version ·
    // application_id · foreign_keys), 암호화(SEE), SQLite(버전 · 컴파일 옵션), 유지 관리(무결성 · 외래 키 검사 · ANALYZE ·
    // 최적화 · VACUUM · WAL 체크포인트). 유지 관리는 작업 스레드의 새 연결에서 돌리고 결과를 아래 줄에 남긴다.
    class DatabaseInfoDialog final : public QDialog
    {
        Q_OBJECT
    public:
        using Opener = std::function<std::unique_ptr<Connection>(QString*)>;
        // Gui 는 문서의 연결(PRAGMA 읽기 · 바꾸기), Open 은 유지 관리용 새 연결. HasPending 이 참이면 VACUUM 전에 커밋을 묻는다.
        DatabaseInfoDialog(Connection& Gui, Opener Open, const QString& Path, std::function<bool()> CommitPending, bool ReadOnly, QWidget* Parent = nullptr);
        ~DatabaseInfoDialog() override;
        // 시험: 유지 관리를 이름으로(integrity · foreignKeys · analyze · optimize · vacuum · checkpoint) 돌리고 끝까지 기다린다.
        QString RunMaintenance(const QString& Task, bool Wait);
        QString LastResult() const;
        // 암호화 칸(설계 8): 방식(비면 평문) · 저장한 키 글과 ‘암호화…’ · ‘관리…’ 동작. 다시 그릴 때마다 묻는다.
        struct Encryption
        {
            QString method_, savedKey_;
            std::function<bool()> change_;      // 바꿨으면 true(정보를 다시 그린다)
            std::function<void()> manageKeys_;
        };
        void SetEncryption(std::function<Encryption()> Provider);
    signals:
        // VACUUM · 저널 모드처럼 파일 · 스키마가 바뀌었다(작업 창이 다시 읽는다).
        void DatabaseChanged();
    private:
        void build();
        void refresh();
        void runTask(const QString& Task);
        void finishTask(QString Task, const QString& Result, bool Ok);
        QString pragma(const QString& Name) const;

        Connection& gui_;
        Opener open_;
        QString path_;
        std::function<bool()> commitPending_;
        std::function<Encryption()> encryption_;
        bool readOnly_ = false;
        QVBoxLayout* body_ = nullptr;
        QLabel* result_ = nullptr;
        QList<QPushButton*> tasks_;
        QFutureWatcher<QPair<bool, QString>>* watcher_ = nullptr;
        QString running_, last_;
        bool lastOk_ = true;
    };
}
