#pragma once

inline void RegistryStaleRecoveryCase()
{
    using namespace hc::registry;
    FakeRegistryBackend Backend;
    const auto Root = RegistryLocation::Parse("HKCU").value(); Backend.Seed(Root);
    RegistryTransaction Transaction(Backend); QTemporaryDir Directory; RegistryChangeStore Store(Directory.path());
    const auto Plan = Transaction.Prepare({{OperationKind::SetValue, Root, {"first", REG_BINARY, "one"}},
        {OperationKind::SetValue, Root, {"second", REG_BINARY, "two"}}}, "S-1-5-21-1", "machine"); QVERIFY(Plan);
    auto Saved = Store.SavePrepared(Plan.value); QVERIFY(Saved);
    RegistryHistoryRecord Stale;
    const auto Applied = Store.Apply(Saved.value, [&](const RegistryStep& Step, int Index) {
        if (Index == 0) Stale = Saved.value;
        return Transaction.ApplyStep(Step);
    });
    QCOMPARE(Applied.error, quint32(0)); QCOMPARE(Saved.value.completed, 2); QCOMPARE(Stale.inFlight, 0);
    const auto Sequence = Saved.value.sequence; const auto Before = Backend.mutations_;
    QCOMPARE(Store.Recover(Stale, Transaction), quint32(0));
    QCOMPARE(Stale.completed, 2); QCOMPARE(Stale.inFlight, -1); QCOMPARE(Stale.state, QString("committed"));
    QCOMPARE(Stale.sequence, Sequence); QCOMPARE(Backend.mutations_, Before);
    const auto Reloaded = Store.Read(Plan.value.id); QVERIFY(Reloaded);
    QCOMPARE(Reloaded.value.completed, 2); QCOMPARE(Reloaded.value.state, QString("committed"));
}