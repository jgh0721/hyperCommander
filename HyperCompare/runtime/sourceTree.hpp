#pragma once
#include "core/compare.hpp"
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <atomic>
#include <chrono>
#include <memory>

namespace hc::compare
{
    inline constexpr char sourceParserVersion[] = "tree-sitter-0.27.1/c-0.24.2/cpp-0.23.4/python-0.25.0";
    // Text, parse tree and per-function digests of one source document.
    // After Edit() the next Analyze() reparses incrementally and recomputes only
    // the digests of functions whose text or syntax changed. The output is
    // byte-identical to analyzing the same text in a fresh SourceTree.
    class SourceTree final
    {
    public:
        explicit SourceTree(QString Language);
        ~SourceTree();
        SourceTree(const SourceTree&) = delete;
        SourceTree& operator=(const SourceTree&) = delete;
        const QString& Language() const { return language_; }
        const QByteArray& Text() const { return text_; }
        void Reset(QByteArray Text);
        // Replaces [Begin, End) of the current text. Throws Failure on an invalid range.
        void Edit(qsizetype Begin, qsizetype End, const QByteArray& Bytes);
        // Throws Failure with TimeLimit, Canceled, NodeLimit, FunctionLimit or InputLimit.
        // A failed analysis drops the tree, so the next call parses from scratch.
        QJsonObject Analyze(std::chrono::steady_clock::time_point Deadline, const std::atomic_bool* Cancel = nullptr);
        // Function digests reused by the last Analyze(); diagnostics for tests.
        qsizetype Reused() const { return reused_; }
    private:
        struct State;
        QString language_;
        QByteArray text_;
        std::unique_ptr<State> state_;
        qsizetype reused_ = 0;
    };
}
