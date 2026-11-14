#include "stdafx.h"
#include "sqliteTransfer.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteApi.hpp"

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/StyleProps.h>
#include <fmstyle/ThemeColors.h>
#include <fmstyle/ThemeManager.h>
#include <fmwidgets/SegmentedControl.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <cmath>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QDateTime>
#include <QSet>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTableWidget>
#include <QVBoxLayout>

namespace hc::sqlite
{
    namespace
    {
        using T = fm::style::Token;
        constexpr qint64 MaxImportBytes = 512ll * 1024 * 1024;

        // 같은 이름의 열(SELECT a.id, b.id)은 id, id_2 처럼 나눈다(JSON 열쇠 · 새 테이블 열).
        QStringList Unique(QStringList Names)
        {
            QSet<QString> Seen;
            for (int Index = 0; Index < Names.size(); ++Index) {
                auto Name = Names[Index].trimmed();
                if (Name.isEmpty()) Name = QStringLiteral("column%1").arg(Index + 1);
                auto Candidate = Name;
                for (int Suffix = 2; Seen.contains(Candidate.toLower()); ++Suffix) Candidate = QStringLiteral("%1_%2").arg(Name).arg(Suffix);
                Seen.insert(Candidate.toLower());
                Names[Index] = Candidate;
            }
            return Names;
        }

        QJsonValue JsonOf(const QVariant& Value)
        {
            if (!Value.isValid() || Value.isNull()) return QJsonValue::Null;
            switch (Value.typeId()) {
            case QMetaType::LongLong: case QMetaType::Int: case QMetaType::ULongLong: return QJsonValue(Value.toLongLong());
            case QMetaType::Double: { const double Number = Value.toDouble(); return std::isfinite(Number) ? QJsonValue(Number) : QJsonValue(QJsonValue::Null); }
            case QMetaType::QByteArray: return QString::fromLatin1(Value.toByteArray().toBase64());
            default: return Value.toString();
            }
        }

        QVariant ValueOf(const QJsonValue& Value)
        {
            switch (Value.type()) {
            case QJsonValue::Null: case QJsonValue::Undefined: return {};
            case QJsonValue::Bool: return qint64(Value.toBool() ? 1 : 0);
            case QJsonValue::Double: {
                const auto Variant = Value.toVariant();
                if (Variant.typeId() == QMetaType::LongLong || Variant.typeId() == QMetaType::Int) return Variant.toLongLong();
                return Value.toDouble();
            }
            case QJsonValue::String: return Value.toString();
            case QJsonValue::Array: return QString::fromUtf8(QJsonDocument(Value.toArray()).toJson(QJsonDocument::Compact));
            case QJsonValue::Object: return QString::fromUtf8(QJsonDocument(Value.toObject()).toJson(QJsonDocument::Compact));
            }
            return {};
        }

        // 열 이름 수만큼 행을 맞춘다(짧으면 NULL 로 채우고, 길면 열을 더한다).
        void Square(ImportSource& Source)
        {
            qsizetype Width = Source.columns_.size();
            for (const auto& Row : Source.rows_) Width = qMax(Width, Row.size());
            for (qsizetype Index = Source.columns_.size(); Index < Width; ++Index) Source.columns_.append(QString());
            Source.columns_ = Unique(Source.columns_);
            for (auto& Row : Source.rows_) Row.resize(Width);
        }

        QString DelimiterName(QChar Delimiter)
        {
            if (Delimiter == u'\t') return QObject::tr("탭");
            if (Delimiter == u';') return QObject::tr("세미콜론");
            if (Delimiter == u'|') return QObject::tr("세로줄");
            return QObject::tr("쉼표");
        }

        void Tone(QWidget* Widget, T Token)
        {
            auto Palette = Widget->palette();
            Palette.setColor(QPalette::WindowText, fm::style::themeColorsFor(Widget)[Token]);
            Widget->setPalette(Palette);
        }
    }

    QString ExtensionFor(TransferFormat Format)
    {
        return Format == TransferFormat::Json ? QStringLiteral("json") : Format == TransferFormat::Sql ? QStringLiteral("sql") : QStringLiteral("csv");
    }

    QString CsvField(const QVariant& Value, QChar Delimiter)
    {
        if (!Value.isValid() || Value.isNull()) return {};
        QString Text;
        switch (Value.typeId()) {
        case QMetaType::QByteArray: return QString::fromLatin1(Value.toByteArray().toHex().toUpper());
        case QMetaType::Double: Text = QString::number(Value.toDouble(), 'g', QLocale::FloatingPointShortest); break;
        default: Text = Value.toString(); break;
        }
        const bool Quote = Text.contains(Delimiter) || Text.contains(u'"') || Text.contains(u'\n') || Text.contains(u'\r')
                           || (!Text.isEmpty() && (Text.front().isSpace() || Text.back().isSpace()));
        if (!Quote) return Text;
        Text.replace(QStringLiteral("\""), QStringLiteral("\"\""));
        return QLatin1Char('"') + Text + QLatin1Char('"');
    }

