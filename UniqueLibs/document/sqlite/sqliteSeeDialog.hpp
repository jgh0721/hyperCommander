#pragma once

/// 고유 라이브러리
#include "sqliteConnection.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <functional>
#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QToolButton;

namespace fm::ui { class SegmentedControl; }

namespace hc::sqlite
{
    // 키 칸(눈 단추로 보이기 · 숨기기). 닫을 때 글을 지운다.
    class SecretField final : public QWidget
    {
        Q_OBJECT
    public:
        explicit SecretField(const QString& Name, QWidget* Parent = nullptr);
        ~SecretField() override;
        QLineEdit* Edit() const { return edit_; }
        QByteArray Take();          // 글을 UTF-8 로 내주고 칸을 비운다
        void Wipe();
        void SetError(bool Error);
    private:
        QLineEdit* edit_ = nullptr;
        QToolButton* eye_ = nullptr;
    };

    // 암호화된 데이터베이스 열기(설계 8.2, 시안 ‘SEE’ 보드 · 2026-11-13 결정: 이 모양 그대로).
    // 키 · 키 형식(암호문 · 문자열 · 16진) · 알고리즘(AES-256 고정 표시) · 키를 이 PC 에 저장 · 읽기 전용으로 열기.
    // ‘열기’ 는 Attempt 로 키를 시도하고, 틀리면 대화를 닫지 않고 시도한 조합과 함께 ‘다시 시도’ 로 바꾼다.
    class SeeKeyDialog final : public QDialog
    {
        Q_OBJECT
    public:
        // 빈 글이면 열었다. SQLITE_NOTADB 면 WrongKey, 그 밖의 오류는 글.
        struct Outcome { bool ok_ = false, wrongKey_ = false; QString error_; };
        using Attempt = std::function<Outcome(const QByteArray& Key, KeyFormat Format, bool ReadOnly)>;
        SeeKeyDialog(const QString& Path, KeyFormat Format, bool Save, bool ReadOnly, Attempt Try, QWidget* Parent = nullptr);

        KeyFormat Format() const;
        void SetFormat(KeyFormat Format);
        bool SaveKey() const;
        bool ReadOnly() const;
        // 열 때 쓴 키(Accepted 뒤 한 번만). 저장하고 나면 호출한 쪽이 WipeKey 로 지운다.
        QByteArray TakeKey();
        // 시험 · 자동화.
        SecretField* Key() const { return key_; }
        bool ShowingError() const;
        QPushButton* OpenButton() const { return open_; }
        void accept() override;
    private:
        void showError(const Outcome& Result, KeyFormat Format);

        Attempt try_;
        SecretField* key_ = nullptr;
        fm::ui::SegmentedControl* format_ = nullptr;
        QCheckBox* save_ = nullptr;
        QCheckBox* readOnly_ = nullptr;
        QWidget* error_ = nullptr;
        QLabel* errorText_ = nullptr;
        QLabel* subtitle_ = nullptr;
        QPushButton* open_ = nullptr;
        QByteArray accepted_;
        QString fileName_;
    };

    // 데이터베이스 › 암호화…(시안 ‘SEE’ 보드의 ‘암호화 설정’). 암호화된 파일은 키 바꾸기 · 암호화 해제,
    // 평문 파일은 암호화. 새 키는 두 번 넣고, ‘먼저 백업 사본 만들기’ 를 고를 수 있다.
    class SeeEncryptDialog final : public QDialog
    {
        Q_OBJECT
    public:
        enum class Operation { Change, Decrypt, Encrypt };
        SeeEncryptDialog(const QString& Path, bool Encrypted, KeyFormat Current, KeyFormat DefaultFormat, bool Backup, QWidget* Parent = nullptr);
        Operation Chosen() const;
        void Choose(Operation Value);
        KeyFormat Format() const;
        void SetFormat(KeyFormat Format);
        bool Backup() const;
        QString BackupPath() const { return backupPath_; }
        // 새 키(해제면 빈 값). 한 번만 내주고 칸을 지운다.
        QByteArray TakeKey();
        SecretField* NewKey() const { return key_; }
        SecretField* ConfirmKey() const { return confirm_; }
        QPushButton* ApplyButton() const { return apply_; }
        QString Problem() const;
        void accept() override;
    private:
        void refresh();

        bool encrypted_ = false;
        QRadioButton* change_ = nullptr;
        QRadioButton* decrypt_ = nullptr;
        SecretField* key_ = nullptr;
        SecretField* confirm_ = nullptr;
        fm::ui::SegmentedControl* format_ = nullptr;
        QCheckBox* backup_ = nullptr;
        QLabel* problem_ = nullptr;
        QPushButton* apply_ = nullptr;
        QString backupPath_;
        QByteArray taken_;
    };

    // 16진 키가 맞는 꼴인가(짝수 길이 · 16진 글자만).
    bool ValidHexKey(const QByteArray& Key);
    // 다른 연결이 없을 때 Db 의 암호화를 바꾼다(설계 8.1): 키가 있으면 rekey(평문 파일은 암호화), 비면 해제.
    // 이 SEE 는 평문 · WAL 파일도 rekey 한 번으로 바뀐다(2026-11-14 확인, VACUUM 필요 없음).
    bool ChangeEncryption(Connection& Db, const QByteArray& Key, KeyFormat Format, QString* Error);
    // 백업 사본 경로: <파일>.bak, 있으면 .bak2 · .bak3 ….
    QString BackupPathFor(const QString& Path);
    // 바이트 그대로 복사한다(암호화 상태 · 키도 그대로). WAL 이면 먼저 체크포인트한다.
    bool CopyDatabaseFile(Connection& Db, const QString& Target, QString* Error);
}
