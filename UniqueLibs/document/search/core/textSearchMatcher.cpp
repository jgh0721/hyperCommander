#include "stdafx.h"
#include "textSearchMatcher.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )

namespace hc::document::search {
TextMap::TextMap(QString Text) : original_(std::move(Text))
{
    // Most source trees use LF. Share that string instead of appending every
    // UTF-16 unit; only CR boundaries need an offset-map entry.
    qsizetype At=original_.indexOf('\r'),Begin=0;
    if(At<0)normalized_=original_;
    else {
        normalized_.reserve(original_.size());
        do {
            normalized_.append(QStringView(original_).mid(Begin,At-Begin));
            Begin=At+1;
            if(Begin<original_.size() && original_[Begin]=='\n'){removed_.append(normalized_.size());++Begin;}
            normalized_.append('\n');At=original_.indexOf('\r',Begin);
        }while(At>=0);
        normalized_.append(QStringView(original_).mid(Begin));
    }
    for(At=normalized_.indexOf('\n');At>=0;At=normalized_.indexOf('\n',At+1))lines_.append(At+1);
}
Range TextMap::Source(Range Value) const
{
    if (!Value.Valid(normalized_.size())) return {};
    const auto Offset = [this](qint64 P) { return P + std::distance(removed_.begin(), std::lower_bound(removed_.begin(), removed_.end(), P)); };
    const auto Start = Offset(Value.start_);
    return {Start, Offset(Value.start_ + Value.length_) - Start};
}
qint64 TextMap::Line(qint64 Position) const { return std::upper_bound(lines_.begin(), lines_.end(), Position) - lines_.begin(); }
qint64 TextMap::Column(qint64 Position) const { const auto Index = Line(Position) - 1; return Position - lines_.value(Index) + 1; }
Matcher::Matcher(QString Query, Options Settings) : empty_(Query.isEmpty())
{
    Query.replace("\r\n", "\n").replace('\r', '\n');
    expression_.setPattern(Settings.regex_ ? Query : QRegularExpression::escape(Query));
    auto Flags = QRegularExpression::UseUnicodePropertiesOption | QRegularExpression::MultilineOption;
    if (!Settings.caseSensitive_) Flags |= QRegularExpression::CaseInsensitiveOption;
    expression_.setPatternOptions(Flags);
}
QString Matcher::Error() const { return expression_.isValid() ? runtimeError_ : expression_.errorString(); }
void Matcher::Start(const TextMap& Text) { runtimeError_.clear(); text_ = &Text; iterator_ = expression_.globalMatch(Text.Normalized()); }
std::optional<Match> Matcher::Next()
{
    if (empty_ || !text_) return {};
    if (!iterator_.isValid()) { runtimeError_ = QStringLiteral("정규식 실행에 실패했습니다."); return {}; }
    if (!iterator_.hasNext()) return {};
    const auto Found = iterator_.next();
    Match Result;
    Result.range_ = text_->Source({Found.capturedStart(), Found.capturedLength()});
    Result.line_ = text_->Line(Found.capturedStart()); Result.column_ = text_->Column(Found.capturedStart());
    for (int I = 0; I <= expression_.captureCount(); ++I)
        Result.captures_.append(text_->Source({Found.capturedStart(I), Found.capturedLength(I)}));
    return Result;
}
Replacement::Replacement(QString Value, bool Regex, const QStringList& Groups)
{
    if (!Regex) { parts_.append({std::move(Value), -1}); return; }
    QString Literal;
    const auto Flush = [&] { if (!Literal.isEmpty()) { parts_.append({Literal, -1}); Literal.clear(); } };
    for (qsizetype I = 0; I < Value.size(); ++I) {
        const auto Ch = Value[I];
        if (Ch == '\\') {
            if (++I == Value.size()) { error_ = QStringLiteral("치환문의 끝에 단독 역슬래시가 있습니다."); break; }
            if (Value[I] == 'n') Literal += '\n';
            else if (Value[I] == 't') Literal += '\t';
            else if (Value[I] == '\\') Literal += '\\';
            else { error_ = QStringLiteral("지원하지 않는 치환 escape입니다."); break; }
        } else if (Ch == '$') {
            if (++I == Value.size()) { error_ = QStringLiteral("치환문의 그룹 참조가 불완전합니다."); break; }
            if (Value[I] == '$') { Literal += '$'; continue; }
            int Group = -1;
            if (Value[I] == '&') Group = 0;
            else if (Value[I] >= '1' && Value[I] <= '9') {
                Group = Value[I].digitValue();
                if (I + 1 < Value.size() && Value[I + 1] >= '0' && Value[I + 1] <= '9') Group = Group * 10 + Value[++I].digitValue();
            } else if (Value[I] == '<') {
                const auto End = Value.indexOf('>', I + 1);
                if (End >= 0) { const auto Name = Value.mid(I + 1, End - I - 1); if (!Name.isEmpty()) Group = int(Groups.indexOf(Name)); I = End; }
            }
            if (Group < 0 || Group >= Groups.size()) { error_ = QStringLiteral("존재하지 않거나 잘못된 캡처 그룹입니다."); break; }
            Flush(); parts_.append({{}, Group});
        } else Literal += Ch;
    }
    Flush();
}
std::optional<QString> Replacement::Expand(const QString& Original, const Match& Hit, const QString& Newline) const
{
    if (!error_.isEmpty() || !Hit.range_.Valid(Original.size())) return {};
    if (Newline != "\n" && Newline != "\r\n" && Newline != "\r") return {};
    QString Result;
    for (const auto& Item : parts_) {
        if (Item.group_ < 0) {
            auto Literal = Item.text_;
            Literal.replace("\r\n", "\n").replace('\r', '\n');
            if (Newline != "\n") Literal.replace(QStringLiteral("\n"), Newline);
            Result += Literal; continue;
        }
        if (Item.group_ >= Hit.captures_.size()) return {};
        const auto Capture = Hit.captures_[Item.group_];
        if (Capture.start_ == -1 && Capture.length_ == 0) continue;
        if (!Capture.Valid(Original.size())) return {};
        Result += Original.mid(Capture.start_, Capture.length_);
    }
    return Result;
}
ReplacementPlan::ReplacementPlan(QString Original, QVector<Match> Hits, const Replacement& Replace, const QString& Newline)
    : before_(std::move(Original))
{
    if (!Replace.Error().isEmpty()) { error_ = Replace.Error(); return; }
    const auto Boundary = [this](qint64 Offset) {
        if (Offset <= 0 || Offset >= before_.size()) return true;
        return !(before_[Offset - 1].isHighSurrogate() && before_[Offset].isLowSurrogate())
            && !(before_[Offset - 1] == '\r' && before_[Offset] == '\n');
    };
    std::stable_sort(Hits.begin(), Hits.end(), [](const Match& A, const Match& B) {
        if (A.range_.start_ != B.range_.start_) return A.range_.start_ < B.range_.start_;
        return A.range_.length_ < B.range_.length_;
    });
    qint64 End = 0;
    Range Previous;
    QString Prepared;
    for (const auto& Hit : Hits) {
        const auto Span = Hit.range_;
        if (!Span.Valid(before_.size()) || Span.start_ < End ||
            (Span.start_ == Previous.start_ && Span.length_ == Previous.length_) ||
            !Boundary(Span.start_) || !Boundary(Span.start_ + Span.length_)) {
            error_ = QStringLiteral("치환 범위가 겹치거나 원문 경계와 일치하지 않습니다."); return;
        }
        const auto Expanded = Replace.Expand(before_, Hit, Newline);
        if (!Expanded) { error_ = QStringLiteral("치환문을 확장할 수 없습니다."); return; }
        Prepared += before_.mid(End, Span.start_ - End);
        Prepared += *Expanded;
        End = Span.start_ + Span.length_; Previous = Span;
    }
    Prepared += before_.mid(End);
    after_ = std::move(Prepared);
}
std::optional<QString> ReplacementPlan::ApplyTo(const QString& Current) const
{
    if (!error_.isEmpty() || Current != before_) return {};
    return after_;
}
}