    ImportSource ReadCsv(const QByteArray& Bytes, const CsvOptions& Options)
    {
        ImportSource Source;
        Source.format_ = TransferFormat::Csv;
        const QString Text = QString::fromUtf8(Bytes.startsWith("\xEF\xBB\xBF") ? Bytes.mid(3) : Bytes);
        QChar Delimiter = Options.delimiter_;
        if (Delimiter.isNull()) {
            // 첫 줄(따옴표 밖)에 가장 많이 나온 후보.
            Delimiter = u',';
            int Best = 0;
            for (const QChar Candidate : {QChar(u','), QChar(u';'), QChar(u'\t'), QChar(u'|')}) {
                int Count = 0;
                bool Quoted = false;
                for (const QChar Ch : Text) {
                    if (Ch == u'"') Quoted = !Quoted;
                    else if (!Quoted && (Ch == u'\n' || Ch == u'\r')) break;
                    else if (!Quoted && Ch == Candidate) ++Count;
                }
                if (Count > Best) { Best = Count; Delimiter = Candidate; }
            }
        }
        Source.delimiter_ = Delimiter;
        QVector<QVariant> Row;
        QString Field;
        bool Quoted = false, FieldQuoted = false, RowQuoted = false;
        const auto EndField = [&] {
            Row.append(!FieldQuoted && Field.isEmpty() && Options.emptyIsNull_ ? QVariant() : QVariant(Field));
            Field.clear();
            FieldQuoted = false;
        };
        const auto EndRow = [&] {
            EndField();
            // 빈 줄은 건너뛴다.
            const bool Blank = Row.size() == 1 && !RowQuoted && (!Row[0].isValid() || Row[0].toString().isEmpty());
            if (!Blank) Source.rows_.append(Row);
            Row.clear();
            RowQuoted = false;
        };
        for (qsizetype Index = 0; Index < Text.size(); ++Index) {
            const QChar Ch = Text[Index];
            if (Quoted) {
                if (Ch == u'"') {
                    if (Index + 1 < Text.size() && Text[Index + 1] == u'"') { Field += u'"'; ++Index; }
                    else Quoted = false;
                } else Field += Ch;
                continue;
            }
            if (Ch == u'"' && Field.isEmpty() && !FieldQuoted) { Quoted = FieldQuoted = RowQuoted = true; }
            else if (Ch == Delimiter) EndField();
            else if (Ch == u'\r') { if (Index + 1 < Text.size() && Text[Index + 1] == u'\n') ++Index; EndRow(); }
            else if (Ch == u'\n') EndRow();
            else Field += Ch;
        }
        if (Quoted) { Source.error_ = QObject::tr("닫히지 않은 따옴표가 있습니다(%1번째 행).").arg(Source.rows_.size() + 1); return Source; }
        if (!Field.isEmpty() || FieldQuoted || !Row.isEmpty()) EndRow();
        if (Options.header_ && !Source.rows_.isEmpty()) {
            for (const auto& Name : Source.rows_.takeFirst()) Source.columns_.append(Name.toString());
        }
        Square(Source);
        return Source;
    }

    ImportSource ReadJson(const QByteArray& Bytes)
    {
        ImportSource Source;
        Source.format_ = TransferFormat::Json;
        QJsonParseError Problem{};
        const auto Document = QJsonDocument::fromJson(Bytes, &Problem);
        QList<QJsonValue> Items;
        if (Problem.error == QJsonParseError::NoError) {
            if (Document.isArray()) for (const auto& Item : Document.array()) Items.append(Item);
            else Items.append(Document.object());
        } else {
            // JSON Lines: 한 줄에 객체(또는 배열) 하나.
            int Line = 0;
            for (auto Each : Bytes.split('\n')) {
                ++Line;
                Each = Each.trimmed();
                if (Each.isEmpty()) continue;
                QJsonParseError LineProblem{};
                const auto One = QJsonDocument::fromJson(Each, &LineProblem);
                if (LineProblem.error != QJsonParseError::NoError) {
                    Source.error_ = Line == 1 ? QObject::tr("JSON 을 읽지 못했습니다: %1 (%2바이트)").arg(Problem.errorString()).arg(Problem.offset)
                                              : QObject::tr("JSON Lines %1번째 줄을 읽지 못했습니다: %2").arg(Line).arg(LineProblem.errorString());
                    return Source;
                }
                Items.append(One.isArray() ? QJsonValue(One.array()) : QJsonValue(One.object()));
            }
        }
        if (Items.isEmpty()) return Source;
        if (Items.first().isObject()) {
            // 열 순서는 처음 나온 순서.
            QHash<QString, int> Index;
            for (const auto& Item : Items)
                for (const auto& Key : Item.toObject().keys())
                    if (!Index.contains(Key)) { Index.insert(Key, int(Source.columns_.size())); Source.columns_.append(Key); }
            // QJsonObject::keys 는 정렬되어 있으므로 첫 객체의 원래 순서는 알 수 없다 — 정렬 순서를 쓴다.
            for (const auto& Item : Items) {
                const auto Object = Item.toObject();
                QVector<QVariant> Row(Source.columns_.size());
                for (auto It = Object.begin(); It != Object.end(); ++It) Row[Index.value(It.key())] = ValueOf(It.value());
                Source.rows_.append(Row);
            }
        } else {
            // 배열의 배열: 첫 줄이 모두 글이고 줄이 둘 이상이면 열 이름.
            auto First = Items.first().toArray();
            bool Names = Items.size() > 1 && !First.isEmpty();
            for (const auto& Cell : First) Names &= Cell.isString();
            int Start = 0;
            if (Names) { for (const auto& Cell : First) Source.columns_.append(Cell.toString()); Start = 1; }
            for (int Row = Start; Row < Items.size(); ++Row) {
                QVector<QVariant> Values;
                for (const auto& Cell : Items[Row].toArray()) Values.append(ValueOf(Cell));
                Source.rows_.append(Values);
            }
        }
        Square(Source);
        return Source;
    }

