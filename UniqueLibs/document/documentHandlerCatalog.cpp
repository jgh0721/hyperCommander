#include "documentOpenController.hpp"
#include "pluginHost/manifestFile.hpp"
#include "commonLibs/document/documentFormatSupport.hpp"
#include "commonLibs/layout/productLayout.hpp"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCryptographicHash>
namespace hc::document {
DocumentHandlerTarget BuildPluginViewerTarget(const QString& Name, hc::plugins::PluginViewerOptions Options,
    const QStringList& RegisteredExtensions) {
    DocumentHandlerTarget Target; Target.plugin_ = std::move(Options); auto& Descriptor = Target.descriptor_;
    const auto Key = QCryptographicHash::hash(QFileInfo(Target.plugin_.binaryPath_).absoluteFilePath().toLower().toUtf8(), QCryptographicHash::Sha256).toHex().left(20);
    Descriptor.stableId_ = "plugin." + Key.toStdString(); Descriptor.name_ = Name.toStdString();
    Descriptor.kind_ = HandlerKind::Plugin; Descriptor.view_ = true; Descriptor.maxFiles_ = 16;
    for (const auto& Extension : RegisteredExtensions) Descriptor.extensions_.push_back(Extension.toStdString());
    if (Descriptor.extensions_.empty()) Descriptor.extensions_ = {"*"};
    if (const auto Rule = Target.plugin_.applicationRule_.toObject(); Rule.value("mode").toString() == "extensions") {
        Descriptor.registeredExtensions_ = Descriptor.extensions_; Descriptor.extensions_.clear();
        for (const auto& Extension : Rule.value("extensions").toArray()) Descriptor.extensions_.push_back(Extension.toString().toStdString());
    }
    const auto& Approval = Target.plugin_.approval_;
    Descriptor.available_ = Approval.approved_ && QFileInfo(Target.plugin_.binaryPath_).isFile() && QFileInfo(Target.plugin_.executable_).isFile();
    Descriptor.editable_ = Approval.reviewed_.compatibility_ == "hyperviewer-v1";
    Descriptor.delivery_ = Descriptor.editable_ ? FileDelivery::Workspace : FileDelivery::OnePerProcess;
    return Target;
}
DocumentHandlerCatalog BuildDocumentHandlerCatalog(hc::settings::Repository& Settings, const QString& Directory,
    std::vector<DocumentHandlerTarget> Plugins) {
    DocumentHandlerCatalog Result;
    const auto Editors = MigrateEditors(Settings.Read(hc::settings::Section::Editors));
    const auto Config = Editors["documentHandlers"].toObject();
    Result.defaultView_ = Config["defaultViewHandlerId"].toString("builtin.hyperviewer").toStdString();
    Result.defaultEdit_ = Config["defaultEditHandlerId"].toString("builtin.hypereditor").toStdString();
    Result.overrides_ = Config["fileTypeOverrides"].toObject();
    // Preserve old saved handler selections while moving F3 into the workspace.
    if(Result.defaultView_=="builtin.rhwp-viewer")Result.defaultView_="builtin.hyperviewer";
    for(auto It=Result.overrides_.begin();It!=Result.overrides_.end();++It){auto Override=It.value().toObject();
        if(Override["view"]=="builtin.rhwp-viewer"){Override["view"]="builtin.hyperviewer";It.value()=Override;}}
    const bool Compatible = Config["schemaVersion"].toInt() == 1;
    const auto Add = [&](DocumentHandlerTarget Target) {
        auto& Descriptor = Target.descriptor_;
        QSet<QString> Seen;
        for (const auto& Value : Config["handlers"].toArray()) {
            const auto Item = Value.toObject(); const auto Id = Item["stableId"].toString();
            if (Seen.contains(Id)) continue; Seen.insert(Id);
            if (Id.toStdString() != Descriptor.stableId_) continue;
            if (Item["view"].isBool()) Descriptor.view_ = Item["view"].toBool();
            if (Item["edit"].isBool()) Descriptor.edit_ = Item["edit"].toBool();
            if (Item["enabled"].isBool() && !Item["enabled"].toBool()) Descriptor.available_ = false;
        }
        if (!Compatible) { Descriptor.available_ = false; Descriptor.unavailableReason_ = "document-handler-schema-unsupported"; }
        if (Result.registry_.Register(Descriptor)) Result.targets_.emplace(Descriptor.stableId_, std::move(Target));
    };
    std::vector<std::string> Formats; for (const auto Ext : DocumentExtensions) Formats.emplace_back(Ext);
    DocumentHandlerTarget Viewer;
    Viewer.descriptor_ = {"builtin.hyperviewer", "HyperViewer", HandlerKind::Plugin, FileDelivery::Workspace, Formats, true, false, true, false, 16};
    Viewer.descriptor_.maxFiles_=std::numeric_limits<std::size_t>::max();
    const QDir ViewerPackage(QDir(Directory).filePath(hc::layout::HyperViewerPackage));
    [[maybe_unused]] const auto RhwpInventory = QDir::cleanPath(QDir(QDir(Directory).filePath(hc::layout::RhwpRuntime)).filePath("runtime-inventory.json"));
    auto& Options = Viewer.plugin_; Options.binaryPath_ = ViewerPackage.filePath("HyperViewer.dll");
    Options.manifestPath_ = ViewerPackage.filePath("hyperviewer.json");
    Options.executable_ = QDir(Directory).filePath(hc::layout::PluginHost);
    QFile Manifest(Options.manifestPath_);
    if (Manifest.open(QIODevice::ReadOnly) && Manifest.size() <= 1024 * 1024) {
        const auto Parsed = hc::plugins::ParseManifest(Manifest.readAll());
        if (Parsed.HasValue() && Parsed.Value().id_ == "builtin.hyperviewer" && Parsed.Value().compatibility_ == "hyperviewer-v1") {
            Options.approval_.reviewed_ = Parsed.Value(); Options.approval_.approved_ = true; Options.approval_.granted_ = Parsed.Value().permissions_;
            Viewer.descriptor_.available_ = QFileInfo(Options.binaryPath_).isFile() && QFileInfo(Options.executable_).isFile();
        }
    }
    Viewer.descriptor_.unavailableReason_ = "hyperviewer-package-unavailable"; Add(Viewer);
#ifdef HC_RHWP_F4_RUNTIME
    DocumentHandlerTarget RhwpEditor;
    RhwpEditor.rhwpEditor_ = true; RhwpEditor.executable_ = QDir(Directory).filePath("HyperEditor.exe");
    RhwpEditor.arguments_ = "--rhwp-edit -- %FILE%";
    RhwpEditor.descriptor_ = {"builtin.rhwp-editor", "HyperEditor (HWP/HWPX)", HandlerKind::Executable,
        FileDelivery::OnePerProcess, {"hwp", "hwpx"}, false, true, true,
        QFileInfo(RhwpEditor.executable_).isFile() && QFileInfo(RhwpInventory).isFile(), 16};
    RhwpEditor.descriptor_.unavailableReason_ = "rhwp-editor-runtime-unavailable";
    Add(std::move(RhwpEditor));
#endif
    DocumentHandlerTarget Editor; Editor.productEditor_ = true; Editor.plugin_ = Viewer.plugin_;
    Editor.executable_ = QDir(Directory).filePath("HyperEditor.exe");
    Editor.descriptor_ = {"builtin.hypereditor", "HyperEditor", HandlerKind::Executable, FileDelivery::ArgumentList, {"*"}, false, true, true, QFileInfo(Editor.executable_).isFile(), 16};
#ifdef HC_RHWP_F4_RUNTIME
    Editor.descriptor_.excludedExtensions_ = {"hwp", "hwpx"};
#endif
    Editor.descriptor_.unavailableReason_ = "hypereditor-executable-unavailable"; Add(Editor);
    for (const auto& Value : Editors["items"].toArray()) {
        const auto Item = Value.toObject(); DocumentHandlerTarget Target;
        Target.executable_ = Item["program"].toString(); Target.executable_.replace("%COMMANDER_PATH%", Directory, Qt::CaseInsensitive);
        if (QDir::isRelativePath(Target.executable_)) Target.executable_ = QDir(Directory).absoluteFilePath(Target.executable_);
        Target.arguments_ = Item["arguments"].toString(); auto& Descriptor = Target.descriptor_;
        Descriptor.stableId_ = Item["stableId"].toString().toStdString(); Descriptor.name_ = Item["name"].toString().toStdString();
        Descriptor.edit_ = true; Descriptor.editable_ = true; Descriptor.available_ = QFileInfo(Target.executable_).isFile();
        Descriptor.unavailableReason_ = "external-executable-unavailable";
        // Existing editors receive each selected file in their own process; no
        // unrecognized detect expression is treated as executable code.
        Descriptor.maxFiles_ = 16;
        for (const auto& Ext : Item["extensions"].toArray()) if (Ext.isString()) Descriptor.extensions_.push_back(Ext.toString().toStdString());
        if (Descriptor.extensions_.empty() && Item["detect"].toString() == "*") Descriptor.extensions_ = {"*"};
        Add(std::move(Target));
    }
    for (auto& Target : Plugins) {
        if (QFileInfo(Target.plugin_.binaryPath_).absoluteFilePath().compare(QFileInfo(Viewer.plugin_.binaryPath_).absoluteFilePath(), Qt::CaseInsensitive) == 0) continue;
        Add(std::move(Target));
    }
    return Result;
}
}
