#pragma once

#include <QString>
#include <utility>

namespace hc::rst {

/// 캐시 판정과 빌드 완료가 현재 프리뷰 대상에 속하는지 판단한다.
class PreviewRequestState
{
public:
    struct Ticket
    {
        quint64 generation = 0;
        QString sourceFile;
        QString projectId;
        bool sphinx = false;
    };

    void invalidate()
    {
        current_ = {++generation_, {}, {}, false};
    }

    void begin(QString sourceFile, QString projectId, bool sphinx)
    {
        current_ = {++generation_, std::move(sourceFile), std::move(projectId), sphinx};
    }

    Ticket ticket() const { return current_; }

    bool accepts(const Ticket& ticket) const
    {
        return ticket.generation == current_.generation && ticket.sphinx
            && acceptsSphinxResult(ticket.sourceFile, ticket.projectId);
    }

    bool acceptsSphinxResult(const QString& sourceFile, const QString& projectId) const
    {
#ifdef Q_OS_WIN
        constexpr auto sensitivity = Qt::CaseInsensitive;
#else
        constexpr auto sensitivity = Qt::CaseSensitive;
#endif
        return current_.sphinx && !sourceFile.isEmpty()
            && current_.sourceFile.compare(sourceFile, sensitivity) == 0
            && current_.projectId == projectId;
    }

private:
    quint64 generation_ = 0;
    Ticket current_;
};

} // namespace hc::rst
