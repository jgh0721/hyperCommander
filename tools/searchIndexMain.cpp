#include "searchIndexPath.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <windows.h>
#include "externalLibs/Everything/src/include/Everything.h"

int main(int Argc, char** Argv)
{
    QCoreApplication App(Argc, Argv);
    const auto Args = App.arguments();
    if (Args.size() == 4 && Args[1] == "--quick-find") {
        const auto Term = Args[2].normalized(QString::NormalizationForm_C);
        const auto Match = Args[3];
        if (Term.isEmpty() || Term.size() > 4096 || Term.contains(QChar::Null) ||
            (Match != "starts" && Match != "contains" && Match != "ends")) return 2;
        if (!Everything_IsDBLoaded()) return 3;
        const auto Pattern = ((Match == "starts" ? "^" : "") + QRegularExpression::escape(Term) + (Match == "ends" ? "$" : "")).toStdWString();
        Everything_SetRegex(TRUE); Everything_SetMatchPath(FALSE); Everything_SetMatchCase(FALSE);
        Everything_SetSearchW(Pattern.c_str()); Everything_SetMax(100);
        Everything_SetSort(EVERYTHING_SORT_NAME_ASCENDING);
        if (!Everything_QueryW(TRUE)) return 4;
        QJsonArray Paths;
        for (DWORD Row = 0; Row < Everything_GetNumResults(); ++Row) {
            std::wstring Buffer(32768, L'\0');
            const auto Size = Everything_GetResultFullPathNameW(Row, Buffer.data(), DWORD(Buffer.size()));
            if (!Size || Size >= Buffer.size()) return 5;
            Paths.append(QString::fromWCharArray(Buffer.data(), int(Size)));
        }
        QFile Output; if (!Output.open(stdout, QIODevice::WriteOnly)) return 6;
        Output.write(QJsonDocument(QJsonObject{{"paths", Paths}, {"total", double(Everything_GetTotResults())}}).toJson(QJsonDocument::Compact));
        return 0;
    }
    if (Args.size() == 3 && Args[1] == "--folder-sizes") {
        const auto Parent = QDir::toNativeSeparators(QDir::cleanPath(Args[2]));
        if (!QDir::isAbsolutePath(Parent) || Parent.contains('"') || Parent.contains(QChar::Null)) return 2;
        if (!Everything_IsDBLoaded()) return 3;
        const auto Query = (QStringLiteral("folder: parent:\"") + Parent + '"').toStdWString();
        Everything_SetSearchW(Query.c_str());
        Everything_SetRequestFlags(EVERYTHING_REQUEST_FILE_NAME | EVERYTHING_REQUEST_PATH | EVERYTHING_REQUEST_SIZE);
        Everything_SetMax(65536);
        if (!Everything_QueryW(TRUE)) return 4;
        QJsonArray Sizes;
        for (DWORD Row = 0; Row < Everything_GetNumResults(); ++Row) {
            LARGE_INTEGER Bytes{};
            const auto* Name = Everything_GetResultFileNameW(Row);
            const auto* Path = Everything_GetResultPathW(Row);
            if (!Name || !Path || !Everything_IsFolderResult(Row) || !Everything_GetResultSize(Row, &Bytes) || Bytes.QuadPart < 0) continue;
            if (!hc::search::IndexParentMatches(QString::fromWCharArray(Path), Parent)) continue;
            Sizes.append(QJsonObject{{"name", QString::fromWCharArray(Name)}, {"bytes", QString::number(Bytes.QuadPart)}});
        }
        QFile Output; if (!Output.open(stdout, QIODevice::WriteOnly)) return 6;
        Output.write(QJsonDocument(QJsonObject{{"sizes", Sizes}}).toJson(QJsonDocument::Compact));
        return 0;
    }
    if (Args.size() != 4) return 2;
    const QString Root = QDir::toNativeSeparators(QDir::cleanPath(Args[1]));
    bool OffsetOk = false, LimitOk = false;
    const auto Offset = Args[2].toUInt(&OffsetOk), Limit = Args[3].toUInt(&LimitOk);
    if (!QDir::isAbsolutePath(Root) || Root.contains('"') || Root.contains(QChar::Null) || !OffsetOk || !LimitOk || !Limit || Limit > 256) return 2;
    // The parent bounds this IPC call with process cancellation. SDK state is
    // private to this process, independent of the legacy directory-size query.
    if (!Everything_IsDBLoaded()) return 3;
    auto Prefix = Root; if (!Prefix.endsWith('\\')) Prefix += '\\';
    const auto Query = (QStringLiteral("path:\"") + Prefix + '"').toStdWString();
    Everything_SetSearchW(Query.c_str());
    Everything_SetSort(EVERYTHING_SORT_PATH_ASCENDING);
    Everything_SetOffset(Offset); Everything_SetMax(Limit);
    if (!Everything_QueryW(TRUE)) return 4;
    QJsonArray Paths;
    for (DWORD Row = 0; Row < Everything_GetNumResults(); ++Row)
    {
        std::wstring Buffer(32768, L'\0');
        const auto Size = Everything_GetResultFullPathNameW(Row, Buffer.data(), DWORD(Buffer.size()));
        if (!Size || Size >= Buffer.size()) return 5;
        Paths.append(QString::fromWCharArray(Buffer.data(), int(Size)));
    }
    QFile Output;
    if (!Output.open(stdout, QIODevice::WriteOnly)) return 6;
    Output.write(QJsonDocument(QJsonObject{{"paths", Paths}, {"total", double(Everything_GetTotResults())}}).toJson(QJsonDocument::Compact));
    return 0;
}
