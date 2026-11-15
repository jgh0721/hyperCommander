#pragma once

#include "UniqueLibs/registry/registryHistoryModel.hpp"
#include <QAbstractItemModelTester>

inline void RegistryHistoryModelCases()
{
    using namespace hc::registry;
    RegistryHistoryModel Model;
    QAbstractItemModelTester Tester(&Model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    RegistryHistoryRecord First, Second, Undo;
    First.plan.id = "first"; First.plan.createdUtc = "2026-11-15T01:00:00.000Z";
    First.sequence = 10; First.state = "committed"; First.completed = 1;
    RegistryStep Step; Step.location = RegistryLocation::Parse("HKCU\\Software\\HyperCommanderTests").value();
    Step.name = "test"; Step.after = RegistryValue{"test", REG_BINARY, "test"}; First.plan.steps.append(Step);
    Second = First; Second.plan.id = "second"; Second.sequence = 20;
    Model.SetRecords({First, Second});
    QCOMPARE(Model.rowCount(), 2); QCOMPARE(Model.index(0, 0).data(RegistryHistoryModel::RecordIdRole).toString(), QString("second"));
    QVERIFY(Model.UndoCandidate()); QCOMPARE(Model.UndoCandidate()->plan.id, QString("second")); QVERIFY(!Model.RedoCandidate());
    Second.state = "undone"; Second.redoAllowed = true;
    Undo = Second; Undo.plan.id = "undo-second"; Undo.plan.parentId = "second"; Undo.plan.purpose = "undo";
    Undo.state = "committed"; Undo.sequence = 30;
    Model.SetRecords({Second, Undo, First});
    QCOMPARE(Model.UndoCandidate()->plan.id, QString("first"));
    QVERIFY(Model.RedoCandidate()); QCOMPARE(Model.RedoCandidate()->first.plan.id, QString("second"));
    Second.redoAllowed = false; Model.SetRecords({Second, Undo, First}); QVERIFY(!Model.RedoCandidate());
    First.inFlight = 0; Model.SetRecords({First}); QVERIFY(!Model.UndoCandidate());
    QVERIFY(!Model.Record({})); QCOMPARE(Model.rowCount(Model.index(0, 0)), 1);
    const auto Child = Model.index(0, 2, Model.index(0, 0));
    QVERIFY(Child.isValid()); QCOMPARE(Model.parent(Child), Model.index(0, 0));
    QCOMPARE(Model.rowCount(Child), 0); QCOMPARE(Model.Record(Child)->plan.id, QString("first"));
    QCOMPARE(Model.SelectedStep(Child)->name, QString("test"));
}