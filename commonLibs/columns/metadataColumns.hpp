#pragma once
#include "columnContracts.hpp"
#include "commonLibs/plugins/metadataProvider.hpp"
#include <QRegularExpression>
#include <QTimeZone>
namespace hc::columns {
    inline bool IsPluginField(const QString& Id) {
        static const QRegularExpression Pattern("^wdx\\.[a-f0-9]{32}\\.[0-9]{1,3}\\.[0-9]{1,4}$");
        return Pattern.match(Id).hasMatch();
    }
    inline FieldType MetadataFieldType(hc::plugins::MetadataType Type) {
        using T = hc::plugins::MetadataType;
        switch (Type) { case T::Integer: return FieldType::Integer; case T::Real: return FieldType::Real;
        case T::Boolean: return FieldType::Boolean; case T::Date: return FieldType::Date; case T::Time: return FieldType::Time;
        case T::Timestamp: return FieldType::Timestamp; default: return FieldType::Text; }
    }
    inline CellValue MetadataCell(const hc::plugins::MetadataValue& Value) {
        using namespace hc::plugins;
        if (Value.status_ != MetadataStatus::Value)
            return CellValue::Missing(Value.status_ == MetadataStatus::Empty ? ValueState::Empty : ValueState::OnDemand).WithDisplay(QString::fromStdString(Value.display_));
        auto Result = std::visit([&](const auto& Raw) -> CellValue {
            using T = std::decay_t<decltype(Raw)>;
            if constexpr (std::is_same_v<T, std::monostate>) return CellValue::Missing(ValueState::Empty);
            else if constexpr (std::is_same_v<T, std::string>) {
                const auto Text = QString::fromStdString(Raw);
                if (Value.type_ == MetadataType::Date) { const auto Date = QDate::fromString(Text, Qt::ISODate); return Date.isValid() ? CellValue::Ready(Date) : CellValue::Missing(ValueState::Error, "metadata-date"); }
                if (Value.type_ == MetadataType::Time) { const auto Time = QTime::fromString(Text, Qt::ISODate); return Time.isValid() ? CellValue::Ready(Time) : CellValue::Missing(ValueState::Error, "metadata-time"); }
                return CellValue::Ready(Text);
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                if (Value.type_ == MetadataType::Timestamp) {
                    if (Raw < 0) return CellValue::Missing(ValueState::Error, "metadata-filetime");
                    return CellValue::Ready(QDateTime::fromMSecsSinceEpoch(Raw / 10000 - 11644473600000LL, QTimeZone::UTC));
                }
                return CellValue::Ready(qint64(Raw));
            } else return CellValue::Ready(Raw);
        }, Value.value_);
        return Result.WithDisplay(QString::fromStdString(Value.display_));
    }
}
