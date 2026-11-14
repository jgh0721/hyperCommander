#include "stdafx.h"
#include "sqliteResultModel.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteApi.hpp"
#include "sqliteConnection.hpp"
#include "sqliteWorker.hpp"

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <mutex>
#include <QLocale>
#include <QThreadPool>

namespace hc::sqlite
{
    Affinity AffinityOf(const QString& DeclaredType)
    {
        const auto Type = DeclaredType.toUpper();
        if (Type.contains(QLatin1String("INT"))) return Affinity::Integer;
        if (Type.contains(QLatin1String("CHAR")) || Type.contains(QLatin1String("CLOB")) || Type.contains(QLatin1String("TEXT"))) return Affinity::Text;
        if (Type.isEmpty() || Type.contains(QLatin1String("BLOB"))) return Affinity::Blob;
        if (Type.contains(QLatin1String("REAL")) || Type.contains(QLatin1String("FLOA")) || Type.contains(QLatin1String("DOUB"))) return Affinity::Real;
        return Affinity::Numeric;
    }

    bool StorageMismatch(Affinity Column, int StorageType)
    {
        if (StorageType == SQLITE_NULL) return false;
        switch (Column) {
        case Affinity::Integer: case Affinity::Real: case Affinity::Numeric:
            return StorageType == SQLITE_TEXT || StorageType == SQLITE_BLOB;
        case Affinity::Text: return StorageType == SQLITE_BLOB;
        case Affinity::Blob: return false;
        }
        return false;
    }

    QString BlobSummary(const QByteArray& Blob)
    {
        const auto Head = Blob.left(8).toHex(' ').toUpper();
        return QObject::tr("BLOB · %1").arg(QLocale().formattedDataSize(Blob.size(), 1, QLocale::DataSizeTraditionalFormat))
            + (Head.isEmpty() ? QString() : QStringLiteral(" · ") + QString::fromLatin1(Head));
    }

    QString DisplayText(const QVariant& Value)
    {
        if (!Value.isValid() || Value.isNull()) return QStringLiteral("NULL");
        switch (Value.typeId()) {
        case QMetaType::QByteArray: return BlobSummary(Value.toByteArray());
        case QMetaType::Double: return QString::number(Value.toDouble(), 'g', 15);
        case QMetaType::LongLong: return QString::number(Value.toLongLong());
        default: {
            // 그리드는 한 줄만 보인다. 줄바꿈은 ↵ 로, 아주 긴 글은 앞부분만(전체는 값 패널).
            auto Text = Value.toString();
            if (Text.size() > 2000) Text = Text.left(2000) + QChar(0x2026);
            Text.replace(QLatin1String("\r\n"), QString(QChar(0x21B5)));
            Text.replace(QLatin1Char('\n'), QChar(0x21B5));
            Text.replace(QLatin1Char('\r'), QChar(0x21B5));
            Text.replace(QLatin1Char('\t'), QLatin1Char(' '));
            return Text;
        }
        }
    }

    QVariant CellData(const QVariant& Value, int Role, Affinity Column, const QString& DeclaredType)
    {
        const bool Null = !Value.isValid() || Value.isNull();
        const int Storage = Null ? SQLITE_NULL
            : Value.typeId() == QMetaType::LongLong ? SQLITE_INTEGER
            : Value.typeId() == QMetaType::Double ? SQLITE_FLOAT
            : Value.typeId() == QMetaType::QByteArray ? SQLITE_BLOB : SQLITE_TEXT;
        switch (Role) {
        case Qt::DisplayRole: return DisplayText(Value);
        case Qt::EditRole: case ValueRole: return Value;
        case Qt::ToolTipRole:
            if (Storage == SQLITE_TEXT && Value.toString().size() > 80) return Value.toString().left(1000);
            return {};
        case NullRole: return Null;
        case StorageRole: return Storage;
        case DeclaredTypeRole: return DeclaredType;
        case MismatchRole: return StorageMismatch(Column, Storage);
        case NumericRole: return Storage == SQLITE_INTEGER || Storage == SQLITE_FLOAT;
        case Qt::TextAlignmentRole:
            return QVariant::fromValue(Qt::AlignVCenter | ((Storage == SQLITE_INTEGER || Storage == SQLITE_FLOAT) ? Qt::AlignRight : Qt::AlignLeft));
        default: return {};
        }
    }

    TableModel::TableModel(Connection& Reader, Opener OpenWorker, QObject* Parent)
        : QAbstractTableModel(Parent), reader_(Reader), openWorker_(std::move(OpenWorker))
    {
    }

