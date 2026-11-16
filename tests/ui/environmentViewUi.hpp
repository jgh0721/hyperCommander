#pragma once
/// UIs
#include "HyperCommander/UIs/Components/cmpEnvironmentView.hpp"
#include "HyperCommander/UIs/Components/cmpEnvironmentDetailView.hpp"
/// 고유 라이브러리
#include "UniqueLibs/system/environmentStorage.hpp"
/// 기타 헤더파일
#include "tests/registry/fakeRegistryBackend.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QTest>
#include <QSignalSpy>
#include <QClipboard>
#include <QtitanGrid.h>

inline void EnvironmentViewSmoke()
{
    using namespace hc::environment;
    auto Backend = std::make_shared<FakeRegistryBackend>();
    const QString Sid = "S-1-5-21-1-2-3-1001";
    const auto User = EnvironmentLocation(Scope::User, Sid), System = EnvironmentLocation(Scope::System);
    Backend->Seed(User); Backend->Seed(System);
    Backend->Write(User, EncodeEnvironmentValue({L"Path", L"C:\\Synthetic\\bin;C:\\Synthetic\\tools", ValueType::ExpandString, Scope::User}));
    Backend->Write(User, EncodeEnvironmentValue({L"DEMO_SECRET", L"synthetic-hidden", ValueType::String, Scope::User}));
    Backend->Write(User, EncodeEnvironmentValue({L"Ordinary", L"synthetic-ordinary", ValueType::String, Scope::User}));
    const int Before = Backend->mutations_;
    MergeInput Defaults; Defaults.coreSystem_.push_back({L"SystemRoot", L"C:\\SyntheticWindows"});
    EnvironmentSession Session(Backend, Defaults, Sid);
    CEnvironmentView View(nullptr, &Session); View.resize(1100, 720); View.show(); View.activateWindow();
    QTRY_VERIFY(Session.Ready()); QTRY_COMPARE(View.Model()->rowCount(), 3);
    auto* Grid = View.findChild<Qtitan::TreeGrid*>("hc.environment.variables"); QVERIFY(Grid);
    auto* Tree = Grid->view<Qtitan::GridTreeView>(); QVERIFY(Tree);
    View.Select("Path"); QTRY_VERIFY(View.Current()); QCOMPARE(View.Current()->name_, QString("Path"));
    const auto Parent = Tree->focusedRow().modelIndex(0);
    const bool WasExpanded = Tree->modelController()->isExpanded(Parent);
    QVERIFY(View.ExecuteCommand("hc.environment.activate")); QCOMPARE(Tree->modelController()->isExpanded(Parent), !WasExpanded);
    View.Select("DEMO_SECRET"); QTRY_COMPARE(View.Current()->name_, QString("DEMO_SECRET"));
    QVERIFY(View.Current()->masked_); View.ExecuteCommand("hc.systemEnvironment.copy");
    QVERIFY(!QApplication::clipboard()->text().contains("synthetic-hidden"));
    View.SetFilter("synthetic-hidden"); QTRY_COMPARE(Tree->model()->rowCount(), 0);
    View.SetFilter({}); View.Select("Ordinary"); QTRY_COMPARE(View.Current()->name_, QString("Ordinary"));
    CEnvironmentDetailView Detail; Detail.SetSource(&View); Detail.resize(550, 600); Detail.show();
    QVERIFY(Detail.Source() == &View); QVERIFY(Detail.RequestClose());
    QSignalSpy Commands(&View, &CEnvironmentView::sigCommandRequested);
    QVERIFY(View.ExecuteCommand("hc.viewer.quick")); QCOMPARE(Commands.size(), 1);
    Tree->getColumn(0)->setWidth(247);
    const auto State = View.CaptureState(); QVERIFY(!QJsonDocument(State).toJson().contains("synthetic-ordinary"));
    View.SetScope(EnvironmentViewScope::Effective); View.Back(); QCOMPARE(View.Scope(), EnvironmentViewScope::User);
    View.RestoreState(State); QTRY_COMPARE(View.Current()->name_, QString("Ordinary"));
    QCOMPARE(Tree->getColumn(0)->width(), 247);
    ProcessEnvironment Failure; Failure.status_ = ProcessReadStatus::Denied; Failure.nativeError_ = ERROR_ACCESS_DENIED;
    View.SetProcess({123, 456}, Failure); QCOMPARE(View.Model()->rowCount(), 0);
    QCOMPARE(Backend->mutations_, Before);
    View.CloseView(); View.close(); Detail.close();
}
