#include "qtAutomation.hpp"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

namespace hc::automation {
namespace {
class LineParser {
public:
    explicit LineParser(QString Text) : text_(std::move(Text)) {}
    void Space() { while (position_ < text_.size() && text_[position_].isSpace()) ++position_; }
    bool Take(QChar Char) { Space(); if (position_ == text_.size() || text_[position_] != Char) return false; ++position_; return true; }
    bool Keyword(const QString& Word) {
        Space(); if (text_.mid(position_, Word.size()) != Word) return false;
        const auto End = position_ + Word.size(); if (End < text_.size() && !text_[End].isSpace()) return false;
        position_ = End; return true;
    }
    std::optional<QString> String() {
        Space(); const auto Start = position_; if (!Take('"')) return {};
        bool Escaped = false, Closed = false;
        while (position_ < text_.size()) {
            const auto Char = text_[position_++];
            if (!Escaped && Char == '"') { Closed = true; break; }
            if (!Escaped && Char == '\\') Escaped = true; else Escaped = false;
        }
        if (!Closed) return {};
        QJsonParseError Error;
        const auto Json = QJsonDocument::fromJson(('[' + text_.mid(Start, position_ - Start) + ']').toUtf8(), &Error);
        if (Error.error != QJsonParseError::NoError || Json.array().size() != 1 || !Json[0].isString()) return {};
        const auto Value = Json[0].toString(); if (Value.contains(QChar::Null)) return {}; return Value;
    }
    std::optional<Value> Scalar() {
        Space(); if (position_ < text_.size() && text_[position_] == '"') {
            const auto Text = String(); if (Text) return Value(Text->toStdString()); return {};
        }
        const auto Start = position_;
        while (position_ < text_.size() && text_[position_] != ',' && text_[position_] != '}' && !text_[position_].isSpace()) ++position_;
        const auto Token = text_.mid(Start, position_ - Start);
        if (Token == "true") return Value(true); if (Token == "false") return Value(false);
        // Preserve int64 exactly: QJson's floating conversion must not round it.
        bool Ok = false; const auto Integer = Token.toLongLong(&Ok);
        if (!Ok || Token.startsWith('+') || Token.contains('.') || Token.contains('e', Qt::CaseInsensitive)) return {};
        const auto Digits = Token.startsWith('-') ? Token.mid(1) : Token;
        if (Digits.size() > 1 && Digits.startsWith('0')) return {};
        return Value(std::int64_t(Integer));
    }
    qsizetype Column() const { return position_ + 1; }
    bool End() { Space(); return position_ == text_.size(); }
private:
    QString text_;
    qsizetype position_ = 0;
};
}
Result<Program> Compile(const QString& Source, Diagnostic* Details)
{
    if (Details) *Details = {};
    const auto Fail = [Details](qsizetype Line, qsizetype Column, QString Message) -> Result<Program> {
        if (Details) *Details = {Line, Column, std::move(Message)};
        return Error{ErrorCategory::InvalidRequest, "automation-syntax:" + std::to_string(Line) + ":" + std::to_string(Column)};
    };
    if (Source.size() > 65536 || Source.contains(QChar::Null)) return Fail(1, 1, QObject::tr("스크립트 길이 제한 또는 NUL 문자 오류"));
    Program Result;
    const auto Lines = Source.split('\n');
    for (qsizetype I = 0; I < Lines.size(); ++I) {
        if (Lines[I].trimmed().isEmpty() || Lines[I].trimmed().startsWith('#')) continue;
        LineParser Parser(Lines[I]); Parser.Keyword("await");
        if (!Parser.Keyword("invoke")) return Fail(I + 1, Parser.Column(), QObject::tr("invoke 명령이 필요합니다"));
        auto Command = Parser.String();
        if (!Command || Command->isEmpty() || Command->size() > 128 || !Parser.Take('{')) return Fail(I + 1, Parser.Column(), QObject::tr("명령 문자열과 인자 객체가 필요합니다"));
        Step Step; Step.command_ = Command->toStdString(); Step.line_ = I + 1;
        if (!Parser.Take('}')) {
            for (;;) {
                auto Key = Parser.String();
                if (!Key || Key->isEmpty() || !Parser.Take(':')) return Fail(I + 1, Parser.Column(), QObject::tr("인자 이름과 콜론이 필요합니다"));
                auto Value = Parser.Scalar();
                if (!Value) return Fail(I + 1, Parser.Column(), QObject::tr("문자열·정수·불리언 인자가 필요합니다"));
                if (!Step.parameters_.emplace(Key->toStdString(), *Value).second || Step.parameters_.size() > 64) return Fail(I + 1, Parser.Column(), QObject::tr("인자 중복 또는 개수 제한 초과"));
                if (Parser.Take('}')) break;
                if (!Parser.Take(',')) return Fail(I + 1, Parser.Column(), QObject::tr("쉼표 또는 닫는 중괄호가 필요합니다"));
            }
        }
        if (!Parser.End()) return Fail(I + 1, Parser.Column(), QObject::tr("문장 뒤에 해석할 수 없는 내용이 있습니다"));
        Result.push_back(std::move(Step));
        if (Result.size() > 256) return Fail(I + 1, 1, QObject::tr("명령은 최대 256개입니다"));
    }
    if (Result.empty()) return Fail(1, 1, QObject::tr("실행할 명령이 없습니다"));
    return Result;
}
namespace {
class AutomationDialog final : public QDialog {
public:
    AutomationDialog(QWidget* Parent, std::function<Host()> MakeHost) : QDialog(Parent), makeHost_(std::move(MakeHost)) {
        setAttribute(Qt::WA_DeleteOnClose); setObjectName("hc.automationDialog"); setWindowTitle(tr("명령 자동화")); resize(720, 460);
        auto* Layout = new QVBoxLayout(this);
        auto* Note = new QLabel(tr("시작한 탭과 선택 항목을 사용합니다. 각 명령의 작업이 끝나면 다음 줄을 실행합니다."), this); Note->setWordWrap(true); Layout->addWidget(Note);
        source_ = new QPlainTextEdit(this); source_->setObjectName("hc.automation.source"); source_->setPlainText("invoke \"hc.navigate.refresh\" {}\n"); Layout->addWidget(source_);
        write_ = new QCheckBox(tr("이번 실행에서 파일 변경 허용"), this); write_->setObjectName("hc.automation.write"); Layout->addWidget(write_);
        status_ = new QLabel(this); status_->setObjectName("hc.automation.status"); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true); Layout->addWidget(status_);
        auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Close, this); Layout->addWidget(Buttons);
        run_ = Buttons->addButton(tr("실행"), QDialogButtonBox::ActionRole); run_->setObjectName("hc.automation.run");
        stop_ = Buttons->addButton(tr("중지"), QDialogButtonBox::ActionRole); stop_->setObjectName("hc.automation.stop"); stop_->setEnabled(false);
        connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
        connect(run_, &QPushButton::clicked, this, [this] { run(); });
        connect(stop_, &QPushButton::clicked, this, [this] { if (execution_) execution_->Cancel(); update(); });
        timer_ = new QTimer(this); timer_->setInterval(20); connect(timer_, &QTimer::timeout, this, [this] { execution_->Tick(); update(); });
        connect(this, &QDialog::finished, this, [this] { timer_->stop(); if (execution_) execution_->Cancel(); });
    }
private:
    void run() {
        if (execution_ && execution_->Status() == State::Running) return;
        Diagnostic Details; auto Parsed = Compile(source_->toPlainText(), &Details);
        if (!Parsed.HasValue()) { status_->setText(tr("%1행 %2열: %3").arg(Details.line_).arg(Details.column_).arg(Details.message_)); return; }
        std::set<std::string> Permissions{"navigation"}; if (write_->isChecked()) Permissions.insert("file-write");
        execution_ = std::make_unique<Execution>(std::move(Parsed.Value()), makeHost_(), Permissions, QUuid::createUuid().toString(QUuid::Id128).toStdString());
        source_->setReadOnly(true); write_->setEnabled(false); run_->setEnabled(false); stop_->setEnabled(true); timer_->start(); update();
    }
    void update() {
        if (!execution_) return;
        if (execution_->Status() == State::Running) { status_->setText(tr("%1개 명령 완료 · %2행 실행 중").arg(execution_->CompletedSteps()).arg(execution_->Line())); return; }
        timer_->stop(); source_->setReadOnly(false); write_->setEnabled(true); run_->setEnabled(true); stop_->setEnabled(false);
        status_->setText(execution_->Status() == State::Completed ? tr("완료 · %1개 명령").arg(execution_->CompletedSteps())
            : tr("%1행에서 중단: %2").arg(execution_->Line()).arg(QString::fromStdString(execution_->Failure()->code_)));
    }
    std::function<Host()> makeHost_;
    std::unique_ptr<Execution> execution_;
    QPlainTextEdit* source_;
    QCheckBox* write_;
    QLabel* status_;
    QPushButton* run_;
    QPushButton* stop_;
    QTimer* timer_;
};
}
void Show(QWidget* Parent, std::function<Host()> MakeHost) { (new AutomationDialog(Parent, std::move(MakeHost)))->show(); }
}
