#pragma once

inline void RegistryCrudCases()
{
    using namespace hc::registry;
    const QString Sid = "S-1-5-21-1", Machine = "fixture-machine";
    FakeRegistryBackend Backend; QTemporaryDir Directory;
    RegistryChangeStore Store(Directory.filePath("registry-backup"));
    RegistrySessionServer Server(Backend, Sid, Machine);
    RegistryRemoteSession Remote([&](const QString& Kind, QJsonObject Request) { return Server.Handle(Kind, Request); });
    RegistryMutation Mutation(Backend, Store, Sid, Machine, &Remote);
    for (const auto HiveValue : {Hive::CurrentUser, Hive::LocalMachine}) {
        RegistryLocation Root; Root.hive = HiveValue; Root.key = "Software\\HyperCommanderTests";
        auto Bound = Root; if (HiveValue == Hive::CurrentUser) Bound.sid = Sid; Backend.Seed(Bound);
        const auto Created = Mutation.CreateKey(Root.Child("child")); QCOMPARE(Created.applied.error, quint32(0));
        const auto Before = Backend.mutations_;
        QVERIFY(Mutation.CreateKey(Root.Child("child")).applied.conflict); QCOMPARE(Backend.mutations_, Before);
        QVERIFY(Mutation.CreateKey(Root.Child("missing\\child")).applied.conflict); QCOMPARE(Backend.mutations_, Before);
        QVERIFY(!Backend.Enumerate(Bound.Child("missing")));
        const RegistryValue Original{"value", REG_BINARY, "original"};
        QCOMPARE(Backend.Write(Bound.Child("child"), Original), quint32(0));
        QCOMPARE(Backend.Write(Bound.Child("child"), {"value", REG_BINARY, "external"}), quint32(0));
        const auto BeforeDelete = Backend.mutations_;
        QVERIFY(Mutation.DeleteSelected({{OperationKind::DeleteValue, Root.Child("child"), Original}}).applied.conflict);
        QCOMPARE(Backend.mutations_, BeforeDelete);
        auto Current = Backend.Read(Bound.Child("child"), "value").value;
        const auto Deleted = Mutation.DeleteSelected({{OperationKind::DeleteKey, Root.Child("child")},
            {OperationKind::DeleteValue, Root.Child("child"), Current}});
        QCOMPARE(Deleted.applied.error, quint32(0)); QVERIFY(!Backend.Enumerate(Bound.Child("child")));
        QCOMPARE(Mutation.Undo(Deleted.record).applied.error, quint32(0));
        QVERIFY(Backend.Read(Bound.Child("child"), "value").value == Current);
        const auto ValueDelete = Mutation.DeleteSelected({{OperationKind::DeleteValue, Root.Child("child"), Current},
            {OperationKind::DeleteValue, Root.Child("child"), Current}});
        QCOMPARE(ValueDelete.applied.error, quint32(0)); QCOMPARE(ValueDelete.record.plan.steps.size(), 1);
        QVERIFY(!Backend.Read(Bound.Child("child"), "value"));
    }
}