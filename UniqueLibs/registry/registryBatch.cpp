#include "stdafx.h"
#include "registryBatch.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/system/registryValue.hpp"
#include "commonLibs/columns/fieldCondition.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDateTime>
#include <QUuid>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryBatch, "hc.registry.batch")
namespace hc::registry {
RegistryBatchPreview RegistryBatch::Replace(const QList<RegistryResultRow>& Rows,
    const RegistrySearchOptions& Options, const QString& Replacement, const QString& Sid, const QString& Machine)
{
    RegistryBatchPreview Result;
    Result.plan.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Result.plan.createdUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    Result.plan.callerSid = Sid; Result.plan.machineId = Machine;
    if (Options.text.isEmpty()) { Result.error = QObject::tr("치환할 문자열을 입력하세요."); return Result; }
    const auto Pattern = hc::columns::FieldRegex(Options.whole ? "\\A(?:" + Options.text + ")\\z" : Options.text);
    if (Options.regex && !Pattern.isValid()) { Result.error = Pattern.errorString(); return Result; }
    QSet<QString> Seen; qsizetype Budget = 0;
    for (const auto& Row : Rows) {
        if (!Row.location || !Row.value || Row.location->hive == Hive::ClassesRoot) { ++Result.skipped; continue; }
        const auto Before = RegistryValueCodec::DecodeText(*Row.value, false);
        if (!Before.lossless) { ++Result.skipped; continue; }
        auto Text = Before.text;
        if (Options.regex) {
            if (Text.size() > 1024 * 1024) { Result.error = QObject::tr("큰 문자열의 정규식 치환은 지원하지 않습니다."); return Result; }
            auto Matches = Pattern.globalMatch(Text);
            while (Matches.hasNext()) { if (!Matches.next().isValid()) { Result.error = QObject::tr("정규식 실행 한도를 넘었습니다."); return Result; } }
            if (!Matches.isValid()) { Result.error = QObject::tr("정규식 실행 한도를 넘었습니다."); return Result; }
            Text.replace(Pattern, Replacement);
        } else if (Options.whole) {
            if (Text.compare(Options.text, Qt::CaseInsensitive) == 0) Text = Replacement;
        } else Text.replace(Options.text, Replacement, Qt::CaseInsensitive);
        if (Text == Before.text) { ++Result.skipped; continue; }
        const auto Encoded = RegistryValueCodec::EncodeText(Row.value->type, Text);
        auto After = *Row.value; After.bytes = Encoded.value;
        if (!Encoded || !RegistryValueCodec::DecodeText(After, false).lossless || RegistryValueCodec::DecodeText(After, false).text != Text) {
            Result.error = QObject::tr("치환 결과를 원래 문자열 형식으로 손실 없이 저장할 수 없습니다."); return Result;
        }
        auto Location = *Row.location; if (Location.hive == Hive::CurrentUser) Location.sid = Sid;
        const auto Id = RegistryTreeModel::ValueId(Location, Row.value->name); if (Seen.contains(Id)) continue; Seen.insert(Id);
        Budget += Row.value->bytes.size() + After.bytes.size();
        if (Budget > 64 * 1024 * 1024) { Result.error = QObject::tr("치환 크기가 한도를 넘었습니다. 범위를 좁히세요."); return Result; }
        RegistryStep Step; Step.location = Location; Step.name = Row.value->name; Step.before = *Row.value; Step.after = After;
        Result.plan.steps.append(Step); Result.operations.append({OperationKind::SetValue, Location, After});
        auto Preview = Row; Preview.location = Location; Preview.cells = {Location.PhysicalPath() + " › " + (After.name.isEmpty() ? QObject::tr("(기본값)") : After.name),
            Before.text, Text, RegistryTreeModel::ValueTypeLabel(After.type)};
        Result.rows.append(std::move(Preview));
    }
    return Result;
}
RegistryResult<QList<RegistryOperation>> RegistryBatch::Export(IRegistryBackend& Backend,
    const QList<RegistryLocation>& Keys, const QList<RegistryResultRow>& Values, const std::atomic_bool* Cancel)
{
    RegistryResult<QList<RegistryOperation>> Result; auto Pending = Keys; QSet<QString> SeenKeys, SeenValues; qsizetype Budget = 0;
    const auto AddValue = [&](const RegistryLocation& Location, const RegistryValue& Value) {
        const auto Id = RegistryTreeModel::ValueId(Location, Value.name); if (SeenValues.contains(Id)) return;
        SeenValues.insert(Id); Budget += Value.bytes.size() + Value.name.size() * 2 + Location.key.size() * 2;
        Result.value.append({OperationKind::SetValue, Location, Value});
    };
    while (!Pending.isEmpty()) {
        if (Cancel && Cancel->load()) return {{}, ERROR_CANCELLED};
        const auto Key = Pending.takeLast(); if (SeenKeys.contains(Key.Id())) continue; SeenKeys.insert(Key.Id());
        const auto Snapshot = Backend.Enumerate(Key, Cancel); if (!Snapshot) return {{}, Snapshot.error};
        if (Snapshot.value.info.link) return {{}, ERROR_NOT_SUPPORTED}; // .reg cannot recreate a link key.
        Result.value.append({OperationKind::CreateKey, Key}); Budget += Key.key.size() * 2 + 128;
        for (const auto& Value : Snapshot.value.values) AddValue(Key, Value);
        for (const auto& Child : Snapshot.value.keys) Pending.append(Key.Child(Child.name));
        if (Budget > RegFileParser::MaxFileBytes / 8 || Result.value.size() > RegFileParser::MaxOperations) return {{}, ERROR_FILE_TOO_LARGE};
    }
    for (const auto& Row : Values) if (Row.location && Row.value) {
        AddValue(*Row.location, *Row.value);
        if (Budget > RegFileParser::MaxFileBytes / 8 || Result.value.size() > RegFileParser::MaxOperations) return {{}, ERROR_FILE_TOO_LARGE};
    }
    std::stable_sort(Result.value.begin(), Result.value.end(), [](const RegistryOperation& A, const RegistryOperation& B) {
        const int Key = QString::compare(A.location.PhysicalPath(), B.location.PhysicalPath(), Qt::CaseInsensitive);
        if (Key) return Key < 0;
        if (A.kind != B.kind) return A.kind == OperationKind::CreateKey;
        return QString::compare(A.value.name, B.value.name, Qt::CaseInsensitive) < 0;
    });
    return Result;
}
QList<RegistryOperation> RegistryBatch::Normalize(const QList<RegistryOperation>& Operations)
{
    QList<RegistryOperation> Result; QHash<QString, RegistryOperation> Block;
    const auto Flush = [&] {
        auto Values = Block.values(); Block.clear();
        std::sort(Values.begin(), Values.end(), [](const auto& A, const auto& B) {
            const int Path = QString::compare(A.location.PhysicalPath(), B.location.PhysicalPath(), Qt::CaseInsensitive);
            if (Path) return Path < 0;
            if (A.kind != B.kind) return A.kind == OperationKind::CreateKey;
            return QString::compare(A.value.name, B.value.name, Qt::CaseInsensitive) < 0;
        });
        Result.append(Values);
    };
    for (const auto& Operation : Operations) {
        // Deletion is an ordering barrier; sorting across it changes meaning.
        if (Operation.kind == OperationKind::DeleteKey || Operation.kind == OperationKind::DeleteValue) { Flush(); Result.append(Operation); continue; }
        const auto Id = Operation.kind == OperationKind::CreateKey ? "key:" + Operation.location.Id() : "value:" + RegistryTreeModel::ValueId(Operation.location, Operation.value.name);
        Block[Id] = Operation;
    }
    Flush(); return Result;
}
RegistryResult<QByteArray> RegistryBatch::Csv(const QList<RegistryResultRow>& Rows, const std::atomic_bool* Cancel)
{
    QByteArray Result = QByteArray::fromHex("efbbbf") + "Key,Name,Type,Data\r\n";
    const auto Quote = [](QString Value) { Value.replace('"', "\"\""); if (!Value.isEmpty() && QString("=+-@\t\r").contains(Value.front())) Value.prepend('\''); return '"' + Value + '"'; };
    for (const auto& Row : Rows) if (Row.location) {
        if (Cancel && Cancel->load()) return {{}, ERROR_CANCELLED};
        QString Data;
        if (Row.value) {
            const auto Text = RegistryValueCodec::DecodeText(*Row.value, false); const auto Number = RegistryValueCodec::DecodeNumber(*Row.value);
            Data = Text.lossless ? Text.text : Number ? QString::number(Number.value) : QString::fromLatin1(Row.value->bytes.toHex(' '));
        }
        const auto Line = Quote(Row.location->PhysicalPath()) + ',' + Quote(Row.value ? Row.value->name : QString()) + ','
            + Quote(Row.value ? RegistryTreeModel::ValueTypeLabel(Row.value->type) : QString("Key")) + ',' + Quote(Data) + "\r\n";
        const auto Bytes = Line.toUtf8(); if (Result.size() + Bytes.size() > RegFileParser::MaxFileBytes) return {{}, ERROR_FILE_TOO_LARGE};
        Result += Bytes;
    }
    return {Result};
}
}