#include "stdafx.h"
#include "registryLocationMenu.hpp"

/// 모듈
/// UIs
#include "cmpRegistryView.hpp"
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
#include <fmwidgets/Button.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAction>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryLocations, "hc.ui.registryLocations")
using namespace hc::registry;

RegistryLocationMenu::RegistryLocationMenu(CRegistryView* View, QWidget* Parent)
    : fm::ui::Menu(Parent), view_(View)
{
    setObjectName("hc.registry.locations");
    connect(this, &QMenu::aboutToShow, this, [this] {
        RefreshFavorite(); rebuild();
        for (auto* Action : actions()) {
            const auto Path = Action->property("registryPath").toString();
            if (const auto Location = RegistryLocation::Parse(Path, hc::registry::View(view_->CaptureState()["view"].toInt())))
                view_->Service()->Request(*Location, true);
        }
    });
    connect(view_->Service(), &RegistryService::Changed, this, [this](const QString& Id) {
        if (Id == RegistryFavorites::SharedLocation().Id()) {
            updateStar();
            emit view_->sigSummaryChanged();
            if (isVisible()) rebuild();
        } else if (isVisible()) updateLocations();
    });
    connect(view_, &CRegistryView::sigSummaryChanged, this, [this] { updateStar(); });
    RefreshFavorite();
}
void RegistryLocationMenu::RefreshFavorite()
{
    view_->Service()->Request(RegistryFavorites::SharedLocation(), true);
}
void RegistryLocationMenu::updateStar()
{
    auto* Star = view_->findChild<fm::ui::Button*>("hc.registry.favorite"); if (!Star) return;
    const auto Current = RegistryLocation::Parse(view_->PathText());
    const auto* Cache = view_->Service()->Cached(RegistryFavorites::SharedLocation());
    bool Found = false;
    if (Current && Cache && Cache->loaded && Cache->snapshot)
        for (const auto& Favorite : RegistryFavorites::Decode(Cache->snapshot.value, Current->view))
            Found |= Favorite.location.Id() == Current->Id();
    Star->setText(Found ? QStringLiteral("★") : QStringLiteral("☆"));
    Star->setEnabled(Current.has_value());
    Star->setToolTip(Found ? tr("현재 키를 즐겨찾기에서 빼기") : tr("현재 키를 즐겨찾기에 추가"));
}
void RegistryLocationMenu::addLocation(const QString& Label, const RegistryLocation& Location)
{
    auto* Action = addAction(Label); Action->setProperty("registryPath", Location.Path());
    Action->setProperty("registryLabel", Label); Action->setToolTip(Location.Path());
    connect(Action, &QAction::triggered, this, [this, Location] { view_->Navigate(Location.Path()); });
}
void RegistryLocationMenu::updateLocations()
{
    const auto SelectedView = hc::registry::View(view_->CaptureState()["view"].toInt());
    for (auto* Action : actions()) {
        const auto Location = RegistryLocation::Parse(Action->property("registryPath").toString(), SelectedView);
        if (!Location) continue;
        const auto* Cache = view_->Service()->Cached(*Location); QString Status = tr("확인 중…");
        if (Cache && Cache->loaded) {
            if (Cache->snapshot) Status = tr("값 %1 · 하위 키 %2").arg(Cache->snapshot.value.values.size()).arg(Cache->snapshot.value.keys.size());
            else if (Cache->snapshot.error == ERROR_FILE_NOT_FOUND || Cache->snapshot.error == ERROR_PATH_NOT_FOUND) Status = tr("이 PC에 없음");
            else Status = tr("읽기 실패 (%1)").arg(Cache->snapshot.error);
        }
        Action->setText(Action->property("registryLabel").toString() + "  —  " + Status);
    }
}
void RegistryLocationMenu::rebuild()
{
    const auto ActivePath = activeAction() ? activeAction()->property("registryPath").toString() : QString();
    clear(); addSection(tr("즐겨찾기 · regedit와 공유"));
    const auto SelectedView = hc::registry::View(view_->CaptureState()["view"].toInt());
    const auto* Cache = view_->Service()->Cached(RegistryFavorites::SharedLocation());
    if (Cache && Cache->loaded && Cache->snapshot) {
        const auto Favorites = RegistryFavorites::Decode(Cache->snapshot.value, SelectedView); int Number = 0;
        for (const auto& Favorite : Favorites) {
            const auto Prefix = Number < 10 ? QStringLiteral("★ &%1 ").arg((++Number) % 10) : QStringLiteral("★ ");
            addLocation(Prefix + QString(Favorite.name).replace('&', "&&"), Favorite.location);
        }
        if (Favorites.isEmpty()) addAction(tr("저장된 즐겨찾기 없음"))->setEnabled(false);
    } else {
        const bool Absent = Cache && Cache->loaded && (Cache->snapshot.error == ERROR_FILE_NOT_FOUND || Cache->snapshot.error == ERROR_PATH_NOT_FOUND);
        auto* Status = addAction(Absent ? tr("저장된 즐겨찾기 없음") : Cache && Cache->loaded ? tr("즐겨찾기 읽기 실패 (%1)").arg(Cache->snapshot.error) : tr("즐겨찾기 불러오는 중…"));
        Status->setEnabled(false);
    }
    const auto* Star = view_->findChild<fm::ui::Button*>("hc.registry.favorite");
    auto* Toggle = addAction(Star ? Star->toolTip() : tr("현재 키를 즐겨찾기에 추가"));
    Toggle->setEnabled(RegistryLocation::Parse(view_->PathText()).has_value());
    connect(Toggle, &QAction::triggered, view_, [this] { emit view_->sigCommandRequested("hc.registry.favorite"); });
    struct Entry { const char* group; const char* label; const char* path; };
    // Navigation landmarks, not fabricated values or example descriptions.
    static constexpr Entry Entries[]{
        {"알려진 위치 · 정책", "컴퓨터 정책", "HKLM\\SOFTWARE\\Policies"},
        {nullptr, "사용자 정책", "HKCU\\Software\\Policies"},
        {nullptr, "Windows 업데이트 정책", "HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate"},
        {nullptr, "시스템 정책 (UAC 등)", "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System"},
        {"알려진 위치 · 자동 실행", "Run (모든 사용자)", "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"},
        {nullptr, "Run (현재 사용자)", "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"},
        {nullptr, "Winlogon", "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon"},
        {nullptr, "Image File Execution Options", "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options"},
        {nullptr, "Session Manager (BootExecute)", "HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager"},
        {nullptr, "시작 앱 사용 여부 (StartupApproved)", "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved"},
        {"알려진 위치 · 파일 형식 · COM", ".txt 파일 형식", "HKCR\\.txt"},
        {nullptr, "사용자 기본 앱 (.txt)", "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.txt\\UserChoice"},
        {nullptr, "바로 가기 메뉴 처리기 (모든 파일)", "HKCR\\*\\shellex\\ContextMenuHandlers"},
        {nullptr, "COM 클래스 (CLSID)", "HKCR\\CLSID"},
        {"알려진 위치 · 탐색기 · 셸", "탐색기 고급 설정", "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced"},
        {nullptr, "프로그램 제거 목록", "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
        {nullptr, "App Paths", "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths"},
        {nullptr, "바탕 화면 설정", "HKCU\\Control Panel\\Desktop"},
        {"알려진 위치 · 시스템", "서비스 · 드라이버", "HKLM\\SYSTEM\\CurrentControlSet\\Services"},
        {nullptr, "환경 변수 (시스템)", "HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"},
        {nullptr, "환경 변수 (사용자)", "HKCU\\Environment"}
    };
    for (const auto& Entry : Entries) {
        if (Entry.group) { addSeparator(); addSection(tr(Entry.group)); }
        addLocation(tr(Entry.label), RegistryLocation::Parse(QString::fromLatin1(Entry.path), SelectedView).value());
    }
    updateLocations();
    if (!ActivePath.isEmpty()) for (auto* Action : actions())
        if (Action->property("registryPath").toString() == ActivePath) { setActiveAction(Action); break; }
}
