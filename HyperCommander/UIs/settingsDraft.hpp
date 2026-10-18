#pragma once
#include "UniqueLibs/settings/appSettings.hpp"

namespace hc::ui {
// Page-local edits never mutate global settings before Apply. Existing legacy
// editors retain their own transaction until P4 migrates them to this adapter.
class SettingsDraft
{
public:
    explicit SettingsDraft(hc::settings::Repository& Repository) : repository_(Repository) { Reload(); }
    QJsonValue Read(hc::settings::Section Section) const { return draft_.value(hc::settings::SectionName(Section)); }
    void Stage(hc::settings::Section Section, const QJsonValue& Value)
    {
        draft_[hc::settings::SectionName(Section)] = Value;
    }
    bool Dirty() const { return draft_ != base_; }
    const QString& Error() const { return error_; }
    void Discard() { draft_ = base_; error_.clear(); }
    void Reload() { base_ = repository_.Object(); draft_ = base_; error_.clear(); }
    bool Apply()
    {
        if (repository_.Editing()) { error_ = QStringLiteral("settings-editor-busy"); return false; }
        if (repository_.Object() != base_) { error_ = QStringLiteral("settings-changed-in-memory"); return false; }
        if (!repository_.Writable()) { error_ = repository_.Error(); return false; }
        repository_.BeginEdit();
        const bool Saved = repository_.Replace(draft_) && repository_.Commit();
        const auto Failure = repository_.Error();
        // Reload closes the edit transaction, including every failure path. The
        // page draft survives a failed write; Repository checks external bytes.
        repository_.DiscardEdit();
        if (!Saved) { error_ = Failure; return false; }
        Reload();
        return true;
    }
private:
    hc::settings::Repository& repository_;
    QJsonObject base_, draft_;
    QString error_;
};
} // namespace hc::ui
