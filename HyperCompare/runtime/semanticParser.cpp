#include "stdafx.h"
#include "semantic.hpp"
#include "sourceTree.hpp"
#include <tree_sitter/api.h>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <array>
#include <cstdlib>
#include <limits>
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
            const std::atomic_bool* cancel_ = nullptr;
            // The visit budget scales with the parsed tree (set after parsing); this
            // floor only bounds work before the node count is known.
            std::size_t nodes_ = 0, nodeLimit_ = 2'000'000, tokenBytes_ = 0;
            void check()
            {
                if (++nodes_ > nodeLimit_) throw Failure("NodeLimit");
                if (std::chrono::steady_clock::now() >= deadline_) throw Failure("TimeLimit");
                if (cancel_ && cancel_->load(std::memory_order_relaxed)) throw Failure("Canceled");
            }
        };
        const char* readInput(void* Payload, uint32_t Offset, TSPoint, uint32_t* Size)
        {
            const auto& source = static_cast<ParseContext*>(Payload)->source_;
            *Size = Offset >= source.size() ? 0 : std::min<uint32_t>(65536, uint32_t(source.size()) - Offset);
            return source.constData() + std::min<qsizetype>(Offset, source.size());
        }
        bool stopParsing(TSParseState* State)
        {
            const auto* context = static_cast<ParseContext*>(State->payload);
            return std::chrono::steady_clock::now() >= context->deadline_ || (context->cancel_ && context->cancel_->load(std::memory_order_relaxed));
        }
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
        // One pre-order walk feeding several hash passes. A pass stops descending
        // below a node by clearing its flag; the cursor descends while any pass is active.
        template<std::size_t Passes, class Visitor> void walkPasses(TSNode Root, ParseContext& Context, Visitor Visit)
        {
            constexpr auto running = std::numeric_limits<uint32_t>::max();
            auto cursor = ts_tree_cursor_new(Root);
            struct Guard { TSTreeCursor* cursor_; ~Guard() { ts_tree_cursor_delete(cursor_); } } guard{&cursor};
            std::array<uint32_t, Passes> stopped; stopped.fill(running);
            for (;;)
            {
                Context.check(); const auto node = ts_tree_cursor_current_node(&cursor); const auto depth = ts_tree_cursor_current_depth(&cursor);
                std::array<bool, Passes> active{};
                for (std::size_t i = 0; i < Passes; ++i) { if (stopped[i] != running && depth <= stopped[i]) stopped[i] = running; active[i] = stopped[i] == running; }
                Visit(node, active);
                bool descend = false;
                for (std::size_t i = 0; i < Passes; ++i) if (stopped[i] == running) { if (active[i]) descend = true; else stopped[i] = depth; }
                if (descend && ts_tree_cursor_goto_first_child(&cursor)) continue;
                while (!ts_tree_cursor_goto_next_sibling(&cursor))
                    if (!ts_tree_cursor_goto_parent(&cursor)) return;
            }
        }
        void addLeaf(QCryptographicHash& Hash, const QByteArray& Value)
        { Hash.addData(QByteArray::number(Value.size())); Hash.addData(":"); Hash.addData(Value); }
        // Python indentation changes tree structure without changing leaf tokens.
        void addShape(QCryptographicHash& Hash, QByteArrayView Kind, int Children)
        { Hash.addData(Kind); Hash.addData(":"); Hash.addData(QByteArray::number(Children)); Hash.addData(";"); }
        QString hex(const QCryptographicHash& Hash) { return QString::fromLatin1(Hash.result().toHex()); }
        // Position-free digests of one function. They depend only on the text and
        // syntax inside the function, so an unchanged function reuses them after edits.
        struct Digest
        {
            QString signature_, renameSignature_, body_, literalShape_, tokenStructure_;
            int bodyTokens_ = 0, numberTokens_ = 0;
            QJsonArray sequence_; std::size_t sequenceBytes_ = 0; bool sequenceBounded_ = true, sequenceOverflow_ = false;
        };
        // The original token sequence walk. The single pass below stops after 256
        // tokens; this runs only if such a body still counts at most 256 hashed tokens
        // (zero-width tokens past the hashed range).
        void sequenceDigest(TSNode Body, ParseContext& Context, Digest& Result)
        {
            QCryptographicHash structure(QCryptographicHash::Sha256);
            Result.sequence_ = {}; Result.sequenceBytes_ = 0; Result.sequenceBounded_ = true; Result.sequenceOverflow_ = false;
            walk(Body, Context, [&](TSNode Child) {
                const QByteArrayView kind(ts_node_type(Child));
                if (kind == "comment") return false;
                if (Context.python_)
                {
                    int children = 0;
                    for (uint32_t i = 0; i < ts_node_child_count(Child); ++i)
                    { Context.check(); if (QByteArrayView(ts_node_type(ts_node_child(Child, i))) != "comment") ++children; }
                    addShape(structure, ts_node_is_named(Child) ? kind : QByteArrayView("<token>"), children);
                }
                if (!ts_node_child_count(Child) && Result.sequenceBounded_)
                {
                    const auto value = text(Child, Context); Result.sequenceBytes_ += std::size_t(value.size());
                    if (value.size() > 128 || Result.sequenceBytes_ > 8192) Result.sequenceBounded_ = false;
                    else Result.sequence_.append(QString::fromUtf8(value));
                }
                return true;
            });
            Result.tokenStructure_ = hex(structure);
        }
        // Rename-only digests (literal shape, token sequence, renamed signature) are
        // computed for rename candidates only; the cache keys on candidacy.
        Digest digest(TSNode Root, TSNode Body, TSNode Name, bool Candidate, ParseContext& Context)
        {
            Digest result;
            QCryptographicHash body(QCryptographicHash::Sha256), literal(QCryptographicHash::Sha256), structure(QCryptographicHash::Sha256);
            // Pass 0 body hash, 1 literal-insensitive shape, 2 token sequence and structure.
            // Token-count eligibility is decided when the function is emitted.
            const auto end = ts_node_end_byte(Body);
            // A sequence is emitted only for 32..256 bounded tokens; past that, pass 2 idles.
            bool sequencing = true;
            walkPasses<3>(Body, Context, [&](TSNode Child, std::array<bool, 3>& Active) {
                // Clearing a pass at the root disables it for the whole walk.
                if (!Candidate) Active[1] = Active[2] = false;
                const QByteArrayView kind(ts_node_type(Child));
                const bool comment = kind == "comment", hashed = !comment && ts_node_start_byte(Child) < end, leaf = !ts_node_child_count(Child);
                int children = -1;
                const auto count = [&] {
                    if (children < 0)
                    {
                        children = 0;
                        for (uint32_t i = 0; i < ts_node_child_count(Child); ++i)
                        { Context.check(); if (QByteArrayView(ts_node_type(ts_node_child(Child, i))) != "comment") ++children; }
                    }
                    return children;
                };
                QByteArray value; bool read = false;
                const auto token = [&]() -> const QByteArray& { if (!read) { value = text(Child, Context); read = true; } return value; };
                if (!hashed) Active[0] = Active[1] = false;
                if (comment) Active[2] = false;
                if (Active[0])
                {
                    if (Context.python_) addShape(body, kind, count());
                    if (leaf) { ++result.bodyTokens_; addLeaf(body, token()); }
                }
                if (Active[1])
                {
                    if (Context.python_) addShape(literal, kind, count());
                    const bool pythonNumber = Context.python_ && (kind == "integer" || kind == "float") && !token().endsWith('j') && !token().endsWith('J');
                    if (kind == "number_literal" || pythonNumber) { ++result.numberTokens_; literal.addData("N;"); Active[1] = false; }
                    else if (leaf) addLeaf(literal, token());
                }
                if (Active[2] && sequencing)
                {
                    if (Context.python_) addShape(structure, ts_node_is_named(Child) ? kind : QByteArrayView("<token>"), count());
                    if (leaf && result.sequence_.size() == 256) { result.sequenceOverflow_ = true; sequencing = false; }
                    else if (leaf)
                    {
                        result.sequenceBytes_ += std::size_t(token().size());
                        if (token().size() > 128 || result.sequenceBytes_ > 8192) { result.sequenceBounded_ = false; sequencing = false; }
                        else result.sequence_.append(QString::fromUtf8(token()));
                    }
                }
            });
            result.body_ = hex(body); result.literalShape_ = hex(literal); result.tokenStructure_ = hex(structure);
            // Pass 0 signature, 1 the signature with the declared name replaced.
            QCryptographicHash signature(QCryptographicHash::Sha256), rename(QCryptographicHash::Sha256);
            const auto stop = ts_node_start_byte(Body);
            walkPasses<2>(Root, Context, [&](TSNode Child, std::array<bool, 2>& Active) {
                if (!Candidate) Active[1] = false;
                if (Active[1] && !ts_node_is_null(Name) && ts_node_eq(Child, Name)) { rename.addData("<function-name>"); Active[1] = false; }
                const QByteArrayView kind(ts_node_type(Child));
                if (kind == "comment" || ts_node_start_byte(Child) >= stop) { Active[0] = Active[1] = false; return; }
                int children = -1;
                const auto count = [&] {
                    if (children < 0)
                    {
                        children = 0;
                        for (uint32_t i = 0; i < ts_node_child_count(Child); ++i)
                        { Context.check(); if (QByteArrayView(ts_node_type(ts_node_child(Child, i))) != "comment") ++children; }
                    }
                    return children;
                };
                const bool leaf = !ts_node_child_count(Child); const auto value = leaf ? text(Child, Context) : QByteArray{};
                for (std::size_t pass = 0; pass < 2; ++pass)
                {
                    if (!Active[pass]) continue;
                    auto& hash = pass ? rename : signature;
                    if (Context.python_) addShape(hash, kind, count());
                    if (leaf) addLeaf(hash, value);
                }
            });
            result.signature_ = hex(signature); result.renameSignature_ = hex(rename);
            return result;
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
        const TSLanguage* grammar(const QString& Language)
        { return Language == "python" ? tree_sitter_python() : Language == "c" ? tree_sitter_c() : tree_sitter_cpp(); }
        TSPoint advance(TSPoint Point, const char* Data, qsizetype Bytes)
        {
            const auto* last = static_cast<const char*>(nullptr);
            for (qsizetype i = 0; i < Bytes; ++i) if (Data[i] == '\n') { ++Point.row; last = Data + i; }
            Point.column = last ? uint32_t(Data + Bytes - last - 1) : Point.column + uint32_t(Bytes);
            return Point;
        }
    }
    struct SourceTree::State
    {
        struct Entry { uint32_t begin_ = 0, end_ = 0; QByteArray rootType_; bool candidate_ = false; Digest digest_; };
        TSTree* tree_ = nullptr;
        // Digests of the last analysis in current text coordinates, sorted by begin_.
        // Edit() removes the entries it touches and shifts the ones after it.
        std::vector<Entry> cache_;
        void Drop() { if (tree_) ts_tree_delete(tree_); tree_ = nullptr; cache_.clear(); }
        ~State() { Drop(); }
    };
    SourceTree::SourceTree(QString Language) : language_(std::move(Language)), state_(std::make_unique<State>())
    { if (language_ != "c" && language_ != "cpp" && language_ != "python") throw Failure("LanguageUnsupported"); }
    SourceTree::~SourceTree() = default;
    void SourceTree::Reset(QByteArray Text) { text_ = std::move(Text); state_->Drop(); }
    void SourceTree::Edit(qsizetype Begin, qsizetype End, const QByteArray& Bytes)
    {
        if (Begin < 0 || Begin > End || End > text_.size() || text_.size() - (End - Begin) + Bytes.size() > 16 * 1024 * 1024) throw Failure("SourceEditRange");
        const auto delta = Bytes.size() - (End - Begin);
        if (state_->tree_)
        {
            TSInputEdit edit{};
            edit.start_byte = uint32_t(Begin); edit.old_end_byte = uint32_t(End); edit.new_end_byte = uint32_t(Begin + Bytes.size());
            edit.start_point = advance({0, 0}, text_.constData(), Begin);
            edit.old_end_point = advance(edit.start_point, text_.constData() + Begin, End - Begin);
            edit.new_end_point = advance(edit.start_point, Bytes.constData(), Bytes.size());
            ts_tree_edit(state_->tree_, &edit);
            // Touching counts as overlap: an edit at a function boundary recomputes it.
            auto& cache = state_->cache_;
            cache.erase(std::remove_if(cache.begin(), cache.end(), [&](const State::Entry& Entry) {
                return qsizetype(Entry.end_) >= Begin && qsizetype(Entry.begin_) <= End; }), cache.end());
            for (auto& entry : cache) if (qsizetype(entry.begin_) > End) { entry.begin_ = uint32_t(entry.begin_ + delta); entry.end_ = uint32_t(entry.end_ + delta); }
        }
        text_.replace(Begin, End - Begin, Bytes);
    }
    QJsonObject SourceTree::Analyze(std::chrono::steady_clock::time_point Deadline, const std::atomic_bool* Cancel)
    {
        reused_ = 0;
        if (text_.size() > 16 * 1024 * 1024 || text_.contains('\0')) { state_->Drop(); throw Failure("InputLimit"); }
        ParseContext context; context.python_ = language_ == "python"; context.source_ = text_; context.deadline_ = Deadline; context.cancel_ = Cancel;
        try
        {
            std::unique_ptr<TSParser, decltype(&ts_parser_delete)> parser(ts_parser_new(), ts_parser_delete);
            if (!parser || !ts_parser_set_language(parser.get(), grammar(language_))) throw Failure("LanguageAbi");
            TSInput input{}; input.payload = &context; input.read = readInput; input.encoding = TSInputEncodingUTF8;
            TSParseOptions options{}; options.payload = &context; options.progress_callback = stopParsing;
            auto* tree = ts_parser_parse_with_options(parser.get(), state_->tree_, input, options);
            if (!tree) throw Failure(Cancel && Cancel->load() ? "Canceled" : "TimeLimit");
            std::vector<TSRange> changed;
            if (state_->tree_)
            {
                uint32_t count = 0; auto* ranges = ts_tree_get_changed_ranges(state_->tree_, tree, &count);
                changed.assign(ranges, ranges + count); std::free(ranges); ts_tree_delete(state_->tree_);
            }
            state_->tree_ = tree;
            const auto root = ts_tree_root_node(tree);
            // Every pass visits each node about once; Python shape counting adds one more visit per child.
            context.nodeLimit_ = std::clamp<std::size_t>(std::size_t(ts_node_descendant_count(root)) * 4, 2'000'000, 64'000'000);
            const auto syntaxChanged = [&](uint32_t Begin, uint32_t End) {
                return std::any_of(changed.begin(), changed.end(), [&](const TSRange& Range) { return Range.start_byte <= End && Range.end_byte >= Begin; });
            };
            std::vector<State::Entry> cache; auto previous = std::move(state_->cache_); state_->cache_.clear();
            QJsonArray functions; int skipped = 0;
            walk(root, context, [&](TSNode node)
            {
                if (QByteArrayView(ts_node_type(node)) != "function_definition") return true;
                if (ts_node_has_error(node)) { ++skipped; return false; }
                const auto body = ts_node_child_by_field_name(node, "body", 4);
                const auto declarator = context.python_ ? ts_node_child_by_field_name(node, "name", 4) : ts_node_child_by_field_name(node, "declarator", 10);
                const auto name = functionName(declarator, context);
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
                        auto scope = ts_node_is_null(scopeNode) ? QString("(anonymous)") : QString::fromUtf8(text(scopeNode, context));
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
                if (context.python_ && !ts_node_is_null(enclosing) && QByteArrayView(ts_node_type(enclosing)) == "decorated_definition") signatureRoot = enclosing;
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
                const bool ordinary = !context.python_ && !ts_node_is_null(nameNode) &&
                    (QByteArrayView(ts_node_type(nameNode)) == "identifier" || QByteArrayView(ts_node_type(nameNode)) == "field_identifier") &&
                    !ts_node_is_null(ts_node_child_by_field_name(node, "type", 4));
                // Python decorators and special methods can depend on the declared name.
                // Only bare ordinary functions use structural rename matching.
                const bool pythonRename = context.python_ && !ts_node_is_null(nameNode) &&
                    ts_node_eq(signatureRoot, node) && !(name.startsWith("__") && name.endsWith("__"));
                const auto begin = ts_node_start_byte(signatureRoot), end = ts_node_end_byte(node);
                const QByteArray rootType(ts_node_type(signatureRoot));
                Digest value;
                const auto at = std::lower_bound(previous.begin(), previous.end(), begin, [](const State::Entry& Entry, uint32_t Begin) { return Entry.begin_ < Begin; });
                const bool candidate = ordinary || pythonRename;
                if (at != previous.end() && at->begin_ == begin && at->end_ == end && at->rootType_ == rootType && at->candidate_ == candidate && !syntaxChanged(begin, end))
                { value = std::move(at->digest_); ++reused_; }
                else value = digest(signatureRoot, body, nameNode, candidate, context);
                const bool literal = candidate && value.bodyTokens_ >= 24 && value.bodyTokens_ <= 4096;
                // The 512 KiB token budget is shared by the functions in document order.
                QJsonArray bodySequence;
                if (candidate && value.bodyTokens_ >= 32 && value.bodyTokens_ <= 256 && context.tokenBytes_ < 512 * 1024)
                {
                    if (value.sequenceOverflow_) sequenceDigest(body, context, value);
                    if (value.sequenceBounded_ && context.tokenBytes_ + value.sequenceBytes_ <= 512 * 1024)
                    { bodySequence = value.sequence_; context.tokenBytes_ += value.sequenceBytes_; }
                }
                const auto separator = qualified.lastIndexOf("::");
                functions.append(QJsonObject{{"name", qualified}, {"scope", separator < 0 ? QString{} : qualified.left(separator)},
                    {"signature", value.signature_}, {"body", value.body_}, {"bodyTokens", value.bodyTokens_},
                    {"literalShape", literal ? value.literalShape_ : QString{}}, {"numberTokens", literal ? value.numberTokens_ : 0}, {"bodySequence", bodySequence},
                    {"tokenStructure", context.python_ && !bodySequence.isEmpty() ? value.tokenStructure_ : QString{}},
                    {"renameSignature", candidate ? value.renameSignature_ : QString{}},
                    {"begin", double(begin)}, {"end", double(end)},
                    {"nameBegin", ts_node_is_null(nameNode) ? -1.0 : double(ts_node_start_byte(nameNode))},
                    {"nameEnd", ts_node_is_null(nameNode) ? -1.0 : double(ts_node_end_byte(nameNode))},
                    {"line", int(ts_node_start_point(signatureRoot).row)}, {"endLine", int(ts_node_end_point(node).row)}});
                cache.push_back({begin, end, rootType, candidate, std::move(value)});
                return false;
            });
            state_->cache_ = std::move(cache);
            return {{"status", ts_node_has_error(root) || skipped ? "Partial" : "Complete"}, {"functions", functions}, {"skipped", skipped},
                {"syntaxErrors", ts_node_has_error(root)}};
        }
        catch (...) { state_->Drop(); throw; }
    }
    QJsonObject SemanticWorkerRequest(const QJsonObject& Payload)
    {
        const auto language = Payload["language"].toString();
        if (language != "c" && language != "cpp" && language != "python") throw Failure("LanguageUnsupported");
        // Older hosts omit the deadline; their worker timeout leaves room for 1.5 s of parsing.
        const auto deadline = Payload.contains("deadlineMs") ? Payload["deadlineMs"].toInt(-1) : 1500;
        if (deadline < 1 || deadline > 30000) throw Failure("WorkerPolicy");
        QByteArray source;
        {
            const auto encoded = Payload["source"].toString();
            if (encoded.size() > 24 * 1024 * 1024) throw Failure("InputLimit");
            auto decoded = QByteArray::fromBase64Encoding(encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            if (!decoded) throw Failure("InvalidInputEncoding");
            source = std::move(decoded.decoded);
        }
        if (source.size() > 16 * 1024 * 1024 || source.contains('\0')) throw Failure("InputLimit");
        SourceTree tree(language); tree.Reset(std::move(source));
        auto result = tree.Analyze(std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline)); result["language"] = language;
        result["parserVersion"] = sourceParserVersion;
        return result;
    }
}
