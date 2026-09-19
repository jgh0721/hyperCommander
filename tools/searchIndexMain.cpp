#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <windows.h>
#include "externalLibs/Everything/src/include/Everything.h"

int main(int Argc, char** Argv)
{
    QCoreApplication App(Argc, Argv);
    const auto Args = App.arguments();
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
