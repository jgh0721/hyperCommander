#include "qtAutomation.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QObject>
#include <cstring>

namespace hc::automation {
namespace {
struct ParseFailure { qsizetype column_; QString message_; };
bool Letter(QChar C) { return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || C == '_'; }
bool Digit(QChar C) { return C >= '0' && C <= '9'; }
class Parser {
public:
    explicit Parser(QString Text) : text_(std::move(Text)) {}
    void Space() { while (position_ < text_.size() && text_[position_].isSpace()) ++position_; }
    bool Take(const QString& Token) { Space(); if (text_.mid(position_, Token.size()) != Token) return false; position_ += Token.size(); return true; }
    bool Word(const QString& Token) {
        Space(); const auto End = position_ + Token.size();
        if (text_.mid(position_, Token.size()) != Token || (End < text_.size() && (Letter(text_[End]) || Digit(text_[End])))) return false;
        position_ = End; return true;
    }
    [[noreturn]] void Fail(const QString& Message) const { throw ParseFailure{position_ + 1, Message}; }
    void Require(const QString& Token) { if (!Take(Token)) Fail(QObject::tr("필요한 기호: %1").arg(Token)); }
    bool End() { Space(); return position_ == text_.size(); }
    QString String() {
        Space(); const auto Start = position_; Require("\""); bool Escape = false, Closed = false;
        while (position_ < text_.size()) {
            const auto C = text_[position_++];
            if (!Escape && C == '"') { Closed = true; break; }
            if (!Escape && C == '\\') Escape = true; else Escape = false;
        }
        if (!Closed) Fail(QObject::tr("문자열이 닫히지 않았습니다"));
        QJsonParseError Error; const auto Doc = QJsonDocument::fromJson(('[' + text_.mid(Start, position_ - Start) + ']').toUtf8(), &Error);
        if (Error.error != QJsonParseError::NoError || !Doc[0].isString() || Doc[0].toString().contains(QChar::Null)) Fail(QObject::tr("JSON 문자열 오류"));
        return Doc[0].toString();
    }
    std::string Name(bool Dotted = false) {
        Space(); const auto Start = position_; bool NeedLetter = true;
        while (position_ < text_.size()) {
            const auto C = text_[position_];
            if (Letter(C) || (!NeedLetter && Digit(C))) { NeedLetter = false; ++position_; }
            else if (Dotted && C == '.' && !NeedLetter) { NeedLetter = true; ++position_; }
            else break;
        }
        if (position_ == Start || NeedLetter || position_ - Start > 128) Fail(QObject::tr("변수 이름 오류"));
        const auto Name = text_.mid(Start, position_ - Start).toStdString();
        if (!Dotted && (Name == "result" || Name == "context")) Fail(QObject::tr("예약된 변수 이름입니다"));
        return Name;
    }
    ExpressionPtr Expr(int Minimum = 1, int Depth = 0) {
        if (Depth > 32 || ++nodes_ > 256) Fail(QObject::tr("수식 복잡도 제한 초과"));
        Space(); auto Node = std::make_shared<Expression>();
        if (Take("!")) { Node->kind_ = ExpressionKind::Not; Node->left_ = Expr(5, Depth + 1); }
        else if (Take("(")) { auto Inner = Expr(1, Depth + 1); Require(")"); Node = std::make_shared<Expression>(*Inner); }
        else if (Take("$")) { Node->kind_ = ExpressionKind::Variable; Node->variable_ = Name(true); }
        else if (position_ < text_.size() && text_[position_] == '"') Node->literal_ = String().toStdString();
        else if (Word("true")) Node->literal_ = true;
        else if (Word("false")) Node->literal_ = false;
        else {
            const auto Start = position_; Take("-"); while (position_ < text_.size() && Digit(text_[position_])) ++position_;
            const auto Token = text_.mid(Start, position_ - Start); bool Ok = false; const auto Number = Token.toLongLong(&Ok);
            const auto Digits = Token.startsWith('-') ? Token.mid(1) : Token;
            if (!Ok || (Digits.size() > 1 && Digits.startsWith('0'))) Fail(QObject::tr("문자열·정수·불리언·변수 수식이 필요합니다"));
            Node->literal_ = std::int64_t(Number);
        }
        struct Operator { const char* text_; int precedence_; ExpressionKind kind_; };
        const Operator Operators[]{{"||",1,ExpressionKind::Or},{"&&",2,ExpressionKind::And},{"==",3,ExpressionKind::Equal},
            {"!=",3,ExpressionKind::NotEqual},{"<=",4,ExpressionKind::LessEqual},{">=",4,ExpressionKind::GreaterEqual},
            {"<",4,ExpressionKind::Less},{">",4,ExpressionKind::Greater}};
        for (;;) {
            Space(); const Operator* Found = nullptr;
            for (const auto& Operator : Operators) if (text_.mid(position_, int(strlen(Operator.text_))) == Operator.text_) { Found = &Operator; break; }
            if (!Found || Found->precedence_ < Minimum) break;
            Take(Found->text_); auto Combined = std::make_shared<Expression>(); Combined->kind_ = Found->kind_; Combined->left_ = Node;
            Combined->right_ = Expr(Found->precedence_ + 1, Depth + 1); Node = std::move(Combined);
        }
        return Node;
    }
private:
    QString text_;
    qsizetype position_ = 0;
    int nodes_ = 0;
};
}
Result<Program> Compile(const QString& Source, Diagnostic* Details, SchemaLookup Schema)
{
    if (Details) *Details = {};
    qsizetype Line = 1;
    try {
        if (Source.size() > 65536 || Source.contains(QChar::Null)) throw ParseFailure{1, QObject::tr("소스 길이 제한 또는 NUL 문자 오류")};
        Program Result; struct Block { std::size_t branch_; std::optional<std::size_t> else_; };
        std::vector<Block> Blocks; bool Version = false, Policy = false;
        const auto Lines = Source.split('\n');
        for (const auto& Text : Lines) {
            if (Text.trimmed().isEmpty() || Text.trimmed().startsWith('#')) { ++Line; continue; }
            Parser P(Text); Step Step; Step.line_ = Line;
            if (P.Word("version")) {
                if (Version || Policy || !Result.empty()) P.Fail(QObject::tr("version은 첫 지시문이어야 합니다"));
                const auto E = P.Expr(); if (E->kind_ != ExpressionKind::Literal || E->literal_ != Value(std::int64_t(1))) P.Fail(QObject::tr("지원하는 DSL 버전은 1입니다"));
                Version = true;
            } else if (P.Word("onerror")) {
                if (Policy || !Result.empty()) P.Fail(QObject::tr("onerror는 실행문 전에 한 번 지정합니다"));
                if (P.Word("continue")) Result.failurePolicy_ = FailurePolicy::Continue;
                else if (!P.Word("stop")) P.Fail(QObject::tr("stop 또는 continue가 필요합니다"));
                Policy = true;
            } else if (P.Word("let")) {
                Step.kind_ = StepKind::Assign; Step.binding_ = P.Name(); P.Require("="); Step.expression_ = P.Expr(); Result.push_back(std::move(Step));
            } else if (P.Word("if")) {
                if (Blocks.size() >= 16) P.Fail(QObject::tr("조건문 깊이 제한 초과"));
                Step.kind_ = StepKind::Branch; Step.expression_ = P.Expr(); Blocks.push_back({Result.size(), {}}); Result.push_back(std::move(Step));
            } else if (P.Word("else")) {
                if (Blocks.empty() || Blocks.back().else_) P.Fail(QObject::tr("대응하는 if가 없습니다"));
                Result[Blocks.back().branch_].target_ = Result.size() + 1;
                Blocks.back().else_ = Result.size(); Step.kind_ = StepKind::Jump; Result.push_back(std::move(Step));
            } else if (P.Word("end")) {
                if (Blocks.empty()) P.Fail(QObject::tr("대응하는 if가 없습니다"));
                Result[Blocks.back().else_.value_or(Blocks.back().branch_)].target_ = Result.size(); Blocks.pop_back();
            } else {
                const auto Group = Result.size() + 1; bool Pipeline = false;
                for (;;) {
                    Step = {}; Step.line_ = Line; P.Word("await");
                    if (!P.Word("invoke")) P.Fail(QObject::tr("invoke 명령이 필요합니다"));
                    Step.command_ = P.String().toStdString(); if (Step.command_.empty() || Step.command_.size() > 128) P.Fail(QObject::tr("명령 이름 오류"));
                    P.Require("{");
                    if (!P.Take("}")) for (;;) {
                        const auto Key = P.String().toStdString(); P.Require(":"); auto Expression = P.Expr();
                        if (Key.empty() || !Step.arguments_.emplace(Key, Expression).second || Step.arguments_.size() > 64) P.Fail(QObject::tr("인자 중복 또는 개수 제한 초과"));
                        if (Expression->kind_ == ExpressionKind::Literal) Step.parameters_[Key] = Expression->literal_;
                        if (P.Take("}")) break; P.Require(",");
                    }
                    if (P.Word("as")) Step.binding_ = P.Name();
                    if (Schema) {
                        auto Descriptor = Schema(Step.command_); if (!Descriptor.HasValue()) P.Fail(QString::fromStdString(Descriptor.Failure().code_));
                        for (const auto& [Key, Spec] : Descriptor.Value().parameters_)
                            if (Spec.required_ && !Step.arguments_.contains(Key)) P.Fail(QObject::tr("필수 인자 누락: %1").arg(QString::fromStdString(Key)));
                        for (const auto& [Key, Expression] : Step.arguments_) {
                            const auto Found = Descriptor.Value().parameters_.find(Key);
                            if (Found == Descriptor.Value().parameters_.end() || (Expression->kind_ == ExpressionKind::Literal && Expression->literal_.index() != std::size_t(Found->second.type_)))
                                P.Fail(QObject::tr("인자 이름 또는 타입 오류: %1").arg(QString::fromStdString(Key)));
                        }
                    }
                    const bool More = P.Take("|>"); Pipeline = Pipeline || More;
                    if (Pipeline) Step.pipeline_ = Group;
                    Result.push_back(std::move(Step)); if (Result.size() > 256) P.Fail(QObject::tr("실행 계획은 최대 256개 항목입니다"));
                    if (!More) break;
                }
            }
            if (!P.End()) P.Fail(QObject::tr("문장 뒤에 해석할 수 없는 내용이 있습니다"));
            if (Result.size() > 256) P.Fail(QObject::tr("실행 계획은 최대 256개 항목입니다"));
            ++Line;
        }
        if (!Blocks.empty()) throw ParseFailure{1, QObject::tr("조건문 end가 없습니다")};
        if (Result.empty()) throw ParseFailure{1, QObject::tr("실행할 명령이 없습니다")};
        return Result;
    } catch (const ParseFailure& Error) {
        if (Details) *Details = {Line, Error.column_, Error.message_};
        return storage::Error{ErrorCategory::InvalidRequest, "automation-syntax:" + std::to_string(Line) + ":" + std::to_string(Error.column_)};
    }
}
}
