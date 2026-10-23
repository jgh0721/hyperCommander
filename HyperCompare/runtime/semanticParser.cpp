#include "stdafx.h"
#include "semantic.hpp"
#include <tree_sitter/api.h>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <memory>

extern "C" const TSLanguage* tree_sitter_c();
extern "C" const TSLanguage* tree_sitter_cpp();

namespace hc::compare
{
    namespace
    {
        struct ParseContext
        {
            QByteArray source_;
            std::chrono::steady_clock::time_point deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
            std::size_t nodes_ = 0;
            void check()
            {
                if (++nodes_ > 2'000'000) throw Failure("NodeLimit");
                if (std::chrono::steady_clock::now() >= deadline_) throw Failure("TimeLimit");
            }
        };
        const char* readInput(void* Payload, uint32_t Offset, TSPoint, uint32_t* Size)
        {
            const auto& source = static_cast<ParseContext*>(Payload)->source_;
            *Size = Offset >= source.size() ? 0 : std::min<uint32_t>(65536, uint32_t(source.size()) - Offset);
            return source.constData() + std::min<qsizetype>(Offset, source.size());
        }
        bool stopParsing(TSParseState* State)
        { return std::chrono::steady_clock::now() >= static_cast<ParseContext*>(State->payload)->deadline_; }
        QByteArray text(TSNode Node, const ParseContext& Context)
        {
            if (ts_node_is_null(Node)) return {};
            const auto start = ts_node_start_byte(Node), end = ts_node_end_byte(Node);
            if (end < start || end > Context.source_.size()) throw Failure("InvalidSpan");
            return Context.source_.mid(start, end - start);
        }
        template<class Visitor> void walk(TSNode Root, ParseContext& Context, Visitor Visit)
        {
            auto cursor = ts_tree_cursor_new(Root);
            struct Guard { TSTreeCursor* cursor_; ~Guard() { ts_tree_cursor_delete(cursor_); } } guard{&cursor};
            for (;;)
            {
                Context.check(); const auto node = ts_tree_cursor_current_node(&cursor);
                const bool descend = Visit(node);
                if (descend && ts_tree_cursor_goto_first_child(&cursor)) continue;
                while (!ts_tree_cursor_goto_next_sibling(&cursor))
                    if (!ts_tree_cursor_goto_parent(&cursor)) return;
            }
        }
        QString tokenHash(TSNode Node, ParseContext& Context, uint32_t Stop)
        {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            walk(Node, Context, [&](TSNode child)
            {
                if (ts_node_start_byte(child) >= Stop || QByteArrayView(ts_node_type(child)) == "comment") return false;
                if (!ts_node_child_count(child))
                {
                    const auto value = text(child, Context);
                    hash.addData(QByteArray::number(value.size())); hash.addData(":"); hash.addData(value);
                }
                return true;
            });
            return QString::fromLatin1(hash.result().toHex());
        }
        QString functionName(TSNode Node, const ParseContext& Context)
        {
            for (int depth = 0; depth < 64 && !ts_node_is_null(Node); ++depth)
            {
                const QByteArrayView type(ts_node_type(Node));
                if (type == "identifier" || type == "field_identifier" || type == "qualified_identifier" ||
                    type == "destructor_name" || type == "operator_name" || type == "operator_cast")
                {
                    if (ts_node_end_byte(Node) - ts_node_start_byte(Node) > 4096) return {};
                    auto name = QString::fromUtf8(text(Node, Context));
                    name.remove(QRegularExpression("\\s+")); return name;
                }
                Node = ts_node_child_by_field_name(Node, "declarator", 10);
            }
            return {};
        }
        QJsonObject extract(ParseContext& Context, const QString& Language)
        {
            std::unique_ptr<TSParser, decltype(&ts_parser_delete)> parser(ts_parser_new(), ts_parser_delete);
            if (!parser || !ts_parser_set_language(parser.get(), Language == "c" ? tree_sitter_c() : tree_sitter_cpp())) throw Failure("LanguageAbi");
            TSInput input{}; input.payload = &Context; input.read = readInput; input.encoding = TSInputEncodingUTF8;
            TSParseOptions options{}; options.payload = &Context; options.progress_callback = stopParsing;
            std::unique_ptr<TSTree, decltype(&ts_tree_delete)> tree(ts_parser_parse_with_options(parser.get(), nullptr, input, options), ts_tree_delete);
            if (!tree) throw Failure("TimeLimit");
            const auto root = ts_tree_root_node(tree.get());
            QJsonArray functions; int skipped = 0;
            walk(root, Context, [&](TSNode node)
            {
                if (QByteArrayView(ts_node_type(node)) != "function_definition") return true;
                if (ts_node_has_error(node)) { ++skipped; return false; }
                const auto body = ts_node_child_by_field_name(node, "body", 4);
                const auto declarator = ts_node_child_by_field_name(node, "declarator", 10);
                const auto name = functionName(declarator, Context);
                if (ts_node_is_null(body) || name.isEmpty()) { ++skipped; return false; }
                if (functions.size() >= 10000) throw Failure("FunctionLimit");
                QStringList scopes; auto parent = ts_node_parent(node); int depth = 0;
                for (; !ts_node_is_null(parent) && depth < 256; ++depth, parent = ts_node_parent(parent))
                {
                    const QByteArrayView type(ts_node_type(parent));
                    if (type == "namespace_definition" || type == "class_specifier" || type == "struct_specifier" || type == "union_specifier")
                    {
                        const auto scopeNode = ts_node_child_by_field_name(parent, "name", 4);
                        if (!ts_node_is_null(scopeNode) && ts_node_end_byte(scopeNode) - ts_node_start_byte(scopeNode) > 4096) { ++skipped; return false; }
                        const auto scope = ts_node_is_null(scopeNode) ? QString("(anonymous)") : QString::fromUtf8(text(scopeNode, Context));
                        scopes.prepend(scope);
                    }
                    if (type == "lambda_expression" || type == "function_definition") { ++skipped; return false; }
                }
                if (depth >= 256) { ++skipped; return false; }
                const auto qualified = scopes.isEmpty() ? name : scopes.join("::") + "::" + name;
                const auto separator = qualified.lastIndexOf("::");
                functions.append(QJsonObject{{"name", qualified}, {"scope", separator < 0 ? QString{} : qualified.left(separator)},
                    {"signature", tokenHash(node, Context, ts_node_start_byte(body))},
                    {"body", tokenHash(body, Context, ts_node_end_byte(body))},
                    {"begin", double(ts_node_start_byte(node))}, {"end", double(ts_node_end_byte(node))},
                    {"line", int(ts_node_start_point(node).row)}, {"endLine", int(ts_node_end_point(node).row)}});
                return false;
            });
            return {{"status", ts_node_has_error(root) || skipped ? "Partial" : "Complete"}, {"functions", functions}, {"skipped", skipped},
                {"syntaxErrors", ts_node_has_error(root)}};
        }
    }
    int SemanticWorkerMain()
    {
        QFile input, output; if (!input.open(stdin, QIODevice::ReadOnly) || !output.open(stdout, QIODevice::WriteOnly)) return 2;
        const auto bytes = input.read(24 * 1024 * 1024 + 1); QJsonObject result;
        if (bytes.size() > 24 * 1024 * 1024) return 2;
        QJsonParseError error; const auto request = QJsonDocument::fromJson(bytes, &error).object();
        const auto nonce = request["nonce"].toString(), language = request["language"].toString();
        if (error.error != QJsonParseError::NoError || request["schema"].toInt() != 1 || nonce.size() != 36 || (language != "c" && language != "cpp")) return 2;
        try
        {
            ParseContext context; context.source_ = QByteArray::fromBase64(request["source"].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            if (context.source_.size() > 16 * 1024 * 1024 || context.source_.contains('\0')) throw Failure("InputLimit");
            result = extract(context, language);
        }
        catch (const std::exception& failure) { result = {{"status", "Unavailable"}, {"reason", QString::fromUtf8(failure.what())}}; }
        result["schema"] = 1; result["nonce"] = nonce; result["language"] = language;
        result["engine"] = "tree-sitter-0.27.1/c-0.24.2/cpp-0.23.4";
        const auto response = QJsonDocument(result).toJson(QJsonDocument::Compact);
        if (response.size() > 8 * 1024 * 1024 || output.write(response) != response.size() || !output.flush()) return 2;
        return 0;
    }
}
