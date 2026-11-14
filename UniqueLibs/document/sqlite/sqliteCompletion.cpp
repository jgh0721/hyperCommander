#include "stdafx.h"
#include "sqliteCompletion.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteConnection.hpp"
#include "sqliteSql.hpp"

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QLocale>
#include <QRegularExpression>
#include <QSet>

namespace hc::sqlite
{
    namespace
    {
        struct Token
        {
            enum Type { Word, Quoted, String, Number, Punct };
            Type type_ = Punct;
            QString text_;          // Quoted: 따옴표를 벗긴 이름
            int start_ = 0, end_ = 0;
        };

        bool WordChar(QChar Ch) { return Ch.isLetterOrNumber() || Ch == QLatin1Char('_') || Ch == QLatin1Char('$'); }
        bool IsName(const Token& Item) { return Item.type_ == Token::Word || Item.type_ == Token::Quoted; }
        QString Up(const Token& Item) { return Item.type_ == Token::Word ? Item.text_.toUpper() : QString(); }

        // 문법 단어(별칭 · 테이블 이름이 될 수 없는 자리를 가르는 데만 쓴다). sqlite3_keyword 목록 전체는 이름으로도
        // 쓰이는 단어(KEY · ACTION · TEMP …)가 많아 쓰지 않는다.
        const QSet<QString>& ClauseWords()
        {
            static const QSet<QString> Words{
                QStringLiteral("SELECT"), QStringLiteral("FROM"), QStringLiteral("WHERE"), QStringLiteral("JOIN"), QStringLiteral("INNER"),
                QStringLiteral("LEFT"), QStringLiteral("RIGHT"), QStringLiteral("FULL"), QStringLiteral("CROSS"), QStringLiteral("NATURAL"),
                QStringLiteral("OUTER"), QStringLiteral("ON"), QStringLiteral("USING"), QStringLiteral("GROUP"), QStringLiteral("ORDER"),
                QStringLiteral("BY"), QStringLiteral("LIMIT"), QStringLiteral("OFFSET"), QStringLiteral("HAVING"), QStringLiteral("WINDOW"),
                QStringLiteral("UNION"), QStringLiteral("EXCEPT"), QStringLiteral("INTERSECT"), QStringLiteral("SET"), QStringLiteral("VALUES"),
                QStringLiteral("AS"), QStringLiteral("INDEXED"), QStringLiteral("NOT"), QStringLiteral("DEFAULT"), QStringLiteral("RETURNING"),
                QStringLiteral("WITH"), QStringLiteral("INSERT"), QStringLiteral("UPDATE"), QStringLiteral("DELETE"), QStringLiteral("INTO"),
                QStringLiteral("AND"), QStringLiteral("OR"), QStringLiteral("CASE"), QStringLiteral("WHEN"), QStringLiteral("THEN"),
                QStringLiteral("ELSE"), QStringLiteral("END"), QStringLiteral("ASC"), QStringLiteral("DESC"), QStringLiteral("DO"),
                QStringLiteral("CONFLICT"), QStringLiteral("REPLACE"), QStringLiteral("TABLE"), QStringLiteral("VIEW"), QStringLiteral("INDEX")};
            return Words;
        }

        // Text 의 [0, Limit) 를 토큰으로 나눈다. *Inside 는 Limit 이 문자열 · 주석 · 따옴표 이름 안에 있는가.
        QVector<Token> Lex(const QString& Text, int Limit, bool* Inside)
        {
            QVector<Token> Tokens;
            if (Inside) *Inside = false;
            const int Size = int(Text.size()), End = qMin(Limit, Size);
            int At = 0;
            const auto Enclosed = [&](int Stop, bool Closed) {
                // 닫는 글자 뒤가 아니면 안이다. 닫지 않은 것은 끝까지 안이다.
                if (Limit < Stop || (!Closed && Limit >= At)) { if (Inside) *Inside = true; return true; }
                return false;
            };
            while (At < End) {
                const QChar Ch = Text[At];
                if (Ch.isSpace()) { ++At; continue; }
                if (Ch == QLatin1Char('-') && At + 1 < Size && Text[At + 1] == QLatin1Char('-')) {
                    const int Newline = int(Text.indexOf(QLatin1Char('\n'), At));
                    const int Stop = Newline < 0 ? Size : Newline;
                    if (Limit <= Stop) { if (Inside) *Inside = true; return Tokens; }
                    At = Stop;
                    continue;
                }
                if (Ch == QLatin1Char('/') && At + 1 < Size && Text[At + 1] == QLatin1Char('*')) {
                    const int Close = int(Text.indexOf(QStringLiteral("*/"), At + 2));
                    const int Stop = Close < 0 ? Size : Close + 2;
                    if (Enclosed(Stop, Close >= 0)) return Tokens;
                    At = Stop;
                    continue;
                }
                const QChar Closer = Ch == QLatin1Char('\'') ? QLatin1Char('\'') : Ch == QLatin1Char('"') ? QLatin1Char('"')
                                   : Ch == QLatin1Char('`') ? QLatin1Char('`') : Ch == QLatin1Char('[') ? QLatin1Char(']') : QChar();
                if (!Closer.isNull()) {
                    int Index = At + 1, Close = -1;
                    QString Body;
                    while (Index < Size) {
                        if (Text[Index] == Closer) {
                            if (Closer != QLatin1Char(']') && Index + 1 < Size && Text[Index + 1] == Closer) { Body += Closer; Index += 2; continue; }
                            Close = Index;
                            break;
                        }
                        Body += Text[Index++];
                    }
                    const int Stop = Close < 0 ? Size : Close + 1;
                    if (Enclosed(Stop, Close >= 0)) return Tokens;
                    Tokens.append({Ch == QLatin1Char('\'') ? Token::String : Token::Quoted, Body, At, Stop});
                    At = Stop;
                    continue;
                }
                if (WordChar(Ch)) {
                    int Index = At;
                    while (Index < End && WordChar(Text[Index])) ++Index;
                    Tokens.append({Ch.isDigit() ? Token::Number : Token::Word, Text.mid(At, Index - At), At, Index});
                    At = Index;
                    continue;
                }
                Tokens.append({Token::Punct, QString(Ch), At, At + 1});
                ++At;
            }
            return Tokens;
        }

        // Index 의 ( 와 짝인 ) 다음 위치.
        int SkipParens(const QVector<Token>& Tokens, int Index)
        {
            int Depth = 0;
            for (; Index < Tokens.size(); ++Index) {
                if (Tokens[Index].text_ == QLatin1String("(") && Tokens[Index].type_ == Token::Punct) ++Depth;
                else if (Tokens[Index].text_ == QLatin1String(")") && Tokens[Index].type_ == Token::Punct && --Depth == 0) return Index + 1;
            }
            return Index;
        }

        const ObjectInfo* FindObject(const SchemaInfo& Schema, const QString& Database, const QString& Name)
        {
            for (const auto& Db : Schema.databases_) {
                if (!Database.isEmpty() && Db.name_.compare(Database, Qt::CaseInsensitive) != 0) continue;
                for (const auto& Object : Db.objects_)
                    if ((Object.type_ == ObjectType::Table || Object.type_ == ObjectType::View) && Object.name_.compare(Name, Qt::CaseInsensitive) == 0)
                        return &Object;
            }
            return nullptr;
        }

        struct ScopeEntry
        {
            QString database_, name_, alias_;
            bool cte_ = false, definition_ = false;     // definition_: WITH 절에서 만든 자리
            const ObjectInfo* object_ = nullptr;
            int position_ = 0;
            QString Qualifier() const { return alias_.isEmpty() ? name_ : alias_; }
        };