    ImportSource ReadImportFile(const QString& Path, const CsvOptions& Options)
    {
        ImportSource Source;
        QFile File(Path);
        if (!File.open(QIODevice::ReadOnly)) { Source.error_ = QObject::tr("파일을 열 수 없습니다: %1").arg(File.errorString()); return Source; }
        if (File.size() > MaxImportBytes) { Source.error_ = QObject::tr("512 MiB 보다 큰 파일은 가져오지 않습니다."); return Source; }
        const auto Bytes = File.readAll();
        const auto Suffix = QFileInfo(Path).suffix().toLower();
        if (Suffix == QLatin1String("json") || Suffix == QLatin1String("jsonl") || Suffix == QLatin1String("ndjson")) return ReadJson(Bytes);
        return ReadCsv(Bytes, Options);
    }

    QString InferType(const QVector<QVector<QVariant>>& Rows, int Column)
    {
        static const QRegularExpression Integer(QStringLiteral("^[+-]?\\d{1,18}$"));
        bool Any = false, Real = false;
        for (const auto& Row : Rows) {
            if (Column >= Row.size()) continue;
            const auto& Value = Row[Column];
            if (!Value.isValid() || Value.isNull()) continue;
            Any = true;
            switch (Value.typeId()) {
            case QMetaType::LongLong: case QMetaType::Int: continue;
            case QMetaType::Double: Real = true; continue;
            case QMetaType::QByteArray: return QStringLiteral("BLOB");
            default: break;
            }
            const auto Text = Value.toString().trimmed();
            if (Integer.match(Text).hasMatch()) continue;
            bool Ok = false;
            Text.toDouble(&Ok);
            if (!Ok || Text.isEmpty()) return QStringLiteral("TEXT");
            Real = true;
        }
        return !Any ? QStringLiteral("TEXT") : Real ? QStringLiteral("REAL") : QStringLiteral("INTEGER");
    }

    QStringList MatchColumns(const QStringList& Source, const QStringList& Target, int* Matched)
    {
        QStringList Result(Source.size());
        int Count = 0;
        for (int Index = 0; Index < Source.size(); ++Index)
            for (const auto& Name : Target)
                if (Name.compare(Source[Index].trimmed(), Qt::CaseInsensitive) == 0) { Result[Index] = Name; ++Count; break; }
        if (Count == 0) {
            Count = int(qMin(Source.size(), Target.size()));
            for (int Index = 0; Index < Count; ++Index) Result[Index] = Target[Index];
        }
        if (Matched) *Matched = Count;
        return Result;
    }

