#pragma once

#include <QDateTime>
#include <QLocale>
#include <QObject>
#include <QSet>
#include <QUuid>
#include <QVariant>
#include <QVector>
#include <array>
#include <variant>
#include "commonLibs/view/columnWidthPolicy.hpp"

namespace hc::columns
{
    enum class FieldType { Text, Integer, UnsignedInteger, Real, Boolean, Date, Time, Timestamp };
    enum class ValueState { Ready, Empty, Pending, OnDemand, Unsupported, Error };
    enum class ProviderKind { BuiltIn, Wdx, DOpus, Lua };
    enum class EvaluationCost { Snapshot, Background, OnDemand };
    enum class LayoutMode { Details, NameFirst, NameLast };
    enum class SizeFormat { Bytes, Adaptive, KiB, MiB, GiB };
    using Payload = std::variant<QString, qint64, quint64, double, bool, QDate, QTime, QDateTime>;

    // Missing values never masquerade as zero, false, an empty string or an error label.
    // Raw integers remain exact; display formatting is a separate operation.
    class CellValue
    {
    public:
        static CellValue Ready(Payload Value)
        {
            if (const auto Real = std::get_if<double>(&Value); Real && !std::isfinite(*Real))
                return Missing(ValueState::Error, "non-finite number");
            CellValue Result;
            Result.state_ = ValueState::Ready;
            Result.value_ = std::move(Value);
            return Result;
        }
        static CellValue Missing(ValueState State, QString Error = {})
        {
            CellValue Result;
            Result.state_ = State == ValueState::Ready ? ValueState::Error : State;
            Result.error_ = std::move(Error);
            return Result;
        }
        ValueState State() const { return state_; }
        std::optional<FieldType> Type() const { return value_ ? std::optional(static_cast<FieldType>(value_->index())) : std::nullopt; }
        const std::optional<Payload>& Value() const { return value_; }
        const QString& Error() const { return error_; }
        QVariant Raw() const
        {
            return value_ ? std::visit([](const auto& Value) { return QVariant::fromValue(Value); }, *value_) : QVariant{};
        }
    private:
        ValueState state_ = ValueState::Empty;
        std::optional<Payload> value_;
        QString error_;
    };