    TableModel::~TableModel()
    {
        Cancel();
        // 시작한 작업이 연결을 닫을 때까지 기다린다(닫힌 문서의 파일을 곧바로 옮길 수 있게).
        for (const auto& Job : retired_) Job->Wait(3000);
    }

    void TableModel::SetSource(const ObjectInfo& Object)
    {
        Cancel();
        beginResetModel();
        source_ = Object;
        // 가상 테이블의 숨은 열은 SELECT * 처럼 보이지 않는다(생성 열은 hidden_ 이 아니다).
        source_.columns_.erase(std::remove_if(source_.columns_.begin(), source_.columns_.end(), [](const ColumnInfo& Column) { return Column.hidden_; }), source_.columns_.end());
        affinity_.clear();
        for (const auto& Column : source_.columns_) affinity_.append(AffinityOf(Column.type_));
        keyset_ = source_.HasRowid();
        where_.clear(); orderBy_.clear();
        keys_.clear(); pages_.clear(); recent_.clear(); rows_ = 0; error_.clear();
        // WITHOUT ROWID 는 기본 키로 행을 찾는다.
        keyColumns_.clear();
        if (!keyset_)
            for (const auto& Name : source_.PrimaryKey())
                for (int Column = 0; Column < source_.columns_.size(); ++Column)
                    if (source_.columns_[Column].name_ == Name) keyColumns_.append(Column);
        const bool Had = HasPending();
        edits_.clear(); inserted_.clear(); conflicts_.clear();
        endResetModel();
        if (Had) emit PendingChanged();
    }

    void TableModel::SetFilter(const QString& Where, const QString& OrderBy)
    {
        where_ = Where.trimmed();
        orderBy_ = OrderBy.trimmed();
    }

    QString TableModel::selectList() const
    {
        QStringList Names;
        for (const auto& Column : source_.columns_) Names.append(QuoteIdentifier(Column.name_));
        return Names.isEmpty() ? QStringLiteral("*") : Names.join(QStringLiteral(", "));
    }

    QString TableModel::fromClause() const { return Qualified(source_.database_, source_.name_); }

    QString TableModel::GeneratedSql(int Limit) const
    {
        auto Sql = QStringLiteral("SELECT %1* FROM %2").arg(keyset_ ? QStringLiteral("rowid, ") : QString(), QuoteIdentifier(source_.name_));
        if (!where_.isEmpty()) Sql += QStringLiteral(" WHERE ") + where_;
        if (!orderBy_.isEmpty()) Sql += QStringLiteral(" ORDER BY ") + orderBy_;
        if (Limit > 0) Sql += QStringLiteral(" LIMIT %1").arg(Limit);
        return Sql;
    }

    void TableModel::Cancel()
    {
        if (job_) { job_->Cancel(); retired_.append(std::exchange(job_, {})); }
        retired_.erase(std::remove_if(retired_.begin(), retired_.end(), [](const auto& Job) { return Job->Done(); }), retired_.end());
        if (loading_) { loading_ = false; emit LoadingChanged(false); }
    }

    void TableModel::Reload()
    {
        Cancel();
        beginResetModel();
        keys_.clear(); pages_.clear(); recent_.clear(); rows_ = 0; error_.clear(); pageError_.clear();
        endResetModel();
        if (source_.name_.isEmpty() || !openWorker_) return;
        const auto Generation = ++generation_;
        auto Shared = std::make_shared<WorkerJob>();
        job_ = Shared;
        loading_ = true;
        timer_.start();
        emit LoadingChanged(true);
        QString Sql;
        if (keyset_) {
            Sql = QStringLiteral("SELECT rowid FROM %1").arg(fromClause());
            if (!where_.isEmpty()) Sql += QStringLiteral(" WHERE ") + where_;
            Sql += QStringLiteral(" ORDER BY ") + (orderBy_.isEmpty() ? QStringLiteral("rowid") : orderBy_ + QStringLiteral(", rowid"));
        } else {
            Sql = QStringLiteral("SELECT count(*) FROM %1").arg(fromClause());
            if (!where_.isEmpty()) Sql += QStringLiteral(" WHERE ") + where_;
        }
        const bool Keyset = keyset_;
        QThreadPool::globalInstance()->start([Shared, Owner = this, Opener = openWorker_, Sql, Keyset, Generation] {
            if (!Shared->Begin()) return;
            QString Error;
            QVector<qint64> Keys;
            qint64 Count = 0;
            if (auto Db = Opener(&Error); Db && Shared->Attach(Db.get())) {
                Statement Query(*Db, Sql);
                if (!Query.Valid()) Error = Query.Error();
                else if (Keyset) {
                    int Rc;
                    while ((Rc = Query.Step()) == SQLITE_ROW) Keys.append(sqlite3_column_int64(Query.Handle(), 0));
                    if (Rc != SQLITE_DONE) Error = Query.Error();
                    Count = Keys.size();
                } else if (Query.Step() == SQLITE_ROW) Count = Query.Value(0).toLongLong();
                else Error = Query.Error();
                Shared->Detach();
            }
            Shared->Finish();
            Shared->Post([&] {
                QMetaObject::invokeMethod(Owner, [Owner, Generation, Keys = std::move(Keys), Count, Error]() mutable {
                    Owner->finishLoad(Generation, std::move(Keys), Count, Error);
                }, Qt::QueuedConnection);
            });
        });
    }

