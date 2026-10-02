#include "stdafx.h"
#include "columnPluginFields.hpp"
#include "columnNativeFields.hpp"
#include "qtPluginHostSettings.hpp"
#include "storageProviders.hpp"
#include "pluginHost/hostedDataProviders.hpp"
#include <QCryptographicHash>
namespace hc::columns {
    using namespace hc::plugins;
    using namespace hc::storage;
    struct PluginFields::State {
        struct Entry { FieldDefinition field_; std::shared_ptr<IMetadataProvider> provider_; MetadataRequest request_; bool descending_ = false; };
        QMap<QString, Entry> entries_;
    };
    PluginFields& PluginFields::Instance() { static auto* Value = new PluginFields(qApp); return *Value; }
    PluginFields::PluginFields(QObject* Parent) : QObject(Parent) { Refresh(); }
    void PluginFields::RefreshIfChanged() {
        const auto Plugins = hc::settings::AppSettings().Read(hc::settings::Section::Plugins).toObject();
        const auto Configuration = QJsonDocument(QJsonObject{{"wdx", Plugins["wdx"]}, {"dopusViewer", Plugins["dopusViewer"]}}).toJson(QJsonDocument::Compact);
        if (Configuration != configuration_) Refresh();
    }
    void PluginFields::Refresh() {
        if (busy_) { refreshAgain_ = true; return; } busy_ = true;
        const auto Plugins = hc::settings::AppSettings().Read(hc::settings::Section::Plugins).toObject();
        configuration_ = QJsonDocument(QJsonObject{{"wdx", Plugins["wdx"]}, {"dopusViewer", Plugins["dopusViewer"]}}).toJson(QJsonDocument::Compact);
        auto Choices = QPluginHostSettings::DataChoices(hc::settings::AppSettings(), PluginFormat::Wdx);
        const auto Viewers = QPluginHostSettings::DataChoices(hc::settings::AppSettings(), PluginFormat::DopusViewer);
        Choices.insert(Choices.end(), Viewers.begin(), Viewers.end());
        auto* Watcher = new QFutureWatcher<std::pair<std::shared_ptr<State>, QString>>(this);
        connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher] {
            auto Result = Watcher->result(); Watcher->deleteLater(); busy_ = false;
            // A newer configuration supersedes this result. Publishing the old
            // catalog would briefly invalidate fields and preview dependencies.
            if (refreshAgain_) { refreshAgain_ = false; Refresh(); return; }
            state_ = std::move(Result.first); status_ = Result.second; emit Changed();
        });
        Watcher->setFuture(QtConcurrent::run([Choices] {
            auto Result = std::shared_ptr<State>(new State, [](State* Value) { (void)QtConcurrent::run([Value] { delete Value; }); });
            QStringList Errors;
            for (const auto& Choice : Choices) {
                const auto& Options = Choice.options_;
                OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                if (!Options.approval_.approved_) { Errors.append(Choice.name_ + ": approval-required"); continue; }
                auto Inspected = InspectManifestFile(Options.manifestPath_, Context);
                if (!Inspected.HasValue()) { Errors.append(Choice.name_ + ": " + QString::fromStdString(Inspected.Failure().code_)); continue; }
                if (QFileInfo(Options.binaryPath_).canonicalFilePath().compare(Inspected.Value().binaryPath_, Qt::CaseInsensitive)) { Errors.append(Choice.name_ + ": manifest-mismatch"); continue; }
                auto Started = PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_, {}, Context, Options.defaultIniPath_);
                if (!Started.HasValue()) { Errors.append(Choice.name_ + ": " + QString::fromStdString(Started.Failure().code_)); continue; }
                auto Process = std::shared_ptr<PluginProcess>(Started.Value().release(), [](PluginProcess* Value) { (void)QtConcurrent::run([Value] { delete Value; }); });
                auto Materializer = MakeNativeMaterializer(QDir::tempPath().toStdString()); if (!Materializer.HasValue()) continue;
                const bool Dopus = Options.approval_.reviewed_.format_ == PluginFormat::DopusViewer;
                auto Provider = Dopus ? MakeHostedDopusMetadataProvider(Process, Materializer.Value()) : MakeHostedWdxProvider(Process, Materializer.Value()); if (!Provider.HasValue()) continue;
                auto Fields = Provider.Value()->Fields(Context).get(); if (!Fields.HasValue()) { Errors.append(Choice.name_ + ": fields-unavailable"); continue; }
                const auto Key = QString::fromLatin1(QCryptographicHash::hash(Options.binaryPath_.toCaseFolded().toUtf8(), QCryptographicHash::Md5).toHex());
                for (const auto& Field : Fields.Value()) {
                    if (Field.type_ == MetadataType::FullText) continue;
                    const int Count = std::max(1, int(Field.units_.size()));
                    for (int Unit = 0; Unit < Count; ++Unit) {
                        const auto Id = (Dopus ? "dopusviewer." : "wdx.") + Key + "." + QString::number(Field.index_) + "." + QString::number(Unit);
                        auto Title = Choice.name_ + " / " + QString::fromStdString(Field.name_);
                        if (Unit < Field.units_.size() && !Field.units_[Unit].empty()) Title += " [" + QString::fromStdString(Field.units_[Unit]) + "]";
                        if (!Field.choices_.empty()) { QStringList Values; for (const auto& Value : Field.choices_) Values.append(QString::fromStdString(Value)); Title += " {" + Values.join(", ") + "}"; }
                        FieldDefinition Definition{Id, Title, MetadataFieldType(Field.type_), Dopus ? ProviderKind::DOpus : ProviderKind::Wdx, EvaluationCost::Background};
                        Result->entries_.insert(Id, {Definition, Provider.Value(), {Field.index_, Unit, true, false}, Field.descending_});
                    }
                }
            }
            return std::make_pair(Result, Errors.join("\n"));
        }));
    }
    QVector<FieldDefinition> PluginFields::Fields() const {
        auto Result = BuiltInFields(); if (state_) for (const auto& Entry : state_->entries_) Result.append(Entry.field_); return Result;
    }
    bool PluginFields::Descending(const QString& Field) const { return state_ && state_->entries_.contains(Field) && state_->entries_[Field].descending_; }
    QueryEngine::Loader PluginFields::Loader(bool Explicit) const {
        return [State = state_, Explicit](const QString& Path, const QString& Family, std::stop_token Stop) -> FieldValues {
            if (!IsPluginField(Family)) return ReadNativeFields(Path, Family, Stop);
            const auto Fail = [&](ValueState Kind, const QString& Reason) { return FieldValues{{Family, CellValue::Missing(Kind, Reason)}}; };
            if (!State || !State->entries_.contains(Family)) return Fail(ValueState::Unsupported, "plugin-field-unavailable");
            const auto Entry = State->entries_.value(Family);
            OperationContext Context; Context.stopToken_ = Stop; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            auto Storage = MakeNativeReadProvider(Path.startsWith("\\\\") ? LocatorKind::Smb : LocatorKind::Local);
            auto Item = Storage->Resolve({Path.startsWith("\\\\") ? LocatorKind::Smb : LocatorKind::Local, Path.toStdString()}, Context).get();
            if (!Item.HasValue()) return Fail(ValueState::Error, QString::fromStdString(Item.Failure().code_));
            auto Source = std::make_shared<ProviderContentSource>(Storage, Item.Value());
            auto Request = Entry.request_; Request.deferSlow_ = !Explicit;
            auto Value = Entry.provider_->Query(Source, Request, Context).get();
            if (Value.HasValue() && Value.Value().status_ == MetadataStatus::Delayed) { Request.deferSlow_ = false; Value = Entry.provider_->Query(Source, Request, Context).get(); }
            if (!Value.HasValue()) return Fail(ValueState::Error, QString::fromStdString(Value.Failure().code_));
            return {{Family, MetadataCell(Value.Value())}};
        };
    }
}