        // 문 전체에서 FROM · JOIN · UPDATE · INTO 의 테이블과 별칭, WITH 의 CTE 를 모은다(부분 질의도 함께).
        QList<ScopeEntry> ParseScope(const QVector<Token>& Tokens, const SchemaInfo& Schema)
        {
            QList<ScopeEntry> Scope;
            QStringList Ctes;
            const auto Clause = [](const Token& Item) { return Item.type_ == Token::Word && ClauseWords().contains(Up(Item)); };
            for (int Index = 0; Index < Tokens.size(); ++Index) {
                const auto Keyword = Up(Tokens[Index]);
                if (Keyword == QLatin1String("WITH")) {
                    int At = Index + 1;
                    if (At < Tokens.size() && Up(Tokens[At]) == QLatin1String("RECURSIVE")) ++At;
                    while (At < Tokens.size() && IsName(Tokens[At]) && !Clause(Tokens[At])) {
                        ScopeEntry Entry;
                        Entry.name_ = Tokens[At].text_;
                        Entry.cte_ = Entry.definition_ = true;
                        Entry.position_ = Tokens[At].start_;
                        Ctes.append(Entry.name_);
                        Scope.append(Entry);
                        ++At;
                        if (At < Tokens.size() && Tokens[At].text_ == QLatin1String("(")) At = SkipParens(Tokens, At);
                        if (At < Tokens.size() && Up(Tokens[At]) == QLatin1String("AS")) ++At;
                        if (At < Tokens.size() && Up(Tokens[At]) == QLatin1String("NOT")) ++At;
                        if (At < Tokens.size() && Up(Tokens[At]) == QLatin1String("MATERIALIZED")) ++At;
                        if (At < Tokens.size() && Tokens[At].text_ == QLatin1String("(")) At = SkipParens(Tokens, At);
                        if (At < Tokens.size() && Tokens[At].text_ == QLatin1String(",")) { ++At; continue; }
                        break;
                    }
                    continue;
                }
                if (Keyword != QLatin1String("FROM") && Keyword != QLatin1String("JOIN") && Keyword != QLatin1String("UPDATE") && Keyword != QLatin1String("INTO"))
                    continue;
                int At = Index + 1;
                if (Keyword == QLatin1String("UPDATE") && At + 1 < Tokens.size() && Up(Tokens[At]) == QLatin1String("OR")) At += 2;
                while (At < Tokens.size() && IsName(Tokens[At]) && !Clause(Tokens[At])) {
                    ScopeEntry Entry;
                    Entry.name_ = Tokens[At].text_;
                    Entry.position_ = Tokens[At].start_;
                    ++At;
                    if (At + 1 < Tokens.size() && Tokens[At].text_ == QLatin1String(".") && IsName(Tokens[At + 1])) {
                        Entry.database_ = Entry.name_;
                        Entry.name_ = Tokens[At + 1].text_;
                        At += 2;
                    }
                    if (At < Tokens.size() && Up(Tokens[At]) == QLatin1String("AS")) ++At;
                    if (At < Tokens.size() && IsName(Tokens[At]) && !Clause(Tokens[At])) Entry.alias_ = Tokens[At++].text_;
                    Entry.cte_ = Entry.database_.isEmpty() && Ctes.contains(Entry.name_, Qt::CaseInsensitive);
                    if (!Entry.cte_) Entry.object_ = FindObject(Schema, Entry.database_, Entry.name_);
                    Scope.append(Entry);
                    if (Keyword == QLatin1String("FROM") && At < Tokens.size() && Tokens[At].text_ == QLatin1String(",")) { ++At; continue; }
                    break;
                }
            }
            return Scope;
        }

        QString KeywordText(const QString& Word, const CompletionSource& Source) { return Source.upperKeywords_ ? Word.toUpper() : Word.toLower(); }

        void AddKeywords(CompletionResult& Result, const QStringList& Words, const CompletionSource& Source, int Bias, bool TrailingSpace = true)
        {
            for (const auto& Word : Words) {
                CompletionCandidate Item;
                Item.label_ = KeywordText(Word, Source);
                Item.insertText_ = Item.label_ + (TrailingSpace ? QStringLiteral(" ") : QString());
                Item.kind_ = CompletionKind::Keyword;
                Item.bias_ = Bias;
                Result.candidates_.append(Item);
            }
        }

        QString ColumnList(const ObjectInfo& Object)
        {
            QStringList Names;
            for (const auto& Column : Object.columns_) if (!Column.hidden_) Names.append(Column.name_);
            const int Count = int(Names.size());
            if (Names.size() > 6) { Names = Names.mid(0, 6); Names.append(QStringLiteral("…")); }
            return QObject::tr("%1 — 열 %2개 (%3)").arg(Object.name_).arg(Count).arg(Names.join(QStringLiteral(", ")));
        }

        void AddTables(CompletionResult& Result, const CompletionSource& Source, const QList<ScopeEntry>& Scope, int Caret, const QString& OnlyDatabase = {})
        {
            // 같은 문의 WITH 절에서 만든 CTE 가 맨 위(시안).
            if (OnlyDatabase.isEmpty()) {
                QStringList Seen;
                for (const auto& Entry : Scope) {
                    if (!Entry.definition_ || Entry.position_ >= Caret || Seen.contains(Entry.name_, Qt::CaseInsensitive)) continue;
                    Seen.append(Entry.name_);
                    CompletionCandidate Item;
                    Item.label_ = Entry.name_;
                    Item.insertText_ = IdentifierText(Entry.name_);
                    Item.detail_ = QObject::tr("CTE · 이 문");
                    Item.description_ = QObject::tr("%1 — WITH 절").arg(Entry.name_);
                    Item.kind_ = CompletionKind::Cte;
                    Item.bias_ = 20;
                    Result.candidates_.append(Item);
                }
            }
            for (const auto& Db : Source.schema_.databases_) {
                if (!OnlyDatabase.isEmpty() && Db.name_.compare(OnlyDatabase, Qt::CaseInsensitive) != 0) continue;
                // 첨부한 데이터베이스는 이름을 붙여 넣는다(한정어를 이미 쳤으면 이름만).
                const bool Qualify = OnlyDatabase.isEmpty() && Db.name_.compare(QLatin1String("main"), Qt::CaseInsensitive) != 0
                                     && Db.name_.compare(QLatin1String("temp"), Qt::CaseInsensitive) != 0;
                for (const auto Type : {ObjectType::Table, ObjectType::View}) {
                    for (const auto* Object : Db.Objects(Type)) {
                        CompletionCandidate Item;
                        Item.label_ = Qualify ? Db.name_ + QLatin1Char('.') + Object->name_ : Object->name_;
                        Item.insertText_ = Qualify ? IdentifierText(Db.name_) + QLatin1Char('.') + IdentifierText(Object->name_) : IdentifierText(Object->name_);
                        Item.filterText_ = Object->name_;
                        Item.kind_ = Type == ObjectType::View ? CompletionKind::View : CompletionKind::Table;
                        if (Type == ObjectType::View) Item.detail_ = QObject::tr("뷰");
                        else if (const auto Count = Source.rowCounts_.value((Db.name_ + QLatin1Char('.') + Object->name_).toLower(), -1); Count >= 0)
                            Item.detail_ = QObject::tr("%1행").arg(QLocale().toString(Count));
                        Item.description_ = ColumnList(*Object);
                        Result.candidates_.append(Item);
                    }
                }
            }
        }

