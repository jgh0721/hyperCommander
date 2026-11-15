#include "stdafx.h"
#include "tstRegistryCore.hpp"
#include "commonLibs/system/registryValue.hpp"
#include "commonLibs/system/registryEnvironment.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/system/registryAccess.hpp"
#include "commonLibs/system/registryFile.hpp"
#include "commonLibs/system/registryPolicyFile.hpp"
#include "commonLibs/system/registryWatch.hpp"
#include "commonLibs/system/processInfo.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QTest>
#include <QUuid>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QProcess>
#include <QFile>
#include <Aclapi.h>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryCoreTests, "hc.tests.registryCore")

using namespace hc::registry;
namespace {
struct Fixture
{
    RegistryLocation location;
    WinRegistryBackend backend;
    Fixture()
    {
        const auto Base = RegistryLocation::Parse("HKCU\\Software\\HyperCommanderTests").value();
        backend.Create(Base);
        location = Base.Child("RegistryEditor-task_4ceba737038e-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        backend.Create(location);
    }
    ~Fixture()
    {
        // Only remove this test's unpredictable, owned child. Never remove the
        // shared HyperCommanderTests parent or any real application settings.
        if (!location.key.startsWith("Software\\HyperCommanderTests\\RegistryEditor-")) return;
        backend.DeleteKey(location.Child("link"));
        auto Parent = WinRegistryBackend::Open(*location.Parent(), KEY_WRITE | KEY_ENUMERATE_SUB_KEYS);
        if (Parent) RegDeleteTreeW(Parent.value.Get(), reinterpret_cast<const wchar_t*>(location.key.section('\\', -1).utf16()));
    }
};
QByteArray RegText(const QString& Text)
{
    QByteArray Out = QByteArray::fromHex("fffe");
    for (QChar C : Text) { Out += char(C.unicode()); Out += char(C.unicode() >> 8); }
    return Out;
}
}
void RegistryCoreTests::queryInfoAcl()
{
    Fixture F;
    auto Handle = WinRegistryBackend::Open(F.location, READ_CONTROL | WRITE_DAC);
    QVERIFY(Handle); PSECURITY_DESCRIPTOR Descriptor = nullptr; PACL Original = nullptr;
    QCOMPARE(GetSecurityInfo(Handle.value.Get(), SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &Original, nullptr, &Descriptor), DWORD(ERROR_SUCCESS));
    const auto Release = qScopeGuard([&] { LocalFree(Descriptor); });
    BYTE Sid[SECURITY_MAX_SID_SIZE]{}; DWORD Size = sizeof(Sid);
    QVERIFY(CreateWellKnownSid(WinWorldSid, nullptr, Sid, &Size));
    EXPLICIT_ACCESSW Deny{}; Deny.grfAccessPermissions = KEY_ENUMERATE_SUB_KEYS;
    Deny.grfAccessMode = DENY_ACCESS; Deny.grfInheritance = NO_INHERITANCE;
    Deny.Trustee.TrusteeForm = TRUSTEE_IS_SID; Deny.Trustee.ptstrName = reinterpret_cast<LPWSTR>(Sid);
    PACL Changed = nullptr;
    QCOMPARE(SetEntriesInAclW(1, &Deny, Original, &Changed), DWORD(ERROR_SUCCESS));
    const auto FreeAcl = qScopeGuard([&] { LocalFree(Changed); });
    // Retain the WRITE_DAC handle across revocation so even an assertion failure
    // restores this owned fixture before its normal recursive cleanup runs.
    const auto Restore = qScopeGuard([&] {
        SetSecurityInfo(Handle.value.Get(), SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr, Original, nullptr);
    });
    QCOMPARE(SetSecurityInfo(Handle.value.Get(), SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, Changed, nullptr), DWORD(ERROR_SUCCESS));
    QVERIFY(WinRegistryBackend::Open(F.location, KEY_QUERY_VALUE));
    QCOMPARE(F.backend.QueryInfo(F.location).error, quint32(ERROR_ACCESS_DENIED));
    QCOMPARE(F.backend.Enumerate(F.location).error, quint32(ERROR_ACCESS_DENIED));
    QCOMPARE(SetSecurityInfo(Handle.value.Get(), SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, Original, nullptr), DWORD(ERROR_SUCCESS));
    QVERIFY(F.backend.QueryInfo(F.location));
}
void RegistryCoreTests::addresses()
{
    const auto L = RegistryLocation::Parse("hkcu\\Software\\HyperCommanderTests", View::Wow32);
    QVERIFY(L); QCOMPARE(L->hive, Hive::CurrentUser); QCOMPARE(L->view, View::Wow32);
    QCOMPARE(L->Parent()->Path(), "HKEY_CURRENT_USER\\Software");
    QCOMPARE(L->Child("한글").Parent()->Id(), L->Id());
    QVERIFY(!RegistryLocation::Parse("HKCU\\a\\\\b"));
    QVERIFY(!RegistryLocation::Parse("HKXX\\a"));
    QVERIFY(!RegistryLocation::Parse("HKCU\\" + QString(256, 'x')));
    QVERIFY(!RegistryLocation::Parse("HKCU")->Parent());
}
void RegistryCoreTests::rawTypesAndMutation()
{
    Fixture F;
    const QList<RegistryValue> Values{
        {"", REG_SZ, QByteArray::fromHex("0000")}, {"none", REG_NONE, {}},
        {"odd", REG_SZ, QByteArray::fromHex("61")}, {"unterminated", REG_SZ, QByteArray::fromHex("6100")},
        {"binary", REG_BINARY, QByteArray::fromHex("00ff807f")}, {"dword", REG_DWORD, QByteArray::fromHex("78563412")},
        {"qword", REG_QWORD, QByteArray::fromHex("0123456789abcdef")},
        {"expand", REG_EXPAND_SZ, QByteArray::fromHex("2500410025000000")},
        {"multi", REG_MULTI_SZ, QByteArray::fromHex("610000000000")}, {"unknown", 0x1234, QByteArray::fromHex("010002")}};
    for (const auto& V : Values) { QCOMPARE(F.backend.Write(F.location, V), quint32(0)); auto R = F.backend.Read(F.location, V.name); QVERIFY(R); QCOMPARE(R.value.name, V.name); QCOMPARE(R.value.type, V.type); QCOMPARE(R.value.bytes, V.bytes); }
    const auto Snapshot = F.backend.Enumerate(F.location);
    QVERIFY(Snapshot); QCOMPARE(Snapshot.value.values.size(), Values.size()); QVERIFY(Snapshot.value.info.lastWrite != 0);
    QCOMPARE(F.backend.Read(F.location, "missing").error, quint32(ERROR_FILE_NOT_FOUND));
    QVERIFY(!Values[2].Warning().isEmpty()); QVERIFY(!Values[3].Warning().isEmpty());
    QCOMPARE(F.backend.Create(F.location.Child("child")), quint32(0));
    QCOMPARE(F.backend.RenameKey(F.location.Child("child"), "renamed"), quint32(0));
    QCOMPARE(F.backend.DeleteKey(F.location.Child("renamed")), quint32(0));
    QCOMPARE(F.backend.DeleteValue(F.location, "binary"), quint32(0));
    QCOMPARE(F.backend.Read(F.location, "binary").error, quint32(ERROR_FILE_NOT_FOUND));
    QCOMPARE(F.backend.DeleteKey(*RegistryLocation::Parse("HKCU")), quint32(ERROR_INVALID_PARAMETER));
    std::atomic_bool Cancel = true;
    QCOMPARE(F.backend.Enumerate(F.location, &Cancel).error, quint32(ERROR_CANCELLED));
}
void RegistryCoreTests::regFileRoundtrip()
{
    const auto Location = RegistryLocation::Parse("HKCU\\Software\\HyperCommanderTests\\File", View::Wow32).value();
    QList<RegistryOperation> Operations{{OperationKind::CreateKey, Location}};
    for (quint32 Type : {REG_NONE, REG_SZ, REG_EXPAND_SZ, REG_BINARY, REG_DWORD, REG_MULTI_SZ, REG_QWORD, 0x1111UL})
        Operations.append({OperationKind::SetValue, Location, {QString::number(Type), Type, QByteArray::fromHex("0011225d00ff")}});
    Operations.append({OperationKind::SetValue, Location, {"quotes\\\"", REG_SZ, QByteArray::fromHex("41000000")}});
    Operations.append({OperationKind::SetValue, Location, {"long", REG_BINARY, QByteArray(128, '\xff')}});
    Operations.append({OperationKind::DeleteValue, Location, {""}});
    Operations.append({OperationKind::DeleteKey, Location});
    const auto Bytes = RegFileWriter::Write(Operations);
    auto Parsed = RegFileParser::Parse(Bytes, View::Wow32);
    QVERIFY2(Parsed, qPrintable(Parsed.error)); QCOMPARE(Parsed.operations.size(), Operations.size());
    for (qsizetype I = 0; I < Operations.size(); ++I) {
        QCOMPARE(Parsed.operations[I].kind, Operations[I].kind);
        QCOMPARE(Parsed.operations[I].location.Id(), Operations[I].location.Id());
        QVERIFY(Parsed.operations[I].value == Operations[I].value);
    }
    const auto Legacy = RegFileParser::Parse("REGEDIT4\r\n[HKCU\\Software\\HyperCommanderTests]\r\n@=dword:12345678\r\n", View::Native64);
    QVERIFY(Legacy); QCOMPARE(Legacy.operations.last().value.bytes, QByteArray::fromHex("78563412"));
}
void RegistryCoreTests::regFileErrors()
{
    const QString Header = "Windows Registry Editor Version 5.00\r\n[HKCU\\Software\\HyperCommanderTests]\r\n";
    for (const QString& Bad : {QString("@=dword:123456789"), QString("@=hex:00,zz"), QString("@=hex:00,\\"), QString("\"a\"=\"bad\\x\""), QString("@=hex(123456789):00")}) {
        const auto Parsed = RegFileParser::Parse(RegText(Header + Bad), View::Native64);
        QVERIFY(!Parsed); QVERIFY(Parsed.operations.isEmpty()); QCOMPARE(Parsed.line, 3);
    }
    QVERIFY(RegFileParser::Parse(Header.toUtf8(), View::Native64).encodingConfirmation);
    QVERIFY(!RegFileParser::Parse(RegText("Windows Registry Editor Version 5.00\n[-HKLM]\n"), View::Native64));
    QVERIFY(!RegFileParser::Parse(QByteArray::fromHex("fffe41"), View::Native64));
    const auto Ordered = RegFileParser::Parse(RegText(Header + "@=\"one\"\n@=-\n@=\"two\"\n"), View::Native64);
    QVERIFY(Ordered); QCOMPARE(Ordered.operations.size(), 4);
    QCOMPARE(Ordered.operations[2].kind, OperationKind::DeleteValue);
}
void RegistryCoreTests::policyRoundtripAndTruncation()
{
    // Captured from IGroupPolicyObject::Save on the shared VM in P0; only the
    // inert HyperCommanderTestRegistryEditor fixture is contained in this record.
    const QByteArray Native = QByteArray::fromHex("50526567010000005B0053006F006600740077006100720065005C0050006F006C00690063006900650073005C004800790070006500720043006F006D006D0061006E0064006500720054006500730074005200650067006900730074007200790045006400690074006F00720000003B00500072006F0062006500560061006C007500650000003B00040000003B00040000003B00504352485D00");
    const auto NativeRead = RegistryPolCodec::Read(Native); QVERIFY(NativeRead);
    QCOMPARE(NativeRead.records.size(), 1); QCOMPARE(NativeRead.records[0].value.name, "ProbeValue");
    QCOMPARE(NativeRead.records[0].value.bytes, QByteArray::fromHex("50435248"));
    QCOMPARE(RegistryPolCodec::Write(NativeRead.records).value, Native);
    const QList<RegistryPolRecord> Records{{"Software\\Policies\\HyperCommanderTest", {"value", REG_BINARY, QByteArray::fromHex("5d003b000000")}},
        {"Software\\Policies\\HyperCommanderTest", {"**Del.value", REG_SZ, QByteArray::fromHex("0000")}},
        {"Software\\Policies\\HyperCommanderTest", {"**Future", 0x9999, {}}}};
    const auto Encoded = RegistryPolCodec::Write(Records); QVERIFY(Encoded);
    const auto Parsed = RegistryPolCodec::Read(Encoded.value); QVERIFY(Parsed); QVERIFY(Parsed.records == Records);
    QCOMPARE(Parsed.records[1].Directive(), PolicyDirective::DeleteValue);
    QCOMPARE(Parsed.records[2].Directive(), PolicyDirective::Unknown);
    for (qsizetype Length = 0; Length < 8; ++Length) QVERIFY(!RegistryPolCodec::Read(Encoded.value.first(Length)));
    auto Broken = Encoded.value; Broken[4] = 2; QVERIFY(!RegistryPolCodec::Read(Broken));
    Broken = Encoded.value; Broken.chop(1); QVERIFY(!RegistryPolCodec::Read(Broken));
    const auto Empty = RegistryPolCodec::Write({}); QVERIFY(Empty); QVERIFY(RegistryPolCodec::Read(Empty.value));
}
void RegistryCoreTests::watchAndCancel()
{
    Fixture F; std::atomic_int Count = 0; std::atomic_uint Error = 0;
    RegistryWatcher Watch;
    QVERIFY(Watch.Start(F.location, false, [&](quint32 Code) { Error = Code; ++Count; }));
    // Start is asynchronous; keep modifying only our owned value until the
    // watcher arms, avoiding a timing dependency on thread scheduling.
    QElapsedTimer Deadline; Deadline.start();
    while (!Count.load() && Deadline.elapsed() < 3000) {
        QCOMPARE(F.backend.Write(F.location, {"watch", REG_BINARY, QByteArray::number(Deadline.elapsed())}), quint32(0));
        QTest::qWait(10);
    }
    QVERIFY(Count.load() > 0); QCOMPARE(Error.load(), 0U);
    const int Before = Count;
    QCOMPARE(F.backend.Write(F.location, {"watch", REG_BINARY, "changed"}), quint32(0));
    QTRY_VERIFY_WITH_TIMEOUT(Count.load() > Before, 3000);
    Watch.Stop(); const int Stopped = Count;
    QCOMPARE(F.backend.Write(F.location, {"watch", REG_BINARY, "stopped"}), quint32(0));
    QTest::qWait(40); QCOMPARE(Count.load(), Stopped);
    RegistryWatcher Pending; QVERIFY(Pending.Start(F.location, false, [](quint32) {})); Pending.Stop();
}
void RegistryCoreTests::viewsLinksAndConcurrentDeletion()
{
    Fixture F;
    QCOMPARE(F.backend.Write(F.location, {"view", REG_BINARY, "shared"}), quint32(0));
    auto View32 = F.location; View32.view = View::Wow32;
    // HKCU\Software is shared on x64; the view flag must not add Wow6432Node.
    QCOMPARE(F.backend.Read(View32, "view").value.bytes, QByteArray("shared"));
    auto Caller = F.location; Caller.sid = QString::fromStdWString(hc::system::CurrentUserSid());
    QVERIFY(!Caller.sid.isEmpty()); QCOMPARE(F.backend.Read(Caller, "view").value.bytes, QByteArray("shared"));
    const auto Target = F.location.Child("target");
    QCOMPARE(F.backend.Create(Target), quint32(0));
    QCOMPARE(F.backend.Create(Target.Child("child")), quint32(0));
    auto Parent = WinRegistryBackend::Open(F.location, KEY_CREATE_SUB_KEY);
    QVERIFY(Parent);
    HKEY Raw = nullptr;
    QCOMPARE(RegCreateKeyExW(Parent.value.Get(), L"link", 0, nullptr, REG_OPTION_CREATE_LINK,
        KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_WOW64_64KEY, nullptr, &Raw, nullptr), LSTATUS(0));
    KeyHandle Link(Raw);
    QString NativePath = "\\Registry\\User\\" + Caller.sid + "\\" + Target.key;
    QCOMPARE(RegSetValueExW(Link.Get(), L"SymbolicLinkValue", 0, REG_LINK,
        reinterpret_cast<const BYTE*>(NativePath.utf16()), DWORD(NativePath.size() * 2)), LSTATUS(0));
    const auto LinkLocation = F.location.Child("link");
    const auto LinkSnapshot = F.backend.Enumerate(LinkLocation);
    QVERIFY(LinkSnapshot); QVERIFY(LinkSnapshot.value.info.link); QVERIFY(LinkSnapshot.value.keys.isEmpty());
    QCOMPARE(F.backend.Write(LinkLocation, {"no", REG_BINARY, "no"}), quint32(ERROR_CANT_ACCESS_FILE));
    QCOMPARE(WinRegistryBackend::Open(LinkLocation.Child("child"), KEY_QUERY_VALUE).error, quint32(ERROR_CANT_ACCESS_FILE));
    NativePath = "\\Registry\\User\\" + Caller.sid + "\\" + LinkLocation.key;
    QCOMPARE(RegSetValueExW(Link.Get(), L"SymbolicLinkValue", 0, REG_LINK,
        reinterpret_cast<const BYTE*>(NativePath.utf16()), DWORD(NativePath.size() * 2)), LSTATUS(0));
    QVERIFY(F.backend.Enumerate(LinkLocation).value.info.link); // Self-cycle stays a link row.
    Link = KeyHandle(); QCOMPARE(F.backend.DeleteKey(LinkLocation), quint32(0));
    QVERIFY(F.backend.Enumerate(Target));
    std::atomic_bool Stop = false;
    std::thread Mutator([&] {
        WinRegistryBackend Backend;
        while (!Stop.load()) { Backend.Create(F.location.Child("race")); Backend.DeleteKey(F.location.Child("race")); }
    });
    bool Expected = true;
    for (int I = 0; I != 50; ++I) {
        const auto Read = F.backend.Enumerate(F.location);
        Expected &= Read.error == 0 || Read.error == ERROR_RETRY || Read.error == ERROR_KEY_DELETED || Read.error == ERROR_FILE_NOT_FOUND;
    }
    Stop = true; Mutator.join(); QVERIFY(Expected);
}
void RegistryCoreTests::machineViewsInVm()
{
    const auto AllowedMachine = qEnvironmentVariable("HC_REGISTRY_TEST_MACHINE");
    if (AllowedMachine != "iMonAGR-Win11") QSKIP("Machine writes are enabled only by the dedicated VM runner");
    QCOMPARE(QSysInfo::machineHostName().toLower(), AllowedMachine.toLower());
    WinRegistryBackend Backend;
    auto Native = RegistryLocation::Parse("HKLM\\Software\\HyperCommanderTestRegistryEditor-task_4ceba737038e-" + QUuid::createUuid().toString(QUuid::WithoutBraces)).value();
    auto Wow = Native; Wow.view = View::Wow32;
    QCOMPARE(WinRegistryBackend::Open(Native, KEY_QUERY_VALUE).error, quint32(ERROR_FILE_NOT_FOUND));
    QCOMPARE(WinRegistryBackend::Open(Wow, KEY_QUERY_VALUE).error, quint32(ERROR_FILE_NOT_FOUND));
    const auto Cleanup = qScopeGuard([&] { Backend.DeleteKey(Wow); Backend.DeleteKey(Native); });
    QCOMPARE(Backend.Create(Native), quint32(0)); QCOMPARE(Backend.Create(Wow), quint32(0));
    QCOMPARE(Backend.Write(Native, {"Probe", REG_BINARY, "64"}), quint32(0));
    QCOMPARE(Backend.Write(Wow, {"Probe", REG_BINARY, "32"}), quint32(0));
    QCOMPARE(Backend.Read(Native, "Probe").value.bytes, QByteArray("64"));
    QCOMPARE(Backend.Read(Wow, "Probe").value.bytes, QByteArray("32"));
    QCOMPARE(Backend.DeleteKey(Wow), quint32(0)); QCOMPARE(Backend.DeleteKey(Native), quint32(0));
    QCOMPARE(WinRegistryBackend::Open(Native, KEY_QUERY_VALUE).error, quint32(ERROR_FILE_NOT_FOUND));
    QCOMPARE(WinRegistryBackend::Open(Wow, KEY_QUERY_VALUE).error, quint32(ERROR_FILE_NOT_FOUND));
}
void RegistryCoreTests::regeditCompatibility()
{
    Fixture F; QTemporaryDir Directory; QVERIFY(Directory.isValid());
    const auto Path = Directory.filePath("fixture.reg");
    const auto Reg = qEnvironmentVariable("SystemRoot") + "/System32/reg.exe";
    const QList<RegistryValue> Values{{"Text", REG_SZ, QByteArray::fromHex("41000000")},
        {"Expand", REG_EXPAND_SZ, QByteArray::fromHex("2500540045004d00500025000000")},
        {"Multi", REG_MULTI_SZ, QByteArray::fromHex("41000000420000000000")},
        {"Dword", REG_DWORD, QByteArray::fromHex("78563412")},
        {"Qword", REG_QWORD, QByteArray::fromHex("0123456789abcdef")},
        {"Binary", REG_BINARY, QByteArray(100, '\xff')}};
    QList<RegistryOperation> Operations{{OperationKind::CreateKey, F.location}};
    for (const auto& Value : Values) Operations.append({OperationKind::SetValue, F.location, Value});
    QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly));
    const auto Bytes = RegFileWriter::Write(Operations); QCOMPARE(File.write(Bytes), Bytes.size()); File.close();
    QCOMPARE(QProcess::execute(Reg, {"import", Path, "/reg:64"}), 0);
    for (const auto& Value : Values) QVERIFY(F.backend.Read(F.location, Value.name).value == Value);
    const auto Export = Directory.filePath("native.reg");
    QCOMPARE(QProcess::execute(Reg, {"export", F.location.Path(), Export, "/y", "/reg:64"}), 0);
    QFile Exported(Export); QVERIFY(Exported.open(QIODevice::ReadOnly));
    const auto Parsed = RegFileParser::Parse(Exported.readAll(), View::Native64); QVERIFY2(Parsed, qPrintable(Parsed.error));
    for (const auto& Value : Values) {
        bool Found = false;
        for (const auto& Operation : Parsed.operations) if (Operation.kind == OperationKind::SetValue && Operation.value == Value) Found = true;
        QVERIFY(Found);
    }
    const auto Legacy = "REGEDIT4\r\n[" + F.location.Path().toLatin1() + "]\r\n\"Legacy\"=hex(2):25,54,45,4d,50,25,00\r\n";
    const auto LegacyParsed = RegFileParser::Parse(Legacy, View::Native64); QVERIFY(LegacyParsed);
    QVERIFY(File.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(File.write(Legacy), Legacy.size()); File.close();
    QCOMPARE(QProcess::execute(Reg, {"import", Path, "/reg:64"}), 0);
    QVERIFY(F.backend.Read(F.location, "Legacy").value == LegacyParsed.operations.last().value);
}
void RegistryCoreTests::editorValueCodec()
{
    for (const auto Type : {REG_SZ, REG_EXPAND_SZ, REG_MULTI_SZ}) {
        const QString Text = Type == REG_MULTI_SZ ? QStringLiteral("첫째\n둘째") : QStringLiteral("한글 %TEMP% 😀");
        const auto Encoded = RegistryValueCodec::EncodeText(Type, Text); QVERIFY(Encoded);
        const auto Decoded = RegistryValueCodec::DecodeText({"", Type, Encoded.value});
        QVERIFY2(Decoded.lossless, qPrintable(Decoded.warning)); QCOMPARE(Decoded.text, Text);
        const auto Empty = RegistryValueCodec::EncodeText(Type, {}); QVERIFY(Empty);
        QCOMPARE(Empty.value.size(), Type == REG_MULTI_SZ ? 4 : 2);
        QVERIFY(RegistryValueCodec::DecodeText({"", Type, Empty.value}).lossless);
    }
    for (const auto& Bytes : {QByteArray::fromHex("4100"), QByteArray::fromHex("410000"),
            QByteArray::fromHex("00d80000"), QByteArray::fromHex("4100000042000000")}) {
        const RegistryValue Value{"Bad", REG_SZ, Bytes};
        QVERIFY(!RegistryValueCodec::DecodeText(Value).lossless);
        QCOMPARE(Value.bytes, Bytes);
    }
    QVERIFY(!RegistryValueCodec::DecodeText({"", REG_MULTI_SZ, QByteArray::fromHex("410000000000420000000000")}).lossless);
    QVERIFY(!RegistryValueCodec::EncodeText(REG_SZ, QString(QChar(0xd800))));
    QVERIFY(!RegistryValueCodec::EncodeText(REG_SZ, QString(QChar(0))));
    const auto Multi = RegistryValueCodec::EncodeText(REG_MULTI_SZ, "A\r\n\nB\n"); QVERIFY(Multi);
    QCOMPARE(RegistryValueCodec::DecodeText({"", REG_MULTI_SZ, Multi.value}).text, QString("A\nB"));
    for (const auto Type : {REG_DWORD, REG_DWORD_BIG_ENDIAN, REG_QWORD}) {
        const quint64 Number = Type == REG_QWORD ? Q_UINT64_C(0xffffffffffffffff) : Q_UINT64_C(0xfedcba98);
        const auto Encoded = RegistryValueCodec::EncodeNumber(Type, Number); QVERIFY(Encoded);
        const auto Decoded = RegistryValueCodec::DecodeNumber({"", Type, Encoded.value}); QVERIFY(Decoded);
        QCOMPARE(Decoded.value, Number);
    }
    QCOMPARE(RegistryValueCodec::EncodeNumber(REG_DWORD_BIG_ENDIAN, 0x12345678).value, QByteArray::fromHex("12345678"));
    QCOMPARE(RegistryValueCodec::EncodeNumber(REG_DWORD, 0x12345678).value, QByteArray::fromHex("78563412"));
    QVERIFY(!RegistryValueCodec::EncodeNumber(REG_DWORD, Q_UINT64_C(0x100000000)));
    QVERIFY(!RegistryValueCodec::DecodeNumber({"", REG_DWORD, "abc"}));
    QVERIFY(!RegistryValueCodec::DecodeNumber({"", REG_QWORD, "abcd"}));
    QVERIFY(!RegistryValueCodec::EncodeNumber(REG_BINARY, 1));
}
void RegistryCoreTests::environmentExpansion()
{
    QProcessEnvironment Environment; Environment.insert("ROOT", "C:\\Root"); Environment.insert("Nested", "%ROOT%\\bin");
    Environment.insert("Empty", "");
    auto Result = ExpandEnvironmentText("%root%\\bin;%ROOT%", Environment);
    QCOMPARE(Result.text, QString("C:\\Root\\bin;C:\\Root")); QVERIFY(Result.missing.isEmpty()); QVERIFY(!Result.tooLarge);
    Result = ExpandEnvironmentText("%MISSING%;%missing%;%Empty%", Environment);
    QCOMPARE(Result.text, QString("%MISSING%;%missing%;")); QCOMPARE(Result.missing.size(), 1);
    QCOMPARE(ExpandEnvironmentText("literal%%end%", Environment).text, QString("literal%%end%"));
    QCOMPARE(ExpandEnvironmentText("%Nested%", Environment).text, QString("%ROOT%\\bin"));
    Environment.insert("ROOT", "D:\\Override");
    QCOMPARE(ExpandEnvironmentText("%ROOT%", Environment).text, QString("D:\\Override"));
    Environment.insert("Huge", QString(WinRegistryBackend::MaxValueBytes / 2, 'x'));
    Result = ExpandEnvironmentText("%Huge%X", Environment); QVERIFY(Result.tooLarge); QVERIFY(Result.text.isEmpty());
}
QTEST_GUILESS_MAIN(RegistryCoreTests)
