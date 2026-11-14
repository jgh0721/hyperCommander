#include "stdafx.h"
#include "sqliteTableDesigner.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteApi.hpp"
#include "sqliteSql.hpp"

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/StyleProps.h>
#include <fmstyle/ThemeColors.h>
#include <fmstyle/ThemeManager.h>
#include <fmwidgets/SegmentedControl.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace hc::sqlite
{
    namespace
    {
        using T = fm::style::Token;
        enum Field { NameField, TypeField, PrimaryField, NotNullField, UniqueField, DefaultField, CheckField, ForeignField, FieldCount };
        constexpr int OriginalRole = Qt::UserRole + 1, DefinitionRole = Qt::UserRole + 2;

        // 따옴표 · 괄호를 건너뛰며 Text[From] 의 여는 괄호와 짝인 닫는 괄호 위치.
        int MatchParen(const QString& Text, int From)
        {
            int Depth = 0;
            for (int At = From; At < Text.size(); ++At) {
                const QChar Ch = Text[At];
                if (Ch == QLatin1Char('\'') || Ch == QLatin1Char('"') || Ch == QLatin1Char('`') || Ch == QLatin1Char('[')) {
                    const QChar Close = Ch == QLatin1Char('[') ? QLatin1Char(']') : Ch;
                    for (++At; At < Text.size() && Text[At] != Close; ++At) {}
                    continue;
                }
                if (Ch == QLatin1Char('(')) ++Depth;
                else if (Ch == QLatin1Char(')') && --Depth == 0) return At;
            }
            return -1;
        }

        // 열 정의의 첫 낱말(이름)을 뗀다.
        QString LeadingName(const QString& Definition, int* End = nullptr)
        {
            const auto Text = Definition.trimmed();
            if (Text.isEmpty()) return {};
            const QChar First = Text[0];
            if (First == QLatin1Char('"') || First == QLatin1Char('`') || First == QLatin1Char('[')) {
                const QChar Close = First == QLatin1Char('[') ? QLatin1Char(']') : First;
                int At = 1;
                QString Name;
                while (At < Text.size()) {
                    if (Text[At] == Close) {
                        if (Close != QLatin1Char(']') && At + 1 < Text.size() && Text[At + 1] == Close) { Name += Close; At += 2; continue; }
                        break;
                    }
                    Name += Text[At++];
                }
                if (End) *End = At + 1;
                return Name;
            }
            int At = 0;
            while (At < Text.size() && !Text[At].isSpace() && Text[At] != QLatin1Char('(')) ++At;
            if (End) *End = At;
            return Text.left(At);
        }

        bool TableConstraint(const QString& Definition)
        {
            static const QRegularExpression Pattern(QStringLiteral("^\\s*(CONSTRAINT|PRIMARY\\s+KEY|UNIQUE|CHECK|FOREIGN\\s+KEY)\\b"), QRegularExpression::CaseInsensitiveOption);
            return Pattern.match(Definition).hasMatch();
        }

        QString CheckOf(const QString& Definition)
        {
            static const QRegularExpression Pattern(QStringLiteral("\\bCHECK\\s*\\("), QRegularExpression::CaseInsensitiveOption);
            const auto Match = Pattern.match(Definition);
            if (!Match.hasMatch()) return {};
            const int Open = int(Match.capturedEnd()) - 1, Close = MatchParen(Definition, Open);
            return Close > Open ? Definition.mid(Open + 1, Close - Open - 1).trimmed() : QString();
        }

        // 이름을 통째로(따옴표 이름 포함) 다른 이름으로.
        QString RenameIdentifier(QString Text, const QString& From, const QString& To)
        {
            const auto Quoted = QuoteIdentifier(To);
            Text.replace(QRegularExpression(QStringLiteral("(\"%1\"|`%1`|\\[%1\\])").arg(QRegularExpression::escape(From)), QRegularExpression::CaseInsensitiveOption), Quoted);
            Text.replace(QRegularExpression(QStringLiteral("(?<![\\w\"`\\[])%1(?![\\w\"`\\]])").arg(QRegularExpression::escape(From)), QRegularExpression::CaseInsensitiveOption), Quoted);
            return Text;
        }

        bool Mentions(const QString& Text, const QString& Name)
        {
            return QRegularExpression(QStringLiteral("(?<![\\w])[\"`\\[]?%1[\"`\\]]?(?![\\w])").arg(QRegularExpression::escape(Name)), QRegularExpression::CaseInsensitiveOption).match(Text).hasMatch();
        }

        bool SameShape(const DesignColumn& A, const DesignColumn& B)
        {
            return A.type_.trimmed().compare(B.type_.trimmed(), Qt::CaseInsensitive) == 0 && A.default_.trimmed() == B.default_.trimmed()
                && A.check_.trimmed() == B.check_.trimmed() && A.references_.trimmed().compare(B.references_.trimmed(), Qt::CaseInsensitive) == 0
                && A.primaryKey_ == B.primaryKey_ && A.notNull_ == B.notNull_ && A.unique_ == B.unique_;
        }

        QString DefaultText(const QString& Value)
        {
            const auto Text = Value.trimmed();
            if (Text.isEmpty()) return {};
            // 리터럴 · 이름 · 괄호 식은 그대로, 연산자가 든 식은 괄호로 감싼다(DEFAULT 는 괄호 없는 식을 받지 않는다).
            static const QRegularExpression Simple(QStringLiteral("^('([^']|'')*'|[-+]?[0-9.]+([eE][-+]?[0-9]+)?|[A-Za-z_]+|X'[0-9A-Fa-f]*'|\\(.*\\))$"));
            return Simple.match(Text).hasMatch() ? Text : QStringLiteral("(%1)").arg(Text);
        }
    }

    QStringList SplitDefinitions(const QString& CreateSql)
    {
        // CREATE TABLE 이름 ( … ) [옵션] — 이름이 따옴표 안에 괄호를 가질 수 있어 따옴표를 건너뛰며 찾는다.
        int Open = -1;
        for (int At = 0; At < CreateSql.size(); ++At) {
            const QChar Ch = CreateSql[At];
            if (Ch == QLatin1Char('\'') || Ch == QLatin1Char('"') || Ch == QLatin1Char('`') || Ch == QLatin1Char('[')) {
                const QChar Close = Ch == QLatin1Char('[') ? QLatin1Char(']') : Ch;
                for (++At; At < CreateSql.size() && CreateSql[At] != Close; ++At) {}
                continue;
            }
            if (Ch == QLatin1Char('(')) { Open = At; break; }
        }
        if (Open < 0) return {};
        const int Close = MatchParen(CreateSql, Open);
        if (Close < 0) return {};
        const auto Body = CreateSql.mid(Open + 1, Close - Open - 1);
        QStringList Parts;
        int Depth = 0, Start = 0;
        for (int At = 0; At < Body.size(); ++At) {
            const QChar Ch = Body[At];
            if (Ch == QLatin1Char('\'') || Ch == QLatin1Char('"') || Ch == QLatin1Char('`') || Ch == QLatin1Char('[')) {
                const QChar CloseQuote = Ch == QLatin1Char('[') ? QLatin1Char(']') : Ch;
                for (++At; At < Body.size() && Body[At] != CloseQuote; ++At) {}
                continue;
            }
            if (Ch == QLatin1Char('-') && At + 1 < Body.size() && Body[At + 1] == QLatin1Char('-')) { while (At < Body.size() && Body[At] != QLatin1Char('\n')) ++At; continue; }
            if (Ch == QLatin1Char('(')) ++Depth;
            else if (Ch == QLatin1Char(')')) --Depth;
            else if (Ch == QLatin1Char(',') && Depth == 0) { Parts.append(Body.mid(Start, At - Start).trimmed()); Start = At + 1; }
        }
        Parts.append(Body.mid(Start).trimmed());
        Parts.removeAll(QString());
        return Parts;
    }

    QString ColumnDefinition(const DesignColumn& Column)
    {
        QStringList Parts{QuoteIdentifier(Column.name_)};
        if (!Column.type_.trimmed().isEmpty()) Parts.append(Column.type_.trimmed());
        if (Column.primaryKey_) Parts.append(QStringLiteral("PRIMARY KEY"));
        if (Column.notNull_) Parts.append(QStringLiteral("NOT NULL"));
        if (Column.unique_ && !Column.primaryKey_) Parts.append(QStringLiteral("UNIQUE"));
        if (const auto Default = DefaultText(Column.default_); !Default.isEmpty()) Parts.append(QStringLiteral("DEFAULT ") + Default);
        if (!Column.check_.trimmed().isEmpty()) Parts.append(QStringLiteral("CHECK (%1)").arg(Column.check_.trimmed()));
        if (const auto Target = Column.references_.trimmed(); !Target.isEmpty()) {
            const int Open = int(Target.indexOf(QLatin1Char('(')));
            const auto Table = (Open < 0 ? Target : Target.left(Open)).trimmed();
            const auto Columns = Open < 0 ? QString() : Target.mid(Open + 1).remove(QLatin1Char(')')).trimmed();
            Parts.append(QStringLiteral("REFERENCES %1%2").arg(QuoteIdentifier(Table), Columns.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(QuoteIdentifier(Columns))));
        }
        return Parts.join(QLatin1Char(' '));
    }

    TableDesign DesignFrom(const ObjectInfo& Object)
    {
        TableDesign Design;
        Design.database_ = Object.database_.isEmpty() ? QStringLiteral("main") : Object.database_;
        Design.originalName_ = Design.name_ = Object.name_;
        Design.withoutRowid_ = Design.originalWithoutRowid_ = Object.withoutRowid_;
        Design.strict_ = Design.originalStrict_ = Object.strict_;
        QHash<QString, QString> Definitions;
        for (const auto& Part : SplitDefinitions(Object.sql_)) {
            if (TableConstraint(Part)) Design.tableConstraints_.append(Part);
            else Definitions.insert(LeadingName(Part).toLower(), Part);
        }
        for (const auto& Info : Object.columns_) {
            if (Info.hidden_) continue;
            DesignColumn Column;
            Column.original_ = Column.name_ = Info.name_;
            Column.type_ = Info.type_;
            Column.notNull_ = Info.notNull_;
            Column.default_ = Info.defaultValue_;
            Column.primaryKey_ = Info.primaryKey_ > 0;
            if (!Info.foreignTable_.isEmpty()) Column.references_ = Info.foreignColumn_.isEmpty() ? Info.foreignTable_ : QStringLiteral("%1(%2)").arg(Info.foreignTable_, Info.foreignColumn_);
            Column.originalDefinition_ = Definitions.value(Info.name_.toLower());
            Column.unique_ = QRegularExpression(QStringLiteral("\\bUNIQUE\\b"), QRegularExpression::CaseInsensitiveOption).match(Column.originalDefinition_).hasMatch();
            Column.check_ = CheckOf(Column.originalDefinition_);
            Design.columns_.append(Column);
        }
        return Design;
    }

    DesignPlan PlanDesign(const TableDesign& Design, const SchemaInfo& Schema, const QString& Backup)
    {
        DesignPlan Plan;
        const auto Database = Design.database_.isEmpty() ? QStringLiteral("main") : Design.database_;
        const auto Qualify = [&](const QString& Name) { return Database.compare(QLatin1String("main"), Qt::CaseInsensitive) == 0 ? QuoteIdentifier(Name) : Qualified(Database, Name); };
        const auto Options = [&] {
            QStringList Parts;
            if (Design.withoutRowid_) Parts.append(QStringLiteral("WITHOUT ROWID"));
            if (Design.strict_) Parts.append(QStringLiteral("STRICT"));
            return Parts.isEmpty() ? QString() : QLatin1Char(' ') + Parts.join(QStringLiteral(", "));
        };
        QList<const DesignColumn*> Keys;
        for (const auto& Column : Design.columns_) if (Column.primaryKey_) Keys.append(&Column);

        if (Design.originalName_.isEmpty()) {
            // 새 테이블.
            QStringList Lines;
            for (const auto& Column : Design.columns_) {
                auto Copy = Column;
                if (Keys.size() > 1) Copy.primaryKey_ = false;
                Lines.append(QStringLiteral("    ") + ColumnDefinition(Copy));
            }
            if (Keys.size() > 1) {
                QStringList Names;
                for (const auto* Column : Keys) Names.append(QuoteIdentifier(Column->name_));
                Lines.append(QStringLiteral("    PRIMARY KEY (%1)").arg(Names.join(QStringLiteral(", "))));
            }
            Plan.changed_ = !Design.name_.trimmed().isEmpty() && !Design.columns_.isEmpty();
            Plan.statements_.append(QStringLiteral("CREATE TABLE %1 (\n%2\n)%3").arg(Qualify(Design.name_.trimmed()), Lines.join(QStringLiteral(",\n")), Options()));
            Plan.summary_ = QObject::tr("새 테이블 · 열 %1개").arg(Design.columns_.size());
            return Plan;
        }

        const auto* Object = Schema.Find(Database, Design.originalName_);
        const auto Original = Object ? DesignFrom(*Object) : TableDesign{};
        QHash<QString, const DesignColumn*> Before;
        for (const auto& Column : Original.columns_) Before.insert(Column.name_.toLower(), &Column);
        QList<const DesignColumn*> Added, Renamed, Modified;
        QStringList Dropped;
        QSet<QString> Kept;
        QStringList OriginalOrder, NewOrder;
        for (const auto& Column : Design.columns_) {
            if (Column.original_.isEmpty()) { Added.append(&Column); continue; }
            const auto* Old = Before.value(Column.original_.toLower());
            if (!Old) { Added.append(&Column); continue; }
            Kept.insert(Column.original_.toLower());
            NewOrder.append(Column.original_.toLower());
            if (Column.name_ != Old->name_) Renamed.append(&Column);
            if (!SameShape(Column, *Old)) Modified.append(&Column);
        }
        for (const auto& Column : Original.columns_) {
            if (!Kept.contains(Column.name_.toLower())) Dropped.append(Column.name_);
            else OriginalOrder.append(Column.name_.toLower());
        }
        const bool Reordered = OriginalOrder != NewOrder || [&] {
            // 새 열은 끝에만 붙을 수 있다(ALTER TABLE ADD COLUMN).
            bool SeenNew = false;
            for (const auto& Column : Design.columns_) { if (Column.original_.isEmpty()) SeenNew = true; else if (SeenNew) return true; }
            return false;
        }();
        const bool OptionsChanged = Design.withoutRowid_ != Design.originalWithoutRowid_ || Design.strict_ != Design.originalStrict_;
        const bool TableRenamed = Design.name_.trimmed() != Design.originalName_;
        const auto NewName = Design.name_.trimmed();
        QStringList Indexes, Triggers, Views;
        for (const auto& Db : Schema.databases_) {
            if (Db.name_.compare(Database, Qt::CaseInsensitive) != 0) continue;
            for (const auto& Each : Db.objects_) {
                if (Each.sql_.isEmpty()) continue;
                if (Each.type_ == ObjectType::Index && Each.table_.compare(Design.originalName_, Qt::CaseInsensitive) == 0) Indexes.append(Each.sql_);
                else if (Each.type_ == ObjectType::Trigger && Each.table_.compare(Design.originalName_, Qt::CaseInsensitive) == 0) Triggers.append(Each.sql_);
                else if (Each.type_ == ObjectType::View && Mentions(Each.sql_, Design.originalName_)) Views.append(Each.name_);
            }
        }
        // ALTER TABLE 로 되는가: 형식 · 제약 · 순서 · 옵션 변경이 없고, 새 열은 끝에 붙는 평범한 열, 지울 열은 키 · 색인에 없는 열.
        bool AlterOnly = Modified.isEmpty() && !Reordered && !OptionsChanged;
        for (const auto* Column : Added) {
            const auto Default = DefaultText(Column->default_);
            if (Column->primaryKey_ || Column->unique_ || (Column->notNull_ && Default.isEmpty()) || Default.startsWith(QLatin1Char('('))
                || Default.startsWith(QLatin1String("CURRENT_"), Qt::CaseInsensitive)) AlterOnly = false;
        }
        for (const auto& Name : Dropped) {
            const auto* Old = Before.value(Name.toLower());
            if (Old && (Old->primaryKey_ || Old->unique_)) AlterOnly = false;
            for (const auto& Sql : Indexes + Triggers) if (Mentions(Sql, Name)) AlterOnly = false;
            for (const auto& Constraint : Original.tableConstraints_) if (Mentions(Constraint, Name)) AlterOnly = false;
        }
        QStringList Parts;
        if (TableRenamed) Parts.append(QObject::tr("테이블 이름 바꾸기"));
        if (!Renamed.isEmpty()) Parts.append(QObject::tr("이름 바꾸기 %1").arg(Renamed.size()));
        if (!Added.isEmpty()) Parts.append(QObject::tr("열 추가 %1").arg(Added.size()));
        if (!Dropped.isEmpty()) Parts.append(QObject::tr("열 삭제 %1").arg(Dropped.size()));
        if (!Modified.isEmpty()) Parts.append(QObject::tr("형식 · 제약 바꾸기 %1").arg(Modified.size()));
        if (Reordered) Parts.append(QObject::tr("순서 바꾸기"));
        if (OptionsChanged) Parts.append(QObject::tr("테이블 옵션 바꾸기"));
        Plan.changed_ = !Parts.isEmpty();
        if (!Plan.changed_) { Plan.summary_ = QObject::tr("바뀐 것이 없습니다."); return Plan; }
        const auto Head = Parts.join(QStringLiteral(" · "));

        if (AlterOnly) {
            Plan.statements_.append(QStringLiteral("BEGIN"));
            const auto Current = Qualify(Design.originalName_);
            for (const auto* Column : Renamed)
                Plan.statements_.append(QStringLiteral("ALTER TABLE %1 RENAME COLUMN %2 TO %3").arg(Current, QuoteIdentifier(Before.value(Column->original_.toLower())->name_), QuoteIdentifier(Column->name_)));
            for (const auto* Column : Added) Plan.statements_.append(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2").arg(Current, ColumnDefinition(*Column)));
            for (const auto& Name : Dropped) Plan.statements_.append(QStringLiteral("ALTER TABLE %1 DROP COLUMN %2").arg(Current, QuoteIdentifier(Name)));
            if (TableRenamed) Plan.statements_.append(QStringLiteral("ALTER TABLE %1 RENAME TO %2").arg(Current, QuoteIdentifier(NewName)));
            Plan.statements_.append(QStringLiteral("COMMIT"));
            Plan.summary_ = QObject::tr("%1. ALTER TABLE 로 바꿉니다 — 데이터를 옮기지 않습니다.").arg(Head);
            return Plan;
        }

        // 12단계 재작성(SQLite 문서 ‘Making Other Kinds Of Table Schema Changes’).
        Plan.rebuild_ = true;
        auto Temp = NewName + QStringLiteral("_new");
        for (int Suffix = 2; Schema.Find(Database, Temp); ++Suffix) Temp = NewName + QStringLiteral("_new%1").arg(Suffix);
        QList<int> OldKeys, NewKeys;
        for (int Index = 0; Index < Original.columns_.size(); ++Index) if (Original.columns_[Index].primaryKey_) OldKeys.append(Index);
        QStringList OldKeyNames, NewKeyNames;
        for (const int Index : OldKeys) OldKeyNames.append(Original.columns_[Index].name_.toLower());
        for (const auto* Column : Keys) NewKeyNames.append(Column->original_.isEmpty() ? QStringLiteral("+") + Column->name_ : Column->original_.toLower());
        const bool KeyChanged = OldKeyNames != NewKeyNames;
        QStringList Lines;
        QStringList Into, From;
        for (const auto& Column : Design.columns_) {
            const auto* Old = Column.original_.isEmpty() ? nullptr : Before.value(Column.original_.toLower());
            QString Definition;
            if (Old && SameShape(Column, *Old) && !KeyChanged && !Old->originalDefinition_.isEmpty()) {
                // 바뀌지 않은 열: 원래 정의(COLLATE · AUTOINCREMENT · GENERATED 등)를 그대로, 이름만 새로.
                int End = 0;
                LeadingName(Old->originalDefinition_, &End);
                Definition = QuoteIdentifier(Column.name_) + Old->originalDefinition_.trimmed().mid(End);
            } else {
                auto Copy = Column;
                if (Keys.size() > 1) Copy.primaryKey_ = false;
                Definition = ColumnDefinition(Copy);
            }
            Lines.append(QStringLiteral("    ") + Definition);
            if (Old) { Into.append(QuoteIdentifier(Column.name_)); From.append(QuoteIdentifier(Old->name_)); }
        }
        QStringList Omitted;
        for (auto Constraint : Original.tableConstraints_) {
            if (KeyChanged && QRegularExpression(QStringLiteral("^\\s*(CONSTRAINT\\s+\\S+\\s+)?PRIMARY\\s+KEY"), QRegularExpression::CaseInsensitiveOption).match(Constraint).hasMatch()) continue;
            bool Dead = false;
            for (const auto& Name : Dropped) Dead |= Mentions(Constraint, Name);
            if (Dead) { Omitted.append(Constraint); continue; }
            for (const auto* Column : Renamed) Constraint = RenameIdentifier(Constraint, Before.value(Column->original_.toLower())->name_, Column->name_);
            Lines.append(QStringLiteral("    ") + Constraint);
        }
        if (KeyChanged && Keys.size() > 1) {
            QStringList Names;
            for (const auto* Column : Keys) Names.append(QuoteIdentifier(Column->name_));
            Lines.append(QStringLiteral("    PRIMARY KEY (%1)").arg(Names.join(QStringLiteral(", "))));
        }
        if (!Backup.isEmpty()) Plan.statements_.append(QStringLiteral("VACUUM INTO %1").arg(QuoteLiteral(Backup)));
        Plan.statements_.append(QStringLiteral("PRAGMA foreign_keys = OFF"));
        Plan.statements_.append(QStringLiteral("BEGIN"));
        Plan.statements_.append(QStringLiteral("CREATE TABLE %1 (\n%2\n)%3").arg(Qualify(Temp), Lines.join(QStringLiteral(",\n")), Options()));
        if (!Into.isEmpty())
            Plan.statements_.append(QStringLiteral("INSERT INTO %1 (%2)\n    SELECT %3 FROM %4").arg(Qualify(Temp), Into.join(QStringLiteral(", ")), From.join(QStringLiteral(", ")), Qualify(Design.originalName_)));
        Plan.statements_.append(QStringLiteral("DROP TABLE %1").arg(Qualify(Design.originalName_)));
        // 뷰가 지운 원래 테이블을 가리키는 동안 새 RENAME(3.26+)은 스키마 전체를 다시 읽다 실패한다. 이름만 바꾸게 한다.
        Plan.statements_.append(QStringLiteral("PRAGMA legacy_alter_table = ON"));
        Plan.statements_.append(QStringLiteral("ALTER TABLE %1 RENAME TO %2").arg(Qualify(Temp), QuoteIdentifier(NewName)));
        Plan.statements_.append(QStringLiteral("PRAGMA legacy_alter_table = OFF"));
        int Rebuilt = 0;
        for (auto Sql : Indexes + Triggers) {
            bool Dead = false;
            for (const auto& Name : Dropped) Dead |= Mentions(Sql, Name);
            if (Dead) { Omitted.append(Summary(Sql, 60)); continue; }
            for (const auto* Column : Renamed) Sql = RenameIdentifier(Sql, Before.value(Column->original_.toLower())->name_, Column->name_);
            if (TableRenamed) Sql = RenameIdentifier(Sql, Design.originalName_, NewName);
            Plan.statements_.append(Sql);
            ++Rebuilt;
        }
        Plan.statements_.append(QStringLiteral("PRAGMA foreign_key_check"));
        Plan.statements_.append(QStringLiteral("COMMIT"));
        Plan.statements_.append(QStringLiteral("PRAGMA foreign_keys = ON"));
        auto Text = QObject::tr("%1. ").arg(Head);
        if (!Modified.isEmpty() || Reordered || OptionsChanged) Text += QObject::tr("이 변경은 ALTER TABLE 로 되지 않아 테이블을 새로 만들어 옮깁니다. ");
        else Text += QObject::tr("키 · 색인에 걸린 열을 지우므로 테이블을 새로 만들어 옮깁니다. ");
        if (Rebuilt > 0) Text += QObject::tr("색인 · 트리거 %1개를 다시 만들고, ").arg(Rebuilt);
        if (!Views.isEmpty()) Text += QObject::tr("이 테이블을 쓰는 뷰 %1개(%2)는 그대로 둡니다. ").arg(Views.size()).arg(Views.join(QStringLiteral(", ")));
        if (!Omitted.isEmpty()) Text += QObject::tr("지운 열에 걸린 %1개는 빼고 만듭니다. ").arg(Omitted.size());
        if (!Backup.isEmpty()) Text += QObject::tr("먼저 사본을 만듭니다. ");
        Text += QObject::tr("끝나면 외래 키를 검사하고, 어긋나면 전체를 되돌립니다.");
        Plan.summary_ = Text;
        return Plan;
    }

    TableDesigner::TableDesigner(Connection& Gui, const SchemaInfo& Schema, const ObjectInfo* Object, const QString& Database, const QString& BackupPath, QWidget* Parent)
        : QDialog(Parent), gui_(Gui), schema_(Schema), backup_(BackupPath)
    {
        setObjectName(QStringLiteral("sqliteTableDesigner"));
        creating_ = Object == nullptr;
        if (Object) original_ = DesignFrom(*Object);
        else { original_.database_ = Database.isEmpty() ? QStringLiteral("main") : Database; original_.name_ = QStringLiteral("new_table"); }
        setWindowTitle(creating_ ? tr("새 테이블") : tr("%1 수정").arg(original_.originalName_));
        resize(1040, 820);
        const auto& Colors = fm::style::themeColorsFor(this);
        auto* Layout = new QVBoxLayout(this);
        auto* Head = new QHBoxLayout;
        title_ = new QLabel(this);
        title_->setObjectName(QStringLiteral("sqliteDesignTitle"));
        auto TitleFont = title_->font();
        TitleFont.setWeight(QFont::DemiBold);
        title_->setFont(TitleFont);
        Head->addWidget(title_);
        name_ = new QLineEdit(original_.name_, this);
        name_->setObjectName(QStringLiteral("sqliteDesignName"));
        name_->setPlaceholderText(tr("테이블 이름"));
        name_->setMaximumWidth(260);
        Head->addWidget(name_);
        Head->addStretch(1);
        int IndexCount = 0, TriggerCount = 0, ForeignCount = 0;
        QListWidget* IndexList = new QListWidget(this);
        QListWidget* TriggerList = new QListWidget(this);
        QListWidget* ForeignList = new QListWidget(this);
        QListWidget* ConstraintList = new QListWidget(this);
        for (const auto& Db : Schema.databases_)
            for (const auto& Each : Db.objects_) {
                if (creating_ || Each.table_.compare(original_.originalName_, Qt::CaseInsensitive) != 0 || Db.name_.compare(original_.database_, Qt::CaseInsensitive) != 0) continue;
                if (Each.type_ == ObjectType::Index) { IndexList->addItem(Each.sql_.isEmpty() ? tr("%1 — SQLite 가 만든 색인").arg(Each.name_) : Each.sql_); ++IndexCount; }
                if (Each.type_ == ObjectType::Trigger) { TriggerList->addItem(Each.sql_); ++TriggerCount; }
            }
        for (const auto& Column : original_.columns_) if (!Column.references_.isEmpty()) { ForeignList->addItem(QStringLiteral("%1 → %2").arg(Column.name_, Column.references_)); ++ForeignCount; }
        for (const auto& Constraint : original_.tableConstraints_) ConstraintList->addItem(Constraint);
        pagesBar_ = new fm::ui::SegmentedControl(QStringList{tr("열"), tr("색인 %1").arg(IndexCount), tr("외래 키 %1").arg(ForeignCount), tr("제약"),
                                                             tr("트리거 %1").arg(TriggerCount), QStringLiteral("DDL")}, this);
        pagesBar_->setObjectName(QStringLiteral("sqliteDesignPages"));
        pagesBar_->setSegmentSize(fm::ui::SegmentedControl::Small);
        Head->addWidget(pagesBar_);
        Layout->addLayout(Head);

        pages_ = new QStackedWidget(this);
        auto* ColumnsPage = new QWidget(pages_);
        auto* ColumnsLayout = new QVBoxLayout(ColumnsPage);
        ColumnsLayout->setContentsMargins(0, 0, 0, 0);
        grid_ = new QTableWidget(0, FieldCount, ColumnsPage);
        grid_->setObjectName(QStringLiteral("sqliteDesignColumns"));
        grid_->setHorizontalHeaderLabels({tr("이름"), tr("형식"), QStringLiteral("PK"), QStringLiteral("NN"), QStringLiteral("UQ"), tr("기본값"), tr("검사"), tr("외래 키")});
        grid_->horizontalHeader()->setStretchLastSection(true);
        grid_->setSelectionBehavior(QAbstractItemView::SelectRows);
        grid_->setSelectionMode(QAbstractItemView::SingleSelection);
        grid_->verticalHeader()->setDefaultSectionSize(26);
        ColumnsLayout->addWidget(grid_, 1);
        auto* Tools = new QHBoxLayout;
        const auto Tool = [&](const QString& Name, const QString& Text) {
            auto* Button = new QPushButton(Text, ColumnsPage);
            Button->setObjectName(Name);
            Tools->addWidget(Button);
            return Button;
        };
        auto* Add = Tool(QStringLiteral("sqliteDesignAdd"), tr("열 추가"));
        auto* Remove = Tool(QStringLiteral("sqliteDesignRemove"), tr("열 삭제"));
        auto* Up = Tool(QStringLiteral("sqliteDesignUp"), tr("위로"));
        auto* Down = Tool(QStringLiteral("sqliteDesignDown"), tr("아래로"));
        Tools->addStretch(1);
        withoutRowid_ = new QCheckBox(QStringLiteral("WITHOUT ROWID"), ColumnsPage);
        withoutRowid_->setChecked(original_.withoutRowid_);
        strict_ = new QCheckBox(QStringLiteral("STRICT"), ColumnsPage);
        strict_->setChecked(original_.strict_);
        Tools->addWidget(withoutRowid_);
        Tools->addWidget(strict_);
        ColumnsLayout->addLayout(Tools);
        pages_->addWidget(ColumnsPage);
        for (auto* List : {IndexList, ForeignList, ConstraintList, TriggerList}) { List->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont)); pages_->addWidget(List); }
        auto* Ddl = new QPlainTextEdit(creating_ ? QString() : schema_.Find(original_.database_, original_.originalName_)->sql_ + QLatin1Char(';'), pages_);
        Ddl->setReadOnly(true);
        Ddl->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        pages_->addWidget(Ddl);
        Layout->addWidget(pages_, 3);

        banner_ = new QLabel(this);
        banner_->setObjectName(QStringLiteral("sqliteDesignSummary"));
        banner_->setWordWrap(true);
        banner_->setContentsMargins(10, 8, 10, 8);
        banner_->setAutoFillBackground(true);
        auto BannerPalette = banner_->palette();
        BannerPalette.setColor(QPalette::Window, Colors[T::WarnBg]);
        banner_->setPalette(BannerPalette);
        Layout->addWidget(banner_);
        auto* SqlTitle = new QLabel(tr("실행할 SQL"), this);
        auto SqlFont = SqlTitle->font();
        SqlFont.setWeight(QFont::DemiBold);
        SqlTitle->setFont(SqlFont);
        Layout->addWidget(SqlTitle);
        sql_ = new QPlainTextEdit(this);
        sql_->setObjectName(QStringLiteral("sqliteDesignSql"));
        sql_->setReadOnly(true);
        sql_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        Layout->addWidget(sql_, 2);
        auto* Buttons = new QHBoxLayout;
        auto* Copy = new QPushButton(tr("쿼리 탭으로 복사"), this);
        Copy->setObjectName(QStringLiteral("sqliteDesignCopy"));
        Buttons->addWidget(Copy);
        Buttons->addStretch(1);
        auto* Cancel = new QPushButton(tr("취소"), this);
        auto* ApplyButton = new QPushButton(tr("적용"), this);
        ApplyButton->setObjectName(QStringLiteral("sqliteDesignApply"));
        fm::style::setButtonRole(ApplyButton, fm::style::ButtonRole::Primary);
        Buttons->addWidget(Cancel);
        Buttons->addWidget(ApplyButton);
        Layout->addLayout(Buttons);

        filling_ = true;
        for (const auto& Column : original_.columns_) { grid_->insertRow(grid_->rowCount()); fillRow(grid_->rowCount() - 1, Column); }
        if (creating_) {
            DesignColumn First;
            First.name_ = QStringLiteral("id");
            First.type_ = QStringLiteral("INTEGER");
            First.primaryKey_ = true;
            grid_->insertRow(0);
            fillRow(0, First);
        }
        filling_ = false;
        grid_->resizeColumnsToContents();

        connect(pagesBar_, &fm::ui::SegmentedControl::currentIndexChanged, pages_, &QStackedWidget::setCurrentIndex);
        connect(grid_, &QTableWidget::itemChanged, this, [this] { if (!filling_) refresh(); });
        connect(name_, &QLineEdit::textChanged, this, &TableDesigner::refresh);
        connect(withoutRowid_, &QCheckBox::toggled, this, &TableDesigner::refresh);
        connect(strict_, &QCheckBox::toggled, this, &TableDesigner::refresh);
        connect(Add, &QPushButton::clicked, this, &TableDesigner::addColumn);
        connect(Remove, &QPushButton::clicked, this, &TableDesigner::removeColumn);
        connect(Up, &QPushButton::clicked, this, [this] { moveColumn(-1); });
        connect(Down, &QPushButton::clicked, this, [this] { moveColumn(1); });
        connect(Copy, &QPushButton::clicked, this, [this] {
            emit CopyToQueryRequested(Plan().statements_.join(QStringLiteral(";\n")) + QLatin1Char(';'));
            reject();
        });
        connect(Cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(ApplyButton, &QPushButton::clicked, this, [this] {
            QString Error;
            if (Apply(&Error)) accept();
            else { banner_->setText(tr("적용하지 못했습니다 — 바꾸기 전으로 되돌렸습니다.\n%1").arg(Error)); }
        });
        refresh();
    }

    void TableDesigner::fillRow(int Row, const DesignColumn& Column)
    {
        const auto Text = [&](int Field, const QString& Value) {
            auto* Item = new QTableWidgetItem(Value);
            grid_->setItem(Row, Field, Item);
            return Item;
        };
        const auto Check = [&](int Field, bool Value) {
            auto* Item = new QTableWidgetItem;
            Item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            Item->setCheckState(Value ? Qt::Checked : Qt::Unchecked);
            grid_->setItem(Row, Field, Item);
        };
        auto* Name = Text(NameField, Column.name_);
        Name->setData(OriginalRole, Column.original_);
        Name->setData(DefinitionRole, Column.originalDefinition_);
        Text(TypeField, Column.type_);
        Check(PrimaryField, Column.primaryKey_);
        Check(NotNullField, Column.notNull_);
        Check(UniqueField, Column.unique_);
        Text(DefaultField, Column.default_);
        Text(CheckField, Column.check_);
        Text(ForeignField, Column.references_);
    }

    TableDesign TableDesigner::Design() const
    {
        auto Design = original_;
        Design.name_ = name_->text().trimmed();
        Design.withoutRowid_ = withoutRowid_->isChecked();
        Design.strict_ = strict_->isChecked();
        Design.columns_.clear();
        for (int Row = 0; Row < grid_->rowCount(); ++Row) {
            const auto Value = [&](int Field) { auto* Item = grid_->item(Row, Field); return Item ? Item->text() : QString(); };
            const auto Checked = [&](int Field) { auto* Item = grid_->item(Row, Field); return Item && Item->checkState() == Qt::Checked; };
            DesignColumn Column;
            Column.original_ = grid_->item(Row, NameField) ? grid_->item(Row, NameField)->data(OriginalRole).toString() : QString();
            Column.originalDefinition_ = grid_->item(Row, NameField) ? grid_->item(Row, NameField)->data(DefinitionRole).toString() : QString();
            Column.name_ = Value(NameField).trimmed();
            Column.type_ = Value(TypeField).trimmed();
            Column.primaryKey_ = Checked(PrimaryField);
            Column.notNull_ = Checked(NotNullField);
            Column.unique_ = Checked(UniqueField);
            Column.default_ = Value(DefaultField).trimmed();
            Column.check_ = Value(CheckField).trimmed();
            Column.references_ = Value(ForeignField).trimmed();
            if (!Column.name_.isEmpty()) Design.columns_.append(Column);
        }
        return Design;
    }

    DesignPlan TableDesigner::Plan() const
    {
        return PlanDesign(Design(), schema_, Design().columns_.isEmpty() ? QString() : backup_);
    }

    void TableDesigner::refresh()
    {
        const auto Current = Design();
        const auto Result = PlanDesign(Current, schema_, backup_);
        title_->setText(creating_ ? tr("새 테이블 · %1").arg(Current.database_)
                                  : tr("%1 수정 · %2 · 열 %3 → %4").arg(original_.originalName_, original_.database_).arg(original_.columns_.size()).arg(Current.columns_.size()));
        banner_->setText(Result.summary_);
        QStringList Lines;
        for (const auto& Statement : Result.statements_) Lines.append(Statement + QLatin1Char(';'));
        sql_->setPlainText(Result.changed_ ? Lines.join(QLatin1Char('\n')) : QString());
        // 바뀐 칸은 노랑, 새 열은 초록(시안).
        const auto& Colors = fm::style::themeColorsFor(this);
        const QSignalBlocker Block(grid_);
        for (int Row = 0; Row < grid_->rowCount(); ++Row) {
            const auto Original = grid_->item(Row, NameField) ? grid_->item(Row, NameField)->data(OriginalRole).toString() : QString();
            const DesignColumn* Old = nullptr;
            for (const auto& Column : original_.columns_) if (!Original.isEmpty() && Column.original_ == Original) Old = &Column;
            for (int Field = 0; Field < FieldCount; ++Field) {
                auto* Item = grid_->item(Row, Field);
                if (!Item) continue;
                bool Changed = false;
                if (Old) {
                    switch (Field) {
                    case NameField: Changed = Item->text().trimmed() != Old->name_; break;
                    case TypeField: Changed = Item->text().trimmed().compare(Old->type_, Qt::CaseInsensitive) != 0; break;
                    case PrimaryField: Changed = (Item->checkState() == Qt::Checked) != Old->primaryKey_; break;
                    case NotNullField: Changed = (Item->checkState() == Qt::Checked) != Old->notNull_; break;
                    case UniqueField: Changed = (Item->checkState() == Qt::Checked) != Old->unique_; break;
                    case DefaultField: Changed = Item->text().trimmed() != Old->default_; break;
                    case CheckField: Changed = Item->text().trimmed() != Old->check_; break;
                    case ForeignField: Changed = Item->text().trimmed().compare(Old->references_, Qt::CaseInsensitive) != 0; break;
                    }
                }
                Item->setBackground(!Old && !creating_ ? Colors[T::OkBg] : Changed ? Colors[T::WarnBg] : QBrush());
            }
        }
    }

    void TableDesigner::addColumn()
    {
        const int Row = grid_->rowCount();
        filling_ = true;
        grid_->insertRow(Row);
        DesignColumn Column;
        Column.name_ = tr("column%1").arg(Row + 1);
        fillRow(Row, Column);
        filling_ = false;
        grid_->setCurrentCell(Row, NameField);
        grid_->editItem(grid_->item(Row, NameField));
        refresh();
    }

    void TableDesigner::removeColumn()
    {
        const int Row = grid_->currentRow();
        if (Row < 0) return;
        grid_->removeRow(Row);
        refresh();
    }

    void TableDesigner::moveColumn(int Delta)
    {
        const int Row = grid_->currentRow(), Target = Row + Delta;
        if (Row < 0 || Target < 0 || Target >= grid_->rowCount()) return;
        filling_ = true;
        QList<QTableWidgetItem*> Items;
        for (int Field = 0; Field < FieldCount; ++Field) Items.append(grid_->takeItem(Row, Field));
        grid_->removeRow(Row);
        grid_->insertRow(Target);
        for (int Field = 0; Field < FieldCount; ++Field) grid_->setItem(Target, Field, Items[Field]);
        filling_ = false;
        grid_->setCurrentCell(Target, NameField);
        refresh();
    }

    bool TableDesigner::Apply(QString* Error)
    {
        const auto Result = Plan();
        if (!Result.changed_) return true;
        // foreign_keys 는 트랜잭션 밖에서만 바꿀 수 있다. 원래 값으로 되돌린다.
        bool KeysOn = false;
        {
            Statement Query(gui_, QStringLiteral("PRAGMA foreign_keys"));
            KeysOn = Query.Valid() && Query.Step() == SQLITE_ROW && Query.Value(0).toInt() != 0;
        }
        const auto Restore = [&] {
            if (sqlite3_get_autocommit(gui_.Handle()) == 0) gui_.Exec(QStringLiteral("ROLLBACK"), nullptr);
            gui_.Exec(QStringLiteral("PRAGMA foreign_keys = %1").arg(KeysOn ? QStringLiteral("ON") : QStringLiteral("OFF")), nullptr);
        };
        for (const auto& Each : Result.statements_) {
            if (Each.startsWith(QLatin1String("PRAGMA foreign_keys = ON"))) { if (KeysOn) gui_.Exec(Each, nullptr); continue; }
            if (Each == QLatin1String("PRAGMA foreign_key_check")) {
                Statement Check(gui_, Each);
                QStringList Problems;
                while (Check.Valid() && Check.Step() == SQLITE_ROW && Problems.size() < 5)
                    Problems.append(QStringLiteral("%1 rowid %2 → %3").arg(Check.Value(0).toString(), Check.Value(1).toString(), Check.Value(2).toString()));
                if (!Problems.isEmpty()) {
                    Restore();
                    if (Error) *Error = tr("외래 키가 어긋납니다: %1").arg(Problems.join(QStringLiteral(", ")));
                    return false;
                }
                continue;
            }
            QString Problem;
            if (!gui_.Exec(Each, &Problem)) {
                Restore();
                if (Error) *Error = QStringLiteral("%1\n— %2").arg(Problem, Summary(Each, 80));
                return false;
            }
        }
        emit Applied();
        return true;
    }
}
