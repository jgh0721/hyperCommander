#include "stdafx.h"
#include "sqliteQueryTab.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteCellDelegate.hpp"
#include "sqliteConnection.hpp"
#include "sqliteGlyphs.hpp"
#include "sqliteGrid.hpp"
#include "sqliteResultModel.hpp"
#include "core/ThemeManager.h"
#include "text/ScintillaEditorSettings.h"
#include "text/ScintillaQtDirectBackend.h"
#include "UniqueLibs/ProductDefaults_Edit.hpp"
#include "UniqueLibs/document/completion/completionMatch.hpp"
#include "UniqueLibs/document/completion/completionPopup.hpp"

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/StyleProps.h>
#include <fmstyle/ThemeColors.h>
#include <fmstyle/ThemeManager.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QStackedWidget>
#include <QTabBar>
#include <QApplication>
#include <QHeaderView>
#include <QTime>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <Scintilla.h>

namespace hc::sqlite
{
    namespace
    {
        using T = fm::style::Token;
        // LexSQL 의 사용자 키워드 스타일(SCE_SQL_USER1 · 2 · 3)과 이 탭이 쓰는 표시기 · 표시자 번호.
        constexpr int StyleTable = 19, StyleColumn = 20, StyleFunction = 21;
        constexpr int StatementMarker = 24, StatementMargin = 4, ErrorIndicator = 20;
        constexpr int ErrorRole = Qt::UserRole + 1;

        qintptr Bgr(const QColor& Color) { return qintptr(Color.red() | (Color.green() << 8) | (Color.blue() << 16)); }
        bool Word(char Ch) { return (Ch >= 'A' && Ch <= 'Z') || (Ch >= 'a' && Ch <= 'z') || (Ch >= '0' && Ch <= '9') || Ch == '_' || (static_cast<unsigned char>(Ch) & 0x80); }
        bool WordChar(QChar Ch) { return Ch.isLetterOrNumber() || Ch == QLatin1Char('_') || Ch == QLatin1Char('$'); }

        // 자동 완성 목록의 종류 아이콘(시안 ‘부품 · 자동 완성’ 의 ICC 색). 후보마다 SVG 를 그리지 않게 색 · 이름으로 담아 둔다.
        QIcon KindIcon(CompletionKind Kind, const fm::style::ThemeColors& Colors)
        {
            static QHash<QString, QIcon> Cache;
            QString Name;
            T Tone = T::Fg3;
            switch (Kind) {
            case CompletionKind::Table: Name = QStringLiteral("table"); Tone = T::Ok; break;
            case CompletionKind::View: Name = QStringLiteral("view"); Tone = T::Ok; break;
            case CompletionKind::Cte: Name = QStringLiteral("cte"); Tone = T::AccentFg; break;
            case CompletionKind::Column: Name = QStringLiteral("col"); Tone = T::Fg3; break;
            case CompletionKind::PrimaryKey: Name = QStringLiteral("pk"); Tone = T::Warn; break;
            case CompletionKind::ForeignKey:
            case CompletionKind::JoinCondition: Name = QStringLiteral("fk"); Tone = T::AccentFg; break;
            case CompletionKind::Function: Name = QStringLiteral("fn"); Tone = T::Warn; break;
            case CompletionKind::Pragma: Name = QStringLiteral("info"); Tone = T::Fg3; break;
            case CompletionKind::Collation: Name = QStringLiteral("index"); Tone = T::Fg3; break;
            case CompletionKind::Keyword: Name = QStringLiteral("kw"); Tone = T::Fg3; break;
            }
            const auto Color = Colors[Tone];
            const auto Key = Name + Color.name();
            auto It = Cache.find(Key);
            if (It == Cache.end()) It = Cache.insert(Key, Glyph(Name, Color, 16));
            return It.value();
        }
    }

