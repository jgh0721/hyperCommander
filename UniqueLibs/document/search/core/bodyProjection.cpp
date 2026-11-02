#include "stdafx.h"
#include "bodyProjection.hpp"

/// 공통 라이브러리
#include "commonLibs/document/epub/epubSourceLines.hpp"
#include "commonLibs/document/epub/epubArchive.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QXmlStreamReader>

namespace hc::document::search {
BodyProjection::BodyProjection(QString Source) : source_(std::move(Source)) {
    QXmlStreamReader Reader(source_); epub::detail::XhtmlEntities Entities; Entities.enabled_ = true; Reader.setEntityResolver(&Entities);
    while (!Reader.atEnd()) {
        Reader.readNext();
        if (Reader.isDTD() && !Reader.entityDeclarations().isEmpty()) { error_ = QStringLiteral("본문 매핑에서 사용자 정의 entity를 지원하지 않습니다."); return; }
    }
    if (Reader.hasError()) { error_ = Reader.errorString(); return; }
    struct Element { QString name_; bool body_ = false, hidden_ = false; };
    QVector<Element> Stack;
    const QSet<QString> Blocks{"p","div","section","article","header","footer","aside","blockquote","li","ul","ol","dl","dt","dd","h1","h2","h3","h4","h5","h6","table","tr","td","th","pre","br","hr","rt","rp","ruby"};
    const QRegularExpression Name("^<\\s*/?\\s*([\\w:-]+)");
    const QRegularExpression Hidden("(?:\\shidden(?:\\s|=|/?>)|aria-hidden\\s*=\\s*['\"]true['\"]|style\\s*=\\s*['\"][^'\"]*(?:display\\s*:\\s*none|visibility\\s*:\\s*hidden))", QRegularExpression::CaseInsensitiveOption);
    const auto Visible = [&] { return !Stack.isEmpty() && Stack.last().body_ && !Stack.last().hidden_; };
    const bool Ok = epub::detail::ScanMarkup(source_.constData(), source_.size(), [&](const epub::detail::MarkupToken& Token) {
        using TokenType = epub::detail::MarkupToken;
        if (Token.kind_ == TokenType::Text || Token.kind_ == TokenType::CData) {
            if (Visible()) text(Token.begin_, Token.end_, Token.kind_ == TokenType::CData); return error_.isEmpty();
        }
        if (Token.kind_ == TokenType::End) {
            if (Stack.isEmpty()) return false;
            if (Visible() && Blocks.contains(Stack.last().name_)) boundary(); Stack.removeLast(); return true;
        }
        if (Token.kind_ != TokenType::Start && Token.kind_ != TokenType::Empty) return true;
        const auto Tag = source_.mid(Token.begin_, Token.end_ - Token.begin_);
        const auto Matched = Name.match(Tag); if (!Matched.hasMatch()) return false;
        const auto TagName = Matched.captured(1).section(':', -1).toLower();
        Element Item{TagName, TagName == "body" || (!Stack.isEmpty() && Stack.last().body_),
            (!Stack.isEmpty() && Stack.last().hidden_) || QStringList{"script","style","head"}.contains(TagName) || Hidden.match(Tag).hasMatch()};
        if (Item.body_ && !Item.hidden_ && Blocks.contains(TagName)) boundary();
        if (Token.kind_ == TokenType::Start) Stack.append(Item);
        return true;
    });
    if (!Ok || !Stack.isEmpty()) error_ = QStringLiteral("XHTML 본문 경계를 매핑할 수 없습니다.");
}
void BodyProjection::boundary() {
    if (text_.isEmpty() || text_.endsWith('\n')) return;
    segments_.append({{text_.size(), 1}, {-1, 0}, true}); text_ += '\n';
}
void BodyProjection::text(qint64 Begin, qint64 End, bool Cdata) {
    for (auto At = Begin; At < End;) {
        const auto Start = At; QString Decoded; bool Atomic = false;
        if (!Cdata && source_[At] == '&') {
            const auto Semi = source_.indexOf(';', At + 1);
            if (Semi < 0 || Semi >= End) { error_ = QStringLiteral("잘못된 entity 경계입니다."); return; }
            const auto Entity = source_.mid(At + 1, Semi - At - 1);
            if (Entity.startsWith('#')) {
                const bool Hex = Entity.startsWith("#x", Qt::CaseInsensitive); bool Valid = false;
                const auto Point = Entity.mid(Hex ? 2 : 1).toUInt(&Valid, Hex ? 16 : 10);
                if (!Valid || Point > 0x10ffff || (Point >= 0xd800 && Point <= 0xdfff)) { error_ = QStringLiteral("잘못된 문자 참조입니다."); return; }
                const char32_t Character = Point; Decoded = QString::fromUcs4(&Character, 1);
            } else {
                const QMap<QString, QString> Entities{{"amp","&"},{"lt","<"},{"gt",">"},{"quot","\""},{"apos","'"},{"nbsp",QString(QChar(0xa0))}};
                Decoded = Entities.value(Entity);
                if (Decoded.isEmpty()) { error_ = QStringLiteral("지원하지 않는 entity입니다."); return; }
            }
            At = Semi + 1; Atomic = true;
        } else if (source_[At] == '\r') {
            ++At; if (At < End && source_[At] == '\n') ++At; Decoded = "\n"; Atomic = true;
        } else {
            while (At < End && source_[At] != '\r' && (Cdata || source_[At] != '&')) ++At;
            Decoded = source_.mid(Start, At - Start);
        }
        segments_.append({{text_.size(), Decoded.size()}, Cdata ? Range{-1, 0} : Range{Start, At - Start}, Atomic}); text_ += Decoded;
    }
}
std::optional<QVector<Range>> BodyProjection::Map(Range BodyRange) const {
    if (!error_.isEmpty() || !BodyRange.Valid(text_.size())) return {};
    const auto Boundary = [this](qint64 Offset) { return Offset == 0 || Offset == text_.size() || !(text_[Offset - 1].isHighSurrogate() && text_[Offset].isLowSurrogate()); };
    if (!Boundary(BodyRange.start_) || !Boundary(BodyRange.start_ + BodyRange.length_)) return {};
    QVector<Range> Result; qint64 Covered = 0;
    auto It = std::lower_bound(segments_.begin(), segments_.end(), BodyRange.start_, [](const Segment& Segment, qint64 Position) { return Segment.body_.start_ + Segment.body_.length_ < Position; });
    for (; It != segments_.end(); ++It) {
        const auto& Segment = *It;
        const auto Start = qMax(BodyRange.start_, Segment.body_.start_);
        const auto End = qMin(BodyRange.start_ + BodyRange.length_, Segment.body_.start_ + Segment.body_.length_);
        if (Start > End) { if (Segment.body_.start_ > BodyRange.start_ + BodyRange.length_) break; continue; }
        if (Start == End && BodyRange.length_) continue;
        if (Segment.source_.start_ < 0) return {};
        if (Segment.atomic_ && (Start != Segment.body_.start_ || End != Segment.body_.start_ + Segment.body_.length_)) {
            if (!BodyRange.length_ && (Start == Segment.body_.start_ || Start == Segment.body_.start_ + Segment.body_.length_)) {
                Result.append({Start == Segment.body_.start_ ? Segment.source_.start_ : Segment.source_.start_ + Segment.source_.length_, 0}); return Result;
            }
            return {};
        }
        Range Mapped = Segment.atomic_ ? Segment.source_ : Range{Segment.source_.start_ + Start - Segment.body_.start_, End - Start};
        if (!Result.isEmpty() && Result.last().start_ + Result.last().length_ == Mapped.start_) Result.last().length_ += Mapped.length_;
        else Result.append(Mapped);
        Covered += End - Start;
        if (!BodyRange.length_ || Covered == BodyRange.length_) break;
    }
    return Covered == BodyRange.length_ && !Result.isEmpty() ? std::optional<QVector<Range>>(Result) : std::nullopt;
}
QString BodyProjection::Escape(QString Text) { return Text.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;"); }
}