    void TableModel::finishLoad(quint64 Generation, QVector<qint64> Keys, qint64 Count, QString Error)
    {
        if (Generation != generation_) return;
        job_.reset();
        elapsedMs_ = timer_.isValid() ? timer_.elapsed() : 0;
        loading_ = false;
        if (!Error.isEmpty()) {
            error_ = Error;
            emit LoadingChanged(false);
            emit Failed(Error);
            return;
        }
        beginResetModel();
        keys_ = std::move(Keys);
        rows_ = Count;
        endResetModel();
        emit LoadingChanged(false);
        emit Loaded(rows_, elapsedMs_);
    }

    int TableModel::rowCount(const QModelIndex& Parent) const
    {
        // 새 행(보류)은 읽은 행 뒤에 붙는다.
        return Parent.isValid() ? 0 : int(qMin<qint64>(rows_ + inserted_.size(), std::numeric_limits<int>::max()));
    }

    int TableModel::columnCount(const QModelIndex& Parent) const { return Parent.isValid() ? 0 : int(source_.columns_.size()); }

    bool TableModel::fetchPage(int PageIndex) const
    {
        const qint64 First = qint64(PageIndex) * PageRows;
        if (First >= rows_) return false;
        const int Count = int(qMin<qint64>(PageRows, rows_ - First));
        Page Fetched;
        Fetched.rows_.resize(Count);
        const int Columns = int(source_.columns_.size());
        if (keyset_) {
            QStringList Marks;
            for (int Index = 0; Index < Count; ++Index) Marks.append(QStringLiteral("?%1").arg(Index + 1));
            Statement Query(reader_, QStringLiteral("SELECT rowid, %1 FROM %2 WHERE rowid IN (%3)").arg(selectList(), fromClause(), Marks.join(QLatin1Char(','))));
            if (!Query.Valid()) { pageError_ = Query.Error(); return false; }
            QHash<qint64, int> Position;
            for (int Index = 0; Index < Count; ++Index) {
                const auto Key = keys_[int(First) + Index];
                Query.Bind(Index + 1, Key);
                Position.insert(Key, Index);
                Fetched.rows_[Index].rowid_ = Key;
                Fetched.rows_[Index].values_.resize(Columns);
            }
            while (Query.Step() == SQLITE_ROW) {
                const auto Found = Position.constFind(sqlite3_column_int64(Query.Handle(), 0));
                if (Found == Position.cend()) continue;
                auto& Target = Fetched.rows_[*Found].values_;
                for (int Column = 0; Column < Columns; ++Column) Target[Column] = Query.Value(Column + 1);
            }
        } else {
            auto Sql = QStringLiteral("SELECT %1 FROM %2").arg(selectList(), fromClause());
            if (!where_.isEmpty()) Sql += QStringLiteral(" WHERE ") + where_;
            if (!orderBy_.isEmpty()) Sql += QStringLiteral(" ORDER BY ") + orderBy_;
            Sql += QStringLiteral(" LIMIT %1 OFFSET %2").arg(Count).arg(First);
            Statement Query(reader_, Sql);
            if (!Query.Valid()) { pageError_ = Query.Error(); return false; }
            int Index = 0;
            while (Index < Count && Query.Step() == SQLITE_ROW) {
                auto& Target = Fetched.rows_[Index++].values_;
                Target.resize(Columns);
                for (int Column = 0; Column < Columns && Column < Query.ColumnCount(); ++Column) Target[Column] = Query.Value(Column);
            }
            for (; Index < Count; ++Index) Fetched.rows_[Index].values_.resize(Columns);
        }
        pages_.insert(PageIndex, std::move(Fetched));
        recent_.removeAll(PageIndex);
        recent_.append(PageIndex);
        while (recent_.size() > KeptPages) pages_.remove(recent_.takeFirst());
        return true;
    }

