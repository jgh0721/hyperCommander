#pragma once
#include "columnContracts.hpp"

namespace hc::columns
{
    inline QVector<FieldDefinition> BuiltInFields()
    {
        return {{"file.name", QObject::tr("이름 (확장자 포함)"), FieldType::Text},
            {"file.size", QObject::tr("크기"), FieldType::UnsignedInteger},
            {"file.created", QObject::tr("만든 날짜"), FieldType::Timestamp},
            {"file.attributes", QObject::tr("속성"), FieldType::UnsignedInteger},
            {"file.modified", QObject::tr("수정 날짜"), FieldType::Timestamp},
            {"file.extension", QObject::tr("확장자"), FieldType::Text},
            {"file.stem", QObject::tr("이름 (확장자 제외)"), FieldType::Text},
            {"file.type", QObject::tr("파일 종류"), FieldType::Text},
            {"file.isDirectory", QObject::tr("폴더 여부"), FieldType::Boolean}};
    }
    inline const QUuid BuiltInProfileId{"a4ea8e7c-b621-4db7-bc79-b37dab6e7b8d"};
    inline ColumnProfile DefaultProfile()
    {
        ColumnProfile Profile;
        Profile.id_ = BuiltInProfileId;
        Profile.title_ = QObject::tr("자세히");
        // Assigned once; never derive identity from field expressions or indices.
        const std::array<const char*, 8> Ids{{
            "519c8fbd-8d4a-4160-b742-bfeaf5ea9918", "81404f28-d586-4c19-99e7-f0bcabac3299",
            "e09a576b-14c3-4680-9bea-a0745bcdb9e3", "5753b771-d0a5-4a8b-a00e-f41ba091275a",
            "db962c8f-412a-442b-8cc8-a46f3cd4e7f8", "050d3fe3-b4ce-4a03-b42c-b78d207d3623",
            "0c00423a-e4b7-4e90-8cf2-706b0f2b0a6e", "128c32c8-1a12-4c9b-bb90-fad63f676f25"}};
        const auto Fields = BuiltInFields();
        for (int Index = 0; Index < int(Ids.size()); ++Index) {
            ColumnDefinition Column;
            Column.id_ = QUuid(Ids[Index]);
            Column.fieldId_ = Fields[Index].id_;
            Column.title_ = Index == 0 ? QObject::tr("이름") : Fields[Index].title_;
            Column.alignment_ = Index == 1 ? Qt::AlignRight : Index == 2 || Index == 3 || Index == 4 ? Qt::AlignCenter : Qt::AlignLeft;
            hc::view::ColumnWidthPolicy Width;
            if (Index < 5) Width = {hc::view::WidthUnit::Percent, hc::view::WidthMode::Flexible,
                Index == 0 ? 46.0 : Index == 1 ? 15.0 : Index == 3 ? 9.0 : 30.0};
            Width.hidden_ = Index >= 4;
            Column.widths_.fill(Width);
            if (!Width.hidden_) {
                Width.value_ = Index == 0 ? 100 : std::round(Width.value_ * 100 / 54 * 1000000) / 1000000;
                Column.widths_[1] = Column.widths_[2] = Width;
            }
            Profile.columns_.append(Column);
        }
        Profile.primaryNameId_ = Profile.columns_.front().id_;
        Profile.sorts_.append({Profile.primaryNameId_, Qt::AscendingOrder});
        return Profile;
    }
    struct FileSnapshot
    {
        QString name_;
        bool directory_ = false;
        std::optional<quint64> size_;
        quint64 attributes_ = 0;
        QDateTime created_;
        QDateTime modified_;
    };
    inline QString Extension(const FileSnapshot& File)
    {
        const auto Dot = File.name_.lastIndexOf('.');
        // A leading dot alone is a filename, not an extension separator.
        return !File.directory_ && Dot > 0 ? File.name_.mid(Dot + 1) : QString{};
    }
    inline CellValue Resolve(const QString& FieldId, const FileSnapshot& File)
    {
        if (FieldId == "file.name") return CellValue::Ready(File.name_);
        if (FieldId == "file.stem") {
            const auto Dot = File.name_.lastIndexOf('.');
            return CellValue::Ready(!File.directory_ && Dot > 0 ? File.name_.left(Dot) : File.name_);
        }
        if (FieldId == "file.extension") {
            const auto Text = Extension(File);
            return Text.isEmpty() ? CellValue::Missing(ValueState::Empty) : CellValue::Ready(Text);
        }
        if (FieldId == "file.size") return File.size_ ? CellValue::Ready(*File.size_)
            : CellValue::Missing(File.directory_ ? ValueState::OnDemand : ValueState::Empty);
        if (FieldId == "file.isDirectory") return CellValue::Ready(File.directory_);
        if (FieldId == "file.attributes") return CellValue::Ready(File.attributes_);
        if (FieldId == "file.created" || FieldId == "file.modified") {
            const auto Date = FieldId == "file.created" ? File.created_ : File.modified_;
            return Date.isValid() ? CellValue::Ready(Date) : CellValue::Missing(ValueState::Empty);
        }
        if (FieldId == "file.type") {
            const auto Ext = Extension(File);
            return CellValue::Ready(File.directory_ ? QObject::tr("폴더") : Ext.isEmpty() ? QObject::tr("파일") : QObject::tr("%1 파일").arg(Ext.toUpper()));
        }
        return CellValue::Missing(ValueState::Unsupported);
    }
}
