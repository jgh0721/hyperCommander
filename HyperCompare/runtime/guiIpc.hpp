#pragma once
#include "session.hpp"
#include <QLocalSocket>

namespace hc::compare
{
    inline constexpr qsizetype guiMessageLimit = 65536;
    QString GuiEndpoint();
    bool GuiPeerAllowed(QLocalSocket& Socket, bool ServerPeer);
    QByteArray GuiFrame(const QJsonObject& Value);
    // One request per connection; no source bytes or process-global cwd changes.
    int SubmitGuiRequest(const QStringList& Arguments, const Request& Input,
        const std::shared_ptr<std::atomic_bool>& Canceled, QString& Error);
}
