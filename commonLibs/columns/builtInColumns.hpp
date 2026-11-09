#pragma once
#include "UniqueLibs/productDefaults.hpp"
#include "columnContracts.hpp"
#include "metadataColumns.hpp"
#include "columnProfileStorage.hpp"

namespace hc::columns
{
    inline QVector<FieldDefinition> BuiltInFields() { return hc::defaults::columnPolicy::ColumnFields(); }
    inline const QUuid& BuiltInProfileId = hc::defaults::columnPolicy::BuiltInProfileId;
    inline const QUuid& ReferenceProfileId = hc::defaults::columnPolicy::ReferenceProfileId;
    inline const QUuid& ReferenceTwoLineProfileId = hc::defaults::columnPolicy::ReferenceTwoLineProfileId;
    inline ColumnProfile DefaultProfile() { return hc::defaults::columnPolicy::DefaultColumnProfile(); }
    inline const QUuid& DownloadsProfileId = hc::defaults::columnPolicy::DownloadsProfileId;
    inline ColumnProfile DownloadsProfile() { return hc::defaults::columnPolicy::DownloadsColumnProfile(); }
    inline QJsonObject WithBuiltInProfiles(QJsonObject Library)
    {
        for (const auto& Profile : {DefaultProfile(), DownloadsProfile()})
            if (!Library.contains(IdText(Profile.id_))) Library[IdText(Profile.id_)] = EncodeProfile(Profile);
        return Library;
    }
    inline ColumnProfile ReferenceProfile(bool TwoLine = false) { return hc::defaults::columnPolicy::ReferenceColumnProfile(TwoLine); }
    inline bool IsBuiltInProfile(const QUuid& Id)
    {
        return Id == BuiltInProfileId || Id == DownloadsProfileId || Id == ReferenceProfileId || Id == ReferenceTwoLineProfileId;
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
