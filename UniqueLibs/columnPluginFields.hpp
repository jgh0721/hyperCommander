#pragma once
#include "commonLibs/columns/columnQueryEngine.hpp"
#include "commonLibs/columns/builtInColumns.hpp"
namespace hc::columns {
    // GUI facade publishes immutable catalog snapshots. All host calls run on workers.
    class PluginFields final : public QObject {
        Q_OBJECT
    public:
        static PluginFields& Instance();
        void Refresh();
        QVector<FieldDefinition> Fields() const;
        bool Descending(const QString& Field) const;
        QueryEngine::Loader Loader(bool Explicit = false) const;
        QString Status() const { return status_; }
    signals:
        void Changed();
    private:
        explicit PluginFields(QObject* Parent);
        struct State;
        std::shared_ptr<State> state_;
        QString status_;
        bool busy_ = false, refreshAgain_ = false;
    };
}