        CompletionCandidate ColumnCandidate(const ObjectInfo& Object, const ColumnInfo& Column, const QString& Qualifier)
        {
            CompletionCandidate Item;
            const auto Name = IdentifierText(Column.name_);
            Item.label_ = Qualifier.isEmpty() ? Column.name_ : Qualifier + QLatin1Char('.') + Column.name_;
            Item.insertText_ = Qualifier.isEmpty() ? Name : IdentifierText(Qualifier) + QLatin1Char('.') + Name;
            Item.kind_ = Column.primaryKey_ > 0 ? CompletionKind::PrimaryKey : !Column.foreignTable_.isEmpty() ? CompletionKind::ForeignKey : CompletionKind::Column;
            Item.detail_ = Column.type_;
            if (!Column.foreignTable_.isEmpty()) Item.detail_ += (Item.detail_.isEmpty() ? QString() : QStringLiteral(" · ")) + QStringLiteral("→ ") + Column.foreignTable_;
            QStringList Parts{QStringLiteral("%1.%2").arg(Object.name_, Column.name_)};
            QString Shape = Column.type_;
            if (Column.primaryKey_ > 0) Shape += QStringLiteral(" PRIMARY KEY");
            if (Column.notNull_) Shape += QStringLiteral(" NOT NULL");
            if (!Column.foreignTable_.isEmpty())
                Shape += QStringLiteral(" REFERENCES %1(%2)").arg(Column.foreignTable_, Column.foreignColumn_.isEmpty() ? QStringLiteral("rowid") : Column.foreignColumn_);
            if (!Shape.trimmed().isEmpty()) Parts.append(Shape.trimmed());
            Item.description_ = Parts.join(QStringLiteral(" · "));
            return Item;
        }

        void AddColumns(CompletionResult& Result, const ObjectInfo& Object, const QString& Qualifier, int Bias = 0)
        {
            for (const auto& Column : Object.columns_) {
                if (Column.hidden_) continue;
                auto Item = ColumnCandidate(Object, Column, Qualifier);
                Item.bias_ = Bias;
                Result.candidates_.append(Item);
            }
        }

        void AddFunctions(CompletionResult& Result, const CompletionSource& Source, int Bias)
        {
            QStringList Seen;
            for (const auto& Name : Source.functions_) {
                if (Seen.contains(Name, Qt::CaseInsensitive)) continue;
                Seen.append(Name);
                CompletionCandidate Item;
                Item.label_ = Name;
                Item.insertText_ = Name + QLatin1Char('(');
                Item.kind_ = CompletionKind::Function;
                Item.detail_ = QObject::tr("함수");
                const auto Signature = FunctionSignature(Name);
                Item.description_ = Signature.isEmpty() ? QObject::tr("%1(…) — 함수").arg(Name) : QString(Signature).replace(QLatin1Char('\n'), QStringLiteral(" — "));
                Item.bias_ = Bias;
                Result.candidates_.append(Item);
            }
        }

        const ScopeEntry* Resolve(const QList<ScopeEntry>& Scope, const QString& Qualifier)
        {
            for (const auto& Entry : Scope) if (!Entry.alias_.isEmpty() && Entry.alias_.compare(Qualifier, Qt::CaseInsensitive) == 0) return &Entry;
            for (const auto& Entry : Scope) if (!Entry.definition_ && Entry.name_.compare(Qualifier, Qt::CaseInsensitive) == 0) return &Entry;
            return nullptr;
        }

        // ON 이 JOIN 의 것인가(CREATE INDEX · TRIGGER … ON 이면 테이블).
        bool JoinOn(const QVector<Token>& Tokens)
        {
            for (int Index = int(Tokens.size()) - 2; Index >= 0; --Index) {
                const auto Word = Up(Tokens[Index]);
                if (Word == QLatin1String("JOIN")) return true;
                if (Word == QLatin1String("INDEX") || Word == QLatin1String("TRIGGER") || Word == QLatin1String("FROM")) return false;
            }
            return false;
        }

        // 커서 앞에서 괄호 깊이 0 의 가장 가까운 절 단어. 여는 괄호를 만나면(괄호 안) 그 앞 이름을 Owner 로 돌려준다.
        QString NearestClause(const QVector<Token>& Tokens, int* OpenIndex)
        {
            int Depth = 0;
            if (OpenIndex) *OpenIndex = -1;
            for (int Index = int(Tokens.size()) - 1; Index >= 0; --Index) {
                const auto& Item = Tokens[Index];
                if (Item.type_ == Token::Punct && Item.text_ == QLatin1String(")")) { ++Depth; continue; }
                if (Item.type_ == Token::Punct && Item.text_ == QLatin1String("(")) {
                    if (Depth == 0) { if (OpenIndex) *OpenIndex = Index; return {}; }
                    --Depth;
                    continue;
                }
                if (Depth == 0 && Item.type_ == Token::Word && ClauseWords().contains(Up(Item))) return Up(Item);
            }
            return {};
        }

        QStringList StatementWords()
        {
            return {QStringLiteral("select"), QStringLiteral("insert into"), QStringLiteral("update"), QStringLiteral("delete from"), QStringLiteral("with"),
                    QStringLiteral("create table"), QStringLiteral("create index"), QStringLiteral("create view"), QStringLiteral("create trigger"),
                    QStringLiteral("drop table"), QStringLiteral("alter table"), QStringLiteral("pragma"), QStringLiteral("explain query plan"),
                    QStringLiteral("begin"), QStringLiteral("commit"), QStringLiteral("rollback"), QStringLiteral("vacuum"), QStringLiteral("analyze"),
                    QStringLiteral("attach database"), QStringLiteral("detach database"), QStringLiteral("reindex"), QStringLiteral("savepoint"),
                    QStringLiteral("release"), QStringLiteral("replace into"), QStringLiteral("values")};
        }
    }

