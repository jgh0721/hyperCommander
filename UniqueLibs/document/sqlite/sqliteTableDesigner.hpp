#pragma once

/// 고유 라이브러리
#include "sqliteConnection.hpp"
#include "sqliteSchema.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDialog>
#include <QStringList>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QStackedWidget;
class QTableWidget;
namespace fm::ui { class SegmentedControl; }

namespace hc::sqlite
{
    // 테이블 설계의 열 하나.
    struct DesignColumn
    {
        QString original_;              // 원래 열 이름(새 열은 빈 글)
        QString name_, type_, default_, check_, references_;   // references_: "table(column)"
        bool primaryKey_ = false, notNull_ = false, unique_ = false;
        QString originalDefinition_;    // 원래 DDL 의 열 정의(바뀌지 않았으면 그대로 다시 쓴다)
    };

    struct TableDesign
    {
        QString database_ = QStringLiteral("main"), originalName_, name_;
        QList<DesignColumn> columns_;
        bool withoutRowid_ = false, strict_ = false;
        bool originalWithoutRowid_ = false, originalStrict_ = false;
        QStringList tableConstraints_;  // 원래 DDL 의 표 제약(PRIMARY KEY (…) · UNIQUE · CHECK · FOREIGN KEY)
    };

    // 설계 7절: ALTER TABLE 로 되는 변경(테이블 · 열 이름 바꾸기, 열 추가, 열 삭제)은 그대로, 그 밖(형식 · 제약 · 순서)은
    // SQLite 문서의 12단계 재작성. 실행할 문(PRAGMA foreign_keys 를 끄고 켜는 것까지)과 설명을 만든다.
    struct DesignPlan
    {
        bool rebuild_ = false;
        QStringList statements_;        // 차례대로 실행할 문(; 없이)
        QString summary_;               // ‘이름 바꾸기 1 · 열 추가 1 · 형식 바꾸기 1. …’
        bool changed_ = false;
    };

    // 원래 개체에서 설계를 읽는다(CREATE TABLE 문의 열 정의 · 표 제약을 나눠 보관).
    TableDesign DesignFrom(const ObjectInfo& Object);
    // Backup 이 비어 있지 않으면 재작성 전에 VACUUM INTO 로 사본을 만든다.
    DesignPlan PlanDesign(const TableDesign& Design, const SchemaInfo& Schema, const QString& Backup);
    // 열 정의 한 줄("Name" TEXT NOT NULL DEFAULT 'x' …).
    QString ColumnDefinition(const DesignColumn& Column);
    // CREATE TABLE 문의 괄호 안을 맨 위 쉼표로 나눈다.
    QStringList SplitDefinitions(const QString& CreateSql);

    // 테이블 수정 창(시안 ‘테이블 수정(DDL 미리보기 후 재작성)’). 열 · 색인 · 외래 키 · 제약 · 트리거 · DDL 탭,
    // 바뀐 칸 표시, 방법과 영향 설명 띠, 실행할 SQL 미리보기, ‘쿼리 탭으로 복사’ · ‘적용’.
    class TableDesigner final : public QDialog
    {
        Q_OBJECT
    public:
        // Object 가 없으면 새 테이블.
        TableDesigner(Connection& Gui, const SchemaInfo& Schema, const ObjectInfo* Object, const QString& Database, const QString& BackupPath, QWidget* Parent = nullptr);
        TableDesign Design() const;
        DesignPlan Plan() const;
        QTableWidget* Columns() const { return grid_; }
        QLineEdit* NameEdit() const { return name_; }
        // 적용: 실행하고 실패하면 되돌린다. 오류는 Error 에.
        bool Apply(QString* Error);
    signals:
        void CopyToQueryRequested(const QString& Sql);
        void Applied();
    private:
        void fillRow(int Row, const DesignColumn& Column);
        void refresh();
        void addColumn();
        void removeColumn();
        void moveColumn(int Delta);

        Connection& gui_;
        const SchemaInfo& schema_;
        TableDesign original_;
        QString backup_;
        bool creating_ = false, filling_ = false;
        QLineEdit* name_ = nullptr;
        QLabel* title_ = nullptr;
        fm::ui::SegmentedControl* pagesBar_ = nullptr;
        QStackedWidget* pages_ = nullptr;
        QTableWidget* grid_ = nullptr;
        QCheckBox* withoutRowid_ = nullptr;
        QCheckBox* strict_ = nullptr;
        QLabel* banner_ = nullptr;
        QPlainTextEdit* sql_ = nullptr;
    };
}
