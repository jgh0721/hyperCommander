#include "stdafx.h"

/// Product integration
#include "UniqueLibs/document/documentThemeAdapter.hpp"

/// Platform/framework headers
#include <fmstyle/ThemeManager.h>
#include <QCloseEvent>
#include <QCommandLineParser>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

Q_LOGGING_CATEGORY(epubPocLog, "hc.epub.poc")

namespace
{
    constexpr qsizetype ReportLimit = 4 * 1024 * 1024;
    class ProbeWindow final : public QMainWindow
    {
    public:
        explicit ProbeWindow(const QString& Capture) : capture_(Capture)
        {
            setObjectName("epubPocWindow");
            setWindowTitle(tr("EPUB P0 · 패키지 검사 — Watercolor"));
            resize(1160, 820);
            auto* Content = new QWidget(this);
            setCentralWidget(Content);
            auto* Layout = new QVBoxLayout(Content);
            auto* Actions = new QHBoxLayout;
            open_ = new QPushButton(tr("EPUB 검사…"), Content);
            open_->setObjectName("epubProbeOpen");
            cancel_ = new QPushButton(tr("취소"), Content);
            cancel_->setEnabled(false);
            Actions->addWidget(open_);
            Actions->addWidget(cancel_);
            Actions->addStretch();
            for (const auto& Pair : {qMakePair(QString("system"), tr("시스템")),
                                     qMakePair(QString("light"), tr("라이트")),
                                     qMakePair(QString("dark"), tr("다크"))})
            {
                auto* Button = new QPushButton(Pair.second, Content);
                connect(Button, &QPushButton::clicked, this, [Mode = Pair.first] {
                    hc::document::DocumentThemeAdapter::SetMode(Mode);
                });
                Actions->addWidget(Button);
            }
            Layout->addLayout(Actions);
            source_ = new QLineEdit(Content);
            source_->setReadOnly(true);
            source_->setAccessibleName(tr("검사한 EPUB 경로"));
            Layout->addWidget(source_);
            summary_ = new QLabel(tr("EPUB을 선택하면 원본 변경 없이 구조와 이미지 참조를 검사합니다."), Content);
            summary_->setTextFormat(Qt::PlainText);
            summary_->setWordWrap(true);
            summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
            Layout->addWidget(summary_);
            auto* Notice = new QLabel(tr("P0 개발 도구 · 본문 렌더러 / EPUBCheck / 저장 기능 미연결\n"
                "구조 검사 통과는 EPUB 규격 적합성이나 제품 읽기 지원을 의미하지 않습니다."), Content);
            Notice->setWordWrap(true);
            Layout->addWidget(Notice);
            auto* Split = new QSplitter(Content);
            spine_ = new QTreeWidget(Split);
            spine_->setHeaderLabels({tr("읽기 순서 · spine"), tr("리소스")});
            spine_->setRootIsDecorated(false);
            tabs_ = new QTabWidget(Split);
            auto* Tabs = tabs_;
            resources_ = table(Tabs, {tr("파일"), tr("MIME"), tr("바이트")});
            images_ = table(Tabs, {tr("본문 위치"), tr("이미지 리소스"), tr("대체 텍스트")});
            issues_ = table(Tabs, {tr("분류"), tr("코드"), tr("위치")});
            Tabs->addTab(resources_, tr("책 리소스"));
            Tabs->addTab(images_, tr("이미지 참조"));
            Tabs->addTab(issues_, tr("검사 결과"));
            Tabs->setCurrentWidget(images_);
            Split->setSizes({320, 800});
            Layout->addWidget(Split, 1);
            statusBar()->showMessage(tr("준비 · 원본 읽기 전용"));
            connect(open_, &QPushButton::clicked, this, [this] {
                auto* Dialog = new QFileDialog(this, tr("검사할 EPUB"), {}, tr("EPUB (*.epub)"));
                Dialog->setFileMode(QFileDialog::ExistingFile);
                Dialog->setAttribute(Qt::WA_DeleteOnClose);
                connect(Dialog, &QFileDialog::fileSelected, this, &ProbeWindow::Inspect);
                Dialog->open();
            });
            connect(cancel_, &QPushButton::clicked, this, [this] { stop(tr("사용자가 검사를 취소했습니다.")); });
            timeout_.setSingleShot(true);
            connect(&timeout_, &QTimer::timeout, this, [this] { stop(tr("검사 제한 시간 30초를 초과했습니다.")); });
            connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
                output_ += process_.readAllStandardOutput();
                if (output_.size() > ReportLimit) stop(tr("검사 결과의 크기 한도를 초과했습니다."));
            });
            connect(&process_, &QProcess::readyReadStandardError, this, [this] {
                process_.readAllStandardError(); // Do not expose arbitrary paths/tracebacks as UI markup.
            });
            connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError Error) {
                if (Error == QProcess::FailedToStart) {
                    timeout_.stop(); open_->setEnabled(true); cancel_->setEnabled(false);
                    fail(tr("Python 검사 프로세스를 시작하지 못했습니다. 개발 도구 빌드 설정을 확인하세요."));
                }
            });
            connect(&process_, &QProcess::finished, this, [this](int Code, QProcess::ExitStatus Status) {
                timeout_.stop(); open_->setEnabled(true); cancel_->setEnabled(false);
                output_ += process_.readAllStandardOutput();
                if (closing_) { QTimer::singleShot(0, this, &QWidget::close); return; }
                if (stopped_) { finishCapture(2); return; }
                if (output_.size() > ReportLimit || Status != QProcess::NormalExit || (Code != 0 && Code != 2)) {
                    fail(tr("검사 프로세스가 정상적인 결과를 반환하지 않았습니다.")); return;
                }
                if (!LoadReport(output_)) fail(tr("검사 결과 형식이 올바르지 않습니다."));
            });
        }

        void Inspect(const QString& Path)
        {
            if (process_.state() != QProcess::NotRunning) return;
            clear(); source_->setText(Path); stopped_ = false;
            summary_->setText(tr("패키지 검사 중…"));
            statusBar()->showMessage(tr("읽기 전용 검사 · 최대 30초"));
            open_->setEnabled(false); cancel_->setEnabled(true);
            process_.setProgram(QString::fromUtf8(HC_EPUB_PYTHON));
            process_.setArguments({QString::fromUtf8(HC_EPUB_PROBE), QFileInfo(Path).absoluteFilePath()});
            process_.start(); timeout_.start(30000);
        }

        bool LoadReport(const QByteArray& Bytes)
        {
            if (Bytes.size() > ReportLimit) return false;
            QJsonParseError Error;
            const auto Document = QJsonDocument::fromJson(Bytes, &Error);
            const auto Report = Document.object();
            const auto Status = Report.value("status").toString();
            if (Error.error != QJsonParseError::NoError || Report.value("schemaVersion").toInt() != 1 ||
                Report.value("phase").toString() != "P0-A" ||
                !QStringList{"structure-checked", "restricted", "rejected"}.contains(Status)) return false;
            clear(); source_->setText(Report.value("source").toString());
            const auto StatusLabel = Status == "structure-checked" ? tr("구조 검사 통과") :
                Status == "restricted" ? tr("제한 있음") : tr("오류");
            summary_->setText(tr("%1\n%2 · package version %3 · 리소스 %4개 · 이미지 사용 위치 %5개")
                .arg(Report.value("title").toString(), StatusLabel, Report.value("packageVersion").toString())
                .arg(Report.value("resources").toArray().size()).arg(Report.value("images").toArray().size()));
            int Index = 0;
            for (const auto& Value : Report.value("spine").toArray()) {
                const auto Item = Value.toObject();
                new QTreeWidgetItem(spine_, {QString::number(++Index) + " · " + Item.value("id").toString(), Item.value("path").toString()});
            }
            for (const auto& Value : Report.value("resources").toArray()) {
                const auto Item = Value.toObject();
                new QTreeWidgetItem(resources_, {Item.value("path").toString(), Item.value("mediaType").toString(), QString::number(Item.value("bytes").toInteger())});
            }
            for (const auto& Value : Report.value("images").toArray()) {
                const auto Item = Value.toObject();
                const auto State = Item.value("altState").toString();
                new QTreeWidgetItem(images_, {Item.value("document").toString(), Item.value("resource").toString(),
                    State == "missing" ? tr("대체 텍스트 없음") : State == "decorative" ? tr("장식용 · 빈 alt") : Item.value("alt").toString()});
            }
            for (const auto& Value : Report.value("issues").toArray()) {
                const auto Item = Value.toObject();
                new QTreeWidgetItem(issues_, {Item.value("severity").toString(), Item.value("code").toString(), Item.value("location").toString()});
            }
            for (auto* View : {spine_, resources_, images_, issues_}) View->resizeColumnToContents(0);
            if (images_->topLevelItemCount()) images_->setCurrentItem(images_->topLevelItem(0));
            tabs_->setCurrentWidget(Status == "structure-checked" ? images_ : issues_);
            statusBar()->showMessage(tr("%1 · 렌더러 미검증 · EPUBCheck 미실행 · 원본 유지").arg(StatusLabel));
            finishCapture(Status == "rejected" ? 2 : 0);
            return true;
        }

    protected:
        void closeEvent(QCloseEvent* Event) override
        {
            if (process_.state() != QProcess::NotRunning) {
                closing_ = true; stop(tr("검사 종료 중…")); Event->ignore(); return;
            }
            QMainWindow::closeEvent(Event);
        }

    private:
        static QTreeWidget* table(QWidget* Parent, const QStringList& Headers)
        {
            auto* View = new QTreeWidget(Parent);
            View->setHeaderLabels(Headers); View->setRootIsDecorated(false);
            View->setAlternatingRowColors(true); View->setUniformRowHeights(true);
            return View;
        }
        void clear() { output_.clear(); for (auto* View : {spine_, resources_, images_, issues_}) View->clear(); }
        void fail(const QString& Message) { summary_->setText(Message); statusBar()->showMessage(tr("검사 실패 · 원본 유지")); qCWarning(epubPocLog) << Message; finishCapture(2); }
        void stop(const QString& Message) { stopped_ = true; summary_->setText(Message); timeout_.stop(); cancel_->setEnabled(false); process_.kill(); }
        void finishCapture(int Code)
        {
            if (capture_.isEmpty()) return;
            const auto Path = capture_; capture_.clear();
            QTimer::singleShot(250, this, [this, Path, Code] { const auto Saved = grab().save(Path); QCoreApplication::exit(Saved ? Code : 3); });
        }
        QString capture_;
        QByteArray output_;
        QProcess process_;
        QTimer timeout_;
        bool stopped_ = false, closing_ = false;
        QPushButton* open_ = nullptr;
        QPushButton* cancel_ = nullptr;
        QLineEdit* source_ = nullptr;
        QLabel* summary_ = nullptr;
        QTreeWidget* spine_ = nullptr;
        QTreeWidget* resources_ = nullptr;
        QTreeWidget* images_ = nullptr;
        QTreeWidget* issues_ = nullptr;
        QTabWidget* tabs_ = nullptr;
    };
}