    CompletionResult Complete(const QString& Statement, int Caret, const CompletionSource& Source, bool Explicit)
    {
        Q_UNUSED(Explicit);
        CompletionResult Result;
        Caret = qBound(0, Caret, int(Statement.size()));
        bool Inside = false;
        auto Tokens = Lex(Statement, Caret, &Inside);
        if (Inside) return Result;
        if (!Tokens.isEmpty() && Tokens.last().end_ == Caret && (Tokens.last().type_ == Token::Word || Tokens.last().type_ == Token::Number)) {
            if (Tokens.last().type_ == Token::Number) return Result;
            Result.prefix_ = Tokens.last().text_;
            Tokens.removeLast();
        }
        const auto Scope = ParseScope(Lex(Statement, int(Statement.size()), nullptr), Source.schema_);
        const int PrefixStart = Caret - int(Result.prefix_.size());

        // 한정어: 별칭. · 테이블. · 데이터베이스.
        if (Tokens.size() >= 2 && Tokens.last().type_ == Token::Punct && Tokens.last().text_ == QLatin1String(".") && Tokens.last().end_ == PrefixStart
            && IsName(Tokens[Tokens.size() - 2])) {
            const auto Qualifier = Tokens[Tokens.size() - 2].text_;
            if (const auto* Entry = Resolve(Scope, Qualifier)) {
                Result.context_ = CompletionContext::Columns;
                if (Entry->object_) AddColumns(Result, *Entry->object_, {});
                return Result;
            }
            if (Source.schema_.Database(Qualifier)) {
                Result.context_ = CompletionContext::Tables;
                AddTables(Result, Source, Scope, Caret, Qualifier);
                return Result;
            }
            if (const auto* Object = FindObject(Source.schema_, {}, Qualifier)) {
                Result.context_ = CompletionContext::Columns;
                AddColumns(Result, *Object, {});
            }
            return Result;
        }

        if (Tokens.isEmpty()) {
            Result.context_ = CompletionContext::Statement;
            AddKeywords(Result, StatementWords(), Source, 0);
            return Result;
        }
        const auto& Previous = Tokens.last();
        const auto Word = Up(Previous);
        if (Word == QLatin1String("PRAGMA")) {
            Result.context_ = CompletionContext::Pragma;
            for (const auto& Name : Source.pragmas_) {
                CompletionCandidate Item;
                Item.label_ = Item.insertText_ = Name;
                Item.detail_ = QStringLiteral("PRAGMA");
                Item.description_ = QStringLiteral("PRAGMA %1").arg(Name);
                Item.kind_ = CompletionKind::Pragma;
                Result.candidates_.append(Item);
            }
            return Result;
        }
        if (Word == QLatin1String("COLLATE")) {
            Result.context_ = CompletionContext::Collation;
            auto Names = Source.collations_;
            for (const auto& Builtin : {QStringLiteral("BINARY"), QStringLiteral("NOCASE"), QStringLiteral("RTRIM")})
                if (!Names.contains(Builtin, Qt::CaseInsensitive)) Names.append(Builtin);
            for (const auto& Name : Names) {
                CompletionCandidate Item;
                Item.label_ = Item.insertText_ = Name;
                Item.detail_ = QObject::tr("정렬 규칙");
                Item.kind_ = CompletionKind::Collation;
                Result.candidates_.append(Item);
            }
            return Result;
        }
        if (Word == QLatin1String("FROM") || Word == QLatin1String("JOIN") || Word == QLatin1String("INTO") || Word == QLatin1String("UPDATE")
            || Word == QLatin1String("TABLE")) {
            Result.context_ = CompletionContext::Tables;
            AddTables(Result, Source, Scope, Caret);
            return Result;
        }
        if (Word == QLatin1String("ON")) {
            if (!JoinOn(Tokens)) {
                Result.context_ = CompletionContext::Tables;
                AddTables(Result, Source, Scope, Caret);
                return Result;
            }
            // 이번 JOIN 의 테이블(ON 바로 앞)과 그 앞의 테이블들.
            Result.context_ = CompletionContext::JoinCondition;
            const ScopeEntry* Joined = nullptr;
            QList<const ScopeEntry*> Earlier;
            for (const auto& Entry : Scope) {
                if (Entry.definition_ || Entry.position_ >= Previous.start_) continue;
                if (Joined) Earlier.append(Joined);
                Joined = &Entry;
            }
            if (!Joined) return Result;
            const auto TableName = [](const ScopeEntry& Entry) { return Entry.object_ ? Entry.object_->name_ : Entry.name_; };
            const auto PrimaryOf = [&](const QString& Table) {
                const auto* Object = FindObject(Source.schema_, {}, Table);
                const auto Keys = Object ? Object->PrimaryKey() : QStringList();
                return Keys.size() == 1 ? Keys.first() : QStringLiteral("rowid");
            };
            for (const auto* Other : Earlier) {
                const auto Add = [&](const ScopeEntry& From, const ColumnInfo& Column, const ScopeEntry& To) {
                    if (TableName(To).compare(Column.foreignTable_, Qt::CaseInsensitive) != 0) return;
                    const auto Target = Column.foreignColumn_.isEmpty() ? PrimaryOf(Column.foreignTable_) : Column.foreignColumn_;
                    // 이번 JOIN 쪽을 앞에(시안: a.AlbumId = t.AlbumId).
                    const auto Left = &From == Joined ? QStringLiteral("%1.%2").arg(IdentifierText(From.Qualifier()), IdentifierText(Column.name_))
                                                      : QStringLiteral("%1.%2").arg(IdentifierText(To.Qualifier()), IdentifierText(Target));
                    const auto Right = &From == Joined ? QStringLiteral("%1.%2").arg(IdentifierText(To.Qualifier()), IdentifierText(Target))
                                                       : QStringLiteral("%1.%2").arg(IdentifierText(From.Qualifier()), IdentifierText(Column.name_));
                    CompletionCandidate Item;
                    Item.label_ = Item.insertText_ = Left + QStringLiteral(" = ") + Right;
                    Item.detail_ = QObject::tr("외래 키");
                    Item.description_ = QStringLiteral("%1.%2 REFERENCES %3(%4)").arg(TableName(From), Column.name_, Column.foreignTable_, Target);
                    Item.kind_ = CompletionKind::JoinCondition;
                    Item.bias_ = Source.joinFirst_ ? 1000 : 0;
                    Result.candidates_.append(Item);
                };
                if (Joined->object_) for (const auto& Column : Joined->object_->columns_) if (!Column.foreignTable_.isEmpty()) Add(*Joined, Column, *Other);
                if (Other->object_) for (const auto& Column : Other->object_->columns_) if (!Column.foreignTable_.isEmpty()) Add(*Other, Column, *Joined);
            }
            if (Joined->object_) AddColumns(Result, *Joined->object_, Joined->Qualifier());
            for (const auto* Other : Earlier) if (Other->object_) AddColumns(Result, *Other->object_, Other->Qualifier());
            return Result;
        }

        int Open = -1;
        const auto Clause = NearestClause(Tokens, &Open);
        // INSERT INTO t ( | , | ) — 그 테이블의 열.
        if (Open >= 2 && IsName(Tokens[Open - 1])) {
            int At = Open - 1;
            QString Database, Table = Tokens[At].text_;
            if (At >= 2 && Tokens[At - 1].text_ == QLatin1String(".") && IsName(Tokens[At - 2])) { Database = Tokens[At - 2].text_; At -= 2; }
            if (At >= 1 && Up(Tokens[At - 1]) == QLatin1String("INTO") && (Previous.text_ == QLatin1String("(") || Previous.text_ == QLatin1String(","))) {
                Result.context_ = CompletionContext::Columns;
                if (const auto* Object = FindObject(Source.schema_, Database, Table)) AddColumns(Result, *Object, {});
                return Result;
            }
        }
        // FROM a, | — 테이블.
        if (Previous.type_ == Token::Punct && Previous.text_ == QLatin1String(",") && Clause == QLatin1String("FROM")) {
            Result.context_ = CompletionContext::Tables;
            AddTables(Result, Source, Scope, Caret);
            return Result;
        }
        // FROM 의 테이블 이름 · 별칭 뒤 — 절 단어. 별칭을 치는 자리에서는 띄우지 않는다(팝업이 맞는 것이 없으면 숨는다).
        if (IsName(Previous) && !ClauseWords().contains(Word) && (Clause == QLatin1String("FROM") || Clause == QLatin1String("JOIN")
                                                                  || Clause == QLatin1String("UPDATE") || Clause == QLatin1String("INTO"))) {
            Result.context_ = CompletionContext::Clause;
            QStringList Words{QStringLiteral("as"), QStringLiteral("join"), QStringLiteral("inner join"), QStringLiteral("left join"), QStringLiteral("cross join"),
                              QStringLiteral("natural join"), QStringLiteral("on"), QStringLiteral("using"), QStringLiteral("where"), QStringLiteral("group by"),
                              QStringLiteral("order by"), QStringLiteral("limit"), QStringLiteral("union"), QStringLiteral("union all"), QStringLiteral("except"),
                              QStringLiteral("intersect"), QStringLiteral("having"), QStringLiteral("window"), QStringLiteral("returning")};
            if (Clause == QLatin1String("UPDATE")) Words.prepend(QStringLiteral("set"));
            if (Clause == QLatin1String("INTO")) Words = QStringList{QStringLiteral("values"), QStringLiteral("select"), QStringLiteral("default values")};
            AddKeywords(Result, Words, Source, 0);
            return Result;
        }

        // 식: 범위의 열(같은 이름이 여러 테이블에 있으면 한정어를 붙여 넣는다), 함수, 키워드.
        Result.context_ = CompletionContext::Expression;
        QHash<QString, int> Owners;
        for (const auto& Entry : Scope)
            if (!Entry.definition_ && Entry.object_)
                for (const auto& Column : Entry.object_->columns_) if (!Column.hidden_) ++Owners[Column.name_.toLower()];
        QStringList Done;
        for (const auto& Entry : Scope) {
            if (Entry.definition_ || !Entry.object_) continue;
            const auto Key = Entry.Qualifier().toLower();
            if (Done.contains(Key)) continue;
            Done.append(Key);
            for (const auto& Column : Entry.object_->columns_) {
                if (Column.hidden_) continue;
                auto Item = ColumnCandidate(*Entry.object_, Column, {});
                if (Owners.value(Column.name_.toLower()) > 1 && Scope.size() > 1) {
                    Item.insertText_ = IdentifierText(Entry.Qualifier()) + QLatin1Char('.') + IdentifierText(Column.name_);
                    Item.detail_ = Entry.Qualifier() + (Item.detail_.isEmpty() ? QString() : QStringLiteral(" · ") + Item.detail_);
                }
                Item.bias_ = 10;
                Result.candidates_.append(Item);
            }
        }
        AddFunctions(Result, Source, 0);
        if (!Result.prefix_.isEmpty()) {
            QStringList Words;
            for (const auto& Keyword : Keywords()) Words.append(Keyword);
            AddKeywords(Result, Words, Source, -10, false);
        }
        return Result;
    }

