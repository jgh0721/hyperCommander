#include "stdafx.h"
#include "sqliteDatabaseInfo.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteApi.hpp"
#include "sqliteGlyphs.hpp"
#include "sqliteResultModel.hpp"
#include "sqliteRuntime.hpp"

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/StyleProps.h>
#include <fmstyle/ThemeColors.h>
#include <fmstyle/ThemeManager.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QTime>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace hc::sqlite
{
    namespace
    {
        using T = fm::style::Token;

        QString Size(qint64 Bytes) { return QLocale().formattedDataSize(qMax<qint64>(0, Bytes), 1, QLocale::DataSizeTraditionalFormat); }

        QString TaskTitle(const QString& Task)
        {
            if (Task == QLatin1String("integrity")) return QObject::tr("무결성 검사");
            if (Task == QLatin1String("foreignKeys")) return QObject::tr("외래 키 검사");
            if (Task == QLatin1String("analyze")) return QStringLiteral("ANALYZE");
            if (Task == QLatin1String("optimize")) return QObject::tr("최적화");
            if (Task == QLatin1String("vacuum")) return QStringLiteral("VACUUM");
            return QObject::tr("WAL 체크포인트");
        }

        // 작업 스레드: 새 연결에서 유지 관리 한 가지. (성공, 결과 한 줄).
        QPair<bool, QString> Maintain(const DatabaseInfoDialog::Opener& Open, const QString& Path, const QString& Task)
        {
            QString Error;
            auto Db = Open(&Error);
            if (!Db) return {false, Error};
            const auto Rows = [&](const QString& Sql, QStringList* Lines, int Limit) {
                Statement Query(*Db, Sql);
                if (!Query.Valid()) { Error = Query.Error(); return -1; }
                int Count = 0, Rc;
                while ((Rc = Query.Step()) == SQLITE_ROW) {
                    if (Lines && Lines->size() < Limit) {
                        QStringList Cells;
                        for (int Column = 0; Column < Query.ColumnCount(); ++Column) Cells.append(DisplayText(Query.Value(Column)));
                        Lines->append(Cells.join(QStringLiteral(" · ")));
                    }
                    ++Count;
                }
                if (Rc != SQLITE_DONE) { Error = Query.Error(); return -1; }
                return Count;
            };
            if (Task == QLatin1String("integrity")) {
                QStringList Lines;
                const int Count = Rows(QStringLiteral("PRAGMA integrity_check"), &Lines, 20);
                if (Count < 0) return {false, Error};
                const bool Ok = Count == 1 && Lines.value(0) == QLatin1String("ok");
                return {Ok, Ok ? QStringLiteral("ok") : QObject::tr("문제 %1건 — %2").arg(Count).arg(Lines.join(QStringLiteral(" / ")))};
            }
            if (Task == QLatin1String("foreignKeys")) {
                QStringList Lines;
                const int Count = Rows(QStringLiteral("PRAGMA foreign_key_check"), &Lines, 10);
                if (Count < 0) return {false, Error};
                return {Count == 0, Count == 0 ? QObject::tr("문제 없음") : QObject::tr("어긋난 행 %1개 — %2").arg(Count).arg(Lines.join(QStringLiteral(" / ")))};
            }
            if (Task == QLatin1String("analyze") || Task == QLatin1String("optimize")) {
                if (!Db->Exec(Task == QLatin1String("analyze") ? QStringLiteral("ANALYZE") : QStringLiteral("PRAGMA optimize"), &Error)) return {false, Error};
                return {true, QObject::tr("끝냈습니다")};
            }
            if (Task == QLatin1String("vacuum")) {
                const auto Before = QFileInfo(Path).size();
                if (!Db->Exec(QStringLiteral("VACUUM"), &Error)) return {false, Error};
                Db.reset();
                QFileInfo After(Path);
                After.refresh();
                return {true, QObject::tr("%1 → %2").arg(Size(Before), Size(After.size()))};
            }
            QStringList Lines;
            if (Rows(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"), &Lines, 1) < 0) return {false, Error};
            // busy · log · checkpointed. WAL 이 아니면 -1 · -1.
            const auto Parts = Lines.value(0).split(QStringLiteral(" · "));
            if (Parts.value(1) == QLatin1String("-1")) return {true, QObject::tr("WAL 모드가 아닙니다")};
            return {Parts.value(0) == QLatin1String("0"), QObject::tr("로그 %1쪽 중 %2쪽 옮김").arg(Parts.value(1), Parts.value(2))};
        }
    }

    DatabaseInfoDialog::DatabaseInfoDialog(Connection& Gui, Opener Open, const QString& Path, std::function<bool()> CommitPending, bool ReadOnly, QWidget* Parent)
        : QDialog(Parent), gui_(Gui), open_(std::move(Open)), path_(Path), commitPending_(std::move(CommitPending)), readOnly_(ReadOnly)
    {
        setObjectName(QStringLiteral("sqliteDatabaseInfo"));
        setWindowTitle(tr("데이터베이스 정보 — %1").arg(QFileInfo(Path).fileName()));
        resize(520, 720);
        watcher_ = new QFutureWatcher<QPair<bool, QString>>(this);
        connect(watcher_, &QFutureWatcher<QPair<bool, QString>>::finished, this, [this] {
            const auto Result = watcher_->result();
            finishTask(running_, Result.second, Result.first);
        });
        build();
    }

    DatabaseInfoDialog::~DatabaseInfoDialog()
    {
        // 유지 관리 연결이 닫힐 때까지 기다린다(VACUUM 도중 창을 닫아도 파일을 놓게).
        if (watcher_->isRunning()) watcher_->waitForFinished();
    }

    QString DatabaseInfoDialog::pragma(const QString& Name) const
    {
        Statement Query(gui_, QStringLiteral("PRAGMA main.%1").arg(Name));
        return Query.Valid() && Query.Step() == SQLITE_ROW ? Query.Value(0).toString() : QString();
    }

    void DatabaseInfoDialog::build()
    {
        auto* Layout = new QVBoxLayout(this);
        auto* Scroll = new QScrollArea(this);
        Scroll->setWidgetResizable(true);
        Scroll->setFrameShape(QFrame::NoFrame);
        auto* Content = new QWidget(Scroll);
        body_ = new QVBoxLayout(Content);
        body_->setContentsMargins(4, 4, 4, 4);
        Scroll->setWidget(Content);
        Layout->addWidget(Scroll, 1);
        auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        Layout->addWidget(Buttons);
        refresh();
    }

    void DatabaseInfoDialog::SetEncryption(std::function<Encryption()> Provider)
    {
        encryption_ = std::move(Provider);
        refresh();
    }

    void DatabaseInfoDialog::refresh()
    {
        // 다시 그린다(저널 모드 · user_version 을 바꾼 뒤).
        // 칸 묶음(QFormLayout)의 위젯도 함께 떼어 낸다. 레이아웃만 지우면 옛 줄이 대화에 그대로 남는다.
        const std::function<void(QLayout*)> Clear = [&Clear](QLayout* Layout) {
            while (auto* Item = Layout->takeAt(0)) {
                if (auto* Widget = Item->widget()) { Widget->hide(); Widget->setParent(nullptr); Widget->deleteLater(); }
                else if (auto* Child = Item->layout()) Clear(Child);
                delete Item;
            }
        };
        Clear(body_);
        tasks_.clear();
        const auto& Colors = fm::style::themeColorsFor(this);
        const auto Section = [&](const QString& Title) {
            auto* Label = new QLabel(Title, this);
            auto Font = Label->font();
            Font.setWeight(QFont::DemiBold);
            Label->setFont(Font);
            auto Palette = Label->palette();
            Palette.setColor(QPalette::WindowText, Colors[T::AccentFg]);
            Label->setPalette(Palette);
            Label->setContentsMargins(0, 10, 0, 2);
            body_->addWidget(Label);
            auto* Form = new QFormLayout;
            Form->setLabelAlignment(Qt::AlignLeft);
            body_->addLayout(Form);
            return Form;
        };
        const auto Row = [&](QFormLayout* Form, const QString& Name, const QString& Value, const QString& Action = {}, std::function<void()> Run = {}, bool Always = false) {
            auto* Line = new QWidget(this);
            auto* Box = new QHBoxLayout(Line);
            Box->setContentsMargins(0, 0, 0, 0);
            auto* Text = new QLabel(Value, Line);
            Text->setObjectName(QStringLiteral("sqliteInfo.") + Name);
            Text->setTextInteractionFlags(Qt::TextSelectableByMouse);
            Text->setWordWrap(true);
            Box->addWidget(Text, 1);
            if (!Action.isEmpty()) {
                auto* Button = new QPushButton(Action, Line);
                Button->setObjectName(QStringLiteral("sqliteInfoAction.") + Name);
                Button->setEnabled(Always || !readOnly_ || Action == tr("모두 보기"));
                connect(Button, &QPushButton::clicked, this, std::move(Run));
                Box->addWidget(Button);
            }
            Form->addRow(Name, Line);
        };
        const QFileInfo File(path_);
        const auto PageSize = pragma(QStringLiteral("page_size")).toLongLong();
        const auto PageCount = pragma(QStringLiteral("page_count")).toLongLong();
        auto* Files = Section(tr("파일"));
        Row(Files, tr("경로"), QDir::toNativeSeparators(File.absoluteFilePath()));
        Row(Files, tr("크기"), tr("%1 · 페이지 %2 B × %3").arg(Size(File.size()), QLocale().toString(PageSize), QLocale().toString(PageCount)));
        Row(Files, tr("여유 페이지"), QLocale().toString(pragma(QStringLiteral("freelist_count")).toLongLong()));

        auto* Format = Section(tr("형식 · PRAGMA"));
        Row(Format, tr("인코딩"), pragma(QStringLiteral("encoding")));
        Row(Format, tr("저널 모드"), pragma(QStringLiteral("journal_mode")).toUpper(), tr("바꾸기…"), [this] {
            QMenu Menu(this);
            for (const auto& Mode : {QStringLiteral("DELETE"), QStringLiteral("TRUNCATE"), QStringLiteral("PERSIST"), QStringLiteral("WAL"), QStringLiteral("MEMORY")}) {
                Menu.addAction(Mode, this, [this, Mode] {
                    Statement Query(gui_, QStringLiteral("PRAGMA main.journal_mode = %1").arg(Mode));
                    const auto Result = Query.Valid() && Query.Step() == SQLITE_ROW ? Query.Value(0).toString() : Query.Error();
                    if (Result.compare(Mode, Qt::CaseInsensitive) != 0)
                        QMessageBox::warning(this, tr("저널 모드"), tr("바꾸지 못했습니다(%1). 다른 탭 · 프로그램이 이 파일을 쓰고 있으면 바꿀 수 없습니다.").arg(Result));
                    emit DatabaseChanged();
                    refresh();
                });
            }
            Menu.exec(QCursor::pos());
        });
        static const QStringList Sync{QStringLiteral("OFF"), QStringLiteral("NORMAL"), QStringLiteral("FULL"), QStringLiteral("EXTRA")};
        static const QStringList Vacuum{QStringLiteral("NONE"), QStringLiteral("FULL"), QStringLiteral("INCREMENTAL")};
        Row(Format, tr("동기화"), Sync.value(pragma(QStringLiteral("synchronous")).toInt(), pragma(QStringLiteral("synchronous"))));
        Row(Format, tr("자동 VACUUM"), Vacuum.value(pragma(QStringLiteral("auto_vacuum")).toInt(), pragma(QStringLiteral("auto_vacuum"))));
        Row(Format, QStringLiteral("user_version"), pragma(QStringLiteral("user_version")), tr("편집"), [this] {
            bool Ok = false;
            const int Value = QInputDialog::getInt(this, QStringLiteral("user_version"), tr("새 값"), pragma(QStringLiteral("user_version")).toInt(), INT_MIN, INT_MAX, 1, &Ok);
            if (!Ok) return;
            QString Error;
            if (!gui_.Exec(QStringLiteral("PRAGMA main.user_version = %1").arg(Value), &Error)) QMessageBox::warning(this, QStringLiteral("user_version"), Error);
            refresh();
        });
        Row(Format, QStringLiteral("application_id"), pragma(QStringLiteral("application_id")));
        const bool Keys = pragma(QStringLiteral("foreign_keys")).toInt() != 0;
        Row(Format, QStringLiteral("foreign_keys"), Keys ? tr("켜짐 (이 연결)") : tr("꺼짐 (이 연결)"), Keys ? tr("끄기") : tr("켜기"), [this, Keys] {
            gui_.Exec(QStringLiteral("PRAGMA foreign_keys = %1").arg(Keys ? QStringLiteral("OFF") : QStringLiteral("ON")), nullptr);
            refresh();
        });

        auto* Crypto = Section(tr("암호화"));
        const auto State = encryption_ ? encryption_() : Encryption{};
        if (State.change_)
            Row(Crypto, tr("방식"), State.method_.isEmpty() ? tr("없음 (평문 파일)") : State.method_, tr("암호화…"), [this, Change = State.change_] {
                if (!running_.isEmpty()) { QMessageBox::information(this, tr("암호화"), tr("유지 관리가 끝난 뒤에 바꿀 수 있습니다.")); return; }
                if (Change()) { emit DatabaseChanged(); refresh(); }
            });
        else Row(Crypto, tr("방식"), State.method_.isEmpty() ? tr("없음 (평문 파일)") : State.method_);
        if (!State.savedKey_.isEmpty()) {
            if (State.manageKeys_) Row(Crypto, tr("저장한 키"), State.savedKey_, tr("관리…"), State.manageKeys_, true);
            else Row(Crypto, tr("저장한 키"), State.savedKey_);
        }
        Row(Crypto, tr("SEE"), Runtime::SeeText(Runtime::See()));

        auto* Library = Section(QStringLiteral("SQLite"));
        Row(Library, tr("버전"), Runtime::Version() + (Runtime::See() == SeeState::Available ? tr(" (SEE 빌드)") : QString()));
        QStringList Options;
        {
            Statement Query(gui_, QStringLiteral("SELECT compile_options FROM pragma_compile_options"));
            while (Query.Valid() && Query.Step() == SQLITE_ROW) Options.append(Query.Value(0).toString());
        }
        Row(Library, tr("컴파일 옵션"), QStringList(Options.mid(0, 3)).join(QStringLiteral(" · ")) + (Options.size() > 3 ? QStringLiteral(" …") : QString()), tr("모두 보기"), [this, Options] {
            QDialog List(this);
            List.setWindowTitle(tr("컴파일 옵션 — %1개").arg(Options.size()));
            auto* Box = new QVBoxLayout(&List);
            auto* Items = new QListWidget(&List);
            Items->addItems(Options);
            Box->addWidget(Items);
            List.resize(420, 480);
            List.exec();
        });

        auto* Maintenance = new QLabel(tr("유지 관리"), this);
        auto MaintenanceFont = Maintenance->font();
        MaintenanceFont.setWeight(QFont::DemiBold);
        Maintenance->setFont(MaintenanceFont);
        auto MaintenancePalette = Maintenance->palette();
        MaintenancePalette.setColor(QPalette::WindowText, Colors[T::AccentFg]);
        Maintenance->setPalette(MaintenancePalette);
        Maintenance->setContentsMargins(0, 10, 0, 2);
        body_->addWidget(Maintenance);
        auto* Grid = new QGridLayout;
        const QStringList Names{QStringLiteral("integrity"), QStringLiteral("foreignKeys"), QStringLiteral("analyze"), QStringLiteral("optimize"), QStringLiteral("vacuum"), QStringLiteral("checkpoint")};
        for (int Index = 0; Index < Names.size(); ++Index) {
            auto* Button = new QPushButton(TaskTitle(Names[Index]), this);
            Button->setObjectName(QStringLiteral("sqliteMaintain.") + Names[Index]);
            const bool Writes = Index >= 2;
            Button->setEnabled(!(readOnly_ && Writes) && running_.isEmpty());
            connect(Button, &QPushButton::clicked, this, [this, Task = Names[Index]] { runTask(Task); });
            Grid->addWidget(Button, Index / 3, Index % 3);
            tasks_.append(Button);
        }
        body_->addLayout(Grid);
        result_ = new QLabel(this);
        result_->setObjectName(QStringLiteral("sqliteMaintainResult"));
        result_->setWordWrap(true);
        result_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        result_->setText(last_.isEmpty() ? QString() : last_);
        body_->addWidget(result_);
        auto* Note = new QLabel(tr("VACUUM 처럼 파일 전체를 다시 쓰는 작업은 커밋하지 않은 변경이 있으면 먼저 커밋을 묻습니다. 유지 관리는 따로 연 연결에서 돌리므로 "
                                   "실행 중에도 표를 볼 수 있습니다. 빠른 보기(Ctrl+Q)에는 이 화면이 없습니다."), this);
        Note->setWordWrap(true);
        auto NotePalette = Note->palette();
        NotePalette.setColor(QPalette::WindowText, Colors[T::Fg3]);
        Note->setPalette(NotePalette);
        body_->addWidget(Note);
        body_->addStretch(1);
    }

    void DatabaseInfoDialog::runTask(const QString& Task)
    {
        if (!running_.isEmpty()) return;
        if (Task == QLatin1String("vacuum") && commitPending_ && !commitPending_()) return;
        running_ = Task;
        for (auto* Button : tasks_) Button->setEnabled(false);
        result_->setText(tr("%1 — 실행 중…").arg(TaskTitle(Task)));
        watcher_->setFuture(QtConcurrent::run([Open = open_, Path = path_, Task] { return Maintain(Open, Path, Task); }));
    }

    void DatabaseInfoDialog::finishTask(QString Task, const QString& Result, bool Ok)
    {
        running_.clear();
        lastOk_ = Ok;
        last_ = QStringLiteral("%1 %2 — %3 · %4").arg(Ok ? QStringLiteral("✓") : QStringLiteral("⚠"), TaskTitle(Task), Result, QTime::currentTime().toString(QStringLiteral("HH:mm")));
        if (Task == QLatin1String("vacuum") || Task == QLatin1String("analyze") || Task == QLatin1String("checkpoint")) emit DatabaseChanged();
        refresh();
    }

    QString DatabaseInfoDialog::RunMaintenance(const QString& Task, bool Wait)
    {
        runTask(Task);
        if (Wait && watcher_->isRunning()) {
            watcher_->waitForFinished();
            QCoreApplication::processEvents();
        }
        return last_;
    }

    QString DatabaseInfoDialog::LastResult() const { return last_; }
}