    QueryTab::QueryTab(QueryRunner::Opener Opener, const QString& Title, QObject* Parent)
        : WorkTab(Parent), title_(Title)
    {
        top_ = new QWidget;
        top_->setObjectName(QStringLiteral("sqliteQueryTop"));
        auto* TopLayout = new QVBoxLayout(top_);
        TopLayout->setContentsMargins(0, 0, 0, 0);
        TopLayout->setSpacing(0);
        auto* Bar = new QWidget(top_);
        Bar->setFixedHeight(32);
        auto* BarLayout = new QHBoxLayout(Bar);
        BarLayout->setContentsMargins(6, 0, 8, 0);
        BarLayout->setSpacing(4);
        runButton_ = new QPushButton(tr("실행"), Bar);
        runButton_->setObjectName(QStringLiteral("sqliteRun"));
        runButton_->setToolTip(tr("실행 — 전체 또는 고른 글 (F5)"));
        fm::style::setButtonRole(runButton_, fm::style::ButtonRole::Primary);
        currentButton_ = new QPushButton(tr("현재 문"), Bar);
        currentButton_->setObjectName(QStringLiteral("sqliteRunCurrent"));
        currentButton_->setToolTip(tr("커서가 있는 문만 실행 (Ctrl+Enter)"));
        BarLayout->addWidget(runButton_);
        BarLayout->addWidget(currentButton_);
        auto* LimitLabel = new QLabel(tr("행 제한"), Bar);
        limit_ = new QComboBox(Bar);
        limit_->setObjectName(QStringLiteral("sqliteRowLimit"));
        limit_->addItem(QStringLiteral("100"), 100);
        limit_->addItem(QLocale().toString(1000), 1000);
        limit_->addItem(QLocale().toString(10000), 10000);
        limit_->addItem(tr("모두"), 0);
        limit_->setCurrentIndex(1);
        limit_->setToolTip(tr("결과를 한 번에 받는 행 수. 끝까지 스크롤하면 그만큼 더 받습니다."));
        BarLayout->addSpacing(8);
        BarLayout->addWidget(LimitLabel);
        BarLayout->addWidget(limit_);
        BarLayout->addStretch(1);
        editorNote_ = new QLabel(tr("Ctrl+Enter 현재 문 · F5 전체 · Ctrl+Space 자동 완성"), Bar);
        editorNote_->setObjectName(QStringLiteral("sqliteEditorNote"));
        BarLayout->addWidget(editorNote_);
        TopLayout->addWidget(Bar);

        editor_ = new ScintillaQtDirectBackend(top_, this);
        editor_->widget()->setObjectName(QStringLiteral("sqliteQueryEditor"));
        auto Settings = ScintillaEditorSettings::standard();
        Settings.showCodeFolding = false;
        Settings.lineNumberMarginDigits = 3;
        editor_->applySettings(Settings);
        // 문서 창이 설정 글꼴(ApplyFonts)을 넘기기 전에도 글자가 보이게 고정폭 글꼴로 시작한다(스타일 크기 -1 이면 줄 높이가 몇 px 이다).
        editor_->setEditorFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        editor_->applyLanguage(QStringLiteral("SQL"));
        // 문서 창 명령이 쓰는 키는 Scintilla 기본 동작(줄 바꾸기 · 줄 자르기)에서 뺀다.
        for (const int Key : {'T', 'L'}) editor_->Send(SCI_CLEARCMDKEY, quintptr(Key + (SCMOD_CTRL << 16)));
        editor_->Send(SCI_SETKEYWORDS, 0, reinterpret_cast<qintptr>(Keywords().join(QLatin1Char(' ')).toUtf8().constData()));
        // 현재 문 막대(설계 5.3): 줄 번호 오른쪽 좁은 칸.
        editor_->Send(SCI_SETMARGINTYPEN, StatementMargin, SC_MARGIN_SYMBOL);
        editor_->Send(SCI_SETMARGINWIDTHN, StatementMargin, 3);
        editor_->Send(SCI_SETMARGINMASKN, StatementMargin, 1 << StatementMarker);
        for (int Margin = 0; Margin < StatementMargin; ++Margin)
            editor_->Send(SCI_SETMARGINMASKN, Margin, editor_->Send(SCI_GETMARGINMASKN, Margin) & ~qintptr(1 << StatementMarker));
        editor_->Send(SCI_MARKERDEFINE, StatementMarker, SC_MARK_FULLRECT);
        editor_->Send(SCI_INDICSETSTYLE, ErrorIndicator, INDIC_SQUIGGLE);
        TopLayout->addWidget(editor_->widget(), 1);

        result_ = new QWidget;
        result_->setObjectName(QStringLiteral("sqliteQueryResult"));
        auto* ResultLayout = new QVBoxLayout(result_);
        ResultLayout->setContentsMargins(0, 0, 0, 0);
        ResultLayout->setSpacing(0);
        resultTabs_ = new QTabBar(result_);
        resultTabs_->setDocumentMode(true);
        resultTabs_->setExpanding(false);
        resultTabs_->addTab(tr("메시지"));
        ResultLayout->addWidget(resultTabs_);
        auto* NoteRow = new QWidget(result_);
        NoteRow->setFixedHeight(30);
        auto* NoteLayout = new QHBoxLayout(NoteRow);
        NoteLayout->setContentsMargins(10, 0, 6, 0);
        note_ = new QLabel(tr("F5 로 실행하면 문마다 결과 탭이 생깁니다."), NoteRow);
        note_->setObjectName(QStringLiteral("sqliteQueryNote"));
        NoteLayout->addWidget(note_, 1);
        // 결과 표 도구(시안: ‘내보내기’). 결과 탭을 고르면 켠다.
        exportButton_ = new QToolButton(NoteRow);
        exportButton_->setObjectName(QStringLiteral("sqliteResultExport"));
        exportButton_->setAutoRaise(true);
        exportButton_->setToolTip(tr("내보내기 (CSV · JSON · SQL)"));
        exportButton_->setEnabled(false);
        NoteLayout->addWidget(exportButton_);
        connect(exportButton_, &QToolButton::clicked, this, &QueryTab::ExportRequested);
        ResultLayout->addWidget(NoteRow);
        resultPages_ = new QStackedWidget(result_);
        messages_ = new QListWidget(resultPages_);
        messages_->setObjectName(QStringLiteral("sqliteMessages"));
        messages_->setFrameShape(QFrame::NoFrame);
        messages_->setWordWrap(true);
        resultPages_->addWidget(messages_);
        ResultLayout->addWidget(resultPages_, 1);

        barTimer_ = new QTimer(this);
        barTimer_->setSingleShot(true);
        barTimer_->setInterval(120);
        connect(barTimer_, &QTimer::timeout, this, &QueryTab::updateStatementBar);
        connect(editor_, &ITextEditorBackend::cursorPositionChanged, this, [this] { barTimer_->start(); emit Changed(); });
        connect(editor_, &ITextEditorBackend::textChanged, this, [this] {
            barTimer_->start();
            // 떠 있는 목록은 지금 내용으로 다시 거른다. 이 알림은 캐럿이 옮겨 가기 전에 오므로 다음 차례에 본다
            // (넣은 글자는 charAdded 가 바로 거르고, 지우기는 이 길로만 온다).
            if (quickFixMode_ && !inserting_) hideCompletion();
            else if (CompletionVisible() && !inserting_) QTimer::singleShot(0, this, &QueryTab::refreshCompletion);
            if (errorPosition_ >= 0) {
                editor_->Send(SCI_SETINDICATORCURRENT, ErrorIndicator);
                editor_->Send(SCI_INDICATORCLEARRANGE, 0, editor_->Send(SCI_GETLENGTH));
                errorPosition_ = -1;
            }
            emit Changed();
        });
        connect(runButton_, &QPushButton::clicked, this, &QueryTab::RunAll);
        connect(currentButton_, &QPushButton::clicked, this, &QueryTab::RunCurrent);
        connect(resultTabs_, &QTabBar::currentChanged, this, [this](int Index) {
            resultPages_->setCurrentIndex(Index);
            exportButton_->setEnabled(CurrentResultModel() != nullptr);
            emit Changed();
        });
        connect(messages_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* Item) {
            const int Offset = Item->data(ErrorRole).toInt();
            if (Offset < 0) return;
            editor_->Send(SCI_GOTOPOS, Offset);
            editor_->widget()->setFocus(Qt::OtherFocusReason);
        });
        // 자동 완성(설계 5.2): 공용 팝업 + 접두어 · 머리 글자 일치 + 고정폭 형식 열 + 아래 설명 줄.
        popup_ = new hc::document::CompletionPopupWidget(top_);
        popup_->setObjectName(QStringLiteral("sqliteCompletionPopup"));
        popup_->setMatcher(hc::document::completion::InitialsMatch);
        popup_->setFooterVisible(true);
        {
            const QFont Detail = SizedLike(QFontDatabase::systemFont(QFontDatabase::FixedFont), popup_->font(), -1.0, 7.0);
            popup_->setDetailFont(Detail);
        }
        completeTimer_ = new QTimer(this);
        completeTimer_->setSingleShot(true);
        connect(completeTimer_, &QTimer::timeout, this, [this] { Complete(false); });
        connect(popup_, &hc::document::CompletionPopupWidget::itemSelected, this, &QueryTab::insertCompletion);
        connect(popup_, &hc::document::CompletionPopupWidget::popupHidden, this, [this] {
            if (appFilter_) { qApp->removeEventFilter(this); appFilter_ = false; }
            explicit_ = false;
            shownContext_ = CompletionContext::None;
            // 확정은 목록을 숨긴 뒤 itemSelected 를 낸다. 그 처리가 빠른 수정인지 알 수 있게 다음 차례에 푼다.
            QTimer::singleShot(0, this, [this] { if (!CompletionVisible()) quickFixMode_ = false; });
        });
        connect(editor_, &ScintillaQtDirectBackend::charAdded, this, &QueryTab::charAdded);
        connect(editor_, &ScintillaQtDirectBackend::viewportScrolled, this, &QueryTab::hideCompletion);
        connect(editor_, &ITextEditorBackend::cursorPositionChanged, this, [this] {
            if (inserting_) return;
            if (CompletionVisible()) refreshCompletion();
            updateCallTip();
        });

        connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, &QueryTab::applyEditorTheme);
        connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, &QueryTab::applyEditorTheme);
        applyEditorTheme();

        RunSink Sink;
        Sink.started_ = [this](int Index) {
            note_->setText(tr("실행 중 · %1 / %2 문 — Ctrl+. 로 중지").arg(Index + 1).arg(running_Statements_.size()));
        };
        Sink.result_ = [this](int Index, int Result, QList<QueryModel::Column> Columns, QVector<QVector<QVariant>> Rows, bool More, qint64) {
            // EXPLAIN QUERY PLAN(id, parent, notused, detail)은 트리로(설계 5.3).
            if (planMode_ == PlanMode::Plan && Columns.size() >= 4) { showPlan(Rows); return; }
            auto* Model = new QueryModel(this);
            Model->SetColumns(Columns);
            const auto Batch = batch_;
            Model->SetFetcher([this, Batch, Result] { runner_->FetchMore(Batch, Result, limit()); });
            Model->AppendRows(std::move(Rows), More);
            auto* Grid = new GridView(resultPages_);
            Grid->setModel(Model);
            if (hasFonts_) { Grid->setFont(gridFont_); Grid->Delegate()->SetFonts(gridFont_, monoFont_, monoValues_); }
            const int Position = int(results_.size());
            resultPages_->insertWidget(Position, Grid);
            const auto TabText = [Position, Model, Bytecode = planMode_ == PlanMode::Bytecode] {
                const auto Rows = QLocale().toString(Model->rowCount()) + (Model->HasMore() ? QStringLiteral("+") : QString());
                return Bytecode ? tr("바이트코드 · %1행").arg(Rows) : tr("결과 %1 · %2행").arg(Position + 1).arg(Rows);
            };
            resultTabs_->insertTab(Position, TabText());
            results_.append({Model, Grid, Index});
            Grid->FitColumns();
            connect(Model, &QAbstractItemModel::rowsInserted, this, [this, Position, TabText] { resultTabs_->setTabText(Position, TabText()); emit Changed(); });
            connect(Grid->selectionModel(), &QItemSelectionModel::currentChanged, this, &WorkTab::Changed);
            emit GridAdded(Grid);
            if (Position == 0) resultTabs_->setCurrentIndex(0);
        };
        Sink.rows_ = [this](quint64 Batch, int Result, QVector<QVector<QVariant>> Rows, bool More) {
            // 앞 실행의 늦은 응답은 버린다(결과 번호가 겹친다).
            if (Batch != batch_ || Result < 0 || Result >= results_.size() || !results_[Result].model_) return;
            results_[Result].model_->AppendRows(std::move(Rows), More);
        };
        Sink.message_ = [this](int Index, QString Error, int Offset, qint64 Changes, qint64 Ms, bool) {
            const auto Sql = Index >= 0 && Index < running_Statements_.size() ? Summary(running_Statements_[Index].sql_, 60) : QString();
            if (planMode_ == PlanMode::None && Index >= 0 && Index < running_Statements_.size()) {
                HistoryEntry Entry;
                Entry.sql_ = running_Statements_[Index].sql_;
                Entry.at_ = QDateTime::currentDateTime();
                Entry.ms_ = Ms;
                Entry.ok_ = Error.isEmpty();
                Entry.error_ = Error;
                Entry.changes_ = Changes;
                for (const auto& View : results_)
                    if (View.index_ == Index && View.model_) { Entry.rows_ = View.model_->rowCount(); Entry.moreRows_ = View.model_->HasMore(); }
                executed_.append(Entry);
            }
            if (!Error.isEmpty()) {
                failed_ = true;
                lastError_ = Error;
                // 이름을 고칠 수 있는 오류면 막대 안내에 Alt+Enter 를 보인다(시안 ‘오류’).
                if (const auto Missing = ParseMissingName(Error); Missing.kind_ != MissingName::Kind::None) {
                    editorNote_->setText(tr("Ctrl+Enter 현재 문 · F5 전체 · Alt+Enter 빠른 수정"));
                    // 한정된 열(a.Titel)은 sqlite3_error_offset 이 문 머리를 줄 때가 있다. 그 문에서 이름을 찾아(오류 위치에 가장 가까운 것) 쓴다.
                    if (Index >= 0 && Index < runRanges_.size()) {
                        const auto Utf8 = Text().toUtf8();
                        const auto& Range = runRanges_[Index];
                        const auto Body = Utf8.mid(Range.bodyStart_, Range.bodyEnd_ - Range.bodyStart_).toLower();
                        const auto Name = Missing.name_.toUtf8().toLower();
                        int Best = -1;
                        for (int At = int(Body.indexOf(Name)); At >= 0 && !Name.isEmpty(); At = int(Body.indexOf(Name, At + 1))) {
                            const bool Left = At == 0 || !Word(Body[At - 1]);
                            const bool Right = At + Name.size() >= Body.size() || !Word(Body[At + Name.size()]);
                            const int Position = Range.bodyStart_ + At;
                            if (Left && Right && (Best < 0 || qAbs(Position - Offset) < qAbs(Best - Offset))) Best = Position;
                        }
                        if (Best >= 0) Offset = Best;
                    }
                }
                QString Where = Index >= 0 ? lineSpan(Index) : QString();
                if (Offset >= 0) {
                    const int Line = int(editor_->Send(SCI_LINEFROMPOSITION, Offset)) + 1;
                    const int Column = int(editor_->Send(SCI_GETCOLUMN, Offset)) + 1;
                    Where = tr("%1행 %2열").arg(Line).arg(Column);
                    markError(Offset);
                }
                addMessage(true, Where, Error, Offset);
                resultTabs_->setCurrentIndex(resultTabs_->count() - 1);
                return;
            }
            QString Text;
            if (Changes >= 0) Text = tr("%1 — %2행 바뀜 · %3 ms").arg(Sql, QLocale().toString(Changes)).arg(Ms);
            else {
                const QueryModel* Model = nullptr;
                for (const auto& View : results_) if (View.index_ == Index) Model = View.model_;
                Text = Model ? tr("%1 — %2행%3 · %4 ms").arg(Sql, QLocale().toString(Model->rowCount()), Model->HasMore() ? QStringLiteral("+") : QString()).arg(Ms)
                             : tr("%1 — %2 ms").arg(Sql).arg(Ms);
            }
            addMessage(false, lineSpan(Index), Text, -1);
        };
        Sink.finished_ = [this](bool Interrupted, bool Wrote, qint64 Ms) {
            running_ = false;
            runButton_->setEnabled(true);
            currentButton_->setEnabled(true);
            const auto Plan = std::exchange(planMode_, PlanMode::None);
            QString Text;
            if (Interrupted) Text = tr("중지했습니다 · %1 ms").arg(Ms);
            else if (failed_) Text = tr("오류로 멈췄습니다 — 메시지 탭을 보세요.");
            else if (Plan == PlanMode::Plan && planTree_) Text = planTree_->property("sqlitePlanNote").toString();
            else if (Plan == PlanMode::Bytecode) Text = tr("EXPLAIN · %1 · %2 ms").arg(lineSpan(0)).arg(Ms);
            else if (Plan == PlanMode::Transaction) Text = inTransaction_ ? tr("트랜잭션이 아직 열려 있습니다.") : tr("끝냈습니다 · %1 ms").arg(Ms);
            else if (manual_ && inTransaction_) Text = tr("%1개 문 · %2 ms · 트랜잭션 열림 — Ctrl+S 로 커밋, ‘롤백’ 으로 버립니다").arg(running_Statements_.size()).arg(Ms);
            else Text = tr("%1개 문 · %2 ms%3").arg(running_Statements_.size()).arg(Ms).arg(Wrote ? tr(" · 파일에 썼습니다(자동 커밋)") : QString());
            note_->setText(Text);
            lastNote_ = Text;
            if (Plan == PlanMode::None && !executed_.isEmpty()) emit Executed(std::exchange(executed_, {}));
            executed_.clear();
            if (Wrote) emit DataWritten();
            emit Changed();
        };
        Sink.transaction_ = [this](bool Open) {
            if (Open == inTransaction_) return;
            inTransaction_ = Open;
            emit TransactionChanged(Open);
            emit Changed();
        };
        runner_ = std::make_unique<QueryRunner>(std::move(Opener), this, std::move(Sink));
    }

    QueryTab::~QueryTab()
    {
        // 실행기를 먼저 닫아 연결을 놓는다(탭을 닫은 뒤 파일을 옮길 수 있게).
        runner_.reset();
        // 목록은 보일 때 창에 붙으므로 탭과 함께 지운다.
        if (appFilter_) qApp->removeEventFilter(this);
        delete popup_;
        popup_ = nullptr;
        delete top_.data();
        delete result_.data();
    }

    QString QueryTab::Title() const
    {
        return file_.isEmpty() ? title_ : QFileInfo(file_).fileName();
    }

    QString QueryTab::ToolTip() const { return file_.isEmpty() ? title_ : QDir::toNativeSeparators(file_); }
    bool QueryTab::Modified() const { return !file_.isEmpty() ? Text() != savedText_ : !Text().trimmed().isEmpty() && Text() != savedText_; }
    QString QueryTab::Text() const { return editor_->text(); }

    void QueryTab::SetText(const QString& Value)
    {
        editor_->setText(Value);
        updateStatementBar();
    }

    void QueryTab::InsertStatement(const QString& Sql)
    {
        auto Text = Sql.trimmed();
        if (!Text.endsWith(QLatin1Char(';'))) Text += QLatin1Char(';');
        const int Caret = int(editor_->Send(SCI_GETCURRENTPOS));
        const int LineStart = int(editor_->Send(SCI_POSITIONFROMLINE, editor_->Send(SCI_LINEFROMPOSITION, Caret)));
        if (Caret > LineStart) Text.prepend(QLatin1Char('\n'));
        const auto Utf8 = Text.toUtf8();
        editor_->Send(SCI_REPLACESEL, 0, reinterpret_cast<qintptr>(Utf8.constData()));
        editor_->widget()->setFocus(Qt::OtherFocusReason);
    }

    int QueryTab::Caret() const { return int(editor_->Send(SCI_GETCURRENTPOS)); }

    void QueryTab::SetCaret(int Position)
    {
        editor_->Send(SCI_GOTOPOS, qBound(0, Position, int(editor_->Send(SCI_GETLENGTH))));
        updateStatementBar();
    }

    void QueryTab::SetRowLimit(int Limit)
    {
        auto Preferences = preferences_;
        Preferences.rowLimit_ = Limit;
        SetPreferences(Preferences, true);
    }

    void QueryTab::SetFile(const QString& Path)
    {
        file_ = Path;
        QFile File(Path);
        if (File.open(QIODevice::ReadOnly)) {
            const auto Bytes = File.readAll();
            const auto Text = QString::fromUtf8(Bytes.startsWith("\xEF\xBB\xBF") ? Bytes.mid(3) : Bytes);
            SetText(Text);
            savedText_ = Text;
        }
        emit Changed();
    }

    bool QueryTab::Save(QString* Error)
    {
        if (file_.isEmpty()) return false;
        QSaveFile File(file_);
        if (!File.open(QIODevice::WriteOnly) || File.write(Text().toUtf8()) < 0 || !File.commit()) {
            if (Error) *Error = File.errorString();
            return false;
        }
        savedText_ = Text();
        emit Changed();
        return true;
    }

    void QueryTab::SetSchemaWords(const QStringList& Tables, const QStringList& Columns, const QStringList& Functions)
    {
        tables_ = Tables;
        columns_ = Columns;
        functions_ = Functions;
        applySchemaWords();
    }

    void QueryTab::applySchemaWords()
    {
        // 설정에서 끄면 목록을 비워 기본색으로 둔다.
        const auto Join = [this](QStringList Words) {
            if (!preferences_.schemaColors_) return QByteArray();
            for (auto& Word : Words) Word = Word.toLower();
            Words.removeDuplicates();
            return Words.join(QLatin1Char(' ')).toUtf8();
        };
        const auto TableWords = Join(tables_), ColumnWords = Join(columns_), FunctionWords = Join(functions_);
        editor_->Send(SCI_SETKEYWORDS, 4, reinterpret_cast<qintptr>(TableWords.constData()));
        editor_->Send(SCI_SETKEYWORDS, 5, reinterpret_cast<qintptr>(ColumnWords.constData()));
        editor_->Send(SCI_SETKEYWORDS, 6, reinterpret_cast<qintptr>(FunctionWords.constData()));
        editor_->invalidateStyling();
    }

    void QueryTab::applyEditorTheme()
    {
        const bool Dark = ThemeManager::instance().currentTheme() == ThemeManager::Dark;
        editor_->applyThemeColors(Dark);
        // 스키마 이름 색(시안 SYN): 테이블 · 뷰 초록, 열 보라, 함수 갈색.
        editor_->Send(SCI_STYLESETFORE, StyleTable, Bgr(QColor(Dark ? 0x6CCB8E : 0x1C7A4A)));
        editor_->Send(SCI_STYLESETFORE, StyleColumn, Bgr(QColor(Dark ? 0xC9A3F2 : 0x7A3E9D)));
        editor_->Send(SCI_STYLESETFORE, StyleFunction, Bgr(QColor(Dark ? 0xE3B45C : 0x8A4B00)));
        const auto& Colors = fm::style::themeColorsFor(top_);
        exportButton_->setIcon(Glyph(QStringLiteral("export"), Colors[T::Fg2], 16));
        editor_->Send(SCI_MARKERSETBACK, StatementMarker, Bgr(Colors[T::Accent]));
        editor_->Send(SCI_INDICSETFORE, ErrorIndicator, Bgr(Colors[T::Danger]));
        // 콜팁: 표면 · 글자 · 지금 인자 강조.
        editor_->Send(SCI_CALLTIPSETBACK, Bgr(Colors[T::Surface]));
        editor_->Send(SCI_CALLTIPSETFORE, Bgr(Colors[T::Fg2]));
        editor_->Send(SCI_CALLTIPSETFOREHLT, Bgr(Colors[T::AccentFg]));
        for (auto* Label : {note_, editorNote_}) {
            if (!Label) continue;
            auto Palette = Label->palette();
            Palette.setColor(QPalette::WindowText, Colors[Label == note_ ? T::Fg2 : T::Fg3]);
            Label->setPalette(Palette);
        }
    }

    int QueryTab::limit() const
    {
        const int Value = limit_->currentData().toInt();
        return Value <= 0 ? std::numeric_limits<int>::max() : Value;
    }

    int QueryTab::RowLimit() const { return limit_->currentData().toInt(); }

    void QueryTab::SetPreferences(const QueryPreferences& Preferences, bool Initial)
    {
        const bool Colors = preferences_.schemaColors_ != Preferences.schemaColors_;
        preferences_ = Preferences;
        if (Colors) applySchemaWords();
        // 이름 일치: 접두어, 그다음 머리 글자(기본) · 접두어만.
        if (Preferences.initialsMatch_) popup_->setMatcher(hc::document::completion::InitialsMatch);
        else popup_->setMatcher([](const QString& Pattern, const QString& Candidate, int* Score, QVector<int>* Matches) {
            if (!Candidate.startsWith(Pattern, Qt::CaseInsensitive)) return false;
            if (Score) *Score = 300;
            if (Matches) { Matches->clear(); for (int Index = 0; Index < Pattern.size(); ++Index) Matches->push_back(Index); }
            return true;
        });
        if (!Preferences.autoComplete_) completeTimer_->stop();
        if (!Preferences.callTips_) editor_->Send(SCI_CALLTIPCANCEL);
        editor_->Send(SCI_SETMARGINWIDTHN, StatementMargin, preferences_.statementBar_ ? 3 : 0);
        updateStatementBar();
        if (!Initial) return;
        // 행 제한: 목록에 없는 값(설정의 1,500 같은)은 제 자리에 끼워 넣는다. 0 은 ‘모두’.
        const int Wanted = qMax(0, Preferences.rowLimit_);
        int Index = limit_->findData(Wanted);
        if (Index < 0) {
            Index = 0;
            while (Index < limit_->count() && limit_->itemData(Index).toInt() > 0 && limit_->itemData(Index).toInt() < Wanted) ++Index;
            limit_->insertItem(Index, QLocale().toString(Wanted), Wanted);
        }
        limit_->setCurrentIndex(Index);
    }

    void QueryTab::RunAll() { run(false); }
    void QueryTab::RunCurrent() { run(true); }

    void QueryTab::run(bool Current)
    {
        if (running_) return;
        const auto Utf8 = Text().toUtf8();
        const int SelectionStart = int(editor_->Send(SCI_GETSELECTIONSTART)), SelectionEnd = int(editor_->Send(SCI_GETSELECTIONEND));
        // 전체(F5)는 고른 글이 있으면 그것만. 현재 문(Ctrl+Enter)도 설정이 ‘선택 영역, 없으면 현재 문’ 이면 고른 글을 먼저.
        const bool Selection = SelectionEnd > SelectionStart && (!Current || preferences_.selectionFirst_);
        if (Selection) Current = false;
        const int Base = Selection ? SelectionStart : 0;
        auto Ranges = SplitStatements(Selection ? Utf8.mid(SelectionStart, SelectionEnd - SelectionStart) : Utf8);
        for (auto& Range : Ranges) { Range.start_ += Base; Range.end_ += Base; Range.bodyStart_ += Base; Range.bodyEnd_ += Base; }
        if (Current) {
            const int Index = StatementAt(Ranges, int(editor_->Send(SCI_GETCURRENTPOS)));
            Ranges = Index >= 0 ? QVector<StatementRange>{Ranges[Index]} : QVector<StatementRange>{};
        }
        if (Ranges.isEmpty()) { note_->setText(tr("실행할 문이 없습니다.")); return; }
        QVector<RunStatement> Statements;
        QStringList Unbounded;
        for (const auto& Range : Ranges) {
            Statements.append({Range.bodyStart_, QString::fromUtf8(Utf8.mid(Range.bodyStart_, Range.bodyEnd_ - Range.bodyStart_))});
            if (IsUnboundedWrite(Statements.last().sql_)) Unbounded.append(Summary(Statements.last().sql_, 60));
        }
        // WHERE 없는 UPDATE · DELETE 는 모든 행을 바꾼다. 실행 전에 확인한다(설계 6.3).
        if (preferences_.confirmUnbounded_ && !Unbounded.isEmpty() && QMessageBox::question(top_, tr("모든 행 바꾸기"),
                tr("WHERE 없는 UPDATE · DELETE 가 있습니다. 테이블의 모든 행이 바뀌고 되돌릴 수 없습니다.\n\n%1\n\n실행할까요?").arg(Unbounded.join(QLatin1Char('\n'))),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        // 트랜잭션 안에서 실행할 수 없는 문(VACUUM · ATTACH · DETACH)은 열린 트랜잭션을 먼저 커밋할지 묻는다(설계 6.3).
        bool CommitFirst = false;
        if (inTransaction_)
            for (const auto& Each : Statements)
                if (QueryRunner::NonTransactional(Each.sql_)) {
                    if (QMessageBox::question(top_, tr("먼저 커밋"), tr("%1 은(는) 트랜잭션 안에서 실행할 수 없습니다. 열린 트랜잭션을 먼저 커밋할까요?").arg(Summary(Each.sql_, 40)),
                            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes) != QMessageBox::Yes) return;
                    CommitFirst = true;
                    break;
                }
        if (CommitFirst) {
            Statements.prepend(RunStatement{-1, QStringLiteral("COMMIT")});
            Ranges.prepend(StatementRange{});
        }
        clearResults();
        failed_ = false;
        executed_.clear();
        runRanges_ = Ranges;
        running_Statements_ = Statements;
        ++batch_;
        running_ = true;
        runButton_->setEnabled(false);
        currentButton_->setEnabled(false);
        note_->setText(tr("실행 중…"));
        runner_->Run(batch_, Statements, limit(), QueryRunner::Options{manual_, writable_});
        emit Changed();
    }

    void QueryTab::SetManualCommit(bool Manual)
    {
        if (manual_ == Manual) return;
        manual_ = Manual;
        emit Changed();
    }

    bool QueryTab::EndTransaction(bool Commit, bool Blocking, QString* Error)
    {
        if (!inTransaction_) return true;
        const auto Sql = Commit ? QStringLiteral("COMMIT") : QStringLiteral("ROLLBACK");
        if (Blocking) {
            QString Problem;
            const bool Ok = runner_->ExecBlocking(Sql, &Problem);
            if (!Ok) addMessage(true, {}, Problem, -1);
            if (Error) *Error = Problem;
            return Ok;
        }
        if (running_) return false;
        running_Statements_ = {RunStatement{-1, Sql}};
        runRanges_ = {StatementRange{}};
        planMode_ = PlanMode::Transaction;
        failed_ = false;
        ++batch_;
        running_ = true;
        runButton_->setEnabled(false);
        currentButton_->setEnabled(false);
        note_->setText(Commit ? tr("커밋하는 중…") : tr("롤백하는 중…"));
        runner_->Run(batch_, running_Statements_, 0, QueryRunner::Options{false, true});
        emit Changed();
        return true;
    }

    void QueryTab::clearResults()
    {
        for (auto& View : results_) {
            if (View.model_) View.model_->StopFetching();
            if (View.grid_) { resultPages_->removeWidget(View.grid_); View.grid_->deleteLater(); }
            if (View.model_) View.model_->deleteLater();
        }
        results_.clear();
        if (planTree_) { resultPages_->removeWidget(planTree_); planTree_->deleteLater(); planTree_ = nullptr; }
        while (resultTabs_->count() > 1) resultTabs_->removeTab(0);
        resultTabs_->setTabText(0, tr("메시지"));
        exportButton_->setEnabled(false);
        if (errorPosition_ >= 0) {
            editor_->Send(SCI_SETINDICATORCURRENT, ErrorIndicator);
            editor_->Send(SCI_INDICATORCLEARRANGE, 0, editor_->Send(SCI_GETLENGTH));
            errorPosition_ = -1;
        }
    }

    QString QueryTab::lineSpan(int Index) const
    {
        if (Index < 0 || Index >= runRanges_.size()) return {};
        const int First = int(editor_->Send(SCI_LINEFROMPOSITION, runRanges_[Index].bodyStart_)) + 1;
        const int Last = int(editor_->Send(SCI_LINEFROMPOSITION, qMax(runRanges_[Index].bodyStart_, runRanges_[Index].bodyEnd_ - 1))) + 1;
        return First == Last ? tr("%1행").arg(First) : tr("%1–%2행").arg(First).arg(Last);
    }

    void QueryTab::addMessage(bool Error, const QString& Where, const QString& Text, int Offset)
    {
        const auto& Colors = fm::style::themeColorsFor(messages_);
        auto* Item = new QListWidgetItem(Glyph(Error ? QStringLiteral("err") : QStringLiteral("ok"), Colors[Error ? T::Danger : T::Ok], 14),
            QStringLiteral("%1   %2   %3").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), Where, Text));
        if (Error) {
            Item->setData(ErrorRole, Offset);
            Item->setToolTip(Offset >= 0 ? tr("두 번 누르면 오류 위치로 갑니다.") : QString());
        }
        messages_->addItem(Item);
        while (messages_->count() > 500) delete messages_->takeItem(0);
        messages_->scrollToBottom();
        if (Error) resultTabs_->setTabText(resultTabs_->count() - 1, tr("메시지 · 오류"));
    }

    void QueryTab::markError(int Offset)
    {
        const auto Utf8 = Text().toUtf8();
        if (Offset < 0 || Utf8.isEmpty()) return;
        int Start = qBound(0, Offset, int(Utf8.size()) - 1), End = Start;
        while (End < Utf8.size() && (Word(Utf8[End]) || Utf8[End] == '.')) ++End;
        if (End == Start) End = Start + 1;
        editor_->Send(SCI_SETINDICATORCURRENT, ErrorIndicator);
        editor_->Send(SCI_INDICATORFILLRANGE, Start, End - Start);
        errorPosition_ = Start;
        errorEnd_ = End;
    }

    void QueryTab::GoToError()
    {
        if (errorPosition_ < 0) return;
        editor_->Send(SCI_GOTOPOS, errorPosition_);
        editor_->widget()->setFocus(Qt::OtherFocusReason);
    }

    void QueryTab::updateStatementBar()
    {
        editor_->Send(SCI_MARKERDELETEALL, StatementMarker);
        if (!preferences_.statementBar_) return;
        const auto Ranges = SplitStatements(Text().toUtf8());
        const int Index = StatementAt(Ranges, int(editor_->Send(SCI_GETCURRENTPOS)));
        if (Index < 0) return;
        const int First = int(editor_->Send(SCI_LINEFROMPOSITION, Ranges[Index].bodyStart_));
        const int Last = int(editor_->Send(SCI_LINEFROMPOSITION, qMax(Ranges[Index].bodyStart_, Ranges[Index].bodyEnd_ - 1)));
        for (int Line = First; Line <= Last; ++Line) editor_->Send(SCI_MARKERADD, Line, StatementMarker);
    }

    void QueryTab::ToggleComment()
    {
        const int SelectionStart = int(editor_->Send(SCI_GETSELECTIONSTART)), SelectionEnd = int(editor_->Send(SCI_GETSELECTIONEND));
        const int First = int(editor_->Send(SCI_LINEFROMPOSITION, SelectionStart));
        int Last = int(editor_->Send(SCI_LINEFROMPOSITION, SelectionEnd));
        // 다음 줄 맨 앞에서 끝나는 선택은 그 줄을 넣지 않는다.
        if (Last > First && SelectionEnd == int(editor_->Send(SCI_POSITIONFROMLINE, Last))) --Last;
        const auto LineText = [this](int Line) {
            const int Start = int(editor_->Send(SCI_POSITIONFROMLINE, Line)), End = int(editor_->Send(SCI_GETLINEENDPOSITION, Line));
            return Text().toUtf8().mid(Start, End - Start);
        };
        bool AllCommented = true;
        for (int Line = First; Line <= Last; ++Line) {
            const auto Body = LineText(Line).trimmed();
            if (!Body.isEmpty() && !Body.startsWith("--")) { AllCommented = false; break; }
        }
        editor_->Send(SCI_BEGINUNDOACTION);
        for (int Line = Last; Line >= First; --Line) {
            const auto Body = LineText(Line);
            const int Start = int(editor_->Send(SCI_POSITIONFROMLINE, Line));
            int Indent = 0;
            while (Indent < Body.size() && (Body[Indent] == ' ' || Body[Indent] == '\t')) ++Indent;
            if (AllCommented) {
                if (!Body.mid(Indent).startsWith("--")) continue;
                const int Length = Body.mid(Indent).startsWith("-- ") ? 3 : 2;
                editor_->Send(SCI_DELETERANGE, Start + Indent, Length);
            } else if (Indent < Body.size()) {
                editor_->Send(SCI_INSERTTEXT, Start + Indent, reinterpret_cast<qintptr>("-- "));
            }
        }
        editor_->Send(SCI_ENDUNDOACTION);
        // 선택이 있었으면 바꾼 줄 전체를 다시 고른다(이어서 한 번 더 누르면 같은 줄을 되돌린다).
        if (SelectionEnd > SelectionStart)
            editor_->Send(SCI_SETSEL, editor_->Send(SCI_POSITIONFROMLINE, First), editor_->Send(SCI_GETLINEENDPOSITION, Last));
    }

    QString QueryTab::RowsText() const
    {
        const int Index = resultTabs_->currentIndex();
        if (Index < 0 || Index >= results_.size()) return running_ ? tr("실행 중…") : QString();
        const auto* Model = results_[Index].model_;
        return tr("%1행%2").arg(QLocale().toString(Model->rowCount()), Model->HasMore() ? QStringLiteral("+") : QString());
    }

    QString QueryTab::PositionText() const
    {
        const auto Position = editor_->Send(SCI_GETCURRENTPOS);
        return tr("줄 %1, 열 %2").arg(editor_->Send(SCI_LINEFROMPOSITION, Position) + 1).arg(editor_->Send(SCI_GETCOLUMN, Position) + 1);
    }

    GridView* QueryTab::CurrentGrid() const
    {
        const int Index = resultTabs_->currentIndex();
        return Index >= 0 && Index < results_.size() ? results_[Index].grid_ : nullptr;
    }

    QString QueryTab::SelectionText() const
    {
        if (auto* Grid = CurrentGrid(); Grid && Grid->hasFocus()) return Grid->SelectionText();
        return editor_->selectedText();
    }

    QWidget* QueryTab::FocusTarget() const { return editor_->widget(); }
    void QueryTab::Cancel() { runner_->Interrupt(); }

    QueryModel* QueryTab::CurrentResultModel() const
    {
        const int Page = resultPages_->currentIndex();
        for (const auto& View : results_) if (View.grid_ && resultPages_->indexOf(View.grid_) == Page) return View.model_;
        return nullptr;
    }

    QString QueryTab::CurrentResultSql() const
    {
        const int Page = resultPages_->currentIndex();
        for (const auto& View : results_)
            if (View.grid_ && resultPages_->indexOf(View.grid_) == Page) return View.index_ >= 0 && View.index_ < running_Statements_.size() ? running_Statements_[View.index_].sql_ : QString();
        return {};
    }

    void QueryTab::ReleaseConnection()
    {
        // 남은 결과의 이어 읽기도 그 연결에 매여 있으므로 함께 끝난다(다음 실행에서 새 연결 · 새 키).
        if (runner_) runner_->Release();
    }
    bool QueryTab::HasEditorSelection() const { return editor_->Send(SCI_GETSELECTIONEMPTY) == 0; }

    void QueryTab::SetBrokered(bool Enabled)
    {
        // 호스트는 중개를 켜기만 한다(텍스트 문서와 같다). 지운 Scintilla 키는 되돌리지 않는다.
        if (!Enabled || brokered_) return;
        brokered_ = true;
        // 텍스트 문서(DocumentViewPane::SetClipboardBroker)와 같이 Scintilla 의 클립보드 키 · 팝업 · 끌어 놓기를 끈다.
        editor_->Send(SCI_USEPOPUP, SC_POPUP_NEVER);
        for (const auto Key : hc::defaults::edit::NativeBrokerClipboardKeys) editor_->Send(SCI_CLEARCMDKEY, Key);
        editor_->widget()->setAcceptDrops(false);
        editor_->widget()->installEventFilter(this);
    }

    bool QueryTab::eventFilter(QObject* Watched, QEvent* Event)
    {
        // 자동 완성 목록이 떠 있는 동안: 방향키 · Enter · Tab · Esc 는 목록이 받는다. 앱 전체 필터로도 받으므로(목록을 띄울 때 단다)
        // 문서 창의 Esc(보기 창 닫기)보다 먼저 온다.
        if (CompletionVisible() && Watched == editor_->widget()) {
            if (Event->type() == QEvent::KeyPress && popup_->handleKeyPress(static_cast<QKeyEvent*>(Event))) return true;
            if (Event->type() == QEvent::FocusOut) {
                // 다른 곳을 눌러 포커스가 나갔을 때만 닫는다. 목록 창이 활성 창이 되는 플랫폼(offscreen)은 그대로 둔다.
                QTimer::singleShot(0, this, [this] {
                    if (CompletionVisible() && !editor_->widget()->hasFocus() && QApplication::activeWindow() != popup_) hideCompletion();
                });
            }
        }
        if (appFilter_ && Event->type() == QEvent::WindowDeactivate && Watched == editor_->widget()->window())
            QTimer::singleShot(0, this, [this] { if (CompletionVisible() && QApplication::activeWindow() != popup_) hideCompletion(); });
        if (brokered_ && Watched == editor_->widget() && Event->type() == QEvent::KeyPress) {
            auto* Key = static_cast<QKeyEvent*>(Event);
            const bool Copy = Key->matches(QKeySequence::Copy) || (Key->key() == Qt::Key_Insert && Key->modifiers() == Qt::ControlModifier);
            if (Copy) {
                if (HasEditorSelection()) emit CopyRequested();
                return true;
            }
        }
        return WorkTab::eventFilter(Watched, Event);
    }

    QueryTab::StatementText QueryTab::statementAtCaret() const
    {
        // 커서가 든 문. ; 로 끝난 문 바로 뒤(공백)는 다음 문을 쓰는 중이다.
        const auto Utf8 = Text().toUtf8();
        const int Caret = int(editor_->Send(SCI_GETCURRENTPOS));
        int Start = 0, End = int(Utf8.size());
        for (const auto& Range : SplitStatements(Utf8)) {
            const bool Terminated = Range.bodyEnd_ > Range.bodyStart_ && Utf8[Range.bodyEnd_ - 1] == ';';
            if (Terminated && Range.end_ <= Caret) { Start = Range.end_; continue; }
            if (Range.end_ >= Caret) { End = Range.end_; break; }
        }
        StatementText Result;
        Result.start_ = Start;
        Result.text_ = QString::fromUtf8(Utf8.mid(Start, End - Start));
        Result.caret_ = int(QString::fromUtf8(Utf8.mid(Start, Caret - Start)).size());
        return Result;
    }

    bool QueryTab::CompletionVisible() const { return popup_ && popup_->isActive(); }

    bool QueryTab::Complete(bool Explicit)
    {
        completeTimer_->stop();
        const auto* Source = completion_ ? completion_() : nullptr;
        if (!Source) { hideCompletion(); return false; }
        const auto Statement = statementAtCaret();
        const auto Result = hc::sqlite::Complete(Statement.text_, Statement.caret_, *Source, Explicit);
        // 자동으로는 이름을 치기 시작했을 때와 . · ON 뒤에만 연다.
        const bool Anchored = Result.context_ == CompletionContext::Columns || Result.context_ == CompletionContext::JoinCondition;
        if (Result.context_ == CompletionContext::None || Result.candidates_.isEmpty() || (!Explicit && Result.prefix_.isEmpty() && !Anchored)) {
            hideCompletion();
            return false;
        }
        const auto& Colors = fm::style::themeColorsFor(top_);
        QList<hc::document::CompletionDisplayItem> Items;
        Items.reserve(Result.candidates_.size());
        for (const auto& Candidate : Result.candidates_) {
            hc::document::CompletionDisplayItem Item;
            Item.label = Candidate.label_;
            Item.insertText = Candidate.insertText_;
            Item.filterText = Candidate.filterText_;
            Item.detail = Candidate.detail_;
            Item.description = Candidate.description_;
            Item.scoreBias = Candidate.bias_;
            Item.icon = KindIcon(Candidate.kind_, Colors);
            Items.append(Item);
        }
        popup_->setItems(Items);
        popup_->updateFilter(Result.prefix_);
        if (popup_->visibleCount() == 0) { hideCompletion(); return false; }
        shownContext_ = Result.context_;
        explicit_ = Explicit;
        if (!popup_->isVisible()) {
            // 부모 없는 Tool 창은 Windows 에서 메인 창 뒤로 숨는다. 편집기가 든 창에 붙인다(탭을 다른 창으로 옮길 수 있다).
            auto* Window = editor_->widget()->window();
            if (popup_->parentWidget() != Window) popup_->setParent(Window, popup_->windowFlags());
            const int Caret = int(editor_->Send(SCI_GETCURRENTPOS));
            const int Start = int(editor_->Send(SCI_POSITIONRELATIVE, Caret, -qintptr(Result.prefix_.size())));
            const int Line = int(editor_->Send(SCI_LINEFROMPOSITION, Caret));
            const QPoint Point(int(editor_->Send(SCI_POINTXFROMPOSITION, 0, Start)),
                               int(editor_->Send(SCI_POINTYFROMPOSITION, 0, Start)) + int(editor_->Send(SCI_TEXTHEIGHT, Line)));
            popup_->showAt(editor_->widget()->mapToGlobal(Point));
            if (!appFilter_) { qApp->installEventFilter(this); appFilter_ = true; }
        } else {
            popup_->refreshGeometry();
        }
        return true;
    }

    void QueryTab::refreshCompletion()
    {
        if (!CompletionVisible() || inserting_ || quickFixMode_) return;
        const auto* Source = completion_ ? completion_() : nullptr;
        if (!Source) { hideCompletion(); return; }
        const auto Statement = statementAtCaret();
        const auto Result = hc::sqlite::Complete(Statement.text_, Statement.caret_, *Source, explicit_);
        const bool Anchored = shownContext_ == CompletionContext::Columns || shownContext_ == CompletionContext::JoinCondition;
        // 문맥이 바뀌면(공백 · 괄호 · 쉼표를 쳤다) 닫는다. 다음 이름을 치면 다시 연다.
        if (Result.context_ != shownContext_ || (Result.prefix_.isEmpty() && !explicit_ && !Anchored)) { hideCompletion(); return; }
        popup_->updateFilter(Result.prefix_);
    }

    void QueryTab::hideCompletion()
    {
        if (completeTimer_) completeTimer_->stop();
        if (popup_ && popup_->isVisible()) popup_->hide();
    }

    void QueryTab::charAdded(int Character)
    {
        const QChar Ch = Character >= 0 && Character <= 0xFFFF ? QChar(Character) : QChar(QChar::ReplacementCharacter);
        if (Ch == QLatin1Char('(')) {
            hideCompletion();
            if (preferences_.callTips_) ShowParameterHelp();
            return;
        }
        if (Ch == QLatin1Char(')') || Ch == QLatin1Char(',')) updateCallTip();
        if (CompletionVisible()) { refreshCompletion(); return; }
        if (!preferences_.autoComplete_) return;
        if (Ch == QLatin1Char('.')) { Complete(false); return; }
        if (WordChar(Ch) && !Ch.isDigit()) completeTimer_->start(qMax(0, preferences_.completeDelayMs_));
        else if (!WordChar(Ch)) completeTimer_->stop();
    }

    void QueryTab::insertCompletion(const QString& Insert)
    {
        if (quickFixMode_) {
            // 빠른 수정: 오류 자리의 이름(한정어 뒤)을 바꾼다.
            const auto Utf8 = Insert.toUtf8();
            inserting_ = true;
            editor_->Send(SCI_SETTARGETRANGE, fixStart_, fixEnd_);
            editor_->Send(SCI_REPLACETARGET, Utf8.size(), reinterpret_cast<qintptr>(Utf8.constData()));
            editor_->Send(SCI_GOTOPOS, fixStart_ + Utf8.size());
            inserting_ = false;
            quickFixMode_ = false;
            editor_->Send(SCI_SETINDICATORCURRENT, ErrorIndicator);
            editor_->Send(SCI_INDICATORCLEARRANGE, 0, editor_->Send(SCI_GETLENGTH));
            errorPosition_ = errorEnd_ = -1;
            lastError_.clear();
            editorNote_->setText(tr("Ctrl+Enter 현재 문 · F5 전체 · Ctrl+Space 자동 완성"));
            return;
        }
        // 지금 커서 앞 이름 조각(목록을 띄운 뒤 더 친 글자 포함)을 바꾼다.
        const auto Statement = statementAtCaret();
        int Length = 0;
        while (Length < Statement.caret_ && WordChar(Statement.text_[Statement.caret_ - 1 - Length])) ++Length;
        const int Caret = int(editor_->Send(SCI_GETCURRENTPOS));
        const int Start = int(editor_->Send(SCI_POSITIONRELATIVE, Caret, -qintptr(Length)));
        const auto Utf8 = Insert.toUtf8();
        inserting_ = true;
        editor_->Send(SCI_BEGINUNDOACTION);
        editor_->Send(SCI_SETTARGETRANGE, Start, Caret);
        editor_->Send(SCI_REPLACETARGET, Utf8.size(), reinterpret_cast<qintptr>(Utf8.constData()));
        editor_->Send(SCI_GOTOPOS, Start + Utf8.size());
        editor_->Send(SCI_ENDUNDOACTION);
        inserting_ = false;
        if (Insert.endsWith(QLatin1Char('(')) && preferences_.callTips_) ShowParameterHelp();
    }

    bool QueryTab::ShowParameterHelp()
    {
        const auto Statement = statementAtCaret();
        const auto Call = EnclosingCall(Statement.text_, Statement.caret_);
        const auto Signature = Call.name_.isEmpty() ? QString() : FunctionSignature(Call.name_);
        if (Signature.isEmpty()) { editor_->Send(SCI_CALLTIPCANCEL); return false; }
        const int NameStart = Statement.start_ + int(Statement.text_.left(Call.open_ - int(Call.name_.size())).toUtf8().size());
        const auto Utf8 = Signature.toUtf8();
        editor_->Send(SCI_CALLTIPSHOW, NameStart, reinterpret_cast<qintptr>(Utf8.constData()));
        const auto [From, To] = SignatureArgument(Signature, Call.argument_);
        if (From >= 0) editor_->Send(SCI_CALLTIPSETHLT, Signature.left(From).toUtf8().size(), Signature.left(To).toUtf8().size());
        else editor_->Send(SCI_CALLTIPSETHLT, 0, 0);
        return true;
    }

    void QueryTab::updateCallTip()
    {
        if (editor_->Send(SCI_CALLTIPACTIVE) == 0) return;
        if (!ShowParameterHelp()) editor_->Send(SCI_CALLTIPCANCEL);
    }

    bool QueryTab::QuickFix()
    {
        const auto Missing = ParseMissingName(lastError_);
        const auto* Source = completion_ ? completion_() : nullptr;
        if (errorPosition_ < 0 || errorEnd_ <= errorPosition_ || Missing.kind_ == MissingName::Kind::None || !Source) return false;
        // 오류 자리의 이름(t.Nme 면 Nme)과 그 문.
        const auto Utf8 = Text().toUtf8();
        const auto Token = Utf8.mid(errorPosition_, errorEnd_ - errorPosition_);
        fixStart_ = errorPosition_ + int(Token.lastIndexOf('.') + 1);
        fixEnd_ = errorEnd_;
        int Start = 0, End = int(Utf8.size());
        for (const auto& Range : SplitStatements(Utf8))
            if (errorPosition_ >= Range.start_ && errorPosition_ < Range.end_) { Start = Range.start_; End = Range.end_; }
        const auto Names = Suggestions(Missing.name_, NamesFor(Missing, QString::fromUtf8(Utf8.mid(Start, End - Start)), *Source));
        if (Names.isEmpty()) { note_->setText(tr("‘%1’ 과 비슷한 이름이 없습니다.").arg(Missing.name_)); return false; }
        const auto& Colors = fm::style::themeColorsFor(top_);
        QList<hc::document::CompletionDisplayItem> Items;
        for (const auto& Name : Names) {
            hc::document::CompletionDisplayItem Item;
            Item.label = Name;
            Item.insertText = IdentifierText(Name);
            Item.detail = tr("거리 %1").arg(EditDistance(Missing.name_, Name));
            Item.description = QStringLiteral("%1 → %2").arg(Missing.name_, Name);
            Item.icon = KindIcon(Missing.kind_ == MissingName::Kind::Table ? CompletionKind::Table : CompletionKind::Column, Colors);
            Items.append(Item);
        }
        hideCompletion();
        popup_->setItems(Items);
        popup_->updateFilter(QString());
        quickFixMode_ = true;
        auto* Window = editor_->widget()->window();
        if (popup_->parentWidget() != Window) popup_->setParent(Window, popup_->windowFlags());
        const int Line = int(editor_->Send(SCI_LINEFROMPOSITION, fixStart_));
        const QPoint Point(int(editor_->Send(SCI_POINTXFROMPOSITION, 0, fixStart_)),
                           int(editor_->Send(SCI_POINTYFROMPOSITION, 0, fixStart_)) + int(editor_->Send(SCI_TEXTHEIGHT, Line)));
        popup_->showAt(editor_->widget()->mapToGlobal(Point));
        if (!appFilter_) { qApp->installEventFilter(this); appFilter_ = true; }
        return true;
    }

    void QueryTab::FormatSql()
    {
        hideCompletion();
        const auto Utf8 = Text().toUtf8();
        int Start = int(editor_->Send(SCI_GETSELECTIONSTART)), End = int(editor_->Send(SCI_GETSELECTIONEND));
        if (End <= Start) {
            const auto Ranges = SplitStatements(Utf8);
            const int Index = StatementAt(Ranges, int(editor_->Send(SCI_GETCURRENTPOS)));
            if (Index < 0) return;
            Start = Ranges[Index].bodyStart_;
            End = Ranges[Index].bodyEnd_;
        }
        const auto Before = QString::fromUtf8(Utf8.mid(Start, End - Start));
        const auto After = hc::sqlite::FormatSql(Before, preferences_.upperKeywords_).toUtf8();
        if (After == Before.toUtf8()) return;
        editor_->Send(SCI_BEGINUNDOACTION);
        editor_->Send(SCI_SETTARGETRANGE, Start, End);
        editor_->Send(SCI_REPLACETARGET, After.size(), reinterpret_cast<qintptr>(After.constData()));
        editor_->Send(SCI_SETSEL, Start, Start + After.size());
        editor_->Send(SCI_ENDUNDOACTION);
    }

    void QueryTab::Explain(bool Bytecode)
    {
        if (running_) return;
        hideCompletion();
        const auto Utf8 = Text().toUtf8();
        const auto Ranges = SplitStatements(Utf8);
        const int Index = StatementAt(Ranges, int(editor_->Send(SCI_GETCURRENTPOS)));
        if (Index < 0) { note_->setText(tr("실행 계획을 볼 문이 없습니다.")); return; }
        const auto& Range = Ranges[Index];
        const auto Prefix = Bytecode ? QStringLiteral("EXPLAIN ") : QStringLiteral("EXPLAIN QUERY PLAN ");
        clearResults();
        failed_ = false;
        runRanges_ = {Range};
        // 붙인 머리만큼 문 시작을 앞당겨 오류 위치가 편집기의 글자를 가리키게 한다.
        running_Statements_ = {RunStatement{Range.bodyStart_ - int(Prefix.toUtf8().size()),
                                            Prefix + QString::fromUtf8(Utf8.mid(Range.bodyStart_, Range.bodyEnd_ - Range.bodyStart_))}};
        planMode_ = Bytecode ? PlanMode::Bytecode : PlanMode::Plan;
        ++batch_;
        running_ = true;
        runButton_->setEnabled(false);
        currentButton_->setEnabled(false);
        note_->setText(Bytecode ? tr("바이트코드를 읽는 중…") : tr("실행 계획을 읽는 중…"));
        runner_->Run(batch_, running_Statements_, Bytecode ? 100000 : 10000);
        emit Changed();
    }

    void QueryTab::showPlan(const QVector<QVector<QVariant>>& Rows)
    {
        // EXPLAIN QUERY PLAN 의 (id, parent, notused, detail) 을 트리로. 색인 없는 전체 검색은 주황(시안 ‘실행 계획’).
        planTree_ = new QTreeWidget(resultPages_);
        planTree_->setObjectName(QStringLiteral("sqlitePlanTree"));
        planTree_->setHeaderHidden(true);
        planTree_->setFrameShape(QFrame::NoFrame);
        planTree_->setUniformRowHeights(true);
        const auto& Colors = fm::style::themeColorsFor(top_);
        QHash<int, QTreeWidgetItem*> Items;
        int Scans = 0;
        for (const auto& Row : Rows) {
            if (Row.size() < 4) continue;
            const int Id = Row[0].toInt(), Parent = Row[1].toInt();
            const auto Detail = Row[3].toString();
            auto* Item = Items.contains(Parent) ? new QTreeWidgetItem(Items.value(Parent)) : new QTreeWidgetItem(planTree_);
            Item->setText(0, Detail);
            const bool FullScan = Detail.startsWith(QLatin1String("SCAN ")) && !Detail.contains(QLatin1String(" USING "))
                                  && !Detail.contains(QLatin1String("CONSTANT ROW")) && !Detail.contains(QLatin1String("SUBQUERY"));
            if (FullScan) {
                ++Scans;
                Item->setIcon(0, Glyph(QStringLiteral("warn"), Colors[T::Warn], 14));
                Item->setToolTip(0, tr("전체 검색 — 색인을 쓰지 않고 모든 행을 읽습니다."));
            } else if (Detail.startsWith(QLatin1String("SEARCH ")) || Detail.startsWith(QLatin1String("SCAN "))) {
                Item->setIcon(0, Glyph(QStringLiteral("index"), Colors[T::Ok], 14));
            } else {
                Item->setIcon(0, Glyph(QStringLiteral("plan"), Colors[T::Fg3], 14));
            }
            Items.insert(Id, Item);
        }
        planTree_->expandAll();
        planTree_->setProperty("sqlitePlanNote", tr("실행 계획 — %1단계, 전체 검색 %2").arg(Rows.size()).arg(Scans));
        resultPages_->insertWidget(0, planTree_);
        resultTabs_->insertTab(0, tr("실행 계획"));
        resultTabs_->setCurrentIndex(0);
    }

    void QueryTab::ApplyFonts(const QFont& Grid, const QFont& Mono, bool MonoValues)
    {
        gridFont_ = Grid;
        monoFont_ = Mono;
        monoValues_ = MonoValues;
        hasFonts_ = true;
        editor_->setEditorFont(Mono);
        // 목록의 형식 열은 쿼리 편집기 글꼴(고정폭)로, 목록 글자보다 조금 작게(시안).
        const QFont Detail = SizedLike(Mono, popup_->font(), -1.0, 7.0);
        popup_->setDetailFont(Detail);
        // setEditorFont 는 테마 색을 다시 입힌다. 스키마 이름 색 · 현재 문 막대 · 오류 표시 색을 다시 얹는다.
        applyEditorTheme();
        for (const auto& View : results_) {
            View.grid_->setFont(Grid);
            View.grid_->Delegate()->SetFonts(Grid, Mono, MonoValues);
        }
    }
}
