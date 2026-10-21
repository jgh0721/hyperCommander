#pragma once
#include <QString>
#include <QRegularExpression>

namespace hc::plugins {
enum class ArchiveVolumeKind { None, Numbered, Zip, Zipx, Rar };
struct ArchiveVolumeName {
    ArchiveVolumeKind kind_ = ArchiveVolumeKind::None;
    QString firstName_, alternateName_;
    unsigned number_ = 1;
};
// Candidate selection only. The host validates the input and its set membership.
inline ArchiveVolumeName ParseArchiveVolumeName(const QString& Name) {
    static const QRegularExpression Numbered("^(.+)\\.([0-9]{3,9})$");
    static const QRegularExpression Zipx("^(.+)\\.zx([0-9]{2,9})$", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression Zip("^(.+)\\.z([0-9]{2,9})$", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression Rar("^(.+\\.part)([0-9]{1,9})(\\.rar)$", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression OldRar("^(.+)\\.[r-y][0-9]{2}$", QRegularExpression::CaseInsensitiveOption);
    for (const auto* Pattern : {&Numbered, &Zipx, &Zip, &Rar}) {
        const auto Match = Pattern->match(Name); if (!Match.hasMatch()) continue;
        const auto Stem = Match.captured(1);
        const auto Digits = Match.captured(2);
        bool Valid = false; const auto Number = Digits.toUInt(&Valid); if (!Valid) return {};
        if (!Number) return Pattern == &Zip && Digits == "00" ? ArchiveVolumeName{ArchiveVolumeKind::Rar, Stem + ".rar", {}, 1} : ArchiveVolumeName{};
        if (Pattern == &Numbered) {
            const auto Padded = Stem + '.' + QString::number(1).rightJustified(Digits.size(), '0');
            // 7-Zip expands .999 to .1000 without changing .001. A separately
            // padded .0001 set is also valid; the host resolves its sibling.
            if (Digits.size() > 3 && !Digits.startsWith('0')) return {ArchiveVolumeKind::Numbered, Stem + ".001", Padded, Number};
            return {ArchiveVolumeKind::Numbered, Padded, {}, Number};
        }
        if (Pattern == &Zipx) return {ArchiveVolumeKind::Zipx, Stem + ".zipx", {}, Number};
        if (Pattern == &Zip) return {ArchiveVolumeKind::Zip, Stem + ".zip", Match.capturedLength(2) == 2 ? Stem + ".rar" : QString{}, Number};
        return {ArchiveVolumeKind::Rar, Stem + QString::number(1).rightJustified(Match.capturedLength(2), '0') + Match.captured(3), {}, Number};
    }
    const auto Match = OldRar.match(Name);
    if (Match.hasMatch()) return {ArchiveVolumeKind::Rar, Match.captured(1) + ".rar", {}, 1};
    return {};
}
inline bool IsArchiveVolumeName(const QString& Name) { return ParseArchiveVolumeName(Name).kind_ != ArchiveVolumeKind::None; }
}
