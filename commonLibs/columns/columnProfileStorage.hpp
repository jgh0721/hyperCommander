#pragma once
#include <QJsonArray>
#include "columnContracts.hpp"
#include "commonLibs/view/columnWidthCodec.hpp"

namespace hc::columns
{
    // Deliberately separate from all pre-release expression/width namespaces.
    inline constexpr auto WorkspaceKey = "columnLayoutV2";
    inline constexpr auto SettingsPrefix = "ColumnsV2/Widths/";

    inline QJsonObject EncodeProfile(const ColumnProfile& Profile)
    {
        if (!Validate(Profile).isEmpty()) return {};
        QJsonArray Columns, Sorts, Scripts;
        for (const auto& Script : Profile.scripts_) {
            QJsonArray Outputs; for (const auto& Output : Script.outputs_) Outputs.append(QJsonObject{{"name", Output.name_}, {"title", Output.title_}, {"type", int(Output.type_)}});
            Scripts.append(QJsonObject{{"id", IdText(Script.id_)}, {"title", Script.title_}, {"source", Script.source_}, {"enabled", Script.enabled_}, {"dependencies", QJsonArray::fromStringList(Script.dependencies_)}, {"outputs", Outputs}});
        }
        for (const auto& Column : Profile.columns_) {
            QJsonObject Widths;
            for (int Mode = 0; Mode < 3; ++Mode)
                Widths[ModeName(static_cast<LayoutMode>(Mode))] = hc::view::EncodeWidth(Column.widths_[Mode]);
            Columns.append(QJsonObject{{"id", IdText(Column.id_)}, {"field", Column.fieldId_}, {"title", Column.title_}, {"template", Column.template_}, {"sortField", Column.sortField_},
                {"alignment", Column.alignment_ == Qt::AlignLeft ? "left" : Column.alignment_ == Qt::AlignRight ? "right" : "center"},
                {"format", QJsonObject{{"size", int(Column.format_.size_)}, {"precision", Column.format_.precision_}, {"date", Column.format_.datePattern_}, {"grouping", Column.format_.grouping_}, {"plus", Column.format_.plusSign_}, {"utc", Column.format_.utc_}, {"providerDisplay", Column.format_.providerDisplay_}}},
                {"widths", Widths}});
        }
        for (const auto& Sort : Profile.sorts_)
            Sorts.append(QJsonObject{{"column", IdText(Sort.columnId_)}, {"order", Sort.order_ == Qt::AscendingOrder ? "ascending" : "descending"}});
        return {{"version", 1}, {"id", IdText(Profile.id_)}, {"title", Profile.title_},
            {"primaryName", IdText(Profile.primaryNameId_)}, {"mode", ModeName(Profile.mode_)}, {"columns", Columns}, {"sorts", Sorts}, {"scripts", Scripts}};
    }
    inline std::optional<ColumnProfile> DecodeProfile(const QJsonObject& Object)
    {
        if (Object.value("version") != QJsonValue(1) || !Object.value("columns").isArray() ||
            !Object.value("sorts").isArray() || !Object.value("title").isString()) return {};
        const auto Mode = ParseMode(Object.value("mode").toString());
        if (!Mode) return {};
        ColumnProfile Profile;
        Profile.id_ = QUuid(Object.value("id").toString());
        Profile.title_ = Object.value("title").toString();
        Profile.primaryNameId_ = QUuid(Object.value("primaryName").toString());
        Profile.mode_ = *Mode;
        for (const auto& Value : Object.value("columns").toArray()) {
            if (!Value.isObject()) return {};
            const auto Item = Value.toObject();
            ColumnDefinition Column;
            Column.id_ = QUuid(Item.value("id").toString());
            Column.fieldId_ = Item.value("field").toString();
            if ((Item.contains("template") && !Item["template"].isString()) || (Item.contains("sortField") && !Item["sortField"].isString())) return {};
            Column.template_ = Item.value("template").toString(); Column.sortField_ = Item.value("sortField").toString();
            if (!Item.value("title").isString()) return {};
            Column.title_ = Item.value("title").toString();
            const auto Alignment = Item.value("alignment").toString();
            if (Alignment != "left" && Alignment != "right" && Alignment != "center") return {};
            Column.alignment_ = Alignment == "left" ? Qt::AlignLeft : Alignment == "right" ? Qt::AlignRight : Qt::AlignCenter;
            const auto Format = Item.value("format").toObject();
            const auto Size = Format.value("size"); const auto Precision = Format.value("precision");
            if (!Size.isDouble() || Size.toInt(-1) < 0 || Size.toInt(-1) > int(SizeFormat::GiB) ||
                !Precision.isDouble() || Precision.toInt(-1) < 0 || Precision.toInt(-1) > 6 || !Format.value("date").isString()) return {};
            Column.format_ = {static_cast<SizeFormat>(Size.toInt()), Precision.toInt(), Format.value("date").toString()};
            for (const auto* Key : {"grouping", "plus", "utc", "providerDisplay"}) if (Format.contains(Key) && !Format[Key].isBool()) return {};
            Column.format_.grouping_ = Format["grouping"].toBool(); Column.format_.plusSign_ = Format["plus"].toBool();
            Column.format_.utc_ = Format["utc"].toBool(); Column.format_.providerDisplay_ = Format["providerDisplay"].toBool(true);
            const auto Widths = Item.value("widths").toObject();
            if (Widths.size() != 3) return {};
            for (int Index = 0; Index < 3; ++Index) {
                const auto Width = hc::view::DecodeWidth(Widths.value(ModeName(static_cast<LayoutMode>(Index))).toObject());
                if (!Width) return {};
                Column.widths_[Index] = *Width;
            }
            Profile.columns_.append(Column);
        }
        for (const auto& Value : Object.value("sorts").toArray()) {
            const auto Item = Value.toObject(); const auto Order = Item.value("order").toString();
            if (Order != "ascending" && Order != "descending") return {};
            Profile.sorts_.append({QUuid(Item.value("column").toString()), Order == "ascending" ? Qt::AscendingOrder : Qt::DescendingOrder});
        }
        if (Object.contains("scripts") && !Object["scripts"].isArray()) return {};
        for (const auto& Value : Object["scripts"].toArray()) {
            const auto Item = Value.toObject(); ScriptDefinition Script;
            if (!Item["title"].isString() || !Item["source"].isString() || !Item["enabled"].isBool() || !Item["dependencies"].isArray() || !Item["outputs"].isArray()) return {};
            Script.id_ = QUuid(Item["id"].toString()); Script.title_ = Item["title"].toString(); Script.source_ = Item["source"].toString(); Script.enabled_ = Item["enabled"].toBool();
            for (const auto& Dependency : Item["dependencies"].toArray()) { if (!Dependency.isString()) return {}; Script.dependencies_.append(Dependency.toString()); }
            for (const auto& Output : Item["outputs"].toArray()) {
                const auto Field = Output.toObject(); const int Type = Field["type"].toInt(-1);
                if (!Field["name"].isString() || !Field["title"].isString() || Type < 0 || Type > int(FieldType::Timestamp)) return {};
                Script.outputs_.append({Field["name"].toString(), Field["title"].toString(), static_cast<FieldType>(Type)});
            }
            Profile.scripts_.append(Script);
        }
        return Validate(Profile).isEmpty() ? std::optional(Profile) : std::nullopt;
    }
}