    const TableModel::Row* TableModel::row(int Index) const
    {
        if (Index < 0 || Index >= rows_) return nullptr;
        const int PageIndex = Index / PageRows;
        auto Found = pages_.constFind(PageIndex);
        if (Found == pages_.cend()) {
            if (!fetchPage(PageIndex)) return nullptr;
            Found = pages_.constFind(PageIndex);
        } else if (recent_.isEmpty() || recent_.last() != PageIndex) {
            recent_.removeAll(PageIndex);
            recent_.append(PageIndex);
        }
        return &Found->rows_[Index % PageRows];
    }

    QVariant TableModel::data(const QModelIndex& Index, int Role) const
    {
        if (!Index.isValid()) return {};
        const int Column = Index.column();
        if (Index.row() >= rows_) {
            // 새 행: 넣은 칸은 그 값, 넣지 않은 칸은 rowid 별칭이면 ‘자동’, 기본값이 있으면 그 식, 아니면 NULL.
            const int Insert = Index.row() - int(rows_);
            if (Insert >= inserted_.size() || Column >= source_.columns_.size()) return {};
            if (Role == PendingRole) return int(PendingState::Inserted);
            const auto& Values = inserted_[Insert].values_;
            if (Values.contains(Column)) return CellData(Values.value(Column), Role, affinity_.value(Column, Affinity::Blob), source_.columns_[Column].type_);
            const auto& Info = source_.columns_[Column];
            if (Role == UnsetRole) return rowidAlias(Column) || !Info.defaultValue_.isEmpty();
            if (Role == Qt::DisplayRole) return rowidAlias(Column) ? QObject::tr("자동") : !Info.defaultValue_.isEmpty() ? Info.defaultValue_ : QStringLiteral("NULL");
            if (Role == NullRole) return !rowidAlias(Column) && Info.defaultValue_.isEmpty();
            return CellData(QVariant(), Role, affinity_.value(Column, Affinity::Blob), Info.type_);
        }
        const auto* Row = row(Index.row());
        if (!Row || Column >= Row->values_.size()) return Role == Qt::DisplayRole && !pageError_.isEmpty() ? QVariant(pageError_) : QVariant();
        if (Role == RowIdRole) return keyset_ ? QVariant(Row->rowid_) : QVariant();
        if (edits_.isEmpty() && conflicts_.isEmpty()) {
            if (Role == PendingRole) return int(PendingState::None);
            if (Role == ConflictRole || Role == UnsetRole) return false;
            return CellData(Row->values_[Column], Role, affinity_.value(Column, Affinity::Blob), source_.columns_[Column].type_);
        }
        const auto Key = rowKey(*Row);
        const auto Found = edits_.constFind(Key);
        if (Role == PendingRole) {
            if (Found == edits_.cend()) return int(PendingState::None);
            if (Found->deleted_) return int(PendingState::Deleted);
            return int(Found->changed_.contains(Column) ? PendingState::Modified : PendingState::None);
        }
        if (Role == ConflictRole) return conflicts_.contains(Key + QLatin1Char('/') + QString::number(Column));
        if (Role == UnsetRole) return false;
        const bool Changed = Found != edits_.cend() && Found->changed_.contains(Column);
        return CellData(Changed ? Found->changed_.value(Column) : Row->values_[Column], Role, affinity_.value(Column, Affinity::Blob), source_.columns_[Column].type_);
    }

    QVariant TableModel::headerData(int Section, Qt::Orientation Orientation, int Role) const
    {
        if (Orientation == Qt::Vertical) {
            // 새 행은 번호 대신 +(시안 ‘테이블 탭 — 보류 편집’).
            if (Role == Qt::DisplayRole) return Section >= rows_ ? QVariant(QStringLiteral("+")) : QVariant(Section + 1);
            if (Role == Qt::TextAlignmentRole) return QVariant::fromValue((Section >= rows_ ? Qt::AlignHCenter : Qt::AlignRight) | Qt::AlignVCenter);
            return {};
        }
        if (Section < 0 || Section >= source_.columns_.size()) return {};
        const auto& Column = source_.columns_[Section];
        switch (Role) {
        case Qt::DisplayRole: return Column.name_;
        case DeclaredTypeRole: return Column.type_;
        case Qt::ToolTipRole: {
            QStringList Parts{Column.name_};
            if (!Column.type_.isEmpty()) Parts.append(Column.type_);
            if (Column.primaryKey_ > 0) Parts.append(QObject::tr("기본 키"));
            if (!Column.foreignTable_.isEmpty()) Parts.append(QObject::tr("외래 키 → %1(%2)").arg(Column.foreignTable_, Column.foreignColumn_));
            if (Column.notNull_) Parts.append(QStringLiteral("NOT NULL"));
            return Parts.join(QStringLiteral(" · "));
        }
        case NumericRole: {
            const auto Kind = affinity_.value(Section, Affinity::Blob);
            return Kind == Affinity::Integer || Kind == Affinity::Real || Kind == Affinity::Numeric;
        }
        default: return {};
        }
    }