    CallInfo EnclosingCall(const QString& Statement, int Caret)
    {
        CallInfo Info;
        bool Inside = false;
        const auto Tokens = Lex(Statement, qBound(0, Caret, int(Statement.size())), &Inside);
        if (Inside) return Info;
        int Depth = 0, Commas = 0;
        for (int Index = int(Tokens.size()) - 1; Index >= 0; --Index) {
            const auto& Item = Tokens[Index];
            if (Item.type_ != Token::Punct) continue;
            if (Item.text_ == QLatin1String(")")) { ++Depth; continue; }
            if (Item.text_ == QLatin1String(",") && Depth == 0) { ++Commas; continue; }
            if (Item.text_ == QLatin1String(";")) return Info;
            if (Item.text_ == QLatin1String("(")) {
                if (Depth > 0) { --Depth; continue; }
                if (Index > 0 && Tokens[Index - 1].type_ == Token::Word && Tokens[Index - 1].end_ == Item.start_) {
                    Info.name_ = Tokens[Index - 1].text_;
                    Info.argument_ = Commas;
                    Info.open_ = Item.start_;
                }
                return Info;
            }
        }
        return Info;
    }

    QString FunctionSignature(const QString& Name)
    {
        // SQLite 내장 함수(lang_corefunc · lang_aggfunc · lang_datefunc · lang_mathfunc · json1). 설명은 한 줄.
        static const QHash<QString, QString> Table = [] {
            QHash<QString, QString> Map;
            const auto Add = [&](const char* Signature, const QString& Text) {
                const auto Line = QString::fromLatin1(Signature);
                Map.insert(Line.left(Line.indexOf(QLatin1Char('('))).toLower(), Line + QLatin1Char('\n') + Text);
            };
            Add("abs(X)", QObject::tr("절댓값"));
            Add("changes()", QObject::tr("마지막 INSERT · UPDATE · DELETE 가 바꾼 행 수"));
            Add("char(X1, X2, ..., XN)", QObject::tr("유니코드 코드 값으로 만든 문자열"));
            Add("coalesce(X, Y, ...)", QObject::tr("NULL 이 아닌 첫 인자"));
            Add("concat(X, ...)", QObject::tr("인자를 이어 붙인 문자열(NULL 은 빈 문자열)"));
            Add("concat_ws(SEP, X, ...)", QObject::tr("구분자로 이어 붙인 문자열"));
            Add("format(FORMAT, ...)", QObject::tr("printf 형식 문자열"));
            Add("glob(X, Y)", QObject::tr("Y GLOB X"));
            Add("hex(X)", QObject::tr("16진 문자열"));
            Add("ifnull(X, Y)", QObject::tr("X 가 NULL 이면 Y"));
            Add("iif(X, Y, Z)", QObject::tr("X 가 참이면 Y, 아니면 Z"));
            Add("instr(X, Y)", QObject::tr("X 안에서 Y 가 처음 나오는 위치(1부터, 없으면 0)"));
            Add("last_insert_rowid()", QObject::tr("마지막으로 넣은 행의 rowid"));
            Add("length(X)", QObject::tr("글자 수(BLOB 은 바이트 수)"));
            Add("like(X, Y, Z)", QObject::tr("Y LIKE X ESCAPE Z"));
            Add("likelihood(X, Y)", QObject::tr("X 가 참일 확률 Y 를 플래너에 알린다"));
            Add("likely(X)", QObject::tr("X 가 대개 참이라고 플래너에 알린다"));
            Add("lower(X)", QObject::tr("소문자로"));
            Add("ltrim(X, Y)", QObject::tr("왼쪽의 Y 글자들(기본 공백)을 뗀다"));
            Add("max(X, Y, ...)", QObject::tr("가장 큰 인자(인자 하나면 집계)"));
            Add("min(X, Y, ...)", QObject::tr("가장 작은 인자(인자 하나면 집계)"));
            Add("nullif(X, Y)", QObject::tr("X = Y 이면 NULL, 아니면 X"));
            Add("octet_length(X)", QObject::tr("바이트 수"));
            Add("printf(FORMAT, ...)", QObject::tr("format 과 같다"));
            Add("quote(X)", QObject::tr("SQL 리터럴로"));
            Add("random()", QObject::tr("임의의 64비트 정수"));
            Add("randomblob(N)", QObject::tr("임의의 N 바이트 BLOB"));
            Add("replace(X, Y, Z)", QObject::tr("X 안의 Y 를 모두 Z 로"));
            Add("round(X, Y)", QObject::tr("소수 Y 자리로 반올림"));
            Add("rtrim(X, Y)", QObject::tr("오른쪽의 Y 글자들(기본 공백)을 뗀다"));
            Add("sign(X)", QObject::tr("-1 · 0 · +1"));
            Add("soundex(X)", QObject::tr("soundex 부호"));
            Add("sqlite_version()", QObject::tr("SQLite 버전"));
            Add("substr(X, Y, Z)", QObject::tr("Y 번째 글자부터 Z 글자(Z 를 빼면 끝까지)"));
            Add("substring(X, Y, Z)", QObject::tr("substr 과 같다"));
            Add("total_changes()", QObject::tr("연결을 연 뒤 바뀐 행 수"));
            Add("trim(X, Y)", QObject::tr("양쪽의 Y 글자들(기본 공백)을 뗀다"));
            Add("typeof(X)", QObject::tr("저장 형식: null · integer · real · text · blob"));
            Add("unhex(X, Y)", QObject::tr("16진 문자열을 BLOB 으로(Y 의 글자는 건너뛴다)"));
            Add("unicode(X)", QObject::tr("첫 글자의 코드 값"));
            Add("unlikely(X)", QObject::tr("X 가 대개 거짓이라고 플래너에 알린다"));
            Add("upper(X)", QObject::tr("대문자로"));
            Add("zeroblob(N)", QObject::tr("0 으로 찬 N 바이트 BLOB"));
            Add("avg(X)", QObject::tr("평균(집계)"));
            Add("count(X)", QObject::tr("NULL 이 아닌 X 의 수(count(*) 는 행 수)"));
            Add("group_concat(X, Y)", QObject::tr("Y(기본 ,)로 이어 붙인 문자열(집계)"));
            Add("string_agg(X, Y)", QObject::tr("group_concat 과 같다(집계)"));
            Add("sum(X)", QObject::tr("합(집계, 모두 NULL 이면 NULL)"));
            Add("total(X)", QObject::tr("합(집계, 언제나 실수)"));
            Add("median(X)", QObject::tr("중앙값(집계)"));
            Add("date(TIME, MODIFIER, ...)", QObject::tr("YYYY-MM-DD"));
            Add("time(TIME, MODIFIER, ...)", QObject::tr("HH:MM:SS"));
            Add("datetime(TIME, MODIFIER, ...)", QObject::tr("YYYY-MM-DD HH:MM:SS"));
            Add("julianday(TIME, MODIFIER, ...)", QObject::tr("율리우스일 수"));
            Add("unixepoch(TIME, MODIFIER, ...)", QObject::tr("유닉스 시각(초)"));
            Add("strftime(FORMAT, TIME, MODIFIER, ...)", QObject::tr("형식 문자열로"));
            Add("timediff(TIME1, TIME2)", QObject::tr("두 시각의 차이"));
            Add("acos(X)", QObject::tr("아크코사인"));
            Add("asin(X)", QObject::tr("아크사인"));
            Add("atan(X)", QObject::tr("아크탄젠트"));
            Add("atan2(Y, X)", QObject::tr("Y/X 의 아크탄젠트"));
            Add("ceil(X)", QObject::tr("올림"));
            Add("cos(X)", QObject::tr("코사인"));
            Add("exp(X)", QObject::tr("e 의 X 제곱"));
            Add("floor(X)", QObject::tr("내림"));
            Add("ln(X)", QObject::tr("자연로그"));
            Add("log(B, X)", QObject::tr("밑 B 로그(인자 하나면 상용로그)"));
            Add("mod(X, Y)", QObject::tr("나머지"));
            Add("pi()", QObject::tr("원주율"));
            Add("pow(X, Y)", QObject::tr("X 의 Y 제곱"));
            Add("sin(X)", QObject::tr("사인"));
            Add("sqrt(X)", QObject::tr("제곱근"));
            Add("tan(X)", QObject::tr("탄젠트"));
            Add("trunc(X)", QObject::tr("0 쪽으로 자름"));
            Add("json(X)", QObject::tr("JSON 을 검사하고 줄인 텍스트"));
            Add("json_array(X, ...)", QObject::tr("JSON 배열"));
            Add("json_array_length(X, PATH)", QObject::tr("배열 길이"));
            Add("json_extract(X, PATH, ...)", QObject::tr("PATH 의 값"));
            Add("json_insert(X, PATH, VALUE, ...)", QObject::tr("없는 자리에만 넣는다"));
            Add("json_object(LABEL, VALUE, ...)", QObject::tr("JSON 객체"));
            Add("json_patch(X, PATCH)", QObject::tr("RFC 7396 병합"));
            Add("json_remove(X, PATH, ...)", QObject::tr("PATH 를 지운다"));
            Add("json_replace(X, PATH, VALUE, ...)", QObject::tr("있는 자리만 바꾼다"));
            Add("json_set(X, PATH, VALUE, ...)", QObject::tr("넣거나 바꾼다"));
            Add("json_type(X, PATH)", QObject::tr("JSON 형식 이름"));
            Add("json_valid(X)", QObject::tr("올바른 JSON 이면 1"));
            Add("json_quote(X)", QObject::tr("SQL 값을 JSON 값으로"));
            Add("json_group_array(X)", QObject::tr("JSON 배열(집계)"));
            Add("json_group_object(NAME, VALUE)", QObject::tr("JSON 객체(집계)"));
            Add("row_number()", QObject::tr("창 안의 행 번호(창 함수)"));
            Add("rank()", QObject::tr("순위, 같은 값은 같은 순위 뒤 건너뜀(창 함수)"));
            Add("dense_rank()", QObject::tr("순위, 건너뛰지 않음(창 함수)"));
            Add("lag(EXPR, OFFSET, DEFAULT)", QObject::tr("앞 행의 값(창 함수)"));
            Add("lead(EXPR, OFFSET, DEFAULT)", QObject::tr("뒤 행의 값(창 함수)"));
            Add("first_value(EXPR)", QObject::tr("창의 첫 값(창 함수)"));
            Add("last_value(EXPR)", QObject::tr("창의 마지막 값(창 함수)"));
            Add("nth_value(EXPR, N)", QObject::tr("창의 N 번째 값(창 함수)"));
            Add("ntile(N)", QObject::tr("N 묶음 번호(창 함수)"));
            return Map;
        }();
        return Table.value(Name.toLower());
    }