    TransferResult ExportData(Connection* Db, const ExportRequest& Request, const std::atomic_bool& Cancel, std::atomic<qint64>& Progress)
    {
        TransferResult Result;
        QSaveFile File(Request.path_);
        if (!File.open(QIODevice::WriteOnly)) { Result.error_ = QObject::tr("파일을 쓸 수 없습니다: %1").arg(File.errorString()); return Result; }
        std::unique_ptr<Statement> Query;
        QStringList Columns = Request.columns_;
        if (!Request.sql_.isEmpty()) {
            if (!Db) { File.cancelWriting(); Result.error_ = QObject::tr("데이터베이스 연결이 없습니다."); return Result; }
            Query = std::make_unique<Statement>(*Db, Request.sql_);
            if (!Query->Valid()) { File.cancelWriting(); Result.error_ = Query->Error(); return Result; }
            if (!Query->ReadOnly()) { File.cancelWriting(); Result.error_ = QObject::tr("읽기 문(SELECT)만 내보냅니다."); return Result; }
            Columns.clear();
            for (int Column = 0; Column < Query->ColumnCount(); ++Column) Columns.append(Query->ColumnName(Column));
        }
        const auto Keys = Unique(Columns);
        QByteArray Buffer;
        const auto Put = [&](const QString& Text) {
            Buffer += Text.toUtf8();
            if (Buffer.size() > (1 << 20)) { File.write(Buffer); Buffer.clear(); }
        };
        const QString Table = QuoteIdentifier(Request.table_.isEmpty() ? QStringLiteral("export") : Request.table_);
        QString ColumnList;
        for (const auto& Name : Columns) ColumnList += (ColumnList.isEmpty() ? QString() : QStringLiteral(", ")) + QuoteIdentifier(Name);
        switch (Request.format_) {
        case TransferFormat::Csv:
            if (Request.bom_) Buffer += "\xEF\xBB\xBF";
            if (Request.header_) {
                QStringList Head;
                for (const auto& Name : Columns) Head.append(CsvField(Name, Request.delimiter_));
                Put(Head.join(Request.delimiter_) + QStringLiteral("\r\n"));
            }
            break;
        case TransferFormat::Json: Put(QStringLiteral("[")); break;
        case TransferFormat::Sql:
            Put(QStringLiteral("-- %1 · %2\nBEGIN TRANSACTION;\n").arg(Request.source_, QDateTime::currentDateTime().toString(Qt::ISODate)));
            if (!Request.createSql_.isEmpty()) Put(Request.createSql_.trimmed() + QStringLiteral(";\n"));
            break;
        }
        qint64 Count = 0;
        const auto Write = [&](const QVector<QVariant>& Values) {
            switch (Request.format_) {
            case TransferFormat::Csv: {
                QStringList Cells;
                for (const auto& Value : Values) Cells.append(CsvField(Value, Request.delimiter_));
                Put(Cells.join(Request.delimiter_) + QStringLiteral("\r\n"));
                break;
            }
            case TransferFormat::Json: {
                QJsonObject Object;
                for (int Column = 0; Column < Keys.size(); ++Column) Object.insert(Keys[Column], JsonOf(Values.value(Column)));
                Put((Count == 0 ? QStringLiteral("\n") : QStringLiteral(",\n")) + QString::fromUtf8(QJsonDocument(Object).toJson(QJsonDocument::Compact)));
                break;
            }
            case TransferFormat::Sql: {
                QStringList Literals;
                for (const auto& Value : Values) Literals.append(SqlLiteral(Value));
                Put(QStringLiteral("INSERT INTO %1 (%2) VALUES (%3);\n").arg(Table, ColumnList, Literals.join(QStringLiteral(", "))));
                break;
            }
            }
            Progress.store(++Count);
        };
        if (Query) {
            int Rc = SQLITE_OK;
            QVector<QVariant> Values(Columns.size());
            while ((Rc = Query->Step()) == SQLITE_ROW) {
                for (int Column = 0; Column < Columns.size(); ++Column) Values[Column] = Query->Value(Column);
                Write(Values);
                if ((Count & 255) == 0 && Cancel.load()) { File.cancelWriting(); Result.canceled_ = true; Result.rows_ = Count; return Result; }
            }
            if (Rc != SQLITE_DONE) { File.cancelWriting(); Result.error_ = Query->Error(); return Result; }
        } else {
            for (const auto& Row : Request.rows_) {
                Write(Row);
                if ((Count & 255) == 0 && Cancel.load()) { File.cancelWriting(); Result.canceled_ = true; Result.rows_ = Count; return Result; }
            }
        }
        if (Request.format_ == TransferFormat::Json) Put(Count ? QStringLiteral("\n]\n") : QStringLiteral("]\n"));
        if (Request.format_ == TransferFormat::Sql) Put(QStringLiteral("COMMIT;\n"));
        File.write(Buffer);
        if (!File.commit()) { Result.error_ = QObject::tr("파일을 쓰지 못했습니다: %1").arg(File.errorString()); return Result; }
        Result.ok_ = true;
        Result.rows_ = Count;
        return Result;
    }

