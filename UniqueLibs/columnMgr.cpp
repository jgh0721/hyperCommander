#include "stdafx.h"
#include "columnMgr.hpp"
#include "builtInFsModel.hpp"
#include "cmnHCUtils.hpp"

CColumnMgr::CColumnMgr() { Initialize(); }

void CColumnMgr::Initialize()
{
    columnViews_.clear();
    columnViews_.append(BindColumnProfile(hc::columns::DefaultProfile()));
}

qsizetype CColumnMgr::GetCount() const { return columnViews_.size(); }

ColumnView CColumnMgr::GetColumnView(int Index)
{
    return Index >= 0 && Index < columnViews_.size() ? columnViews_[Index] : ColumnView{};
}

hc::columns::CellValue CColumnMgr::GetValue(const hc::columns::ColumnDefinition& Column, const nsHC::CFileSourceT* Info, const QString& ParentPath)
{
    return GetFieldValue(Column.fieldId_, Info, ParentPath);
}

hc::columns::CellValue CColumnMgr::GetFieldValue(const QString& Field, const nsHC::CFileSourceT* Info, const QString& ParentPath)
{
    using namespace hc::columns;
    if (!Info) return CellValue::Missing(ValueState::Empty);
    FileSnapshot File;
    File.name_ = FlagOn(Info->Flags_, nsHC::CFileSourceT::FS_FLAG_DRIVE) ? Info->Name_.left(2) : Info->Name_;
    File.directory_ = (Info->Attributes_ & FILE_ATTRIBUTE_DIRECTORY) != 0;
    // Filesystem directory metadata may report allocation bytes, not content size.
    // Only an explicit recursive-size result is ready, including a measured zero.
    if (File.directory_) File.size_ = Info->directorySize_;
    else if (Info->storageEntry_) File.size_ = Info->storageEntry_->size_;
    else if (Info->Size_ >= 0) File.size_ = quint64(Info->Size_);
    File.attributes_ = Info->Attributes_;
    File.created_ = Info->Created_; File.modified_ = Info->Modified_;
    File.parentPath_ = ParentPath;
    if (Info->storageEntry_) {
        const auto& Hints = Info->storageEntry_->hints_;
        if (const auto It = Hints.find("accessedUtcMs"); It != Hints.end()) {
            bool Ok = false; const auto Time = QString::fromStdString(It->second).toLongLong(&Ok);
            if (Ok) File.accessed_ = QDateTime::fromMSecsSinceEpoch(Time);
        }
        if (const auto It = Hints.find("typeDescription"); It != Hints.end()) File.typeDescription_ = QString::fromStdString(It->second);
    }
    return Resolve(Field, File);
}

QString CColumnMgr::FormatValue(const hc::columns::ColumnDefinition& Column, const hc::columns::CellValue& Value, const nsHC::CFileSourceT* Info)
{
    if (Column.format_.providerDisplay_ && !Value.Display().isEmpty()) return Value.Display();
    if (Value.State() == hc::columns::ValueState::Pending) return QObject::tr("조회 중…");
    if (Value.State() == hc::columns::ValueState::Error) return QObject::tr("조회 오류");
    if (Value.State() == hc::columns::ValueState::Unsupported) return QObject::tr("지원 안 함");
    if (Value.State() == hc::columns::ValueState::OnDemand && Column.fieldId_ != "file.size") return QObject::tr("조회 대기");
    if (Info && Column.template_.isEmpty() && Column.fieldId_ == "file.attributes" && Value.State() == hc::columns::ValueState::Ready)
        return GetFormattedAttrText(static_cast<DWORD>(Value.Raw().toULongLong()), false);
    if (Info && Column.fieldId_ == "file.size" && Value.State() == hc::columns::ValueState::OnDemand)
        return Info->Attributes_ & FILE_ATTRIBUTE_REPARSE_POINT ? QObject::tr("<링크>") : QObject::tr("<폴더>");
    return hc::columns::Format(Value, Column.format_);
}
