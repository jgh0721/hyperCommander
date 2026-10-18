#pragma once
#include <QWidget>
#include <QJsonObject>
#include <memory>
#include "settings/appSettings.hpp"

class QPluginManagementPage final : public QWidget {
    Q_OBJECT
public:
    explicit QPluginManagementPage(QWidget* Parent = nullptr, QString ProductDirectory = {});
    ~QPluginManagementPage() override;
    void Load(hc::settings::Repository& Settings);
    void LoadDocument(const QJsonObject& Plugins);
    bool Validate();
    bool Save(hc::settings::Repository& Settings);
    QJsonObject Document() const;
    bool SelectPlugin(const QString& Binary, bool Review = false);
private:
    struct State;
    std::unique_ptr<State> state_;
};
