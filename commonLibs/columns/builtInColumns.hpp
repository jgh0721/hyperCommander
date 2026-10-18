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
            Column.title_ = Index == 0 ? QObject::tr("이름") : Index == 7 ? QObject::tr("종류") : Fields[Index].title_;
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
            if (Index != 0) {
                auto Two = Width;
                Two.unit_ = hc::view::WidthUnit::Percent; Two.mode_ = hc::view::WidthMode::Flexible;
                Two.hidden_ = Index != 1 && Index != 3 && Index != 4 && Index != 7;
                Two.value_ = Index == 1 ? 18 : Index == 3 ? 12 : Index == 4 ? 36 : 34;
                Column.widths_[1] = Column.widths_[2] = Two;
            }
            Profile.columns_.append(Column);
        }
        Profile.primaryNameId_ = Profile.columns_.front().id_;
        Profile.sorts_.append({Profile.primaryNameId_, Qt::AscendingOrder});
        return Profile;
    }
    inline const QUuid ReferenceProfileId{"5ff35fcb-93ae-47ac-9a4b-c4e723991e38"};
    inline const QUuid ReferenceTwoLineProfileId{"7dac5cda-e87a-43c7-af0b-58c6a1c8e5da"};
    inline ColumnProfile ReferenceProfile(bool TwoLine = false)
    {
        ColumnProfile Profile;
        Profile.id_ = TwoLine ? ReferenceTwoLineProfileId : ReferenceProfileId;
        Profile.title_ = TwoLine ? QObject::tr("시안 자세히 · 2줄") : QObject::tr("시안 자세히 · 1줄");
        Profile.mode_ = TwoLine ? LayoutMode::NameFirst : LayoutMode::Details;
        Profile.splitNameExtension_ = true;
        const std::array<const char*, 6> Ids{{"d4203a5f-a52a-49da-a849-a5cdbe69fc23",
            "07a00643-8716-4dfe-909a-5f0d02950c7f", "ae7cd1be-0be6-45b0-adc5-5e01c6b91c34",
            "ac66b249-a810-42bc-a8a9-57e67c6c922b", "aa3cac42-1715-4942-a4b7-305d78c42e7f",
            "91fa28bf-c77b-4564-a927-ab979f1797da"}};
        const QStringList Fields{"file.name", "file.type", "file.extension", "file.size", "file.modified", "file.attributes"};
        const QStringList Titles{QObject::tr("이름"), QObject::tr("종류"), QObject::tr("확장자"),
            QObject::tr("크기"), QObject::tr("수정한 날짜"), QObject::tr("속성")};
        const std::array<double, 6> One{{46, 0, 9, 12, 25, 8}}, Two{{100, 34, 0, 18, 36, 12}};
        for (int Index = 0; Index < int(Ids.size()); ++Index) {
            ColumnDefinition Column;
            Column.id_ = QUuid(Ids[Index]); Column.fieldId_ = Fields[Index]; Column.title_ = Titles[Index];
            Column.alignment_ = Index == 3 ? Qt::AlignRight : Index == 2 || Index == 5 ? Qt::AlignCenter : Qt::AlignLeft;
            if (Index == 3) { Column.format_.size_ = SizeFormat::Adaptive; Column.format_.precision_ = 1; }
            if (Index == 4) Column.format_.datePattern_ = "yyyy-MM-dd HH:mm";
            for (int Mode = 0; Mode < 3; ++Mode) {
                auto& Width = Column.widths_[Mode];
                Width.unit_ = hc::view::WidthUnit::Percent; Width.mode_ = hc::view::WidthMode::Flexible;
                Width.value_ = Mode == 0 ? One[Index] : Two[Index];
                Width.hidden_ = Mode == 0 ? Index == 1 : Index == 2;
            }
            Profile.columns_.append(Column);
        }
        Profile.primaryNameId_ = Profile.columns_.front().id_;
        Profile.sorts_.append({Profile.primaryNameId_, Qt::AscendingOrder});
        return Profile;
    }
    inline bool IsBuiltInProfile(const QUuid& Id)
    {
        return Id == BuiltInProfileId || Id == ReferenceProfileId || Id == ReferenceTwoLineProfileId;
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
        const auto Known = [&](const QString& Id) { return FindScript(Profile, Id) || IsPluginField(Id) || IsVfsColumn(Id) || std::any_of(Fields.begin(), Fields.end(), [&](const auto& Field) { return Field.id_ == Id; }); };
        for (const auto& Script : Profile.scripts_) for (const auto& Dependency : Script.dependencies_) if (!Known(Dependency)) return false;
        for (const auto& Column : Profile.columns_) {
            if (Column.template_.isEmpty() && !Known(Column.fieldId_)) return false;
            if (!Column.sortField_.isEmpty() && !Known(Column.sortField_)) return false;
            for (const auto& Id : CompileTemplate(Column.template_).dependencies_) if (!Known(Id)) return false;
        }
        return Validate(Profile).isEmpty();
    }
}
