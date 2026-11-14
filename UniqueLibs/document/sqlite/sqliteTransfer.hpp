#pragma once

/// 고유 라이브러리
#include "sqliteConnection.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <atomic>
#include <QDialog>
#include <QStringList>
#include <QVariant>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTableWidget;

namespace fm::ui { class SegmentedControl; }

namespace hc::sqlite
{
    struct SchemaInfo;

    // 가져오기 · 내보내기(설계 10절 P4, 시안: 스키마 메뉴 ‘가져오기 (CSV · JSON)…’ · ‘내보내기 (CSV · JSON · SQL)…’,
    // 쿼리 탭 도구 모음 · 결과 표 도구의 ‘내보내기’). 파일 읽기 · 쓰기와 SQL 은 작업 스레드 연결에서 한다.
    enum class TransferFormat { Csv, Json, Sql };

    struct TransferResult
    {
        bool ok_ = false, canceled_ = false;
        qint64 rows_ = 0;
        QString error_;
    };

    struct ExportRequest
    {
        TransferFormat format_ = TransferFormat::Csv;
        QString path_;
        // 읽을 SELECT. 비면 columns_ · rows_(결과에서 이미 읽은 행)를 쓴다.
        QString sql_;
        QStringList columns_;
        QVector<QVector<QVariant>> rows_;
        QString source_;                // 대화에 보일 원본 이름(테이블 이름 · ‘쿼리 결과’)
        // SQL: INSERT 대상 이름과 앞에 넣을 CREATE 문(테이블 내보내기에서 고르면).
        QString table_;
        QString createSql_;
        // CSV
        bool header_ = true;
        QChar delimiter_ = u',';
        bool bom_ = true;               // UTF-8 BOM(엑셀이 한글을 바로 읽는다)
    };

    // CSV 읽기 설정. 구분자 0 은 첫 줄로 알아본다(쉼표 · 세미콜론 · 탭 · 세로줄).
    struct CsvOptions
    {
        QChar delimiter_ = QChar(0);
        bool header_ = true;
        bool emptyIsNull_ = true;       // 따옴표 없는 빈 칸은 NULL
    };

    struct ImportSource
    {
        TransferFormat format_ = TransferFormat::Csv;
        QStringList columns_;
        QVector<QVector<QVariant>> rows_;   // 무효 QVariant 는 NULL
        QChar delimiter_;                   // CSV: 쓴 구분자
        QString error_;
    };

    struct ImportRequest
    {
        QString database_ = QStringLiteral("main"), table_;
        bool create_ = false;           // 새 테이블(원본 열 이름 · 추정 형식)
        // 원본 열 i 를 넣을 대상 열. 비어 있는 칸은 버린다.
        QStringList targets_;
        QStringList types_;             // 새 테이블 열 형식(INTEGER · REAL · TEXT)
    };

    // RFC 4180 칸 하나(NULL 은 빈 칸, BLOB 은 16진).
    QString CsvField(const QVariant& Value, QChar Delimiter);
    ImportSource ReadCsv(const QByteArray& Bytes, const CsvOptions& Options);
    // JSON 배열(객체 · 배열의 배열) 또는 한 줄에 객체 하나(JSON Lines).
    ImportSource ReadJson(const QByteArray& Bytes);
    ImportSource ReadImportFile(const QString& Path, const CsvOptions& Options);
    // 열 값으로 형식을 추정한다: 모두 정수 → INTEGER, 모두 수 → REAL, 그 밖 TEXT.
    QString InferType(const QVector<QVector<QVariant>>& Rows, int Column);
    // 원본 열을 기존 테이블 열에 맞춘다: 이름이 같은 열(대소문자 무시), 하나도 없으면 순서대로.
    QStringList MatchColumns(const QStringList& Source, const QStringList& Target, int* Matched = nullptr);
    QString ExtensionFor(TransferFormat Format);

    // Cancel 이 참이 되면 멈춘다(파일은 남기지 않는다). Progress 는 처리한 행 수.
    TransferResult ExportData(Connection* Db, const ExportRequest& Request, const std::atomic_bool& Cancel, std::atomic<qint64>& Progress);
    // 한 트랜잭션(BEGIN IMMEDIATE)으로 넣는다. 실패 · 취소하면 모두 되돌린다.
    TransferResult ImportData(Connection& Db, const ImportSource& Source, const ImportRequest& Request, const std::atomic_bool& Cancel, std::atomic<qint64>& Progress);

    // 내보내기 대화: 형식(CSV · JSON · SQL) · 파일 · CSV 머리 줄 · 구분자 · BOM · SQL CREATE 문 · INSERT 대상 이름.
    class ExportDialog final : public QDialog
    {
        Q_OBJECT
    public:
        ExportDialog(const ExportRequest& Base, bool CanCreate, QWidget* Parent = nullptr);
        ExportRequest Request() const;
        void SetFormat(TransferFormat Format);
        QLineEdit* PathEdit() const { return path_; }
        void accept() override;
    private:
        void refresh();

        ExportRequest base_;
        fm::ui::SegmentedControl* format_ = nullptr;
        QLineEdit* path_ = nullptr;
        QCheckBox* header_ = nullptr;
        QComboBox* delimiter_ = nullptr;
        QCheckBox* bom_ = nullptr;
        QCheckBox* create_ = nullptr;
        QLineEdit* table_ = nullptr;
        QWidget* csvBox_ = nullptr;
        QWidget* sqlBox_ = nullptr;
        QLabel* problem_ = nullptr;
    };

    // 가져오기 대화: 파일 · CSV 구분자 · 첫 줄 열 이름 · 빈 칸 NULL, 대상(기존 테이블 · 새 테이블), 미리 보기(50행) · 열 맞춤.
    class ImportDialog final : public QDialog
    {
        Q_OBJECT
    public:
        // Tables: 넣을 수 있는 테이블(main 의 일반 테이블)과 열 이름.
        ImportDialog(const QString& Path, const QList<std::pair<QString, QStringList>>& Tables, const QString& Table, QWidget* Parent = nullptr);
        const ImportSource& Source() const { return source_; }
        ImportRequest Request() const;
        void SetPath(const QString& Path);
        void SetNewTable(const QString& Name);
        void SetTarget(const QString& Table);
        QTableWidget* Preview() const { return preview_; }
        QLabel* Summary() const { return summary_; }
        void accept() override;
    private:
        void reload();
        void refresh();
        CsvOptions options() const;

        QList<std::pair<QString, QStringList>> tables_;
        ImportSource source_;
        QLineEdit* path_ = nullptr;
        QComboBox* delimiter_ = nullptr;
        QCheckBox* header_ = nullptr;
        QCheckBox* emptyNull_ = nullptr;
        QWidget* csvBox_ = nullptr;
        QRadioButton* existing_ = nullptr;
        QRadioButton* fresh_ = nullptr;
        QComboBox* table_ = nullptr;
        QLineEdit* name_ = nullptr;
        QTableWidget* preview_ = nullptr;
        QLabel* summary_ = nullptr;
        QLabel* problem_ = nullptr;
    };
}