    QueryModel::QueryModel(QObject* Parent) : QAbstractTableModel(Parent) {}

    void QueryModel::SetColumns(const QList<Column>& Columns)
    {
        beginResetModel();
        columns_ = Columns;
        affinity_.clear();
        for (const auto& Each : columns_) affinity_.append(AffinityOf(Each.declaredType_));
        rows_.clear();
        more_ = fetching_ = false;
        endResetModel();
    }

    void QueryModel::AppendRows(QVector<QVector<QVariant>> Rows, bool More)
    {
        fetching_ = false;
        more_ = More;
        if (Rows.isEmpty()) return;
        beginInsertRows({}, int(rows_.size()), int(rows_.size() + Rows.size() - 1));
        rows_ += std::move(Rows);
        endInsertRows();
    }

    int QueryModel::rowCount(const QModelIndex& Parent) const { return Parent.isValid() ? 0 : int(rows_.size()); }
    int QueryModel::columnCount(const QModelIndex& Parent) const { return Parent.isValid() ? 0 : int(columns_.size()); }

    QVariant QueryModel::data(const QModelIndex& Index, int Role) const
    {
        if (!Index.isValid() || Index.row() >= rows_.size() || Index.column() >= columns_.size()) return {};
        const auto& Row = rows_[Index.row()];
        return CellData(Index.column() < Row.size() ? Row[Index.column()] : QVariant(), Role, affinity_.value(Index.column(), Affinity::Blob), columns_[Index.column()].declaredType_);
    }

    QVariant QueryModel::headerData(int Section, Qt::Orientation Orientation, int Role) const
    {
        if (Orientation == Qt::Vertical) {
            if (Role == Qt::DisplayRole) return Section + 1;
            if (Role == Qt::TextAlignmentRole) return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
            return {};
        }
        if (Section < 0 || Section >= columns_.size()) return {};
        const auto& Each = columns_[Section];
        switch (Role) {
        case Qt::DisplayRole: return Each.name_;
        case DeclaredTypeRole: return Each.declaredType_;
        case Qt::ToolTipRole: return Each.table_.isEmpty() ? Each.name_ : QStringLiteral("%1 · %2.%1").arg(Each.name_, Each.table_);
        case NumericRole: {
            const auto Kind = affinity_.value(Section, Affinity::Blob);
            return Kind == Affinity::Integer || Kind == Affinity::Real || Kind == Affinity::Numeric;
        }
        default: return {};
        }
    }

    bool QueryModel::canFetchMore(const QModelIndex& Parent) const { return !Parent.isValid() && more_ && !fetching_ && fetch_; }

    void QueryModel::fetchMore(const QModelIndex& Parent)
    {
        if (!canFetchMore(Parent)) return;
        fetching_ = true;
        fetch_();
    }

    QVariant CoerceValue(const QVariant& Value, Affinity Column)
    {
        if (!Value.isValid() || Value.isNull()) return {};
        switch (Value.typeId()) {
        case QMetaType::QByteArray: return Value;
        case QMetaType::Int: case QMetaType::LongLong: case QMetaType::UInt: case QMetaType::ULongLong: return Value.toLongLong();
        case QMetaType::Double: case QMetaType::Float: return Value.toDouble();
        default: break;
        }
        const auto Text = Value.toString();
        if (Column == Affinity::Text || Column == Affinity::Blob) return Text;
        // 숫자 친화도: 정수로 읽히면 정수, 실수로 읽히면 실수(INTEGER 열의 1.0 은 SQLite 가 정수로 바꾼다), 아니면 글 그대로.
        bool Ok = false;
        if (const auto Number = Text.trimmed().toLongLong(&Ok); Ok && Column != Affinity::Real) return Number;
        if (const auto Number = Text.trimmed().toDouble(&Ok); Ok && !Text.trimmed().isEmpty()) return Number;
        return Text;
    }

    namespace
    {
        bool SameValue(const QVariant& Left, const QVariant& Right)
        {
            const bool LeftNull = !Left.isValid() || Left.isNull(), RightNull = !Right.isValid() || Right.isNull();
            if (LeftNull || RightNull) return LeftNull == RightNull;
            return Left.typeId() == Right.typeId() && Left == Right;
        }
    }

    void TableModel::SetReadOnly(bool ReadOnly) { readOnly_ = ReadOnly; }

    bool TableModel::Editable() const
    {
        if (readOnly_ || source_.name_.isEmpty() || source_.type_ != ObjectType::Table || source_.virtual_) return false;
        return keyset_ || !keyColumns_.isEmpty();
    }

