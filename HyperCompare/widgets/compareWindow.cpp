#include "stdafx.h"
#include "compareWindow.hpp"
#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFontDialog>
#include <QLabel>
#include <QJsonDocument>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <fmstyle/ThemeManager.h>
#include <ScintillaEditBase.h>
#include <Scintilla.h>
#include <ILexer.h>
#include <Lexilla.h>

namespace hc::compare
{
    namespace
    {
        QByteArray contents(ScintillaEditBase* Editor)
        {
            const auto length = Editor->send(SCI_GETLENGTH);
            QByteArray result(qsizetype(length) + 1, '\0');
            Editor->send(SCI_GETTEXT, uptr_t(result.size()), reinterpret_cast<sptr_t>(result.data()));
            result.resize(qsizetype(length)); return result;
        }
        const char* lexer(const QString& Path)
        {
            const auto ext = QFileInfo(Path).suffix().toLower();
            if (QStringList{"c", "cpp", "cc", "cxx", "h", "hpp", "cs", "java", "js", "ts"}.contains(ext)) return "cpp";
            if (ext == "py") return "python";
            if (ext == "json") return "json";
            if (ext == "xml" || ext == "html") return "xml";
            return "null";
        }
        sptr_t color(const QColor& Color) { return Color.red() | (Color.green() << 8) | (Color.blue() << 16); }
    }
    CompareWindow::CompareWindow(Request Input) : request_(std::move(Input))
    {
        setWindowTitle("HyperCompare"); resize(1280, 800);
        QSettings settings; dark_ = request_.theme_.isEmpty() ? settings.value("theme/dark", false).toBool() : request_.theme_ == "dark";
        uiFont_ = qApp->font(); uiFont_.fromString(settings.value("fonts/ui", uiFont_.toString()).toString());
        listFont_ = uiFont_; listFont_.fromString(settings.value("fonts/list", listFont_.toString()).toString());
        textFont_ = QFont("Cascadia Mono", 11); textFont_.fromString(settings.value("fonts/text", textFont_.toString()).toString());
        auto* layout = new QSplitter(Qt::Vertical, this); panes_ = new QSplitter(Qt::Horizontal, layout);
        changes_ = new QTreeWidget(layout); layout->addWidget(panes_); layout->addWidget(changes_);
        panes_->setChildrenCollapsible(false); layout->setChildrenCollapsible(false);
        changes_->setHeaderLabels({tr("차이 / 경로"), tr("왼쪽"), tr("오른쪽")});
        changes_->setObjectName("compareChanges");
        changes_->setRootIsDecorated(false); changes_->setUniformRowHeights(true);
        layout->setStretchFactor(0, 4); layout->setStretchFactor(1, 1); layout->setSizes({560, 140}); setCentralWidget(layout);
        auto* tools = addToolBar(tr("비교"));
        tools->addAction(tr("파일 열기"), this, &CompareWindow::openInputs);
        tools->addAction(tr("다시 비교"), this, [this] {
            if (!dirty_ || QMessageBox::question(this, tr("수정 결과"), tr("저장하지 않은 결과를 버리고 다시 비교하시겠습니까?")) == QMessageBox::Yes) start();
        });
        cancel_ = tools->addAction(tr("취소"), this, [this] { canceled_->store(true); state_->setText(tr("취소 요청 중")); });
        cancel_->setEnabled(false);
        save_ = tools->addAction(tr("결과 저장"), this, &CompareWindow::save); save_->setEnabled(false);
        tools->addAction(tr("왼쪽 전체 선택"), this, [this] { chooseSide(0); });
        tools->addAction(tr("오른쪽 전체 선택"), this, [this] { chooseSide(1); });
        tools->addAction(tr("수동 해결 완료"), this, [this] {
            if (output_ && !watcher_.isRunning()) { unresolved_ = false; state_->setText(tr("수동 해결 완료 — 결과를 저장하십시오.")); }
        });
        auto* viewMenu = menuBar()->addMenu(tr("보기"));
        auto* dark = viewMenu->addAction(tr("Dark")); dark->setCheckable(true); dark->setChecked(dark_);
        connect(dark, &QAction::toggled, this, [this](bool Value) { dark_ = Value; applyAppearance(); });
        for (const auto& role : {QString("ui"), QString("list"), QString("text")})
            viewMenu->addAction(role.toUpper() + tr(" 글꼴"), this, [this, role] { chooseFont(role); });
        state_ = new QLabel(this); statusBar()->addWidget(state_, 1);
        state_->setObjectName("compareStatus");
        connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] {
            cancel_->setEnabled(false);
            result_ = watcher_.result();
            if (saving_)
            {
                saving_ = false;
                if (output_) output_->send(SCI_SETREADONLY, 0);
                if (result_.exitCode_ == 0)
                {
                    if (output_) output_->send(SCI_SETSAVEPOINT);
                    dirty_ = false; exitCode_ = 0;
                    state_->setText(result_.warnings_.isEmpty() && result_.recovery_.isEmpty() ? tr("저장 완료") : tr("저장 완료 — 임시 파일 정리 확인 필요"));
                    state_->setToolTip(QString::fromUtf8(QJsonDocument(result_.Json()).toJson()));
                }
                else { exitCode_ = 2; state_->setText(tr("저장 실패: ") + result_.error_); }
                save_->setEnabled(true);
                if (closing_) close();
                return;
            }
            if (closing_) { close(); return; }
            display();
        });
        connect(changes_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* Item) {
            if (!Item || editors_.size() < 2 || result_.operation_ == "directory2") return;
            editors_[0]->send(SCI_GOTOLINE, uptr_t(Item->data(1, Qt::UserRole).toULongLong()));
            editors_[1]->send(SCI_GOTOLINE, uptr_t(Item->data(2, Qt::UserRole).toULongLong()));
        });
        applyAppearance();
        if (!request_.file1_.isEmpty()) QTimer::singleShot(0, this, [this] { start(); });
        else state_->setText(tr("파일 열기 또는 -file1/-file2로 비교를 시작하십시오."));
    }
    CompareWindow::~CompareWindow() { canceled_->store(true); watcher_.waitForFinished(); }
    void CompareWindow::openInputs()
    {
        if (watcher_.isRunning()) return;
        if (dirty_ && QMessageBox::question(this, tr("수정 결과"), tr("저장하지 않은 결과를 버리고 파일을 여시겠습니까?")) != QMessageBox::Yes) return;
        const auto left = QFileDialog::getOpenFileName(this, tr("왼쪽 파일")); if (left.isEmpty()) return;
        const auto right = QFileDialog::getOpenFileName(this, tr("오른쪽 파일")); if (right.isEmpty()) return;
        request_.file1_ = left; request_.file2_ = right; request_.base_.clear(); request_.file3_.clear();
        request_.output_.clear(); request_.mode_ = "compare"; start();
    }
    void CompareWindow::start()
    {
        if (watcher_.isRunning() || request_.file1_.isEmpty()) return;
        canceled_ = std::make_shared<std::atomic_bool>(false); dirty_ = false; exitCode_ = 3;
        save_->setEnabled(false); state_->setText(tr("비교 중…"));
        cancel_->setEnabled(true);
        if (output_) output_->send(SCI_SETREADONLY, 1);
        watcher_.setFuture(QtConcurrent::run([input = request_, cancel = canceled_] { return Execute(input, cancel, false); }));
    }
    ScintillaEditBase* CompareWindow::addPane(const QString& Title, const QString& Path, const QByteArray& Text, bool ReadOnly)
    {
        auto* pane = new QWidget(panes_); auto* box = new QVBoxLayout(pane); box->setContentsMargins(2, 2, 2, 2);
        auto* label = new QLabel(Title + " — " + Path, pane); label->setTextFormat(Qt::PlainText); label->setToolTip(Path);
        label->setMinimumWidth(0); label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); box->addWidget(label);
        auto* editor = new ScintillaEditBase(pane); editor->setObjectName(Title); box->addWidget(editor); editors_.push_back(editor);
        panes_->addWidget(pane); pane->setMinimumWidth(120);
        editor->send(SCI_SETCODEPAGE, SC_CP_UTF8); editor->send(SCI_SETWRAPMODE, SC_WRAP_NONE);
        editor->send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER); editor->send(SCI_SETMARGINWIDTHN, 0, 56);
        editor->send(SCI_SETTEXT, 0, reinterpret_cast<sptr_t>(Text.constData())); editor->send(SCI_EMPTYUNDOBUFFER);
        editor->send(SCI_SETSAVEPOINT); editor->send(SCI_SETREADONLY, ReadOnly);
        editor->send(SCI_SETILEXER, 0, reinterpret_cast<sptr_t>(CreateLexer(lexer(Path))));
        static constexpr char keywords[] = "alignas auto bool break case catch char class const constexpr continue default delete do double else enum explicit extern false float for friend if inline int long namespace new noexcept nullptr operator private protected public return short signed sizeof static struct switch template this throw true try typedef typename union unsigned using virtual void volatile while";
        editor->send(SCI_SETKEYWORDS, 0, reinterpret_cast<sptr_t>(keywords));
        editor->send(SCI_MARKERDEFINE, 8, SC_MARK_BACKGROUND);
        return editor;
    }
    void CompareWindow::display()
    {
        output_ = nullptr; editors_.clear(); changes_->clear();
        while (panes_->count()) delete panes_->widget(0);
        exitCode_ = result_.exitCode_; unresolved_ = !result_.merged_.conflicts_.empty() || result_.error_ == "DecisionsRequired";
        if (result_.completion_ != "Complete") { state_->setText(result_.error_); return; }
        if (result_.operation_ == "directory2")
        {
            for (const auto& value : result_.entries_)
            { const auto entry = value.toObject(); new QTreeWidgetItem(changes_, {entry["path"].toString(), entry["status"].toString()}); }
        }
        else
        {
            for (int i = 0; i < result_.inputs_.size(); ++i)
            {
                const auto& input = result_.inputs_[i];
                const auto name = i == 2 && !request_.base_.isEmpty() ? "BASE" : QString("FILE %1").arg(i + 1);
                addPane(name, input.path_, input.utf8_, true);
            }
            for (std::size_t i = 0; i < result_.edits_.size(); ++i)
            {
                const auto& edit = result_.edits_[i];
                auto* item = new QTreeWidgetItem(changes_, {QString::number(i + 1), QString::number(edit.begin_ + 1), QString::number(edit.otherBegin_ + 1)});
                item->setData(1, Qt::UserRole, qulonglong(edit.begin_)); item->setData(2, Qt::UserRole, qulonglong(edit.otherBegin_));
                for (auto line = edit.begin_; line < edit.end_; ++line) editors_[0]->send(SCI_MARKERADD, uptr_t(line), 8);
                for (auto line = edit.otherBegin_; line < edit.otherEnd_; ++line) editors_[1]->send(SCI_MARKERADD, uptr_t(line), 8);
            }
            if (request_.mode_ == "merge")
            {
                const auto initial = result_.error_ == "DecisionsRequired" ? result_.inputs_[0].utf8_ : result_.output_;
                output_ = addPane("RESULT", request_.output_, initial, false); save_->setEnabled(true);
                connect(output_, &ScintillaEditBase::modified, this, [this](Scintilla::ModificationFlags Type, Scintilla::Position,
                    Scintilla::Position, Scintilla::Position, const QByteArray&, Scintilla::Position, Scintilla::FoldLevel, Scintilla::FoldLevel) {
                    if (settingText_ || !(static_cast<int>(Type) & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) return;
                    dirty_ = true; exitCode_ = 3;
                    if (!result_.merged_.conflicts_.empty() || result_.error_ == "DecisionsRequired") unresolved_ = true;
                });
            }
        }
        state_->setText(QString("%1 — %2, conflicts: %3").arg(result_.operation_, result_.outcome_).arg(result_.merged_.conflicts_.size()));
        if (request_.mode_ == "merge") exitCode_ = 3;
        QList<int> widths; for (int i = 0; i < panes_->count(); ++i) widths.push_back(1);
        panes_->setSizes(widths);
        auto* layout = qobject_cast<QSplitter*>(centralWidget()); layout->setSizes({560, 140});
        applyAppearance();
    }
    void CompareWindow::replaceResult(const QByteArray& Text)
    {
        if (!output_ || watcher_.isRunning()) return;
        settingText_ = true; output_->send(SCI_BEGINUNDOACTION); output_->send(SCI_SETTARGETSTART, 0);
        output_->send(SCI_SETTARGETEND, uptr_t(output_->send(SCI_GETLENGTH)));
        output_->send(SCI_REPLACETARGET, uptr_t(Text.size()), reinterpret_cast<sptr_t>(Text.constData()));
        output_->send(SCI_ENDUNDOACTION); settingText_ = false; dirty_ = true; unresolved_ = false; exitCode_ = 3;
    }
    void CompareWindow::chooseSide(int Index)
    { if (result_.inputs_.size() > Index) replaceResult(result_.inputs_[Index].utf8_); }
    void CompareWindow::save()
    {
        if (!output_ || watcher_.isRunning()) return;
        if (unresolved_) { QMessageBox::information(this, tr("충돌 해결"), tr("충돌을 검토하고 ‘수동 해결 완료’를 선택하십시오.")); return; }
        const auto text = contents(output_);
        output_->send(SCI_SETREADONLY, 1); save_->setEnabled(false); saving_ = true; state_->setText(tr("저장 중…"));
        watcher_.setFuture(QtConcurrent::run([request = request_, result = result_, text]() mutable {
            try { SaveResult(request, result, text); }
            catch (const std::exception& error) { result.exitCode_ = 2; result.error_ = QString::fromUtf8(error.what()); }
            return result;
        }));
    }
    void CompareWindow::applyAppearance()
    {
        auto& theme = fm::style::ThemeManager::instance(); theme.setScheme(dark_ ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light);
        qApp->setFont(uiFont_); changes_->setFont(listFont_);
        const QColor background = dark_ ? QColor("#202632") : QColor("#fbfcff"), foreground = dark_ ? QColor("#e3e8ef") : QColor("#202630");
        const auto family = textFont_.family().toUtf8();
        for (auto* editor : editors_)
        {
            editor->send(SCI_STYLESETFORE, STYLE_DEFAULT, color(foreground)); editor->send(SCI_STYLESETBACK, STYLE_DEFAULT, color(background));
            editor->send(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<sptr_t>(family.constData()));
            editor->send(SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, sptr_t(textFont_.pointSizeF() * 100)); editor->send(SCI_STYLECLEARALL);
            for (const auto style : {1, 2, 3}) editor->send(SCI_STYLESETFORE, style, color(dark_ ? QColor("#90b58b") : QColor("#51774c")));
            editor->send(SCI_STYLESETFORE, 5, color(dark_ ? QColor("#a6c7ff") : QColor("#255ba4")));
            editor->send(SCI_STYLESETFORE, 6, color(dark_ ? QColor("#edc38a") : QColor("#945410")));
            editor->send(SCI_STYLESETBACK, STYLE_LINENUMBER, color(background));
            editor->send(SCI_SETCARETFORE, color(foreground));
            editor->send(SCI_MARKERSETBACK, 8, color(dark_ ? QColor("#4d4635") : QColor("#fff0bb")));
            editor->send(SCI_COLOURISE, 0, -1);
        }
        QSettings settings; settings.setValue("theme/dark", dark_); settings.setValue("fonts/ui", uiFont_.toString());
        settings.setValue("fonts/list", listFont_.toString()); settings.setValue("fonts/text", textFont_.toString());
    }
    void CompareWindow::chooseFont(const QString& Role)
    {
        auto& font = Role == "ui" ? uiFont_ : Role == "list" ? listFont_ : textFont_; bool ok = false;
        const auto selected = QFontDialog::getFont(&ok, font, this, Role.toUpper()); if (ok) { font = selected; applyAppearance(); }
    }
    void CompareWindow::closeEvent(QCloseEvent* Event)
    {
        if (watcher_.isRunning()) { canceled_->store(true); closing_ = true; Event->ignore(); return; }
        if (dirty_ && QMessageBox::question(this, tr("수정 결과"), tr("저장하지 않은 결과를 버리고 닫으시겠습니까?")) != QMessageBox::Yes) { Event->ignore(); return; }
        Event->accept();
    }
}