    MissingName ParseMissingName(const QString& Error)
    {
        static const QRegularExpression Pattern(QStringLiteral("no such (column|table): (\\S+)"), QRegularExpression::CaseInsensitiveOption);
        MissingName Result;
        const auto Match = Pattern.match(Error);
        if (!Match.hasMatch()) return Result;
        Result.kind_ = Match.captured(1).compare(QLatin1String("column"), Qt::CaseInsensitive) == 0 ? MissingName::Kind::Column : MissingName::Kind::Table;
        Result.name_ = Match.captured(2);
        if (const int Dot = int(Result.name_.lastIndexOf(QLatin1Char('.'))); Dot >= 0) Result.name_ = Result.name_.mid(Dot + 1);
        return Result;
    }

    int EditDistance(const QString& Left, const QString& Right)
    {
        const auto A = Left.toCaseFolded(), B = Right.toCaseFolded();
        QVector<int> Previous(B.size() + 1), Current(B.size() + 1);
        for (int Column = 0; Column <= B.size(); ++Column) Previous[Column] = Column;
        for (int Row = 1; Row <= A.size(); ++Row) {
            Current[0] = Row;
            for (int Column = 1; Column <= B.size(); ++Column)
                Current[Column] = qMin(qMin(Previous[Column] + 1, Current[Column - 1] + 1), Previous[Column - 1] + (A[Row - 1] == B[Column - 1] ? 0 : 1));
            std::swap(Previous, Current);
        }
        return Previous[B.size()];
    }

    QStringList Suggestions(const QString& Wrong, const QStringList& Names, int Count)
    {
        QList<std::pair<int, QString>> Ranked;
        QStringList Seen;
        for (const auto& Name : Names) {
            if (Seen.contains(Name, Qt::CaseInsensitive)) continue;
            Seen.append(Name);
            // 두 이름 가운데 긴 쪽의 절반(적어도 2)까지(trax → tracks 는 3). 이름 전체가 다른 것은 빼다.
            const int Limit = qMax(2, int(qMax(Wrong.size(), Name.size())) / 2);
            const int Distance = EditDistance(Wrong, Name);
            if (Distance > 0 && Distance <= Limit && Distance < Name.size()) Ranked.append({Distance, Name});
        }
        std::stable_sort(Ranked.begin(), Ranked.end(), [](const auto& Left, const auto& Right) { return Left.first < Right.first; });
        QStringList Result;
        for (const auto& [Distance, Name] : Ranked) { if (Result.size() >= Count) break; Result.append(Name); }
        return Result;
    }

    QStringList NamesFor(const MissingName& Missing, const QString& Statement, const CompletionSource& Source)
    {
        QStringList Names;
        const auto Scope = ParseScope(Lex(Statement, int(Statement.size()), nullptr), Source.schema_);
        if (Missing.kind_ == MissingName::Kind::Table) {
            for (const auto& Entry : Scope) if (Entry.definition_) Names.append(Entry.name_);
            for (const auto& Db : Source.schema_.databases_)
                for (const auto Type : {ObjectType::Table, ObjectType::View})
                    for (const auto* Object : Db.Objects(Type)) Names.append(Object->name_);
            return Names;
        }
        for (const auto& Entry : Scope)
            if (Entry.object_) for (const auto& Column : Entry.object_->columns_) if (!Column.hidden_) Names.append(Column.name_);
        if (Names.isEmpty())
            for (const auto& Db : Source.schema_.databases_)
                for (const auto& Object : Db.objects_) for (const auto& Column : Object.columns_) if (!Column.hidden_) Names.append(Column.name_);
        return Names;
    }

