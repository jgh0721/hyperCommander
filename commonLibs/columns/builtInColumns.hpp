#pragma once
#include "columnContracts.hpp"
#include "metadataColumns.hpp"

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
            {"file.type", QObject::tr("파일 형식 설명"), FieldType::Text},
            {"file.isDirectory", QObject::tr("폴더 여부"), FieldType::Boolean},
            {"file.kind", QObject::tr("항목 종류"), FieldType::Text},
            {"file.path", QObject::tr("전체 경로"), FieldType::Text},
            {"file.parentPath", QObject::tr("폴더 경로"), FieldType::Text},
            {"file.parentName", QObject::tr("부모 폴더명"), FieldType::Text},
            {"file.pathLength", QObject::tr("경로 길이"), FieldType::UnsignedInteger},
            {"file.accessed", QObject::tr("접근 시각"), FieldType::Timestamp},
            {"file.readOnly", QObject::tr("읽기 전용"), FieldType::Boolean},
            {"file.hidden", QObject::tr("숨김"), FieldType::Boolean},
            {"file.system", QObject::tr("시스템"), FieldType::Boolean},
            {"file.archive", QObject::tr("보관"), FieldType::Boolean},
            {"file.compressed", QObject::tr("압축"), FieldType::Boolean},
            {"file.encrypted", QObject::tr("암호화"), FieldType::Boolean},
            {"file.sparse", QObject::tr("희소 파일"), FieldType::Boolean},
            {"file.offline", QObject::tr("오프라인"), FieldType::Boolean},
            {"file.allocatedSize", QObject::tr("디스크 할당 크기"), FieldType::UnsignedInteger, ProviderKind::BuiltIn, EvaluationCost::Background},
            {"file.linkTarget", QObject::tr("링크 대상"), FieldType::Text, ProviderKind::BuiltIn, EvaluationCost::Background},
            {"file.fileVersion", QObject::tr("파일 버전"), FieldType::Text, ProviderKind::BuiltIn, EvaluationCost::Background},
            {"file.productVersion", QObject::tr("제품 버전"), FieldType::Text, ProviderKind::BuiltIn, EvaluationCost::Background},
            {"file.productName", QObject::tr("제품 이름"), FieldType::Text, ProviderKind::BuiltIn, EvaluationCost::Background},
            {"file.company", QObject::tr("회사"), FieldType::Text, ProviderKind::BuiltIn, EvaluationCost::Background},
            {"file.description", QObject::tr("파일 설명"), FieldType::Text, ProviderKind::BuiltIn, EvaluationCost::Background},
            {"file.imageWidth", QObject::tr("이미지 가로"), FieldType::UnsignedInteger, ProviderKind::BuiltIn, EvaluationCost::Background},
            {"file.imageHeight", QObject::tr("이미지 세로"), FieldType::UnsignedInteger, ProviderKind::BuiltIn, EvaluationCost::Background},
            {"file.imageDimensions", QObject::tr("이미지 규격"), FieldType::Text, ProviderKind::BuiltIn, EvaluationCost::Background},
            {"file.imageBitDepth", QObject::tr("이미지 비트 깊이"), FieldType::UnsignedInteger, ProviderKind::BuiltIn, EvaluationCost::Background}};
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
        QDateTime accessed_;
        QString parentPath_;
        QString typeDescription_;
    };
    inline QString Extension(const FileSnapshot& File)
    {
        const auto Dot = File.name_.lastIndexOf('.');
        // A leading dot alone is a filename, not an extension separator.
        return !File.directory_ && Dot > 0 ? File.name_.mid(Dot + 1) : QString{};
    }
    inline CellValue Resolve(const QString& FieldId, const FileSnapshot& File)
    {
        if (FieldId == "file.parentPath") return File.parentPath_.isEmpty() ? CellValue::Missing(ValueState::Empty) : CellValue::Ready(File.parentPath_);
        if (FieldId == "file.parentName") {
            auto Path = File.parentPath_; Path.replace('\\', '/');
            while (Path.endsWith('/') && Path.size() > 1) Path.chop(1);
            return Path.isEmpty() ? CellValue::Missing(ValueState::Empty) : CellValue::Ready(Path.mid(Path.lastIndexOf('/') + 1));
        }
        if (FieldId == "file.path" || FieldId == "file.pathLength") {
            if (File.parentPath_.isEmpty()) return CellValue::Missing(ValueState::Empty);
            const auto Separator = File.parentPath_.contains('\\') ? '\\' : '/';
            const auto Path = File.parentPath_ + (File.parentPath_.endsWith('/') || File.parentPath_.endsWith('\\') ? QString{} : QString(Separator)) + File.name_;
            return FieldId == "file.path" ? CellValue::Ready(Path) : CellValue::Ready(quint64(Path.size()));
        }
        const std::array<std::pair<const char*, quint64>, 8> Flags{{{"file.readOnly", 0x1}, {"file.hidden", 0x2},
            {"file.system", 0x4}, {"file.archive", 0x20}, {"file.compressed", 0x800}, {"file.encrypted", 0x4000},
            {"file.sparse", 0x200}, {"file.offline", 0x1000}}};
        for (const auto& [Id, Flag] : Flags) if (FieldId == Id) return CellValue::Ready(bool(File.attributes_ & Flag));
        if (FieldId == "file.kind") return CellValue::Ready((File.attributes_ & 0x400) ? QObject::tr("링크") : File.directory_ ? QObject::tr("폴더") : QObject::tr("파일"));
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
        if (FieldId == "file.created" || FieldId == "file.modified" || FieldId == "file.accessed") {
            const auto Date = FieldId == "file.created" ? File.created_ : FieldId == "file.modified" ? File.modified_ : File.accessed_;
            return Date.isValid() ? CellValue::Ready(Date) : CellValue::Missing(ValueState::Empty);
        }
        if (FieldId == "file.type") {
            if (!File.typeDescription_.isEmpty()) return CellValue::Ready(File.typeDescription_);
            const auto Ext = Extension(File);
            return CellValue::Ready(File.directory_ ? QObject::tr("폴더") : Ext.isEmpty() ? QObject::tr("파일") : QObject::tr("%1 파일").arg(Ext.toUpper()));
        }
        return CellValue::Missing(ValueState::Unsupported);
    }
    inline bool IsSupportedProfile(const ColumnProfile& Profile)
    {
        const auto Fields = BuiltInFields();
        const auto Known = [&](const QString& Id) { return IsPluginField(Id) || std::any_of(Fields.begin(), Fields.end(), [&](const auto& Field) { return Field.id_ == Id; }); };
        for (const auto& Column : Profile.columns_) {
            if (Column.template_.isEmpty() && !Known(Column.fieldId_)) return false;
            if (!Column.sortField_.isEmpty() && !Known(Column.sortField_)) return false;
            for (const auto& Id : CompileTemplate(Column.template_).dependencies_) if (!Known(Id)) return false;
        }
        return Validate(Profile).isEmpty();
    }
}