    bool TableModel::rowidAlias(int Column) const
    {
        // rowid 테이블의 INTEGER PRIMARY KEY 한 열은 rowid 의 다른 이름이다(넣지 않으면 SQLite 가 정한다).
        if (!keyset_ || Column < 0 || Column >= source_.columns_.size()) return false;
        int Keys = 0;
        for (const auto& Each : source_.columns_) Keys += Each.primaryKey_ > 0;
        return Keys == 1 && source_.columns_[Column].primaryKey_ > 0 && source_.columns_[Column].type_.compare(QLatin1String("INTEGER"), Qt::CaseInsensitive) == 0;
    }

    QString TableModel::rowKey(const Row& Item) const
    {
        if (keyset_) return QString::number(Item.rowid_);
        QStringList Parts;
        for (const int Column : keyColumns_) Parts.append(SqlLiteral(Item.values_.value(Column)));
        return Parts.join(QChar(0x1F));
    }

    QString TableModel::tableText() const
    {
        return source_.database_.isEmpty() || source_.database_.compare(QLatin1String("main"), Qt::CaseInsensitive) == 0
            ? IdentifierText(source_.name_) : IdentifierText(source_.database_) + QLatin1Char('.') + IdentifierText(source_.name_);
    }

    QString TableModel::keyCondition(const Edit& Item, bool Literal) const
    {
        if (keyset_) return Literal ? QStringLiteral("rowid = %1").arg(Item.rowid_) : QStringLiteral("rowid = ?");
        QStringList Parts;
        for (int Index = 0; Index < keyColumns_.size(); ++Index)
            Parts.append(QStringLiteral("%1 IS %2").arg(IdentifierText(source_.columns_[keyColumns_[Index]].name_),
                                                       Literal ? SqlLiteral(Item.keyValues_.value(Index)) : QStringLiteral("?")));
        return Parts.join(QStringLiteral(" AND "));
    }

    TableModel::PendingSummary TableModel::Pending() const
    {
        PendingSummary Summary;
        for (const auto& Item : edits_) {
            if (Item.deleted_) ++Summary.deleted_;
            else if (!Item.changed_.isEmpty()) ++Summary.modified_;
        }
        Summary.inserted_ = int(inserted_.size());
        return Summary;
    }

    Qt::ItemFlags TableModel::flags(const QModelIndex& Index) const
    {
        auto Flags = QAbstractTableModel::flags(Index);
        if (!Index.isValid() || !Editable()) return Flags;
        if (data(Index, PendingRole).toInt() == int(PendingState::Deleted)) return Flags;
        // 생성 열(GENERATED ALWAYS AS)은 값을 넣을 수 없다.
        if (Index.column() < source_.columns_.size() && source_.columns_[Index.column()].generated_) return Flags;
        return Flags | Qt::ItemIsEditable;
    }

    bool TableModel::setData(const QModelIndex& Index, const QVariant& Value, int Role)
    {
        if (Role != Qt::EditRole || !Index.isValid() || !Editable() || Index.column() >= source_.columns_.size()) return false;
        if (source_.columns_[Index.column()].generated_) return false;
        const int Column = Index.column();
        const auto Coerced = CoerceValue(Value, affinity_.value(Column, Affinity::Blob));
        if (Index.row() >= rows_) {
            const int Insert = Index.row() - int(rows_);
            if (Insert >= inserted_.size()) return false;
            inserted_[Insert].values_.insert(Column, Coerced);
            emit dataChanged(Index, Index);
            emit PendingChanged();
            return true;
        }
        const auto* Row = row(Index.row());
        if (!Row) return false;
        const auto Key = rowKey(*Row);
        auto It = edits_.find(Key);
        if (It == edits_.end()) {
            Edit Fresh;
            Fresh.original_ = Row->values_;
            Fresh.rowid_ = Row->rowid_;
            for (const int KeyColumn : keyColumns_) Fresh.keyValues_.append(Row->values_.value(KeyColumn));
            It = edits_.insert(Key, Fresh);
        }
        if (It->deleted_) return false;
        if (SameValue(Coerced, It->original_.value(Column))) It->changed_.remove(Column);
        else It->changed_.insert(Column, Coerced);
        if (It->changed_.isEmpty()) edits_.erase(It);
        conflicts_.remove(Key + QLatin1Char('/') + QString::number(Column));
        emit dataChanged(Index.siblingAtColumn(0), Index.siblingAtColumn(columnCount() - 1));
        emit PendingChanged();
        return true;
    }

