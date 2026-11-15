#include "stdafx.h"
#include "registryImport.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryImport, "hc.registry.import")
namespace hc::registry {
RegistryImportPreview RegistryImport::Prepare(IRegistryBackend& Backend, const QList<RegistryOperation>& Operations,
    const QSet<int>& Excluded, const QString& Sid, const QString& Machine, const std::atomic_bool* Cancel)
{
    RegistryImportPreview Result;
    for (int I = 0; I < Operations.size(); ++I) {
        if (Excluded.contains(I)) { ++Result.skipped; continue; }
        auto Operation = Operations[I];
        if (Operation.location.hive == Hive::ClassesRoot) {
            Result.error = QObject::tr("HKCR의 실제 쓰기 위치를 먼저 고르세요."); return Result;
        }
        if (Operation.location.hive == Hive::CurrentUser) Operation.location.sid = Sid;
        Result.included.append(I); Result.operations.append(Operation);
    }
    QList<QPair<int, int>> Ranges;
    RegistryTransaction Transaction(Backend);
    auto Prepared = Transaction.Prepare(Result.operations, Sid, Machine, Cancel,
        [&](int Index, int Begin, int End) { Q_UNUSED(Index); Ranges.append({Begin, End}); });
    if (!Prepared) { Result.error = QObject::tr("미리 볼 레지스트리를 읽지 못했습니다 (오류 %1).").arg(Prepared.error); return Result; }
    Result.plan = Prepared.value;
    for (int I = 0; I < Result.operations.size(); ++I) {
        const auto& Operation = Result.operations[I]; const auto Range = Ranges[I];
        const bool Changed = Range.first != Range.second;
        RegistryResultRow Row; Row.id = QString::number(Result.included[I]); Row.location = Operation.location;
        Row.groups = {Operation.location.PhysicalPath()}; Row.payload["operation"] = Result.included[I]; Row.payload["line"] = Operation.line;
        QString Action, Before, After;
        if (Operation.kind == OperationKind::SetValue || Operation.kind == OperationKind::DeleteValue) {
            Row.value = Operation.value;
            const RegistryStep* Step = Changed ? &Result.plan.steps[Range.first] : nullptr;
            Action = !Changed ? QObject::tr("변경 없음") : Operation.kind == OperationKind::DeleteValue ? QObject::tr("삭제") : Step->before ? QObject::tr("변경") : QObject::tr("추가");
            Before = Step && Step->before ? RegistryTreeModel::ValuePreview(*Step->before) : !Changed && Operation.kind == OperationKind::SetValue ? RegistryTreeModel::ValuePreview(Operation.value) : QObject::tr("(없음)");
            After = Operation.kind == OperationKind::SetValue ? RegistryTreeModel::ValuePreview(Operation.value) : QObject::tr("(없음)");
        } else {
            Action = !Changed ? QObject::tr("변경 없음") : Operation.kind == OperationKind::DeleteKey ? QObject::tr("키 삭제") : QObject::tr("키 추가");
            Before = Operation.kind == OperationKind::DeleteKey ? QObject::tr("하위 키·값 포함 %1단계").arg(Range.second - Range.first) : Changed ? QObject::tr("(없음)") : QObject::tr("있음");
            After = Operation.kind == OperationKind::DeleteKey ? QObject::tr("(없음)") : QObject::tr("있음");
        }
        Row.cells = {Action, Operation.location.PhysicalPath() + (Row.value ? " › " + (Row.value->name.isEmpty() ? QObject::tr("(기본값)") : Row.value->name) : QString()),
            Before, After, QString(), Operation.location.hive == Hive::LocalMachine || Operation.location.hive == Hive::Users ? QObject::tr("관리자 도우미") : QObject::tr("현재 사용자")};
        Row.searchText = Row.cells.join('\n'); Result.rows.append(Row);
    }
    return Result;
}
}