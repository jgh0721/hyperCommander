#pragma once
#include <QWidget>
#include <QByteArray>
#include <vector>
#include <functional>
#include "qtViewerSurface.hpp"

#include "settings/appSettings.hpp"
class QTableWidget;
class QPushButton;
class QComboBox;
class QFileSystemWatcher;
class QTimer;
class QPluginHostSettings final : public QWidget
{
public:
    explicit QPluginHostSettings(QWidget* Parent = nullptr);
    QPluginHostSettings(hc::plugins::PluginFormat Format, QWidget* Parent = nullptr);
    void Load(hc::settings::Repository& Settings);
    void LoadPlugins(const QJsonObject& Plugins);
    void SavePlugins(QJsonObject& Plugins) const;
    void SetCompactMode();
    void SelectPlugin(int Index);
    void SetHostEnabled(int Index, bool Enabled);
    void ReviewPlugin(int Index);
    QString VerificationState(int Index) const;
    std::function<void()> onChanged_;
    void Save(hc::settings::Repository& Settings);
    static std::vector<QtViewerSurface::PluginViewerChoice> ViewerChoices(hc::settings::Repository& Settings);
    static std::vector<QtViewerSurface::PluginViewerChoice> DataChoices(hc::settings::Repository& Settings, hc::plugins::PluginFormat Format);
    static QtViewerSurface::PluginViewerSelectionMode ViewerSelection(hc::settings::Repository& Settings);
private:
    struct Row { int index_; QString path_, manifest_; QByteArray reviewed_; QString verification_; };
    void review();
    void revoke();
    void updateState(int Row);
    void checkFiles();
    QString resolvedBinary(const QString& Path) const;
    QTableWidget* table_;
    QPushButton* review_;
    QComboBox* selection_ = nullptr;
    std::vector<Row> rows_;
    unsigned generation_ = 0;
    bool loading_ = false;
    hc::plugins::PluginFormat format_ = hc::plugins::PluginFormat::Wcx;
    QString group_;
    QString settingsDirectory_;
    QFileSystemWatcher* files_;
    QTimer* checkTimer_;
    quint64 checkGeneration_ = 0;
};
