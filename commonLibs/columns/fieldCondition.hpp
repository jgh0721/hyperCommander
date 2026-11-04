#pragma once
#include "columnContracts.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimeZone>
#include <cmath>
#include <limits>
#include <optional>

// A rule condition over one column field: {"type": "field", "field", "valueType", "op", "value", ...}.
// Pure and synchronous. Values arrive as CellValue copies; nothing here performs I/O.
namespace hc::columns
{
    // Three-valued: a value still being read decides nothing yet.
    enum class Truth : qint8 { False = 0, True = 1, Unknown = 2 };
    // Ordered by price. A group evaluates cheaper conditions first; the result does not depend on order.
    enum class ConditionCost : qint8 { Immediate, Slow, Plugin };

    inline ConditionCost CostOf(const FieldDefinition& Field)
    {
        if (Field.provider_ != ProviderKind::BuiltIn) return ConditionCost::Plugin;
        return Field.cost_ == EvaluationCost::Snapshot ? ConditionCost::Immediate : ConditionCost::Slow;
    }
    // Lua outputs live inside one column profile and VFS columns inside one listing. A global rule
    // cannot rely on either (design 13.1).
    inline bool IsConditionField(const QString& Id)
    {
        return IsFieldId(Id) && !Id.startsWith(QStringLiteral("lua.")) && !Id.startsWith(QStringLiteral("dopusvfs."));
    }
    inline const QStringList& FieldTypeNames()
    {
        static const QStringList Names{"text", "integer", "unsigned", "real", "boolean", "date", "time", "timestamp"};
        return Names;
    }
    inline std::optional<FieldType> ParseFieldType(const QString& Name)
    {
        const auto Index = FieldTypeNames().indexOf(Name);
        return Index < 0 ? std::nullopt : std::optional(static_cast<FieldType>(Index));
    }
    // Operators a field type offers, in editor order. Some fields narrow the list further (design 13.2).
    inline QStringList FieldOperators(FieldType Type, const QString& FieldId = {})
    {
        // ADS names are joined with ", " for display; a name may contain ", " itself, so only
        // substring tests keep their meaning on the joined text.
        if (FieldId == QStringLiteral("file.alternateDataStream.names")) return {"contains", "notContains"};
        switch (Type) {
        case FieldType::Integer: case FieldType::UnsignedInteger: case FieldType::Real:
            return {"ge", "gt", "le", "lt", "eq", "ne", "between", "in", "notIn", "exists", "notExists"};
        case FieldType::Boolean: return {"is"};
        case FieldType::Date: case FieldType::Timestamp: return {"within", "before", "after", "exists", "notExists"};
        case FieldType::Time: return {"before", "after", "exists", "notExists"};
        case FieldType::Text: break;
        }
        return {"contains", "notContains", "matches", "notMatches", "eq", "in", "notIn", "exists", "notExists"};
    }

