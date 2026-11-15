#include "stdafx.h"
#include "cmpRegistryDetailView.hpp"
#include "HyperCommander/UIs/registryLinkPane.hpp"

/// 모듈
/// UIs
#include "cmpRegistryView.hpp"
#include "HyperCommander/UIs/registryEditors.hpp"
#include "HyperCommander/UIs/registryPolicyEditor.hpp"
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/qtMessageDialog.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/system/systemSupport.hpp"
/// 공통 라이브러리
#include "commonLibs/system/registryEnvironment.hpp"
/// 외부 라이브러리
#include <fmwidgets/Label.h>
#include <fmwidgets/SegmentedControl.h>
#include <fmwidgets/ScrollPanel.h>
#include <fmwidgets/ContentStack.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QVBoxLayout>
#include <QStackedWidget>
#include <QFileInfo>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryDetail, "hc.ui.registryDetail")
using namespace hc::registry;

CRegistryDetailView::CRegistryDetailView(QWidget* Parent) : CViewT(Parent)
{
    viewMode_ = VM_SYSTEM; setObjectName("hc.registry.detail");
    auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(12, 8, 12, 8);
    const auto Label = [this] {
        auto* Result = new fm::ui::Label({}, fm::ui::Label::Help, this);
        Result->setTextFormat(Qt::PlainText); Result->setWordWrap(true); Result->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard); return Result;
    };
    title_ = Label(); subtitle_ = Label(); notice_ = Label();
    Layout->addWidget(title_); Layout->addWidget(subtitle_); Layout->addWidget(notice_); notice_->hide();
    tabs_ = new fm::ui::SegmentedControl({tr("설명"), tr("연결"), tr("데이터"), tr("키 정보")}, this);
    tabs_->setObjectName("hc.registry.detailTabs"); Layout->addWidget(tabs_);
    pages_ = new QStackedWidget(this); Layout->addWidget(pages_, 1);
    description_ = Label(); links_ = new RegistryLinkPane(this); keyInfo_ = Label();
    links_->SetCacheDirectory(QDir(QFileInfo(hc::settings::AppSettings().FileName()).absolutePath()).filePath("registry-cache"));
    links_->SetNavigator([this](const RegistryLink& Link) { emit LinkRequested(Link); });
    editor_ = new RegistryValueEditor(this); policyEditor_ = new RegistryPolicyEditor(this);
    dataPages_ = new fm::ui::ContentStack(this); dataPages_->addWidget(editor_); dataPages_->addWidget(policyEditor_);
    connect(policyEditor_, &RegistryPolicyEditor::SaveRequested, this, &CRegistryDetailView::PolicySaveRequested);
    connect(policyEditor_, &RegistryPolicyEditor::CancelRequested, this, [this] { if (busy_) emit PolicyCancelRequested(); else Update(); });
    for (QWidget* Page : std::initializer_list<QWidget*>{description_, links_, dataPages_, keyInfo_}) {
        auto* Scroll = new fm::ui::ScrollPanel(pages_); Scroll->setContentWidget(Page); pages_->addWidget(Scroll);
        if (auto* LabelPage = qobject_cast<QLabel*>(Page)) LabelPage->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    }
    connect(tabs_, &fm::ui::SegmentedControl::currentIndexChanged, pages_, &QStackedWidget::setCurrentIndex);
    connect(tabs_, &fm::ui::SegmentedControl::currentIndexChanged, this, [this](int Index) { if (Index == 1) links_->Activate(); });
    tabs_->setCurrentIndex(0);
    connect(editor_, &RegistryValueEditor::CancelRequested, this, [this] { Update(); });
    connect(editor_, &RegistryValueEditor::SaveRequested, this, [this](const RegistryValue& Original, const RegistryValue& Draft) {
        if (location_ && !busy_) emit SaveRequested(*location_, Original, Draft);
    });
    const auto Environment = QProcessEnvironment::systemEnvironment();
    editor_->SetPathResolver([Environment](const QString& Raw) {
        hc::ui::PathResolution Result; const auto Expanded = ExpandEnvironmentText(Raw, Environment); Result.expanded = Expanded.text;
        if (Raw.isEmpty()) { Result.status = hc::ui::PathResolution::Status::Unresolved; Result.warning = tr("빈 항목 · 실행 프로그램의 현재 폴더에 따라 달라집니다."); }
        else if (Expanded.tooLarge || !Expanded.missing.isEmpty()) {
            Result.status = hc::ui::PathResolution::Status::Unresolved;
            Result.warning = Expanded.tooLarge ? tr("확장 결과가 너무 큽니다.") : tr("현재 프로세스 환경에 없는 변수: %1").arg(Expanded.missing.join(", "));
        } else if (!QDir::isAbsolutePath(Result.expanded)) { Result.status = hc::ui::PathResolution::Status::Unresolved; Result.warning = tr("상대 경로 · 실행 프로그램의 현재 폴더에 따라 달라집니다."); }
        else { Result.status = QFileInfo(Result.expanded).isDir() ? hc::ui::PathResolution::Status::Exists : hc::ui::PathResolution::Status::Missing; Result.warning = tr("현재 프로세스 환경 기준"); }
        return Result;
    });
    connect(editor_, &RegistryValueEditor::ExpansionRequested, this, [this, Environment](const QString& Raw) {
        const auto Generation = ++expansionGeneration_;
        hc::sys::RunAsync(this, [Raw, Environment] { return ExpandEnvironmentText(Raw, Environment); },
            [this, Generation](const EnvironmentExpansion& Result) {
                if (Generation != expansionGeneration_) return;
                const auto Warning = Result.tooLarge ? tr("확장 결과가 너무 큽니다.") : !Result.missing.isEmpty()
                    ? tr("현재 프로세스 환경에 없는 변수: %1").arg(Result.missing.join(", ")) : QString();
                editor_->SetExpandedText(tr("%1 (현재 프로세스 환경 기준)").arg(Result.text), Warning);
            });
    });
    Update();
}
CRegistryView* CRegistryDetailView::Source() const { return source_; }
void CRegistryDetailView::SetSource(CRegistryView* Source)
{
    if (source_ == Source) return;
    if (source_) disconnect(source_, nullptr, this, nullptr);
    source_ = Source;
    if (Source) {
        connect(Source, &CRegistryView::sigCursorChanged, this, &CRegistryDetailView::Update);
        connect(Source, &CRegistryView::sigSummaryChanged, this, &CRegistryDetailView::Update);
        connect(Source, &QObject::destroyed, this, &CRegistryDetailView::Update);
    }
    Update();
}
void CRegistryDetailView::Update()
{
    if (busy_ || editor_->IsModified() || policyEditor_->IsModified()) {
        notice_->setText(busy_ ? tr("저장 중입니다…") : tr("편집 중인 값의 변경을 유지하고 있습니다. 저장하거나 취소한 뒤 다른 항목을 표시합니다."));
        notice_->show(); return;
    }
    notice_->hide(); dataPages_->setCurrentWidget(editor_);
    const auto Index = source_ ? source_->CurrentSource() : QModelIndex();
    if (!Index.isValid()) {
        shownId_.clear(); location_.reset(); title_->setText(tr("컴퓨터")); subtitle_->clear();
        description_->setText(tr("레지스트리의 키 또는 값을 선택하세요.")); links_->SetMessage({}); keyInfo_->clear();
        editor_->setEnabled(false); return;
    }
    if (const auto Policy = source_->CurrentPolicy()) {
        shownId_ = Index.data(RegistryTreeModel::IdRole).toString(); location_.reset();
        title_->setText(Policy->policy.title); subtitle_->setText(RegistryPolicyDefinition::StateText(Policy->observation.state) + " · " + (Policy->machine ? tr("컴퓨터") : tr("사용자")));
        description_->setText(tr("그룹 정책 · ADMX") + "\n\n" + Policy->policy.explanation
            + "\n\n" + tr("지원 대상: %1\n정의: %2\n%3").arg(Policy->policy.supportedOn, Policy->policy.sourcePath, Policy->observation.diagnostic)
            + "\n" + (Policy->observation.local ? tr("로컬 Registry.pol에 대응 항목 있음 · 최종 적용 원본은 별도 확인이 필요합니다.") : tr("실제 값에서 발견 · 적용 원본은 알 수 없습니다.")));
        links_->SetMessage(Policy->policy.key); QStringList Keys;
        for (auto I = Policy->observation.keys.cbegin(); I != Policy->observation.keys.cend(); ++I) {
            Keys.append((Policy->machine ? "HKLM\\" : "HKCU\\") + I.key());
            for (const auto& Value : I->value.values) Keys.append("  " + (Value.name.isEmpty() ? tr("(기본값)") : Value.name));
        }
        keyInfo_->setText(Keys.join('\n')); policyEditor_->SetPolicy(*Policy); dataPages_->setCurrentWidget(policyEditor_); return;
    }
    if ((source_->PoliciesShown() || source_->AutorunsShown() || source_->SearchShown()) && Index.data(RegistryResultModel::GroupRole).toBool()) {
        shownId_.clear(); location_.reset(); title_->setText(Index.data().toString()); subtitle_->clear();
        description_->setText(tr("아래 항목을 선택하세요.")); links_->SetMessage({}); keyInfo_->clear(); editor_->setEnabled(false); return;
    }
    if (const auto Record = source_->CurrentHistory()) {
        shownId_ = Record->plan.id; location_.reset(); title_->setText(tr("변경 기록"));
        subtitle_->setText(Record->plan.createdUtc + " · " + Record->state);
        QStringList Lines{tr("백업: %1").arg(Record->directory), tr("완료: %1 / %2단계").arg(Record->completed).arg(Record->plan.steps.size())};
        for (int Row = 0; Row < Record->plan.steps.size() && Row < 100; ++Row) {
            const auto& Step = Record->plan.steps[Row];
            Lines.append(Step.location.Path() + (Step.kind == StepKind::Value ? " › " + (Step.name.isEmpty() ? tr("(기본값)") : Step.name) : QString()));
            if (Step.kind == StepKind::PolicyFile) Lines.append(tr("Registry.pol · 원본/변경 파일이 백업에 포함되어 있습니다."));
            if (Step.kind == StepKind::Value) {
                const auto Format = [](const std::optional<RegistryValue>& Value) {
                    if (!Value) return tr("(없음)");
                    const auto Text = RegistryValueCodec::DecodeText(*Value);
                    const auto Number = RegistryValueCodec::DecodeNumber(*Value);
                    return Text.lossless ? Text.text.left(2048) : Number ? QString::number(Number.value) : QString::fromLatin1(Value->bytes.left(128).toHex(' '));
                };
                Lines.append(Format(Step.before) + QStringLiteral(" → ") + Format(Step.after));
            }
        }
        if (Record->plan.steps.size() > 100) Lines.append(tr("나머지 단계는 백업 파일에 보존되어 있습니다."));
        if (Record->inFlight >= 0 || Record->error) Lines.append(tr("복구 확인이 필요합니다 (오류 %1). 백업과 현재 값을 비교하세요.").arg(Record->error));
        description_->setText(Lines.join('\n')); links_->SetMessage(tr("변경 기록 항목에는 연결 정보가 없습니다."));
        keyInfo_->setText(tr("기록 ID: %1\n백업: %2").arg(Record->plan.id, Record->directory)); editor_->setEnabled(false); return;
    }
    const auto Location = RegistryLocation::Parse(Index.data(RegistryTreeModel::LocationRole).toString(), source_->SelectedRegistryView());
    if (!Location) return;
    location_ = Location; shownId_ = Index.data(RegistryTreeModel::IdRole).toString();
    const bool Value = Index.data(RegistryTreeModel::KindRole).toInt() == int(RegistryTreeModel::Kind::Value);
    const auto Name = Index.data(RegistryTreeModel::ValueNameRole).toString();
    title_->setText(Value ? (Name.isEmpty() ? tr("(기본값)") : Name) : Index.data().toString());
    subtitle_->setText(Location->Path() + (Location->view == View::Native64 ? tr(" · 64비트") : tr(" · 32비트")));
    const auto Explanation = source_->Descriptions()->Find(*Location, Value ? std::optional<QString>(Name) : std::nullopt,
        Value ? std::optional<quint32>(Index.data(RegistryTreeModel::ValueTypeRole).toUInt()) : std::nullopt);
    QString DescriptionText = Explanation.Known()
        ? (Explanation.policyIndexes.isEmpty() ? tr("일반 설정 · 내장 카탈로그") : tr("그룹 정책 · ADMX")) + '\n' + Explanation.title + "\n\n" + Explanation.text
        : source_->Descriptions()->Loading() ? tr("설명을 불러오는 중…") : tr("알려진 설명이 없습니다.");
    if (!Explanation.warnings.isEmpty()) DescriptionText += "\n\n" + Explanation.warnings.join('\n');
    if (!Explanation.sources.isEmpty()) DescriptionText += "\n\n" + tr("설명 출처") + ":\n" + Explanation.sources.join('\n');
    if (const auto Autorun = source_->CurrentAutorun()) DescriptionText = tr("자동 실행 · 보안") + '\n' + Autorun->category + '\n' + Autorun->status + '\n' + Autorun->publisher + '\n' + Autorun->warning + "\n\n" + DescriptionText;
    description_->setText(DescriptionText);
    links_->SetSelection(*Location, Value ? std::optional<RegistryValue>(RegistryValue{Name, Index.data(RegistryTreeModel::ValueTypeRole).toUInt(), Index.data(RegistryTreeModel::RawValueRole).toByteArray()}) : std::nullopt);
    QString KeyText = tr("경로: %1\n보기: %2비트").arg(Location->Path()).arg(Location->view == View::Native64 ? 64 : 32);
    if (const auto* Entry = source_->Service()->Cached(*Location); Entry && Entry->loaded) {
        if (Entry->snapshot) {
            const auto& Info = Entry->snapshot.value.info;
            KeyText += tr("\n하위 키: %1 · 값: %2\n링크 키: %3").arg(Info.subkeyCount).arg(Info.valueCount).arg(Info.link ? tr("예") : tr("아니오"));
            if (Info.lastWrite) KeyText += tr("\n마지막 쓴 시각: %1").arg(hc::sys::FileTimeText(Info.lastWrite));
        } else KeyText += tr("\n읽기 실패: %1").arg(hc::sys::Win32ErrorText(Entry->snapshot.error));
    } else source_->Service()->Request(*Location);
    keyInfo_->setText(KeyText); editor_->setEnabled(Value);
    if (Value) {
        RegistryValue Raw{Name, Index.data(RegistryTreeModel::ValueTypeRole).toUInt(), Index.data(RegistryTreeModel::RawValueRole).toByteArray()};
        editor_->SetValue(Raw, Name.compare("Path", Qt::CaseInsensitive) == 0 && (Raw.type == REG_SZ || Raw.type == REG_EXPAND_SZ));
    }
}
bool CRegistryDetailView::RequestClose()
{
    if (busy_) { notice_->setText(tr("저장이 끝난 뒤 닫을 수 있습니다.")); notice_->show(); return false; }
    if (!editor_->IsModified() && !policyEditor_->IsModified()) return true;
    if (hc::ui::MessageDialog::question(this, tr("레지스트리 편집"), tr("저장하지 않은 변경을 버리고 닫을까요?"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return false;
    editor_->Discard(); policyEditor_->Discard(); return true;
}
void CRegistryDetailView::CloseView() { if (source_) disconnect(source_, nullptr, this, nullptr); source_.clear(); }
void CRegistryDetailView::EnsureKeyboardFocusOnView() const { if (pages_->currentIndex() == 2) { if (dataPages_->currentWidget() == policyEditor_) policyEditor_->setFocus(); else editor_->FocusEditor(); } else pages_->currentWidget()->setFocus(); }
void CRegistryDetailView::ShowData(bool Focus) { tabs_->setCurrentIndex(2); if (Focus) EnsureKeyboardFocusOnView(); }
void CRegistryDetailView::ShowDescription() { tabs_->setCurrentIndex(0); }
void CRegistryDetailView::ShowLinks(bool Follow) { tabs_->setCurrentIndex(1); if (Follow) links_->Follow(); }
void CRegistryDetailView::SetBusy(bool Busy) { busy_ = Busy; editor_->SetBusy(Busy); policyEditor_->SetBusy(Busy); if (Busy) Update(); }
void CRegistryDetailView::PolicySaveFinished(const QString& Error)
{
    SetBusy(false);
    if (!Error.isEmpty()) { policyEditor_->ShowError(Error); notice_->setText(tr("저장에 실패했습니다. 편집 내용은 유지됩니다.")); notice_->show(); return; }
    policyEditor_->Discard(); notice_->hide();
    if (source_) source_->Refresh();
}
void CRegistryDetailView::SaveFinished(const RegistryValue& Value, const QString& Error)
{
    SetBusy(false);
    if (!Error.isEmpty()) { editor_->ShowError(Error); notice_->setText(tr("저장에 실패했습니다. 편집 내용은 유지됩니다.")); notice_->show(); return; }
    editor_->AcceptSaved(Value); notice_->hide();
    if (source_ && location_) source_->Service()->Invalidate(*location_);
}
void CRegistryDetailView::SetDescription(const QString& Text) { description_->setText(Text); }
void CRegistryDetailView::SetLinks(const QString& Text) { links_->SetMessage(Text); }