    struct FieldDefinition
    {
        QString id_; // Provider-qualified, case-sensitive: file.name, wdx.<plugin>.<field>, lua.<script>.<field>.
        QString title_;
        FieldType type_ = FieldType::Text;
        ProviderKind provider_ = ProviderKind::BuiltIn;
        EvaluationCost cost_ = EvaluationCost::Snapshot;
        bool sortable_ = true;
    };
    struct FormatOptions
    {
        SizeFormat size_ = SizeFormat::Bytes;
        int precision_ = 2;
        QString datePattern_ = "yyyy-MM-dd HH:mm:ss";
        bool operator==(const FormatOptions&) const = default;
    };
    struct ColumnDefinition
    {
        QUuid id_ = QUuid::createUuid(); // Instance identity, independent of title, field or visual/model position.
        QString fieldId_;
        QString title_;
        Qt::Alignment alignment_ = Qt::AlignLeft;
        FormatOptions format_;
        std::array<hc::view::ColumnWidthPolicy, 3> widths_;
        bool operator==(const ColumnDefinition&) const = default;
    };
    struct SortDefinition
    {
        QUuid columnId_;
        Qt::SortOrder order_ = Qt::AscendingOrder;
        bool operator==(const SortDefinition&) const = default;
    };
    struct ColumnProfile
    {
        QUuid id_ = QUuid::createUuid();
        QString title_;
        QUuid primaryNameId_;
        LayoutMode mode_ = LayoutMode::Details;
        QVector<ColumnDefinition> columns_;
        QVector<SortDefinition> sorts_;
        bool operator==(const ColumnProfile&) const = default;
    };
    inline QString IdText(const QUuid& Id) { return Id.toString(QUuid::WithoutBraces); }
    inline QString ModeName(LayoutMode Mode)
    {
        switch (Mode) {
        case LayoutMode::Details: return "details";
        case LayoutMode::NameFirst: return "name-first";
        case LayoutMode::NameLast: return "name-last";
        }
        return {};
    }
    inline std::optional<LayoutMode> ParseMode(const QString& Name)
    {
        for (auto Mode : {LayoutMode::Details, LayoutMode::NameFirst, LayoutMode::NameLast})
            if (ModeName(Mode) == Name) return Mode;
        return {};
    }
    inline bool IsFieldId(const QString& Id)
    {
        const auto Parts = Id.split('.');
        if (Parts.size() < 2) return false;
        if (Parts.front() != "file" && Parts.front() != "wdx" && Parts.front() != "dopus" && Parts.front() != "lua") return false;
        if (Parts.front() != "file" && Parts.size() < 3) return false;
        for (const auto& Part : Parts) {
            if (Part.isEmpty()) return false;
            for (const auto Character : Part)
                if (!Character.isLetterOrNumber() && Character != '_' && Character != '-') return false;
        }
        return true;
    }
    inline QString Validate(const ColumnProfile& Profile)
    {
        if (Profile.id_.isNull() || Profile.columns_.isEmpty() || ModeName(Profile.mode_).isEmpty()) return "profile";
        QSet<QUuid> Ids;
        bool PrimaryFound = false;
        for (const auto& Column : Profile.columns_) {
            if (Column.id_.isNull() || Ids.contains(Column.id_)) return "column-id";
            Ids.insert(Column.id_);
            if (!IsFieldId(Column.fieldId_)) return "field-id";
            if (Column.alignment_ != Qt::AlignLeft && Column.alignment_ != Qt::AlignRight && Column.alignment_ != Qt::AlignCenter) return "alignment";
            if (Column.format_.precision_ < 0 || Column.format_.precision_ > 6 || Column.format_.datePattern_.isEmpty() ||
                int(Column.format_.size_) < 0 || int(Column.format_.size_) > int(SizeFormat::GiB)) return "format";
            for (const auto& Width : Column.widths_) if (!hc::view::Validate(Width).empty()) return "width";
            if (Column.id_ == Profile.primaryNameId_) {
                if (Column.fieldId_ != "file.name" && Column.fieldId_ != "file.stem") return "primary-field";
                if (Column.widths_[1].hidden_ || Column.widths_[2].hidden_) return "primary-hidden";
                PrimaryFound = true;
            }
        }
        if (!PrimaryFound) return "primary-id";
        for (int Mode = 0; Mode < 3; ++Mode) {
            bool Visible = false;
            for (const auto& Column : Profile.columns_) Visible |= !Column.widths_[Mode].hidden_;
            if (!Visible) return "no-visible-column";
        }
        QSet<QUuid> Sorted;
        for (const auto& Sort : Profile.sorts_) {
            if (!Ids.contains(Sort.columnId_) || Sorted.contains(Sort.columnId_) ||
                (Sort.order_ != Qt::AscendingOrder && Sort.order_ != Qt::DescendingOrder)) return "sort";
            Sorted.insert(Sort.columnId_);
        }
        return {};
    }

    inline QString Format(const CellValue& Value, const FormatOptions& Options = {})
    {
        if (Value.State() != ValueState::Ready) return {};
        const auto Raw = Value.Raw();
        if (Raw.metaType() == QMetaType::fromType<QDateTime>()) return Raw.toDateTime().toString(Options.datePattern_);
        if (Raw.metaType() == QMetaType::fromType<QDate>()) return Raw.toDate().toString(Qt::ISODate);
        if (Raw.metaType() == QMetaType::fromType<QTime>()) return Raw.toTime().toString(Qt::ISODate);
        if (Raw.metaType() == QMetaType::fromType<quint64>() && Options.size_ != SizeFormat::Bytes) {
            auto Number = static_cast<double>(Raw.toULongLong());
            int Unit = Options.size_ == SizeFormat::Adaptive ? 0 : int(Options.size_) - 1;
            if (Options.size_ == SizeFormat::Adaptive) {
                while (Number >= 1024 && Unit < 6) { Number /= 1024; ++Unit; }
            } else for (int Index = 0; Index < Unit; ++Index) Number /= 1024;
            const QStringList Units{"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
            return QLocale().toString(Number, 'f', Options.precision_) + ' ' + Units[Unit];
        }
        return Raw.toString();
    }
}