int main(int Argc, char** Argv)
{
    QApplication App(Argc, Argv);
    QCoreApplication::setApplicationName("HyperCommander EPUB P0");
    QCommandLineParser Parser;
    Parser.addHelpOption();
    Parser.addOptions({{{"e", "epub"}, "Inspect a local EPUB read-only", "path"},
        {{"r", "report"}, "Open an existing P0 JSON report", "path"},
        {{"t", "theme"}, "system, light, or dark", "theme", "system"},
        {{"c", "capture"}, "Save a verification screenshot and exit", "path"}});
    Parser.process(App);
    if (Parser.isSet("capture") && QFileInfo::exists(Parser.value("capture"))) return 2;
    hc::document::DocumentThemeAdapter::Install(App);
    if (!hc::document::DocumentThemeAdapter::SetMode(Parser.value("theme"))) return 2;
    ProbeWindow Window(Parser.value("capture"));
    if (Parser.isSet("capture")) Window.setAttribute(Qt::WA_DontShowOnScreen);
    Window.show();
    if (Parser.isSet("report")) {
        QFile File(Parser.value("report"));
        if (!File.open(QIODevice::ReadOnly) || File.size() > ReportLimit || !Window.LoadReport(File.readAll())) return 2;
    } else if (Parser.isSet("epub")) Window.Inspect(Parser.value("epub"));
    else if (Parser.isSet("capture")) return 2;
    return App.exec();
}