    TransferResult ImportData(Connection& Db, const ImportSource& Source, const ImportRequest& Request, const std::atomic_bool& Cancel, std::atomic<qint64>& Progress)
    {
        TransferResult Result;
        QList<int> Indexes;
        QStringList Names;
        for (int Index = 0; Index < Source.columns_.size(); ++Index) {
            const auto Target = Request.create_ ? Source.columns_[Index] : Request.targets_.value(Index);
            if (Target.isEmpty()) continue;
            Indexes.append(Index);
            Names.append(Target);
        }
        if (Names.isEmpty() || Request.table_.trimmed().isEmpty()) { Result.error_ = QObject::tr("넣을 열이나 테이블이 없습니다."); return Result; }
        const auto Table = Qualified(Request.database_.isEmpty() ? QStringLiteral("main") : Request.database_, Request.table_.trimmed());
        if (Cancel.load()) { Result.canceled_ = true; return Result; }
        QString Error;
        if (!Db.Exec(QStringLiteral("BEGIN IMMEDIATE"), &Error)) { Result.error_ = Error; return Result; }
        const auto Fail = [&](const QString& Text) {
            Db.Exec(QStringLiteral("ROLLBACK"), nullptr);
            Result.error_ = Text;
            return Result;
        };
        if (Request.create_) {
            QStringList Definitions;
            for (int Index = 0; Index < Names.size(); ++Index)
                Definitions.append(QuoteIdentifier(Names[Index]) + QLatin1Char(' ') + Request.types_.value(Indexes[Index], QStringLiteral("TEXT")));
            if (!Db.Exec(QStringLiteral("CREATE TABLE %1 (%2)").arg(Table, Definitions.join(QStringLiteral(", "))), &Error)) return Fail(Error);
        }
        QStringList Quoted, Marks;
        for (int Index = 0; Index < Names.size(); ++Index) { Quoted.append(QuoteIdentifier(Names[Index])); Marks.append(QStringLiteral("?%1").arg(Index + 1)); }
        {
            Statement Insert(Db, QStringLiteral("INSERT INTO %1 (%2) VALUES (%3)").arg(Table, Quoted.join(QStringLiteral(", ")), Marks.join(QStringLiteral(", "))));
            if (!Insert.Valid()) return Fail(Insert.Error());
            for (qsizetype Row = 0; Row < Source.rows_.size(); ++Row) {
                if ((Row & 255) == 0 && Cancel.load()) {
                    Db.Exec(QStringLiteral("ROLLBACK"), nullptr);
                    Result.canceled_ = true;
                    return Result;
                }
                const auto& Values = Source.rows_[Row];
                for (int Index = 0; Index < Indexes.size(); ++Index) Insert.Bind(Index + 1, Values.value(Indexes[Index]));
                if (Insert.Step() != SQLITE_DONE) return Fail(QObject::tr("원본 %1번째 행: %2").arg(Row + 1).arg(Insert.Error()));
                Insert.Reset();
                Progress.store(Row + 1);
            }
        }
        if (!Db.Exec(QStringLiteral("COMMIT"), &Error)) return Fail(Error);
        Result.ok_ = true;
        Result.rows_ = Source.rows_.size();
        return Result;
    }

    // ── 내보내기 대화 ───────────────────────────────────────────────────────