    // An exact number from a cell or a condition operand. Mixed signed/unsigned comparisons stay exact;
    // only a real operand falls back to double.
    struct FieldNumber
    {
        enum Kind : quint8 { Signed, Unsigned, Real };
        Kind kind_ = Unsigned;
        qint64 signed_ = 0;
        quint64 unsigned_ = 0;
        double real_ = 0;
        double AsReal() const { return kind_ == Real ? real_ : kind_ == Signed ? double(signed_) : double(unsigned_); }
        static std::optional<FieldNumber> From(const CellValue& Value)
        {
            if (Value.State() != ValueState::Ready || !Value.Value()) return {};
            FieldNumber Result;
            if (const auto* S = std::get_if<qint64>(&*Value.Value())) { Result.kind_ = Signed; Result.signed_ = *S; return Result; }
            if (const auto* U = std::get_if<quint64>(&*Value.Value())) { Result.kind_ = Unsigned; Result.unsigned_ = *U; return Result; }
            if (const auto* R = std::get_if<double>(&*Value.Value())) { Result.kind_ = Real; Result.real_ = *R; return Result; }
            return {};
        }
        static std::optional<FieldNumber> Parse(const QString& Text, FieldType Type, quint64 Factor = 1)
        {
            bool Ok = false; FieldNumber Result;
            const auto Trimmed = Text.trimmed();
            if (Type == FieldType::UnsignedInteger) {
                const auto Value = Trimmed.toULongLong(&Ok);
                if (!Ok || (Factor > 1 && Value > std::numeric_limits<quint64>::max() / Factor)) return {};
                Result.kind_ = Unsigned; Result.unsigned_ = Value * Factor; return Result;
            }
            if (Type == FieldType::Integer) {
                const auto Value = Trimmed.toLongLong(&Ok);
                const auto Limit = std::numeric_limits<qint64>::max() / qint64(Factor);
                if (!Ok || Value > Limit || Value < -Limit) return {};
                Result.kind_ = Signed; Result.signed_ = Value * qint64(Factor); return Result;
            }
            const auto Value = Trimmed.toDouble(&Ok);
            if (!Ok || !std::isfinite(Value)) return {};
            Result.kind_ = Real; Result.real_ = Value * double(Factor); return Result;
        }
        QString Text() const { return kind_ == Real ? QString::number(real_, 'g', 17) : kind_ == Signed ? QString::number(signed_) : QString::number(unsigned_); }
    };
    inline int Compare(const FieldNumber& A, const FieldNumber& B)
    {
        const auto Order = [](auto L, auto R) { return L < R ? -1 : R < L ? 1 : 0; };
        if (A.kind_ == FieldNumber::Real || B.kind_ == FieldNumber::Real) return Order(A.AsReal(), B.AsReal());
        if (A.kind_ == B.kind_) return A.kind_ == FieldNumber::Signed ? Order(A.signed_, B.signed_) : Order(A.unsigned_, B.unsigned_);
        if (A.kind_ == FieldNumber::Signed) return A.signed_ < 0 ? -1 : Order(quint64(A.signed_), B.unsigned_);
        return B.signed_ < 0 ? 1 : Order(A.unsigned_, quint64(B.signed_));
    }

    struct FieldCondition
    {
        enum class Op : quint8 { Eq, Ne, Gt, Ge, Lt, Le, Between, Contains, NotContains, Matches, NotMatches,
            Is, Before, After, Within, In, NotIn, Exists, NotExists };
        QString field_;
        FieldType type_ = FieldType::Text;
        Op op_ = Op::Exists;
        FieldNumber low_, high_;
        QString text_;
        QStringList choices_;
        QVector<FieldNumber> numbers_;
        QList<QRegularExpression> patterns_;
        bool flag_ = false;
        qint64 instant_ = 0;
        quint64 span_ = 0;
        QTime time_;
        bool Negated() const { return op_ == Op::Ne || op_ == Op::NotContains || op_ == Op::NotMatches || op_ == Op::NotIn; }
    };
    inline const QStringList& FieldOperatorNames()
    {
        static const QStringList Names{"eq", "ne", "gt", "ge", "lt", "le", "between", "contains", "notContains", "matches", "notMatches",
            "is", "before", "after", "within", "in", "notIn", "exists", "notExists"};
        return Names;
    }
    // Same engine limits as file-name regex conditions: bounded work, case-insensitive by default.
    inline QRegularExpression FieldRegex(const QString& Pattern)
    {
        return QRegularExpression(QStringLiteral("(*LIMIT_MATCH=100000)(*LIMIT_DEPTH=1000)") + Pattern, QRegularExpression::CaseInsensitiveOption);
    }
    inline std::optional<qint64> FieldInstant(const QString& Text)
    {
        const auto Date = QDateTime::fromString(Text, Qt::ISODateWithMs);
        if (!Date.isValid() || (Date.timeSpec() != Qt::UTC && Date.timeSpec() != Qt::OffsetFromUTC)) return {};
        return Date.toMSecsSinceEpoch();
    }