    int TableModel::InsertRow(int CopyFrom)
    {
        if (!Editable()) return -1;
        Inserted Fresh;
        if (CopyFrom >= 0 && CopyFrom < rowCount()) {
            // 복제: 기본 키 · rowid 별칭은 빼고(겹치지 않게), 넣지 않은 칸은 그대로 비워 둔다.
            for (int Column = 0; Column < source_.columns_.size(); ++Column) {
                if (source_.columns_[Column].primaryKey_ > 0 || source_.columns_[Column].generated_) continue;
                const auto Source = index(CopyFrom, Column);
                if (data(Source, UnsetRole).toBool()) continue;
                Fresh.values_.insert(Column, data(Source, ValueRole));
            }
        }
        const int Row = rowCount();
        beginInsertRows({}, Row, Row);
        inserted_.append(Fresh);
        endInsertRows();
        emit PendingChanged();
        return Row;
    }

    void TableModel::ToggleDelete(QList<int> Rows)
    {
        if (!Editable()) return;
        std::sort(Rows.begin(), Rows.end(), std::greater<int>());
        Rows.erase(std::unique(Rows.begin(), Rows.end()), Rows.end());
        bool Changed = false;
        for (const int Index : Rows) {
            if (Index < 0 || Index >= rowCount()) continue;
            if (Index >= rows_) {
                const int Insert = Index - int(rows_);
                beginRemoveRows({}, Index, Index);
                inserted_.removeAt(Insert);
                endRemoveRows();
                Changed = true;
                continue;
            }
            const auto* Row = row(Index);
            if (!Row) continue;
            const auto Key = rowKey(*Row);
            auto It = edits_.find(Key);
            if (It == edits_.end()) {
                Edit Fresh;
                Fresh.original_ = Row->values_;
                Fresh.rowid_ = Row->rowid_;
                for (const int KeyColumn : keyColumns_) Fresh.keyValues_.append(Row->values_.value(KeyColumn));
                It = edits_.insert(Key, Fresh);
            }
            It->deleted_ = !It->deleted_;
            if (!It->deleted_ && It->changed_.isEmpty()) edits_.erase(It);
            emit dataChanged(index(Index, 0), index(Index, columnCount() - 1));
            Changed = true;
        }
        if (Changed) emit PendingChanged();
    }

    void TableModel::RevertAll()
    {
        if (!HasPending() && conflicts_.isEmpty()) return;
        if (!inserted_.isEmpty()) {
            beginRemoveRows({}, int(rows_), int(rows_ + inserted_.size() - 1));
            inserted_.clear();
            endRemoveRows();
        }
        edits_.clear();
        conflicts_.clear();
        if (rowCount() > 0) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
        emit PendingChanged();
    }

    QStringList TableModel::PendingStatements() const
    {
        QStringList Lines;
        if (!HasPending()) return Lines;
        const auto Table = tableText();
        Lines.append(QStringLiteral("BEGIN IMMEDIATE;"));
        auto Keys = edits_.keys();
        std::sort(Keys.begin(), Keys.end());
        for (const auto& Key : Keys) {
            const auto& Item = edits_[Key];
            if (Item.deleted_) { Lines.append(QStringLiteral("DELETE FROM %1 WHERE %2;").arg(Table, keyCondition(Item, true))); continue; }
            QStringList Sets, Checks;
            auto Columns = Item.changed_.keys();
            std::sort(Columns.begin(), Columns.end());
            for (const int Column : Columns) {
                const auto Name = IdentifierText(source_.columns_[Column].name_);
                Sets.append(QStringLiteral("%1 = %2").arg(Name, SqlLiteral(Item.changed_.value(Column))));
                Checks.append(QStringLiteral("%1 IS %2").arg(Name, SqlLiteral(Item.original_.value(Column))));
            }
            Lines.append(QStringLiteral("UPDATE %1 SET %2 WHERE %3 AND %4;").arg(Table, Sets.join(QStringLiteral(", ")), keyCondition(Item, true), Checks.join(QStringLiteral(" AND "))));
        }
        for (const auto& Item : inserted_) {
            if (Item.values_.isEmpty()) { Lines.append(QStringLiteral("INSERT INTO %1 DEFAULT VALUES;").arg(Table)); continue; }
            QStringList Names, Values;
            auto Columns = Item.values_.keys();
            std::sort(Columns.begin(), Columns.end());
            for (const int Column : Columns) { Names.append(IdentifierText(source_.columns_[Column].name_)); Values.append(SqlLiteral(Item.values_.value(Column))); }
            Lines.append(QStringLiteral("INSERT INTO %1 (%2) VALUES (%3);").arg(Table, Names.join(QStringLiteral(", ")), Values.join(QStringLiteral(", "))));
        }
        Lines.append(QStringLiteral("COMMIT;"));
        return Lines;
    }