    namespace
    {
        struct Piece
        {
            enum Type { Word, Quoted, String, Number, Operator, Open, Close, Comma, Dot, Semicolon, LineComment, BlockComment };
            Type type_;
            QString text_;
        };

        QVector<Piece> FormatLex(const QString& Text)
        {
            QVector<Piece> Pieces;
            const int Size = int(Text.size());
            int At = 0;
            while (At < Size) {
                const QChar Ch = Text[At];
                if (Ch.isSpace()) { ++At; continue; }
                if (Ch == QLatin1Char('-') && At + 1 < Size && Text[At + 1] == QLatin1Char('-')) {
                    int End = int(Text.indexOf(QLatin1Char('\n'), At));
                    if (End < 0) End = Size;
                    Pieces.append({Piece::LineComment, Text.mid(At, End - At).trimmed()});
                    At = End;
                    continue;
                }
                if (Ch == QLatin1Char('/') && At + 1 < Size && Text[At + 1] == QLatin1Char('*')) {
                    int End = int(Text.indexOf(QStringLiteral("*/"), At + 2));
                    End = End < 0 ? Size : End + 2;
                    Pieces.append({Piece::BlockComment, Text.mid(At, End - At)});
                    At = End;
                    continue;
                }
                const QChar Closer = Ch == QLatin1Char('\'') ? QLatin1Char('\'') : Ch == QLatin1Char('"') ? QLatin1Char('"')
                                   : Ch == QLatin1Char('`') ? QLatin1Char('`') : Ch == QLatin1Char('[') ? QLatin1Char(']') : QChar();
                if (!Closer.isNull()) {
                    int End = At + 1;
                    while (End < Size) {
                        if (Text[End] == Closer) {
                            if (Closer != QLatin1Char(']') && End + 1 < Size && Text[End + 1] == Closer) { End += 2; continue; }
                            ++End;
                            break;
                        }
                        ++End;
                    }
                    Pieces.append({Ch == QLatin1Char('\'') ? Piece::String : Piece::Quoted, Text.mid(At, End - At)});
                    At = End;
                    continue;
                }
                if (WordChar(Ch)) {
                    int End = At;
                    while (End < Size && (WordChar(Text[End]) || (Ch.isDigit() && Text[End] == QLatin1Char('.')))) ++End;
                    Pieces.append({Ch.isDigit() ? Piece::Number : Piece::Word, Text.mid(At, End - At)});
                    At = End;
                    continue;
                }
                if (Ch == QLatin1Char('(')) { Pieces.append({Piece::Open, QStringLiteral("(")}); ++At; continue; }
                if (Ch == QLatin1Char(')')) { Pieces.append({Piece::Close, QStringLiteral(")")}); ++At; continue; }
                if (Ch == QLatin1Char(',')) { Pieces.append({Piece::Comma, QStringLiteral(",")}); ++At; continue; }
                if (Ch == QLatin1Char('.')) { Pieces.append({Piece::Dot, QStringLiteral(".")}); ++At; continue; }
                if (Ch == QLatin1Char(';')) { Pieces.append({Piece::Semicolon, QStringLiteral(";")}); ++At; continue; }
                // 연산자: 여러 글자(<= <> != || >= == << >> ->>)를 한 덩어리로.
                int End = At;
                static const QString OperatorChars = QStringLiteral("=<>!|+-*/%&~^:?@$#");
                while (End < Size && OperatorChars.contains(Text[End]) && !(Text[End] == QLatin1Char('-') && End + 1 < Size && Text[End + 1] == QLatin1Char('-'))) ++End;
                if (End == At) End = At + 1;
                Pieces.append({Piece::Operator, Text.mid(At, End - At)});
                At = End;
            }
            return Pieces;
        }
    }