    // Validates and compiles one condition. Returns an error text; empty on success.
    inline QString CompileFieldCondition(const QJsonObject& Json, FieldCondition& Out)
    {
        Out = {};
        Out.field_ = Json["field"].toString();
        if (Out.field_.startsWith(QStringLiteral("lua."))) return QObject::tr("Lua 계산 필드는 조건에 사용할 수 없습니다.");
        if (!IsConditionField(Out.field_)) return QObject::tr("조건에 사용할 수 없는 필드입니다.");
        const auto Type = ParseFieldType(Json["valueType"].toString());
        if (!Type) return QObject::tr("필드 자료형을 확인하세요.");
        Out.type_ = *Type;
        const auto OpName = Json["op"].toString();
        if (!FieldOperators(Out.type_, Out.field_).contains(OpName)) return QObject::tr("이 필드에서 쓸 수 없는 비교 방법입니다.");
        Out.op_ = static_cast<FieldCondition::Op>(FieldOperatorNames().indexOf(OpName));
        using Op = FieldCondition::Op;
        const auto Value = Json["value"];
        const bool Numeric = Out.type_ == FieldType::Integer || Out.type_ == FieldType::UnsignedInteger || Out.type_ == FieldType::Real;
        const auto Unit = Json["unit"].toString();
        if (Out.op_ == Op::Exists || Out.op_ == Op::NotExists) return {};
        if (Out.op_ == Op::In || Out.op_ == Op::NotIn) {
            const auto Items = Value.toArray();
            if (!Value.isArray() || Items.isEmpty() || Items.size() > 64) return QObject::tr("값을 1~64개 고르세요.");
            for (const auto Item : Items) {
                const auto Text = Item.toString().trimmed();
                if (!Item.isString() || Text.isEmpty() || Text.size() > 512) return QObject::tr("목록 값은 1~512자입니다.");
                if (Numeric) {
                    const auto Number = FieldNumber::Parse(Text, Out.type_);
                    if (!Number) return QObject::tr("숫자 목록을 확인하세요.");
                    Out.numbers_.append(*Number);
                } else Out.choices_.append(Text);
            }
            return {};
        }
        if (Numeric) {
            quint64 Factor = 1;
            if (!Unit.isEmpty()) {
                const QStringList Units{"B", "KiB", "MiB", "GiB"};
                const auto Index = Units.indexOf(Unit); if (Index < 0) return QObject::tr("단위를 확인하세요.");
                Factor = quint64(1) << (10 * Index);
            }
            if (Out.op_ == Op::Between) {
                const auto Pair = Value.toArray();
                if (!Value.isArray() || Pair.size() != 2) return QObject::tr("범위는 두 값을 입력하세요.");
                const auto Low = FieldNumber::Parse(Pair[0].toString(), Out.type_, Factor), High = FieldNumber::Parse(Pair[1].toString(), Out.type_, Factor);
                if (!Low || !High) return QObject::tr("범위 값을 확인하세요.");
                if (Compare(*Low, *High) > 0) return QObject::tr("범위의 시작이 끝보다 큽니다.");
                Out.low_ = *Low; Out.high_ = *High; return {};
            }
            const auto Number = FieldNumber::Parse(Value.toString(), Out.type_, Factor);
            if (!Value.isString() || !Number) return QObject::tr("숫자 값을 확인하세요.");
            Out.low_ = *Number; return {};
        }
        if (Out.type_ == FieldType::Boolean) {
            if (Value != QJsonValue("true") && Value != QJsonValue("false")) return QObject::tr("예 또는 아니요를 고르세요.");
            Out.flag_ = Value == QJsonValue("true"); return {};
        }
        if (Out.type_ == FieldType::Date || Out.type_ == FieldType::Timestamp || Out.type_ == FieldType::Time) {
            if (Out.op_ == Op::Within) {
                bool Ok = false; const auto Count = Value.toString().toULongLong(&Ok);
                const QStringList Units{"minutes", "hours", "days"}; const quint64 Factors[]{60000, 3600000, 86400000};
                const auto Index = Units.indexOf(Unit);
                if (!Ok || Index < 0 || Count > std::numeric_limits<quint64>::max() / Factors[Index]) return QObject::tr("기간과 단위를 확인하세요.");
                Out.span_ = Count * Factors[Index]; return {};
            }
            if (Out.type_ == FieldType::Time) {
                Out.time_ = QTime::fromString(Value.toString(), Qt::ISODate);
                return Out.time_.isValid() ? QString{} : QObject::tr("시각은 HH:mm:ss 형식입니다.");
            }
            const auto Instant = FieldInstant(Value.toString());
            if (!Instant) return QObject::tr("날짜는 시간대가 포함된 ISO 날짜입니다.");
            Out.instant_ = *Instant; return {};
        }
        // Text.
        if (Out.op_ == Op::Matches || Out.op_ == Op::NotMatches) {
            const auto Syntax = Json["syntax"].toString(QStringLiteral("wildcard"));
            if (Syntax == QStringLiteral("regex")) {
                const auto Pattern = Value.toString();
                if (!Value.isString() || Pattern.isEmpty() || Pattern.size() > 512 || Pattern.contains(QChar::Null)) return QObject::tr("정규표현식은 1~512자로 입력하세요.");
                auto Regex = FieldRegex(Pattern);
                if (!Regex.isValid()) return QObject::tr("정규표현식 오류: %1").arg(Regex.errorString());
                Regex.optimize(); Out.patterns_.append(Regex); return {};
            }
            if (Syntax != QStringLiteral("wildcard")) return QObject::tr("비교 방식을 확인하세요.");
            const auto Patterns = Value.toArray();
            if (!Value.isArray() || Patterns.isEmpty() || Patterns.size() > 32) return QObject::tr("와일드카드 패턴은 1~32개입니다.");
            for (const auto Item : Patterns) {
                const auto Pattern = Item.toString().trimmed();
                if (!Item.isString() || Pattern.isEmpty() || Pattern.size() > 512 || Pattern.contains(QChar::Null)) return QObject::tr("와일드카드 패턴은 1~512자입니다.");
                // Paths are matched whole: * crosses separators, the pattern is anchored at both ends.
                QRegularExpression Regex(QRegularExpression::wildcardToRegularExpression(Pattern, QRegularExpression::NonPathWildcardConversion),
                    QRegularExpression::CaseInsensitiveOption);
                if (!Regex.isValid()) return QObject::tr("와일드카드 패턴을 확인하세요.");
                Out.patterns_.append(Regex);
            }
            return {};
        }
        Out.text_ = Value.toString();
        if (!Value.isString() || Out.text_.isEmpty() || Out.text_.size() > 512 || Out.text_.contains(QChar::Null)) return QObject::tr("문자열은 1~512자로 입력하세요.");
        return {};
    }