    bool TableModel::Commit(QString* Error)
    {
        if (!HasPending()) return true;
        QString Problem;
        if (!reader_.Exec(QStringLiteral("BEGIN IMMEDIATE"), &Problem)) { if (Error) *Error = Problem; return false; }
        const auto Table = Qualified(source_.database_, source_.name_);
        const auto BindKey = [this](Statement& Query, const Edit& Item, int& Next) {
            if (keyset_) Query.Bind(Next++, QVariant(Item.rowid_));
            else for (const auto& Value : Item.keyValues_) Query.Bind(Next++, Value);
        };
        QSet<QString> Conflicts;
        for (auto It = edits_.cbegin(); It != edits_.cend() && Problem.isEmpty(); ++It) {
            const auto& Item = It.value();
            if (Item.deleted_) {
                Statement Query(reader_, QStringLiteral("DELETE FROM %1 WHERE %2").arg(Table, keyCondition(Item, false)));
                int Next = 1;
                BindKey(Query, Item, Next);
                if (!Query.Valid() || Query.Step() != SQLITE_DONE) { Problem = Query.Error(); break; }
                if (sqlite3_changes64(reader_.Handle()) == 0) Conflicts.insert(It.key() + QStringLiteral("/0"));
                continue;
            }
            QStringList Sets, Checks;
            const auto Columns = Item.changed_.keys();
            for (const int Column : Columns) {
                const auto Name = QuoteIdentifier(source_.columns_[Column].name_);
                Sets.append(Name + QStringLiteral(" = ?"));
                Checks.append(Name + QStringLiteral(" IS ?"));
            }
            Statement Query(reader_, QStringLiteral("UPDATE %1 SET %2 WHERE %3 AND %4").arg(Table, Sets.join(QStringLiteral(", ")), keyCondition(Item, false), Checks.join(QStringLiteral(" AND "))));
            int Next = 1;
            for (const int Column : Columns) Query.Bind(Next++, Item.changed_.value(Column));
            BindKey(Query, Item, Next);
            for (const int Column : Columns) Query.Bind(Next++, Item.original_.value(Column));
            if (!Query.Valid() || Query.Step() != SQLITE_DONE) { Problem = Query.Error(); break; }
            // 다른 프로그램이 그 사이에 값을 바꿨거나 행을 지웠다.
            if (sqlite3_changes64(reader_.Handle()) == 0) for (const int Column : Columns) Conflicts.insert(It.key() + QLatin1Char('/') + QString::number(Column));
        }
        for (int Index = 0; Index < inserted_.size() && Problem.isEmpty(); ++Index) {
            const auto& Item = inserted_[Index];
            auto Columns = Item.values_.keys();
            std::sort(Columns.begin(), Columns.end());
            QStringList Names, Marks;
            for (const int Column : Columns) { Names.append(QuoteIdentifier(source_.columns_[Column].name_)); Marks.append(QStringLiteral("?")); }
            Statement Query(reader_, Columns.isEmpty() ? QStringLiteral("INSERT INTO %1 DEFAULT VALUES").arg(Table)
                                                       : QStringLiteral("INSERT INTO %1 (%2) VALUES (%3)").arg(Table, Names.join(QStringLiteral(", ")), Marks.join(QStringLiteral(", "))));
            int Next = 1;
            for (const int Column : Columns) Query.Bind(Next++, Item.values_.value(Column));
            if (!Query.Valid() || Query.Step() != SQLITE_DONE) Problem = Query.Error();
        }
        if (!Problem.isEmpty() || !Conflicts.isEmpty()) {
            reader_.Exec(QStringLiteral("ROLLBACK"), nullptr);
            conflicts_ = Conflicts;
            if (rowCount() > 0) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
            if (Error) *Error = !Problem.isEmpty() ? Problem
                : QObject::tr("다른 곳에서 바뀐 행 %1개 — 원래 값이 달라 쓰지 않았습니다. 표시한 칸을 확인하세요.").arg(Conflicts.size());
            emit PendingChanged();
            return false;
        }
        if (!reader_.Exec(QStringLiteral("COMMIT"), &Problem)) {
            reader_.Exec(QStringLiteral("ROLLBACK"), nullptr);
            if (Error) *Error = Problem;
            return false;
        }
        edits_.clear();
        inserted_.clear();
        conflicts_.clear();
        emit PendingChanged();
        Reload();
        return true;
    }

    void TableModel::sort(int Column, Qt::SortOrder Order)
    {
        if (Column < 0 || Column >= source_.columns_.size()) return;
        orderBy_ = QuoteIdentifier(source_.columns_[Column].name_) + (Order == Qt::DescendingOrder ? QStringLiteral(" DESC") : QString());
        emit OrderChanged(orderBy_);
        Reload();
    }
}