    QString FormatSql(const QString& Text, bool UpperKeywords)
    {
        static const QSet<QString> KeywordSet = [] { QSet<QString> Set; for (const auto& Word : Keywords()) Set.insert(Word.toUpper()); return Set; }();
        static const QSet<QString> Clauses{QStringLiteral("SELECT"), QStringLiteral("FROM"), QStringLiteral("WHERE"), QStringLiteral("GROUP"),
                                           QStringLiteral("ORDER"), QStringLiteral("HAVING"), QStringLiteral("LIMIT"), QStringLiteral("WINDOW"),
                                           QStringLiteral("VALUES"), QStringLiteral("SET"), QStringLiteral("RETURNING"), QStringLiteral("UNION"),
                                           QStringLiteral("EXCEPT"), QStringLiteral("INTERSECT"), QStringLiteral("JOIN"), QStringLiteral("LEFT"),
                                           QStringLiteral("RIGHT"), QStringLiteral("FULL"), QStringLiteral("INNER"), QStringLiteral("CROSS"),
                                           QStringLiteral("NATURAL"), QStringLiteral("ON")};
        const auto Pieces = FormatLex(Text);
        QString Out;
        QVector<bool> SubQuery{false};          // 괄호마다: 부분 질의인가
        QVector<QString> Clause{QString()};     // 괄호마다: 지금 절
        QVector<bool> BreakList{false};         // 괄호마다: SELECT 목록을 줄마다
        int Level = 0;                          // 들여쓰기 단계(부분 질의 깊이)
        bool StatementStart = true, NeedSpace = false;
        const auto Up = [](const Piece& Item) { return Item.type_ == Piece::Word ? Item.text_.toUpper() : QString(); };
        const auto Indent = [](int Steps) { return QString(Steps * 4, QLatin1Char(' ')); };
        const auto NewLine = [&](int Steps) {
            while (Out.endsWith(QLatin1Char(' '))) Out.chop(1);
            if (!Out.isEmpty() && !Out.endsWith(QLatin1Char('\n'))) Out += QLatin1Char('\n');
            Out += Indent(Steps);
            NeedSpace = false;
        };
        const auto Emit = [&](const QString& Value, bool SpaceBefore) {
            if (SpaceBefore && NeedSpace && !Out.endsWith(QLatin1Char(' ')) && !Out.endsWith(QLatin1Char('\n')) && !Out.isEmpty()) Out += QLatin1Char(' ');
            Out += Value;
            NeedSpace = true;
        };
        for (int Index = 0; Index < Pieces.size(); ++Index) {
            const auto& Item = Pieces[Index];
            const auto Word = Up(Item);
            const auto* Previous = Index > 0 ? &Pieces[Index - 1] : nullptr;
            switch (Item.type_) {
            case Piece::LineComment:
                Emit(Item.text_, true);
                NewLine(Level + (Clause.last().isEmpty() ? 0 : 1));
                continue;
            case Piece::BlockComment:
                Emit(Item.text_, true);
                continue;
            case Piece::Semicolon:
                while (Out.endsWith(QLatin1Char(' '))) Out.chop(1);
                Out += QStringLiteral(";\n\n");
                NeedSpace = false;
                StatementStart = true;
                SubQuery = {false};
                Clause = {QString()};
                BreakList = {false};
                Level = 0;
                continue;
            case Piece::Open: {
                const bool Sub = Index + 1 < Pieces.size() && (Up(Pieces[Index + 1]) == QLatin1String("SELECT") || Up(Pieces[Index + 1]) == QLatin1String("WITH"));
                const bool Call = Previous && (Previous->type_ == Piece::Word || Previous->type_ == Piece::Quoted) && !KeywordSet.contains(Up(*Previous));
                Emit(QStringLiteral("("), !Call);
                NeedSpace = false;
                SubQuery.append(Sub);
                Clause.append(QString());
                BreakList.append(false);
                if (Sub) ++Level;
                continue;
            }
            case Piece::Close: {
                const bool Sub = SubQuery.size() > 1 && SubQuery.last();
                if (SubQuery.size() > 1) { SubQuery.removeLast(); Clause.removeLast(); BreakList.removeLast(); }
                if (Sub) { --Level; NewLine(Level); }
                while (Out.endsWith(QLatin1Char(' '))) Out.chop(1);
                Out += QLatin1Char(')');
                NeedSpace = true;
                continue;
            }
            case Piece::Comma:
                while (Out.endsWith(QLatin1Char(' '))) Out.chop(1);
                Out += QLatin1Char(',');
                NeedSpace = true;
                if (BreakList.last() && Clause.last() == QLatin1String("SELECT")) NewLine(Level + 1);
                continue;
            case Piece::Dot:
                while (Out.endsWith(QLatin1Char(' '))) Out.chop(1);
                Out += QLatin1Char('.');
                NeedSpace = false;
                continue;
            case Piece::Operator: {
                // 앞이 없거나 연산자 · 여는 괄호 · 쉼표 · 키워드면 단항(-1, +x)이라 붙여 쓴다. * 는 SELECT * · count(*) · t.* 그대로.
                const bool Unary = !Previous || Previous->type_ == Piece::Operator || Previous->type_ == Piece::Open || Previous->type_ == Piece::Comma
                                   || (Previous->type_ == Piece::Word && KeywordSet.contains(Up(*Previous)));
                const bool AfterDot = Previous && Previous->type_ == Piece::Dot;
                if (AfterDot || (Previous && Previous->type_ == Piece::Open)) { Out += Item.text_; NeedSpace = true; continue; }
                Emit(Item.text_, true);
                if (Unary && Item.text_ != QLatin1String("*")) NeedSpace = false;
                continue;
            }
            case Piece::Word: {
                const bool Keyword = KeywordSet.contains(Word) && !(Previous && Previous->type_ == Piece::Dot);
                const auto Text = Keyword ? (UpperKeywords ? Word : Item.text_.toLower()) : Item.text_;
                const bool Major = Keyword && Clauses.contains(Word);
                // JOIN 앞 수식어(LEFT · INNER …)가 이미 줄을 바꿨으면 JOIN 은 그 줄에 붙인다.
                const bool Modifier = Previous && (Up(*Previous) == QLatin1String("LEFT") || Up(*Previous) == QLatin1String("RIGHT") || Up(*Previous) == QLatin1String("FULL")
                                                   || Up(*Previous) == QLatin1String("INNER") || Up(*Previous) == QLatin1String("CROSS") || Up(*Previous) == QLatin1String("NATURAL")
                                                   || Up(*Previous) == QLatin1String("OUTER"));
                if (Major && !StatementStart && Word != QLatin1String("ON") && !(Word == QLatin1String("JOIN") && Modifier) && !(Word == QLatin1String("OUTER"))) {
                    // LEFT · RIGHT 가 함수(left(x, 2))면 줄을 바꾸지 않는다.
                    const bool Function = (Word == QLatin1String("LEFT") || Word == QLatin1String("RIGHT")) && Index + 1 < Pieces.size()
                                          && Pieces[Index + 1].type_ == Piece::Open;
                    if (!Function) NewLine(Level);
                }
                if (Word == QLatin1String("ON") && Clause.last() == QLatin1String("JOIN")) { Emit(Text, true); Clause.last() = QStringLiteral("ON"); continue; }
                if ((Word == QLatin1String("AND") || Word == QLatin1String("OR")) && Keyword
                    && (Clause.last() == QLatin1String("WHERE") || Clause.last() == QLatin1String("HAVING") || Clause.last() == QLatin1String("ON"))) {
                    // BETWEEN … AND 의 AND 는 그대로 둔다.
                    bool Between = false;
                    for (int Back = Index - 1; Back >= 0 && Back >= Index - 4; --Back) if (Up(Pieces[Back]) == QLatin1String("BETWEEN")) Between = true;
                    if (!Between) NewLine(Level + 1);
                }
                Emit(Text, true);
                StatementStart = false;
                if (Major) {
                    Clause.last() = Word == QLatin1String("LEFT") || Word == QLatin1String("RIGHT") || Word == QLatin1String("FULL") || Word == QLatin1String("INNER")
                                       || Word == QLatin1String("CROSS") || Word == QLatin1String("NATURAL") ? QStringLiteral("JOIN") : Word;
                    if (Word == QLatin1String("SELECT")) {
                        // 목록이 길면(쉼표 4개 넘게, 또는 80자 넘게) 항목마다 줄을 바꾼다.
                        int Depth = 0, Commas = 0, Length = 0;
                        for (int Ahead = Index + 1; Ahead < Pieces.size(); ++Ahead) {
                            const auto& Next = Pieces[Ahead];
                            if (Next.type_ == Piece::Open) ++Depth;
                            else if (Next.type_ == Piece::Close) { if (--Depth < 0) break; }
                            else if (Depth == 0 && (Next.type_ == Piece::Semicolon || (Next.type_ == Piece::Word && (Up(Next) == QLatin1String("FROM")
                                                                                                                           || Up(Next) == QLatin1String("UNION"))))) break;
                            else if (Depth == 0 && Next.type_ == Piece::Comma) ++Commas;
                            Length += int(Next.text_.size()) + 1;
                        }
                        BreakList.last() = Commas > 3 || Length > 80;
                        if (BreakList.last()) NewLine(Level + 1);
                    }
                    if (Word == QLatin1String("UNION") || Word == QLatin1String("EXCEPT") || Word == QLatin1String("INTERSECT")) {
                        if (Index + 1 < Pieces.size() && Up(Pieces[Index + 1]) == QLatin1String("ALL")) { Emit(UpperKeywords ? QStringLiteral("ALL") : QStringLiteral("all"), true); ++Index; }
                        NewLine(Level);
                        StatementStart = true;
                    }
                }
                continue;
            }
            default:
                Emit(Item.text_, true);
                StatementStart = false;
                continue;
            }
        }
        while (Out.endsWith(QLatin1Char(' ')) || Out.endsWith(QLatin1Char('\n'))) Out.chop(1);
        return Out;
    }

    std::pair<int, int> SignatureArgument(const QString& Signature, int Argument)
    {
        const int Newline = int(Signature.indexOf(QLatin1Char('\n')));
        const auto Line = Newline < 0 ? Signature : Signature.left(Newline);
        const int Open = int(Line.indexOf(QLatin1Char('('))), Close = int(Line.lastIndexOf(QLatin1Char(')')));
        if (Open < 0 || Close <= Open + 1) return {-1, -1};
        QList<std::pair<int, int>> Parts;
        int Start = Open + 1;
        for (int Index = Open + 1; Index <= Close; ++Index) {
            if (Index == Close || Line[Index] == QLatin1Char(',')) {
                int From = Start, To = Index;
                while (From < To && Line[From].isSpace()) ++From;
                Parts.append({From, To});
                Start = Index + 1;
            }
        }
        if (Parts.isEmpty()) return {-1, -1};
        // ‘...’ 가 있으면 마지막 실제 인자가 뒤를 받는다.
        if (Argument >= Parts.size()) {
            const auto& Last = Parts.last();
            if (Line.mid(Last.first, Last.second - Last.first).trimmed() == QLatin1String("...") && Parts.size() >= 2) return Parts[Parts.size() - 2];
            return {-1, -1};
        }
        if (Line.mid(Parts[Argument].first, Parts[Argument].second - Parts[Argument].first).trimmed() == QLatin1String("...") && Argument > 0)
            return Parts[Argument - 1];
        return Parts[Argument];
    }
}