    // Design 4.3: a missing value never matches, negated or not; a value still being read is Unknown.
    inline Truth EvaluateFieldCondition(const FieldCondition& C, const CellValue& Value, qint64 ReferenceUtcMs)
    {
        using Op = FieldCondition::Op;
        const auto Of = [](bool Hit) { return Hit ? Truth::True : Truth::False; };
        switch (Value.State()) {
        case ValueState::Pending: case ValueState::OnDemand: return Truth::Unknown;
        case ValueState::Empty: return Of(C.op_ == Op::NotExists);
        case ValueState::Unsupported: case ValueState::Error: return Truth::False;
        case ValueState::Ready: break;
        }
        if (C.op_ == Op::Exists) return Truth::True;
        if (C.op_ == Op::NotExists) return Truth::False;
        if (C.op_ == Op::In || C.op_ == Op::NotIn) {
            bool Hit = false;
            if (const auto Number = FieldNumber::From(Value); Number && !C.numbers_.isEmpty()) {
                for (const auto& Item : C.numbers_) if (!Compare(*Number, Item)) { Hit = true; break; }
            } else {
                const auto Text = Value.Raw().toString();
                for (const auto& Choice : C.choices_) if (!Text.compare(Choice, Qt::CaseInsensitive)) { Hit = true; break; }
            }
            return Of(Hit != (C.op_ == Op::NotIn));
        }
        switch (C.type_) {
        case FieldType::Integer: case FieldType::UnsignedInteger: case FieldType::Real: {
            const auto Number = FieldNumber::From(Value);
            if (!Number) return Truth::False; // A provider typed the field differently; never guess.
            const int Order = Compare(*Number, C.low_);
            switch (C.op_) {
            case Op::Eq: return Of(Order == 0); case Op::Ne: return Of(Order != 0);
            case Op::Gt: return Of(Order > 0); case Op::Ge: return Of(Order >= 0);
            case Op::Lt: return Of(Order < 0); case Op::Le: return Of(Order <= 0);
            case Op::Between: return Of(Order >= 0 && Compare(*Number, C.high_) <= 0);
            default: return Truth::False;
            }
        }
        case FieldType::Boolean: {
            const auto* Flag = std::get_if<bool>(&*Value.Value());
            return Flag ? Of(*Flag == C.flag_) : Truth::False;
        }
        case FieldType::Time: {
            const auto* Time = std::get_if<QTime>(&*Value.Value());
            if (!Time) return Truth::False;
            return Of(C.op_ == Op::Before ? *Time < C.time_ : *Time > C.time_);
        }
        case FieldType::Date: case FieldType::Timestamp: {
            std::optional<qint64> Instant;
            if (const auto* Stamp = std::get_if<QDateTime>(&*Value.Value()); Stamp && Stamp->isValid()) Instant = Stamp->toMSecsSinceEpoch();
            else if (const auto* Day = std::get_if<QDate>(&*Value.Value()); Day && Day->isValid()) Instant = Day->startOfDay(QTimeZone::UTC).toMSecsSinceEpoch();
            if (!Instant) return Truth::False;
            if (C.op_ == Op::Before) return Of(*Instant < C.instant_);
            if (C.op_ == Op::After) return Of(*Instant > C.instant_);
            return Of(*Instant <= ReferenceUtcMs && quint64(ReferenceUtcMs) - quint64(*Instant) <= C.span_);
        }
        case FieldType::Text: break;
        }
        const auto Text = Value.Raw().toString();
        switch (C.op_) {
        case Op::Eq: return Of(!Text.compare(C.text_, Qt::CaseInsensitive));
        case Op::Contains: return Of(Text.contains(C.text_, Qt::CaseInsensitive));
        case Op::NotContains: return Of(!Text.contains(C.text_, Qt::CaseInsensitive));
        case Op::Matches: case Op::NotMatches: {
            bool Hit = false;
            for (const auto& Pattern : C.patterns_) {
                const auto Match = Pattern.match(Text);
                if (!Match.isValid()) return Truth::False; // Never negate an execution error.
                if (Match.hasMatch()) { Hit = true; break; }
            }
            return Of(Hit != (C.op_ == Op::NotMatches));
        }
        default: return Truth::False;
        }
    }

    // Kleene logic for "all" (AND) and "any" (OR) groups.
    inline Truth Combine(Truth Left, Truth Right, bool All)
    {
        if (All) {
            if (Left == Truth::False || Right == Truth::False) return Truth::False;
            return Left == Truth::Unknown || Right == Truth::Unknown ? Truth::Unknown : Truth::True;
        }
        if (Left == Truth::True || Right == Truth::True) return Truth::True;
        return Left == Truth::Unknown || Right == Truth::Unknown ? Truth::Unknown : Truth::False;
    }
}