    ExportDialog::ExportDialog(const ExportRequest& Base, bool CanCreate, QWidget* Parent) : QDialog(Parent), base_(Base)
    {
        setObjectName(QStringLiteral("sqliteExportDialog"));
        setWindowTitle(tr("내보내기 — %1").arg(Base.source_));
        auto* Layout = new QVBoxLayout(this);
        Layout->setContentsMargins(20, 16, 20, 12);
        Layout->setSpacing(10);
        auto* Form = new QFormLayout;
        Form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        auto* Source = new QLabel(Base.sql_.isEmpty() ? tr("%1 — 읽은 %2행").arg(Base.source_, QLocale().toString(Base.rows_.size())) : Base.source_, this);
        Source->setObjectName(QStringLiteral("sqliteExportSource"));
        Form->addRow(tr("원본"), Source);
        format_ = new fm::ui::SegmentedControl(QStringList{QStringLiteral("CSV"), QStringLiteral("JSON"), tr("SQL (INSERT)")}, this);
        format_->setObjectName(QStringLiteral("sqliteExportFormat"));
        format_->setSegmentSize(fm::ui::SegmentedControl::Small);
        format_->setCurrentIndex(int(Base.format_));
        Form->addRow(tr("형식"), format_);
        auto* PathRow = new QHBoxLayout;
        path_ = new QLineEdit(QDir::toNativeSeparators(Base.path_), this);
        path_->setObjectName(QStringLiteral("sqliteExportPath"));
        auto* Browse = new QPushButton(tr("찾아보기…"), this);
        PathRow->addWidget(path_, 1);
        PathRow->addWidget(Browse);
        Form->addRow(tr("파일"), PathRow);
        Layout->addLayout(Form);

        csvBox_ = new QWidget(this);
        auto* Csv = new QFormLayout(csvBox_);
        Csv->setContentsMargins(0, 0, 0, 0);
        header_ = new QCheckBox(tr("첫 줄에 열 이름"), csvBox_);
        header_->setChecked(Base.header_);
        delimiter_ = new QComboBox(csvBox_);
        delimiter_->setObjectName(QStringLiteral("sqliteExportDelimiter"));
        for (const QChar Each : {QChar(u','), QChar(u';'), QChar(u'\t'), QChar(u'|')}) delimiter_->addItem(DelimiterName(Each), QString(Each));
        delimiter_->setCurrentIndex(qMax(0, delimiter_->findData(QString(Base.delimiter_))));
        bom_ = new QCheckBox(tr("UTF-8 BOM 넣기 (엑셀에서 한글이 바로 보임)"), csvBox_);
        bom_->setChecked(Base.bom_);
        Csv->addRow(QString(), header_);
        Csv->addRow(tr("구분자"), delimiter_);
        Csv->addRow(QString(), bom_);
        Layout->addWidget(csvBox_);

        sqlBox_ = new QWidget(this);
        auto* Sql = new QFormLayout(sqlBox_);
        Sql->setContentsMargins(0, 0, 0, 0);
        table_ = new QLineEdit(Base.table_.isEmpty() ? QStringLiteral("export") : Base.table_, sqlBox_);
        table_->setObjectName(QStringLiteral("sqliteExportTable"));
        create_ = new QCheckBox(tr("CREATE TABLE 문 넣기"), sqlBox_);
        create_->setObjectName(QStringLiteral("sqliteExportCreate"));
        create_->setEnabled(CanCreate && !Base.createSql_.isEmpty());
        create_->setChecked(create_->isEnabled());
        Sql->addRow(tr("INSERT 대상"), table_);
        Sql->addRow(QString(), create_);
        Layout->addWidget(sqlBox_);

        auto* Note = new QLabel(tr("NULL 은 CSV 에서 빈 칸, JSON 에서 null 입니다. BLOB 은 CSV 에서 16진, JSON 에서 Base64 글로 씁니다. 큰 표도 작업 스레드에서 한 행씩 씁니다."), this);
        Note->setWordWrap(true);
        Tone(Note, T::Fg3);
        Layout->addWidget(Note);
        problem_ = new QLabel(this);
        problem_->setWordWrap(true);
        Tone(problem_, T::Danger);
        Layout->addWidget(problem_);
        Layout->addStretch(1);
        auto* Foot = new QHBoxLayout;
        Foot->addStretch(1);
        auto* Cancel = new QPushButton(tr("취소"), this);
        auto* Ok = new QPushButton(tr("내보내기"), this);
        Ok->setObjectName(QStringLiteral("sqliteExportOk"));
        Ok->setDefault(true);
        fm::style::setButtonRole(Ok, fm::style::ButtonRole::Primary);
        Foot->addWidget(Cancel);
        Foot->addWidget(Ok);
        Layout->addLayout(Foot);
        connect(Cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(Ok, &QPushButton::clicked, this, &ExportDialog::accept);
        connect(format_, &fm::ui::SegmentedControl::currentIndexChanged, this, &ExportDialog::refresh);
        connect(Browse, &QPushButton::clicked, this, [this] {
            const auto Format = TransferFormat(format_->currentIndex());
            const auto Filter = Format == TransferFormat::Json ? tr("JSON (*.json)") : Format == TransferFormat::Sql ? tr("SQL (*.sql)") : tr("CSV (*.csv)");
            const auto Path = QFileDialog::getSaveFileName(this, tr("내보낼 파일"), path_->text(), Filter);
            if (!Path.isEmpty()) path_->setText(QDir::toNativeSeparators(Path));
        });
        refresh();
        resize(520, 440);
    }

    void ExportDialog::SetFormat(TransferFormat Format) { format_->setCurrentIndex(int(Format)); refresh(); }

    void ExportDialog::refresh()
    {
        const auto Format = TransferFormat(format_->currentIndex());
        csvBox_->setVisible(Format == TransferFormat::Csv);
        sqlBox_->setVisible(Format == TransferFormat::Sql);
        // 파일 확장자를 형식에 맞춘다(사용자가 다른 확장자를 쓴 경우는 둔다).
        auto Path = path_->text();
        static const QRegularExpression Known(QStringLiteral("\\.(csv|json|sql)$"), QRegularExpression::CaseInsensitiveOption);
        if (Known.match(Path).hasMatch()) path_->setText(Path.replace(Known, QLatin1Char('.') + ExtensionFor(Format)));
    }

    ExportRequest ExportDialog::Request() const
    {
        auto Request = base_;
        Request.format_ = TransferFormat(format_->currentIndex());
        Request.path_ = QDir::fromNativeSeparators(path_->text().trimmed());
        Request.header_ = header_->isChecked();
        const auto Delimiter = delimiter_->currentData().toString();
        Request.delimiter_ = Delimiter.isEmpty() ? QChar(u',') : Delimiter.front();
        Request.bom_ = bom_->isChecked();
        Request.table_ = table_->text().trimmed();
        if (!create_->isChecked()) Request.createSql_.clear();
        return Request;
    }

    void ExportDialog::accept()
    {
        const auto Path = path_->text().trimmed();
        if (Path.isEmpty()) { problem_->setText(tr("파일 경로를 넣어 주세요.")); return; }
        if (!QFileInfo(QFileInfo(Path).absolutePath()).isDir()) { problem_->setText(tr("폴더가 없습니다.")); return; }
        if (TransferFormat(format_->currentIndex()) == TransferFormat::Sql && table_->text().trimmed().isEmpty()) { problem_->setText(tr("INSERT 대상 이름을 넣어 주세요.")); return; }
        QDialog::accept();
    }

    // ── 가져오기 대화 ───────────────────────────────────────────────────────

    ImportDialog::ImportDialog(const QString& Path, const QList<std::pair<QString, QStringList>>& Tables, const QString& Table, QWidget* Parent)
        : QDialog(Parent), tables_(Tables)
    {
        setObjectName(QStringLiteral("sqliteImportDialog"));
        setWindowTitle(tr("가져오기 (CSV · JSON)"));
        auto* Layout = new QVBoxLayout(this);
        Layout->setContentsMargins(20, 16, 20, 12);
        Layout->setSpacing(10);
        auto* Form = new QFormLayout;
        Form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        auto* PathRow = new QHBoxLayout;
        path_ = new QLineEdit(QDir::toNativeSeparators(Path), this);
        path_->setObjectName(QStringLiteral("sqliteImportPath"));
        auto* Browse = new QPushButton(tr("찾아보기…"), this);
        PathRow->addWidget(path_, 1);
        PathRow->addWidget(Browse);
        Form->addRow(tr("파일"), PathRow);
        Layout->addLayout(Form);

        csvBox_ = new QWidget(this);
        auto* Csv = new QHBoxLayout(csvBox_);
        Csv->setContentsMargins(0, 0, 0, 0);
        Csv->addWidget(new QLabel(tr("구분자"), csvBox_));
        delimiter_ = new QComboBox(csvBox_);
        delimiter_->setObjectName(QStringLiteral("sqliteImportDelimiter"));
        delimiter_->addItem(tr("자동"), QString());
        for (const QChar Each : {QChar(u','), QChar(u';'), QChar(u'\t'), QChar(u'|')}) delimiter_->addItem(DelimiterName(Each), QString(Each));
        Csv->addWidget(delimiter_);
        header_ = new QCheckBox(tr("첫 줄은 열 이름"), csvBox_);
        header_->setChecked(true);
        emptyNull_ = new QCheckBox(tr("빈 칸은 NULL"), csvBox_);
        emptyNull_->setChecked(true);
        Csv->addWidget(header_);
        Csv->addWidget(emptyNull_);
        Csv->addStretch(1);
        Layout->addWidget(csvBox_);

        auto* Target = new QGridLayout;
        existing_ = new QRadioButton(tr("기존 테이블"), this);
        existing_->setObjectName(QStringLiteral("sqliteImportExisting"));
        fresh_ = new QRadioButton(tr("새 테이블"), this);
        fresh_->setObjectName(QStringLiteral("sqliteImportNew"));
        table_ = new QComboBox(this);
        table_->setObjectName(QStringLiteral("sqliteImportTable"));
        for (const auto& [Name, Columns] : tables_) table_->addItem(Name, Columns);
        name_ = new QLineEdit(this);
        name_->setObjectName(QStringLiteral("sqliteImportName"));
        Target->addWidget(existing_, 0, 0);
        Target->addWidget(table_, 0, 1);
        Target->addWidget(fresh_, 1, 0);
        Target->addWidget(name_, 1, 1);
        Target->setColumnStretch(1, 1);
        Layout->addLayout(Target);
        if (const int Index = table_->findText(Table, Qt::MatchFixedString); Index >= 0) { table_->setCurrentIndex(Index); existing_->setChecked(true); }
        else { fresh_->setChecked(true); existing_->setEnabled(table_->count() > 0); }
        if (table_->count() == 0) existing_->setEnabled(false);

        preview_ = new QTableWidget(this);
        preview_->setObjectName(QStringLiteral("sqliteImportPreview"));
        preview_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        preview_->verticalHeader()->setDefaultSectionSize(22);
        preview_->setMinimumHeight(180);
        Layout->addWidget(preview_, 1);
        summary_ = new QLabel(this);
        summary_->setObjectName(QStringLiteral("sqliteImportSummary"));
        summary_->setWordWrap(true);
        Tone(summary_, T::Fg2);
        Layout->addWidget(summary_);
        problem_ = new QLabel(this);
        problem_->setWordWrap(true);
        Tone(problem_, T::Danger);
        Layout->addWidget(problem_);
        auto* Foot = new QHBoxLayout;
        auto* Note = new QLabel(tr("한 트랜잭션으로 넣습니다. 중간에 실패하면 모두 되돌립니다. .sql 파일은 쿼리 탭으로 엽니다."), this);
        Note->setWordWrap(true);
        Tone(Note, T::Fg3);
        Foot->addWidget(Note, 1);
        auto* Cancel = new QPushButton(tr("취소"), this);
        auto* Ok = new QPushButton(tr("가져오기"), this);
        Ok->setObjectName(QStringLiteral("sqliteImportOk"));
        Ok->setDefault(true);
        fm::style::setButtonRole(Ok, fm::style::ButtonRole::Primary);
        Foot->addWidget(Cancel);
        Foot->addWidget(Ok);
        Layout->addLayout(Foot);
        connect(Cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(Ok, &QPushButton::clicked, this, &ImportDialog::accept);
        connect(Browse, &QPushButton::clicked, this, [this] {
            const auto Path = QFileDialog::getOpenFileName(this, tr("가져올 파일"), path_->text(), tr("CSV · JSON (*.csv *.tsv *.txt *.json *.jsonl *.ndjson);;모든 파일 (*)"));
            if (!Path.isEmpty()) SetPath(Path);
        });
        connect(path_, &QLineEdit::editingFinished, this, &ImportDialog::reload);
        connect(delimiter_, &QComboBox::currentIndexChanged, this, &ImportDialog::reload);
        for (auto* Box : {header_, emptyNull_}) connect(Box, &QCheckBox::toggled, this, &ImportDialog::reload);
        for (auto* Radio : {existing_, fresh_}) connect(Radio, &QRadioButton::toggled, this, &ImportDialog::refresh);
        connect(table_, &QComboBox::currentIndexChanged, this, &ImportDialog::refresh);
        connect(name_, &QLineEdit::textChanged, this, &ImportDialog::refresh);
        reload();
        resize(640, 520);
    }

    CsvOptions ImportDialog::options() const
    {
        CsvOptions Options;
        const auto Delimiter = delimiter_->currentData().toString();
        Options.delimiter_ = Delimiter.isEmpty() ? QChar(0) : Delimiter.front();
        Options.header_ = header_->isChecked();
        Options.emptyIsNull_ = emptyNull_->isChecked();
        return Options;
    }

    void ImportDialog::SetPath(const QString& Path) { path_->setText(QDir::toNativeSeparators(Path)); reload(); }
    void ImportDialog::SetNewTable(const QString& Name) { fresh_->setChecked(true); name_->setText(Name); refresh(); }
    void ImportDialog::SetTarget(const QString& Table)
    {
        if (const int Index = table_->findText(Table, Qt::MatchFixedString); Index >= 0) { table_->setCurrentIndex(Index); existing_->setChecked(true); }
        refresh();
    }

    void ImportDialog::reload()
    {
        const auto Path = QDir::fromNativeSeparators(path_->text().trimmed());
        source_ = Path.isEmpty() ? ImportSource{} : ReadImportFile(Path, options());
        csvBox_->setVisible(source_.format_ == TransferFormat::Csv);
        if (name_->text().isEmpty() && !Path.isEmpty()) name_->setText(QFileInfo(Path).completeBaseName());
        preview_->clear();
        preview_->setColumnCount(int(source_.columns_.size()));
        preview_->setHorizontalHeaderLabels(source_.columns_);
        const int Rows = int(qMin<qsizetype>(source_.rows_.size(), 50));
        preview_->setRowCount(Rows);
        for (int Row = 0; Row < Rows; ++Row)
            for (int Column = 0; Column < source_.columns_.size(); ++Column) {
                const auto& Value = source_.rows_[Row].value(Column);
                auto* Item = new QTableWidgetItem(Value.isValid() ? Value.toString() : QStringLiteral("NULL"));
                if (!Value.isValid()) Item->setForeground(fm::style::themeColorsFor(this)[T::Fg3]);
                preview_->setItem(Row, Column, Item);
            }
        refresh();
    }

    ImportRequest ImportDialog::Request() const
    {
        ImportRequest Request;
        Request.create_ = fresh_->isChecked();
        if (Request.create_) {
            Request.table_ = name_->text().trimmed();
            for (int Column = 0; Column < source_.columns_.size(); ++Column) Request.types_.append(InferType(source_.rows_, Column));
        } else {
            Request.table_ = table_->currentText();
            Request.targets_ = MatchColumns(source_.columns_, table_->currentData().toStringList());
        }
        return Request;
    }

    void ImportDialog::refresh()
    {
        table_->setEnabled(existing_->isChecked());
        name_->setEnabled(fresh_->isChecked());
        problem_->setText(source_.error_);
        if (!source_.error_.isEmpty() || source_.columns_.isEmpty()) { summary_->clear(); return; }
        QString Text = tr("열 %1개 · 행 %2개").arg(source_.columns_.size()).arg(QLocale().toString(source_.rows_.size()));
        if (source_.format_ == TransferFormat::Csv) Text += tr(" · 구분자 %1").arg(DelimiterName(source_.delimiter_));
        if (existing_->isChecked()) {
            int Matched = 0;
            const auto Targets = MatchColumns(source_.columns_, table_->currentData().toStringList(), &Matched);
            bool ByName = false;
            for (int Index = 0; Index < Targets.size(); ++Index) ByName |= !Targets[Index].isEmpty() && Targets[Index].compare(source_.columns_[Index], Qt::CaseInsensitive) == 0;
            Text += ByName ? tr(" · 이름이 맞는 열 %1개(나머지는 버림)").arg(Matched) : tr(" · 이름이 맞는 열이 없어 앞에서부터 %1개를 순서대로 넣음").arg(Matched);
        } else {
            QStringList Types;
            for (int Column = 0; Column < source_.columns_.size(); ++Column) Types.append(QStringLiteral("%1 %2").arg(source_.columns_[Column], InferType(source_.rows_, Column)));
            Text += tr(" · 새 테이블: %1").arg(Types.join(QStringLiteral(", ")));
        }
        summary_->setText(Text);
    }

    void ImportDialog::accept()
    {
        if (!source_.error_.isEmpty()) { problem_->setText(source_.error_); return; }
        if (source_.columns_.isEmpty()) { problem_->setText(tr("가져올 행이 없습니다.")); return; }
        if (fresh_->isChecked()) {
            const auto Name = name_->text().trimmed();
            if (Name.isEmpty()) { problem_->setText(tr("새 테이블 이름을 넣어 주세요.")); return; }
            for (const auto& Each : tables_) if (Each.first.compare(Name, Qt::CaseInsensitive) == 0) { problem_->setText(tr("같은 이름의 테이블이 있습니다.")); return; }
        }
        QDialog::accept();
    }
}
