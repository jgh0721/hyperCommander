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
extern "C" const TSLanguage* tree_sitter_python();

namespace hc::compare
{
    namespace
    {
        struct ParseContext
        {
            QByteArray source_;
            bool python_ = false;
            std::chrono::steady_clock::time_point deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
            std::size_t nodes_ = 0, tokenBytes_ = 0;
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
        QString tokenHash(TSNode Node, ParseContext& Context, uint32_t Stop, TSNode Skip = {}, int* Tokens = nullptr, int* Numbers = nullptr)
        {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            walk(Node, Context, [&](TSNode child)
            {
                if (!ts_node_is_null(Skip) && ts_node_eq(child, Skip)) { hash.addData("<function-name>"); return false; }
                if (ts_node_start_byte(child) >= Stop || QByteArrayView(ts_node_type(child)) == "comment") return false;
                // Python indentation changes tree structure without changing leaf tokens.
                if (Context.python_)
                {
                    int children = 0;
                    for (uint32_t i = 0; i < ts_node_child_count(child); ++i)
                    {
                        Context.check();
                        if (QByteArrayView(ts_node_type(ts_node_child(child, i))) != "comment") ++children;
                    }
                    hash.addData(ts_node_type(child)); hash.addData(":");
                    hash.addData(QByteArray::number(children)); hash.addData(";");
                }
                if (Numbers && QByteArrayView(ts_node_type(child)) == "number_literal")
                {
                    ++*Numbers; hash.addData("N;"); return false;
                }
                if (!ts_node_child_count(child))
                {
                    if (Tokens) ++*Tokens;
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
            if (!parser || !ts_parser_set_language(parser.get(), Language == "python" ? tree_sitter_python() : Language == "c" ? tree_sitter_c() : tree_sitter_cpp())) throw Failure("LanguageAbi");
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
                const auto declarator = Context.python_ ? ts_node_child_by_field_name(node, "name", 4) : ts_node_child_by_field_name(node, "declarator", 10);
                const auto name = functionName(declarator, Context);
                if (ts_node_is_null(body) || name.isEmpty()) { ++skipped; return false; }
                if (functions.size() >= 10000) throw Failure("FunctionLimit");
                QStringList scopes; auto parent = ts_node_parent(node); int depth = 0;
                for (; !ts_node_is_null(parent) && depth < 256; ++depth, parent = ts_node_parent(parent))
                {
                    const QByteArrayView type(ts_node_type(parent));
                    if (type == "class_definition" || type == "namespace_definition" || type == "class_specifier" || type == "struct_specifier" || type == "union_specifier")
                    {
                        const auto scopeNode = ts_node_child_by_field_name(parent, "name", 4);
                        if (!ts_node_is_null(scopeNode) && ts_node_end_byte(scopeNode) - ts_node_start_byte(scopeNode) > 4096) { ++skipped; return false; }
                        auto scope = ts_node_is_null(scopeNode) ? QString("(anonymous)") : QString::fromUtf8(text(scopeNode, Context));
                        scope.remove(QRegularExpression("\\s+"));
                        scopes.prepend(scope);
                    }
                    if (type == "lambda_expression" || type == "function_definition") { ++skipped; return false; }
                }
                if (depth >= 256) { ++skipped; return false; }
                const auto qualified = name.startsWith("::") ? name.mid(2) : scopes.isEmpty() ? name : scopes.join("::") + "::" + name;
                if (qualified.size() > 16384) { ++skipped; return false; }
                auto signatureRoot = node; const auto enclosing = ts_node_parent(node);
                if (!ts_node_is_null(enclosing) && QByteArrayView(ts_node_type(enclosing)) == "template_declaration") signatureRoot = enclosing;
                if (Context.python_ && !ts_node_is_null(enclosing) && QByteArrayView(ts_node_type(enclosing)) == "decorated_definition") signatureRoot = enclosing;
                // Only ordinary, typed functions are rename candidates. Constructors,
                // operators and unresolved declarators retain name-based matching.
                auto nameNode = declarator;
                for (int level = 0; level < 64 && !ts_node_is_null(nameNode); ++level)
                {
                    const QByteArrayView kind(ts_node_type(nameNode));
                    if (kind == "identifier" || kind == "field_identifier") break;
                    nameNode = ts_node_child_by_field_name(nameNode, kind == "qualified_identifier" ? "name" : "declarator",
                        kind == "qualified_identifier" ? 4 : 10);
                }
                const bool ordinary = !Context.python_ && !ts_node_is_null(nameNode) &&
                    (QByteArrayView(ts_node_type(nameNode)) == "identifier" || QByteArrayView(ts_node_type(nameNode)) == "field_identifier") &&
                    !ts_node_is_null(ts_node_child_by_field_name(node, "type", 4));
                // Python decorators and special methods can depend on the declared name.
                // Only bare ordinary functions use exact structural rename matching.
                const bool pythonRename = Context.python_ && !ts_node_is_null(nameNode) &&
                    ts_node_eq(signatureRoot, node) && !(name.startsWith("__") && name.endsWith("__"));
                int bodyTokens = 0;
                const auto bodyHash = tokenHash(body, Context, ts_node_end_byte(body), {}, &bodyTokens);
                int numbers = 0;
                const auto literalShape = ordinary && bodyTokens >= 24 && bodyTokens <= 4096
                    ? tokenHash(body, Context, ts_node_end_byte(body), {}, nullptr, &numbers) : QString{};
                QJsonArray bodySequence; std::size_t sequenceBytes = 0; bool boundedSequence = true;
                if (ordinary && bodyTokens >= 32 && bodyTokens <= 256 && Context.tokenBytes_ < 512 * 1024)
                {
                    walk(body, Context, [&](TSNode child) {
                        if (QByteArrayView(ts_node_type(child)) == "comment") return false;
                        if (!ts_node_child_count(child))
                        {
                            const auto value = text(child, Context); sequenceBytes += std::size_t(value.size());
                            if (value.size() > 128 || sequenceBytes > 8192 || Context.tokenBytes_ + sequenceBytes > 512 * 1024) boundedSequence = false;
                            if (boundedSequence) bodySequence.append(QString::fromUtf8(value));
                        }
                        return true;
                    });
                    if (!boundedSequence) bodySequence = {}; else Context.tokenBytes_ += sequenceBytes;
                }
                const auto separator = qualified.lastIndexOf("::");
                functions.append(QJsonObject{{"name", qualified}, {"scope", separator < 0 ? QString{} : qualified.left(separator)},
                    {"signature", tokenHash(signatureRoot, Context, ts_node_start_byte(body))},
                    {"body", bodyHash}, {"bodyTokens", bodyTokens}, {"literalShape", literalShape}, {"numberTokens", numbers}, {"bodySequence", bodySequence},
                    {"renameSignature", (ordinary || pythonRename) ? tokenHash(signatureRoot, Context, ts_node_start_byte(body), nameNode) : QString{}},
                    {"begin", double(ts_node_start_byte(signatureRoot))}, {"end", double(ts_node_end_byte(node))},
                    {"line", int(ts_node_start_point(signatureRoot).row)}, {"endLine", int(ts_node_end_point(node).row)}});
                return false;
            });
            return {{"status", ts_node_has_error(root) || skipped ? "Partial" : "Complete"}, {"functions", functions}, {"skipped", skipped},
                {"syntaxErrors", ts_node_has_error(root)}};
        }
    }
    QJsonObject SemanticWorkerRequest(const QJsonObject& Payload)
    {
        const auto language = Payload["language"].toString();
        if (language != "c" && language != "cpp" && language != "python") throw Failure("LanguageUnsupported");
        const auto encoded = Payload["source"].toString();
        if (encoded.size() > 24 * 1024 * 1024) throw Failure("InputLimit");
        auto decoded = QByteArray::fromBase64Encoding(encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) throw Failure("InvalidInputEncoding");
        ParseContext context; context.python_ = language == "python"; context.source_ = std::move(decoded.decoded);
        if (context.source_.size() > 16 * 1024 * 1024 || context.source_.contains('\0')) throw Failure("InputLimit");
        auto result = extract(context, language); result["language"] = language;
        result["parserVersion"] = "tree-sitter-0.27.1/c-0.24.2/cpp-0.23.4/python-0.25.0";
        return result;
    }
}
