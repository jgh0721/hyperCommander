#pragma once
#include "dialogShell.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include "UniqueLibs/permissionService.hpp"
#include "UniqueLibs/permissionJobController.hpp"
#include <QtWidgets>
#include <QtConcurrent>
#include <fmwidgets/Button.h>
#include <fmwidgets/Card.h>
#include <fmwidgets/DialogHeader.h>
namespace hc::ui {
struct PermissionRequest {
    QString screen_ = "A01", source_, destination_, reason_;
    QJsonObject elevatedRequest_;
    bool canSkip_ = false, canCopyOnly_ = false, canRetry_ = false;
    bool canElevate_=false,canApplyAll_=false;
    QString jobId_;
};
class PermissionDialog final : public DialogShell {
public:
    enum Decision { None, Succeeded, Skip, CopyOnly, Retry, Edit, Alternate, Elevate };
    explicit PermissionDialog(PermissionRequest Request, QWidget* Parent = nullptr)
        : DialogShell(tr("권한 확인"), Parent), request_(std::move(Request)) {
        setObjectName("hc.permission." + request_.screen_); resize(720, 480);
        const QMap<QString, QString> Titles{{"A01",tr("작업 전 권한 확인")},{"A02",tr("복사 대상 쓰기 권한")},{"A03",tr("이동 원본 삭제 권한")},
            {"A04",tr("삭제 권한 확인")},{"A05",tr("이름 변경 권한 확인")},{"A06",tr("파일·폴더 생성 권한")},{"A07",tr("접근 거부 · 소유권")},{"A08",tr("관리자 요청 결과 확인")}};
        for (auto* Widget : findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) if (auto* Header = dynamic_cast<fm::ui::DialogHeader*>(Widget)) Header->setGlyph(fm::ui::glyph::Shield);
        SetTitle(Titles.value(request_.screen_, tr("권한 확인"))); SetSummary(request_.reason_);
        auto* Card = new fm::ui::Card(this); auto* Form = new QFormLayout(Card); BodyLayout()->addWidget(Card);
        const auto Field = [&](const QString& Label, const QString& Text) { auto* Value = new QLabel(Text, Card); Value->setTextFormat(Qt::PlainText); Value->setWordWrap(true); Value->setTextInteractionFlags(Qt::TextSelectableByMouse); Form->addRow(Label, Value); };
        if (request_.screen_ == "A05") {
            Field(tr("현재 이름"), QFileInfo(request_.source_).fileName()); Field(tr("새 이름"), QFileInfo(request_.destination_).fileName());
            Field(tr("위치"), QFileInfo(request_.source_).absolutePath()); Field(tr("필요한 권한"), tr("원본 이름 변경 · 대상 폴더에 항목 만들기"));
        } else { Field(tr("항목 / 원본"), request_.source_); Field(tr("대상 / 위치"), request_.destination_); }
        Field(tr("처리 범위"), request_.canElevate_?tr("현재 작업 %1 · 실패한 항목만 재개").arg(request_.jobId_):request_.elevatedRequest_.isEmpty() ? tr("현재 요청의 안내 · 지원 가능한 선택만 활성화") : tr("표시된 요청 1건 · 도우미는 완료 후 종료"));
        if (request_.screen_ == "A04") Field(tr("삭제 방식"), tr("최초 요청 방식 유지 · 휴지통/영구 삭제 자동 전환 없음"));
        if (request_.screen_ == "A03") Field(tr("원본"), tr("삭제 성공이 확인되기 전에는 원본 제거 완료로 표시하지 않습니다."));
        if (request_.screen_ == "A07") {
            Field(tr("소유권·ACL"), tr("별도 요청으로 원래 소유자·그룹·DACL을 백업한 뒤 변경합니다. 복원은 관리자 권한 설정의 이력에서 요청합니다. SACL과 하위 항목 권한은 변경하지 않습니다."));
            auto* Owner = new QLabel(tr("소유자 확인 중…"), this); Owner->setTextFormat(Qt::PlainText); Owner->setWordWrap(true); BodyLayout()->addWidget(Owner);
            auto* Watcher = new QFutureWatcher<permissions::Inspection>(this);
            connect(Watcher, &QFutureWatcher<permissions::Inspection>::finished, this, [Owner, Watcher] { const auto I=Watcher->result(); Owner->setText(tr("현재 소유자: %1\n%2").arg(I.owner_.isEmpty()?tr("확인 못함"):I.owner_, I.error_)); Watcher->deleteLater(); });
            Watcher->setFuture(QtConcurrent::run([Path=request_.source_] { return permissions::Inspect(Path); }));
        }
        same_ = new QCheckBox(tr("남은 권한 항목에도 같은 선택 적용(&A)"), this); same_->setObjectName("hc.permission.same"); same_->setEnabled(request_.canApplyAll_); same_->setVisible(request_.screen_=="A02" || request_.screen_=="A03" || request_.screen_=="A04"); BodyLayout()->addWidget(same_);
        auto* Explanation = new QLabel(tr("같은 선택은 현재 작업의 남은 선택 항목에만 2분간 적용됩니다. 관리자 토큰은 Windows UAC로 요청하며, 도우미는 항목 처리 후 종료합니다. SMB·플러그인 원본은 본체가 읽고 로컬 저장만 승격합니다."), this); Explanation->setWordWrap(true); BodyLayout()->addWidget(Explanation);
        auto* Actions = new QHBoxLayout; BodyLayout()->addLayout(Actions);
        const auto Button = [&](const QString& Text, const char* Name, bool Enabled, Decision Value) { auto* B = new fm::ui::Button(Text,this); B->setObjectName(Name); B->setAutoDefault(false); B->setEnabled(Enabled); Actions->addWidget(B); connect(B,&QPushButton::clicked,this,[this,Value]{ decision_=Value; QDialog::accept(); }); return B; };
        if (request_.screen_ != "A05" && request_.screen_ != "A06" && request_.screen_ != "A07") Button(tr("건너뛰기(&S)"),"hc.permission.skip",request_.canSkip_,Skip);
        if (request_.screen_=="A03") Button(tr("복사만 하기(&C) · 원본 유지"),"hc.permission.copyOnly",request_.canCopyOnly_,CopyOnly);
        if (request_.canRetry_ && request_.screen_!="A08") Button(tr("현재 권한으로 재시도(&R)"),"hc.permission.retry",true,Retry);
        if (request_.screen_=="A05" && !request_.canElevate_) Button(tr("이름 다시 입력(&E)"),"hc.permission.edit",true,Edit);
        if (request_.screen_=="A06" && !request_.canElevate_) {
            auto* Other = Button(tr("다른 위치(&O)…"),"hc.permission.alternate",true,None);
            disconnect(Other,nullptr,this,nullptr); connect(Other,&QPushButton::clicked,this,[this] { const auto Path=QFileDialog::getExistingDirectory(this,tr("생성할 다른 위치"),QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)); if(!Path.isEmpty()){ alternate_=Path; decision_=Alternate; QDialog::accept(); } });
        }
        if (request_.screen_=="A08") Button(tr("다시 확인(&R)"),"hc.permission.retry",request_.canRetry_,Retry);
        if (request_.screen_=="A07") {
            auto* Grant=new QCheckBox(tr("파일 읽기·쓰기·삭제 권한도 추가(&G)"),this);Grant->setObjectName("hc.permission.grantAccess");Grant->setEnabled(!QFileInfo(request_.source_).isDir());BodyLayout()->addWidget(Grant);
            auto* Take=Button(tr("소유권 가져오기(&T)"),"hc.permission.takeOwnership",permissions::Controller()&&filetools::LocalFolder(QFileInfo(request_.source_).absolutePath()),None);
            disconnect(Take,nullptr,this,nullptr);connect(Take,&QPushButton::clicked,this,[this,Grant] {
                if(hc::ui::MessageDialog::question(this,tr("소유권 변경"),tr("%1\n원래 보안 정보를 백업한 뒤 현재 사용자를 소유자로 설정합니다. 기존 거부 규칙은 유지합니다. 계속하시겠습니까?").arg(request_.source_))!=QMessageBox::Yes)return;
                if(auto* Jobs=permissions::Controller()){const auto Error=Jobs->SubmitSecurityJob(request_.source_,Grant->isChecked());if(Error.isEmpty()){decision_=Succeeded;QDialog::accept();}else SetMessage(Error,true);}
            });
        }
        const bool Allowed = hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject()["allowElevation"].toBool(true);
        const bool Supported = (request_.canElevate_||!request_.elevatedRequest_.isEmpty()) && Allowed && QFileInfo::exists(permissions::HelperPath());
        AcceptButton()->setText(request_.screen_=="A05"?tr("관리자 권한으로 이름 바꾸기(&R)"):tr("관리자 권한으로 실행(&U)")); SetAcceptEnabled(Supported);
        CancelButton()->setText(tr("닫기 / 취소"));
        SetMessage(Supported ? tr("실패한 현재 항목에 대해 Windows UAC를 요청합니다. 완료한 항목은 다시 실행하지 않으며 기존 충돌 선택을 유지합니다.")
            : tr("이 요청의 승격 실행이 지원되지 않거나 설정에서 꺼져 있습니다. 서버·플러그인 대상의 권한은 해당 제공자에서 확인해야 합니다."));
    }
    Decision ResultDecision() const { return decision_; }
    bool ApplyAll() const { return same_&&same_->isEnabled()&&same_->isChecked(); }
    QString AlternateFolder() const { return alternate_; }
protected:
    void accept() override {
        if (busy_ || !AcceptButton()->isEnabled()) return;
        if(request_.canElevate_){decision_=Elevate;QDialog::accept();return;}
        busy_=true; for(auto* B:findChildren<QAbstractButton*>()) B->setEnabled(false); SetMessage(tr("Windows 승인 또는 도우미 결과를 기다리고 있습니다…"));
        auto* Watcher=new QFutureWatcher<permissions::LaunchResult>(this); const auto Request=request_.elevatedRequest_;
        connect(Watcher,&QFutureWatcher<permissions::LaunchResult>::finished,this,[this,Watcher,Request] {
            const auto R=Watcher->result(); Watcher->deleteLater(); busy_=false;
            auto Records=permissions::History(); Records.append(QJsonObject{{"utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {"operation",Request["operation"]},
                {"target",QDir(Request["folder"].toString()).filePath(Request["name"].toString())}, {"code",int(R.code_)}, {"started",R.started_}});
            const bool Saved=permissions::SaveHistory(Records);
            if (!R.code_ && Saved) { decision_=Succeeded; QDialog::accept(); return; }
            SetTitle(tr("관리자 요청 결과 확인")); setObjectName("hc.permission.A08"); SetMessage(permissions::ResultText(R)+(Saved?QString():tr("\n요청 이력 저장 실패")),true);
            CancelButton()->setEnabled(true);
            // A started helper may have left a partial result; never blindly replay.
            if (!R.started_) { AcceptButton()->setText(tr("다시 요청(&R)")); AcceptButton()->setEnabled(true); }
        });
        Watcher->setFuture(QtConcurrent::run([Request]{ return permissions::Launch(Request); }));
    }
    void reject() override { if(!busy_) QDialog::reject(); }
    void closeEvent(QCloseEvent* Event) override { if(busy_) Event->ignore(); else DialogShell::closeEvent(Event); }
private:
    PermissionRequest request_;
    QCheckBox* same_=nullptr;
    bool busy_=false;
    Decision decision_=None;
    QString alternate_;
};
inline QWidget* PermissionSettingsPage(QWidget* Parent) {
    auto* Page=new QWidget(Parent); Page->setObjectName("hc.settings.permissions"); auto* Layout=new QVBoxLayout(Page);
    auto* Header=new fm::ui::DialogHeader(Page); Header->setTitle(QObject::tr("관리자 권한")); Header->setSubtitle(QObject::tr("실패한 항목의 로컬 쓰기·삭제와 별도 소유권 변경을 처리합니다.")); Layout->addWidget(Header);
    auto* Allow=new QCheckBox(QObject::tr("지원되는 작업에서 관리자 실행 버튼 사용"),Page); Allow->setObjectName("hc.permissions.allow"); Layout->addWidget(Allow);
    auto* State=new QLabel(QObject::tr("본체 토큰: %1\n도우미: %2\n수명: 요청 1건 종료 시 종료 · 장기 세션 없음").arg(permissions::Elevated()?QObject::tr("관리자"):QObject::tr("일반 사용자"),QFileInfo::exists(permissions::HelperPath())?QObject::tr("설치됨"):QObject::tr("설치되지 않음")),Page); State->setWordWrap(true); Layout->addWidget(State);
    auto* Form=new QFormLayout; Layout->addLayout(Form);
    for (const auto& Row:QList<QPair<QString,QString>>{{QObject::tr("도우미 유지"),QObject::tr("항목마다 · 최대 15분")},{QObject::tr("요청 유효 시간"),QObject::tr("2분 · 작업 시작 전 검사")},{QObject::tr("소유권 정책"),QObject::tr("별도 확인 · 소유자·그룹·DACL 백업 후 변경")},{QObject::tr("보호 위치 정책"),QObject::tr("파일·부모 ID 검증 · 원격 대상 승격 제외")}}) { auto* Value=new QLineEdit(Row.second,Page); Value->setReadOnly(true); Form->addRow(Row.first,Value); }
    auto* History=new QTableWidget(Page); History->setObjectName("hc.permissions.history"); History->setColumnCount(4); History->setHorizontalHeaderLabels({QObject::tr("시각"),QObject::tr("작업"),QObject::tr("대상"),QObject::tr("결과 코드")}); History->verticalHeader()->hide(); History->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); History->setEditTriggers(QAbstractItemView::NoEditTriggers); Layout->addWidget(History,1);
    const auto Reload=[History] { const auto Rows=permissions::History(); History->setRowCount(Rows.size()); for(int I=0;I<Rows.size();++I){const auto R=Rows[I].toObject();const QStringList Text{R["utc"].toString(),R["operation"].toString(),R["target"].toString(),R["error"].toString().isEmpty()?QString::number(R["code"].toInt()):R["error"].toString()}; for(int C=0;C<4;++C) History->setItem(I,C,new QTableWidgetItem(Text[C]));History->item(I,0)->setData(Qt::UserRole,R);} }; Reload();
    auto* Buttons=new QHBoxLayout; Layout->addLayout(Buttons); auto* Refresh=new fm::ui::Button(QObject::tr("이력 새로 고침"),Page); auto* Clear=new fm::ui::Button(QObject::tr("이력 지우기"),Page); Refresh->setAutoDefault(false); Clear->setAutoDefault(false); Buttons->addWidget(Refresh); Buttons->addWidget(Clear);
    QObject::connect(Refresh,&QPushButton::clicked,Page,Reload); QObject::connect(Clear,&QPushButton::clicked,Page,[Page,Reload] { if(permissions::SaveHistory({})) Reload(); else hc::ui::MessageDialog::warning(Page,QObject::tr("저장 실패"),QObject::tr("이력을 지우지 못했습니다.")); });
    auto* Restore=new fm::ui::Button(QObject::tr("선택 항목의 원래 권한 복원(&R)"),Page);Restore->setObjectName("hc.permissions.restore");Restore->setAutoDefault(false);Buttons->addWidget(Restore);
    QObject::connect(Restore,&QPushButton::clicked,Page,[Page,History] {
        if(History->currentRow()<0)return;const auto Record=History->item(History->currentRow(),0)->data(Qt::UserRole).toJsonObject();
        const auto Backup=QJsonDocument::fromJson(Record["detail"].toString().toUtf8()).object();
        if(Backup["state"]!="applied"&&Backup["state"]!="rollback-failed"){hc::ui::MessageDialog::information(Page,QObject::tr("권한 복원"),QObject::tr("복원 가능한 보안 백업이 있는 이력을 선택하세요."));return;}
        if(hc::ui::MessageDialog::question(Page,QObject::tr("원래 권한 복원"),QObject::tr("%1\n파일 ID와 현재 보안 정보가 백업 기록과 일치할 때만 복원합니다. 계속하시겠습니까?").arg(Backup["target"].toString()))!=QMessageBox::Yes)return;
        if(auto* Jobs=permissions::Controller()){const auto Error=Jobs->SubmitSecurityJob(Backup["target"].toString(),false,Backup);if(!Error.isEmpty())hc::ui::MessageDialog::warning(Page,QObject::tr("복원 요청 실패"),Error);}
    });
    auto* Browse=new fm::ui::Button(QObject::tr("백업 파일에서 복원…"),Page);Browse->setObjectName("hc.permissions.restoreBackup");Browse->setAutoDefault(false);Buttons->addWidget(Browse);
    QObject::connect(Browse,&QPushButton::clicked,Page,[Page] {
        const auto Path=QFileDialog::getOpenFileName(Page,QObject::tr("원래 권한 백업 선택"),QDir(settings::Paths().directory_).filePath("security-backups"),"*.json");if(Path.isEmpty())return;
        QFile File(Path);if(!File.open(QIODevice::ReadOnly)||File.size()>1024*1024){hc::ui::MessageDialog::warning(Page,QObject::tr("백업 읽기 실패"),QObject::tr("백업 파일을 읽지 못했습니다."));return;}
        auto Backup=QJsonDocument::fromJson(File.readAll()).object();
        // Original descriptors remain immutable. Resolve the latest independent
        // outcome file rather than requiring users to distinguish both records.
        if(Backup["state"]=="prepared") {
            const auto States=QDir(QFileInfo(Path).absolutePath()).entryInfoList({QFileInfo(Path).fileName()+".state-*.json"},QDir::Files,QDir::Time);
            for(const auto& State:States.mid(0,100)) {
                QFile Outcome(State.absoluteFilePath());if(!Outcome.open(QIODevice::ReadOnly)||Outcome.size()>1024*1024)continue;
                const auto Candidate=QJsonDocument::fromJson(Outcome.readAll()).object();
                if(Candidate["backupIdentity"]==Backup["backupIdentity"]&&Candidate["backupPath"]==Backup["backupPath"]){Backup=Candidate;break;}
            }
        }
        if(Backup["state"]!="applied"&&Backup["state"]!="rollback-failed"&&Backup["state"]!="restore-failed"){hc::ui::MessageDialog::warning(Page,QObject::tr("백업 확인"),QObject::tr("복원 가능한 변경 기록이 아닙니다."));return;}
        if(hc::ui::MessageDialog::question(Page,QObject::tr("원래 권한 복원"),QObject::tr("%1\n관리자 도우미가 백업 소유자·파일 ID·보안 정보의 일치를 확인한 뒤 복원합니다. 계속하시겠습니까?").arg(Backup["target"].toString()))!=QMessageBox::Yes)return;
        if(auto* Jobs=permissions::Controller()){const auto Error=Jobs->SubmitSecurityJob(Backup["target"].toString(),false,Backup);if(!Error.isEmpty())hc::ui::MessageDialog::warning(Page,QObject::tr("복원 요청 실패"),Error);}
    });
    auto* Note=new QLabel(QObject::tr("이력은 30일·최대 100건입니다. 지우기는 즉시 저장됩니다. 백업은 이력을 지워도 남습니다. 원래 권한 복원은 같은 파일 ID·보안 정보일 때만 수행합니다. 디렉터리 하위 권한·SACL 변경과 원격 대상의 승격은 지원하지 않습니다."),Page); Note->setWordWrap(true); Layout->addWidget(Note); return Page;
}
inline void ShowPermissionInspection(const QStringList& Paths,QWidget* Parent) {
    auto* Dialog=new DialogShell(QObject::tr("작업 전 권한 확인"),Parent); Dialog->setAttribute(Qt::WA_DeleteOnClose); Dialog->setObjectName("hc.permission.A01"); Dialog->resize(850,500); Dialog->AcceptButton()->hide(); Dialog->CancelButton()->setText(QObject::tr("닫기"));
    Dialog->SetMessage(QObject::tr("현재 토큰으로 열기 권한을 검사합니다. 이후 실제 작업의 성공을 보장하거나 자동 승격하지 않습니다."));
    auto* Table=new QTableWidget(Dialog); Table->setColumnCount(5); Table->setHorizontalHeaderLabels({QObject::tr("항목"),QObject::tr("소유자"),QObject::tr("읽기"),QObject::tr("삭제 핸들"),QObject::tr("하위 생성")}); Table->verticalHeader()->hide(); Table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); Table->setEditTriggers(QAbstractItemView::NoEditTriggers); Dialog->BodyLayout()->addWidget(Table);
    auto* Ownership=new fm::ui::Button(QObject::tr("선택 항목 소유권·복구 안내"),Dialog); Ownership->setAutoDefault(false); Dialog->BodyLayout()->addWidget(Ownership);
    QObject::connect(Ownership,&QPushButton::clicked,Dialog,[Dialog,Table] { if(Table->currentRow()<0)return; PermissionDialog Detail({"A07",Table->item(Table->currentRow(),0)->text(),{},QObject::tr("소유권 조회와 지원 범위 안내")},Dialog);Detail.exec(); });
    auto* Watcher=new QFutureWatcher<QList<permissions::Inspection>>(Dialog);
    QObject::connect(Watcher,&QFutureWatcher<QList<permissions::Inspection>>::finished,Dialog,[Dialog,Watcher,Table] { const auto Rows=Watcher->result();Table->setRowCount(Rows.size());int Denied=0;for(int I=0;I<Rows.size();++I){const auto& R=Rows[I];if(!R.remove_)++Denied; const QStringList Values{R.path_,R.owner_.isEmpty()?R.error_:R.owner_,R.read_?QObject::tr("가능"):QObject::tr("확인 실패"),R.remove_?QObject::tr("가능"):QObject::tr("확인 실패"),R.create_?QObject::tr("가능"):QObject::tr("불가 / 해당 없음")};for(int C=0;C<5;++C)Table->setItem(I,C,new QTableWidgetItem(Values[C]));}Dialog->SetSummary(QObject::tr("%1개 검사 · 삭제 핸들 확인 실패 %2개 · 파일 변경 없음").arg(Rows.size()).arg(Denied));Watcher->deleteLater();});
    Watcher->setFuture(QtConcurrent::run([Paths]{QList<permissions::Inspection> Results;for(const auto& P:Paths.mid(0,256))Results.append(permissions::Inspect(P));return Results;})); Dialog->show();
}
} // namespace hc::ui
