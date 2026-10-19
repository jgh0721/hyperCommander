#pragma once
#include <QImage>
#include <QString>
#include <memory>
struct GpuResizeResult
{
    QImage image_;
    QString adapter_, error_;
    double uploadMs_ = 0, processMs_ = 0, readbackMs_ = 0;
};
class GpuResizer
{
  public:
    explicit GpuResizer(bool Software = false);
    ~GpuResizer();
    GpuResizeResult Resize(const QImage &Input, QSize Output, const QString &Filter);
  private:
    struct State;
    std::unique_ptr<State> state_;
};
