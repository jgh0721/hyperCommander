#pragma once
#include "imageArchiveTransport.hpp"
namespace hc::plugins
{
// Parent-side owner. The approved archive worker remains a sibling of the viewer.
class ImageArchiveBroker final
{
  public:
    static storage::Result<std::unique_ptr<ImageArchiveBroker>> Start();
    ~ImageArchiveBroker();
    QJsonObject Endpoint() const;
    void Allow(const QString &Path);
    void BindClient(std::uint32_t Process);

  private:
    struct State;
    explicit ImageArchiveBroker(std::shared_ptr<State> Value);
    std::shared_ptr<State> state_;
};
} // namespace hc::plugins
