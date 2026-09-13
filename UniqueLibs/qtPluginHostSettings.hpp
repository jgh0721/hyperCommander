#pragma once
#include <QWidget>
#include <QByteArray>
#include <vector>
#include "qtViewerSurface.hpp"

class QSettings;
class QTableWidget;
class QPushButton;
class QComboBox;
class QPluginHostSettings final : public QWidget
{
public:
    explicit QPluginHostSettings(QWidget* Parent = nullptr);
    QPluginHostSettings(hc::plugins::PluginFormat Format, QWidget* Parent = nullptr);
    void Load(QSettings& Settings);
    void Save(QSettings& Settings);
    static std::vector<QtViewerSurface::PluginViewerChoice> ViewerChoices(QSettings& Settings);
    static std::vector<QtViewerSurface::PluginViewerChoice> DataChoices(QSettings& Settings, hc::plugins::PluginFormat Format);
    static QtViewerSurface::PluginViewerSelectionMode ViewerSelection(QSettings& Settings);
private:
    struct Row { int index_; QString path_, manifest_; QByteArray reviewed_; };
    void review();
    void revoke();
    void updateState(int Row);
    QTableWidget* table_;
    QPushButton* review_;
    QComboBox* selection_ = nullptr;
    std::vector<Row> rows_;
    unsigned generation_ = 0;
    hc::plugins::PluginFormat format_ = hc::plugins::PluginFormat::Wcx;
    QString group_;
};
