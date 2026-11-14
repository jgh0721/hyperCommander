#include "stdafx.h"
#include "sqliteHistory.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteGlyphs.hpp"
#include "sqliteSql.hpp"

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/ThemeColors.h>
#include <fmstyle/ThemeManager.h>
#include <fmwidgets/FindBox.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QLockFile>
#include <QMenu>
#include <QRegularExpression>
#include <QSaveFile>
#include <QVBoxLayout>

namespace hc::sqlite
{
    namespace
    {
        using T = fm::style::Token;
        constexpr int SqlRole = Qt::UserRole + 1;

        QJsonObject ToJson(const HistoryEntry& Entry)
        {
            QJsonObject Object{{"sql", Entry.sql_}, {"at", Entry.at_.toUTC().toString(Qt::ISODateWithMs)}, {"ms", Entry.ms_}, {"ok", Entry.ok_}};
            if (Entry.rows_ >= 0) Object["rows"] = Entry.rows_;
            if (Entry.moreRows_) Object["more"] = true;
            if (Entry.changes_ >= 0) Object["changes"] = Entry.changes_;
            if (!Entry.error_.isEmpty()) Object["error"] = Entry.error_;
            return Object;
        }

        HistoryEntry FromJson(const QJsonObject& Object)
        {
            HistoryEntry Entry;
            Entry.sql_ = Object["sql"].toString();
            Entry.at_ = QDateTime::fromString(Object["at"].toString(), Qt::ISODateWithMs).toLocalTime();
            Entry.ms_ = Object["ms"].toInteger();
            Entry.ok_ = Object["ok"].toBool(true);
            Entry.rows_ = Object["rows"].toInteger(-1);
            Entry.moreRows_ = Object["more"].toBool();
            Entry.changes_ = Object["changes"].toInteger(-1);
            Entry.error_ = Object["error"].toString();
            return Entry;
        }
    }

    ProfileStore::ProfileStore(const QString& Directory, const QString& DatabasePath)
        : directory_(Directory)
    {
        if (directory_.isEmpty()) return;
        path_ = QFileInfo(DatabasePath).absoluteFilePath();
        // Windows 경로는 대소문자를 가리지 않는다.
        key_ = QString::fromLatin1(QCryptographicHash::hash(QDir::cleanPath(path_).toLower().toUtf8(), QCryptographicHash::Sha1).toHex());
    }

    QString ProfileStore::HistoryFile() const { return directory_.isEmpty() ? QString() : QDir(directory_).filePath(QStringLiteral("sqlite/history/%1.json").arg(key_)); }
    QString ProfileStore::TabsFile() const { return directory_.isEmpty() ? QString() : QDir(directory_).filePath(QStringLiteral("sqlite/tabs/%1.json").arg(key_)); }

    QJsonObject ProfileStore::read(const QString& File) const
    {
        QFile Input(File);
        if (File.isEmpty() || !Input.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(Input.readAll()).object();
    }

    bool ProfileStore::write(const QString& File, const QJsonObject& Object)
    {
        if (File.isEmpty() || !QDir().mkpath(QFileInfo(File).absolutePath())) return false;
        QSaveFile Output(File);
        return Output.open(QIODevice::WriteOnly) && Output.write(QJsonDocument(Object).toJson(QJsonDocument::Compact)) >= 0 && Output.commit();
    }

    bool ProfileStore::Sensitive(const QString& Sql)
    {
        static const QRegularExpression Pragma(QStringLiteral("^\\s*PRAGMA\\s+(?:[\\w\"\\[\\]`]+\\s*\\.\\s*)?[\"\\[`]?(?:hex|text)?(?:re)?key\\b"),
                                               QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression Uri(QStringLiteral("[?&](?:hex|text)?key\\s*="), QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression AttachKey(QStringLiteral("\\bATTACH\\b.*\\bKEY\\b"), QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
        // sqlite3_key 를 SQL 함수로 부르는 확장(sqlcipher_export 등)도 함께 막는다.
        static const QRegularExpression Function(QStringLiteral("\\b(?:sqlcipher_export|see_key|activate_see)\\s*\\("), QRegularExpression::CaseInsensitiveOption);
        return Pragma.match(Sql).hasMatch() || Uri.match(Sql).hasMatch() || AttachKey.match(Sql).hasMatch() || Function.match(Sql).hasMatch();
    }

    QString ProfileStore::WithoutSensitive(const QString& Text)
    {
        const auto Utf8 = Text.toUtf8();
        QByteArray Kept;
        int From = 0;
        for (const auto& Range : SplitStatements(Utf8)) {
            const auto Body = QString::fromUtf8(Utf8.mid(Range.bodyStart_, Range.bodyEnd_ - Range.bodyStart_));
            if (!Sensitive(Body)) continue;
            Kept += Utf8.mid(From, Range.bodyStart_ - From);
            From = Range.bodyEnd_;
        }
        if (From == 0) return Text;
        Kept += Utf8.mid(From);
        return QString::fromUtf8(Kept);
    }

    QList<HistoryEntry> ProfileStore::LoadHistory() const
    {
        QList<HistoryEntry> Entries;
        for (const auto& Value : read(HistoryFile())["entries"].toArray()) {
            auto Entry = FromJson(Value.toObject());
            if (!Entry.sql_.isEmpty() && !Sensitive(Entry.sql_)) Entries.append(Entry);
        }
        return Entries;
    }

    bool ProfileStore::AppendHistory(const QList<HistoryEntry>& Entries, int Limit)
    {
        if (!IsValid() || Limit <= 0) return false;
        QList<HistoryEntry> Fresh;
        for (const auto& Entry : Entries) if (!Entry.sql_.trimmed().isEmpty() && !Sensitive(Entry.sql_)) Fresh.append(Entry);
        if (Fresh.isEmpty()) return true;
        const auto File = HistoryFile();
        QDir().mkpath(QFileInfo(File).absolutePath());
        QLockFile Lock(File + QStringLiteral(".lock"));
        Lock.setStaleLockTime(10000);
        if (!Lock.tryLock(2000)) return false;
        QJsonArray Array;
        // 새것을 앞에. 같은 문이 바로 앞에 있으면 겹쳐 쓰지 않고 시각만 새로.
        for (int Index = int(Fresh.size()) - 1; Index >= 0; --Index) Array.append(ToJson(Fresh[Index]));
        for (const auto& Value : read(File)["entries"].toArray()) {
            if (Array.size() >= Limit) break;
            const auto Sql = Value.toObject()["sql"].toString();
            bool Duplicate = false;
            for (const auto& Entry : Fresh) Duplicate |= Entry.sql_ == Sql;
            if (!Duplicate) Array.append(Value);
        }
        while (Array.size() > Limit) Array.removeLast();
        return write(File, QJsonObject{{"version", 1}, {"path", path_}, {"entries", Array}});
    }

    bool ProfileStore::ClearHistory()
    {
        const auto File = HistoryFile();
        return File.isEmpty() || !QFile::exists(File) || QFile::remove(File);
    }

    QJsonObject ProfileStore::LoadTabs() const { return read(TabsFile()); }

    bool ProfileStore::SaveTabs(const QJsonObject& State)
    {
        if (!IsValid()) return false;
        const auto File = TabsFile();
        QDir().mkpath(QFileInfo(File).absolutePath());
        QLockFile Lock(File + QStringLiteral(".lock"));
        Lock.setStaleLockTime(10000);
        if (!Lock.tryLock(2000)) return false;
        auto Object = State;
        Object["version"] = 1;
        Object["path"] = path_;
        return write(File, Object);
    }

    bool ProfileStore::ClearAllHistory(const QString& Directory)
    {
        if (Directory.isEmpty()) return false;
        QDir History(QDir(Directory).filePath(QStringLiteral("sqlite/history")));
        if (!History.exists()) return true;
        bool Ok = true;
        for (const auto& Name : History.entryList({QStringLiteral("*.json")}, QDir::Files)) Ok &= History.remove(Name);
        return Ok;
    }

    HistoryPanel::HistoryPanel(QWidget* Parent) : QWidget(Parent)
    {
        setObjectName(QStringLiteral("sqliteHistoryPanel"));
        auto* Layout = new QVBoxLayout(this);
        Layout->setContentsMargins(8, 8, 8, 4);
        Layout->setSpacing(6);
        filter_ = new fm::ui::FindBox(this);
        filter_->setObjectName(QStringLiteral("sqliteHistoryFilter"));
        filter_->setPlaceholderText(tr("실행한 문 찾기"));
        Layout->addWidget(filter_);
        list_ = new QListWidget(this);
        list_->setObjectName(QStringLiteral("sqliteHistoryList"));
        list_->setFrameShape(QFrame::NoFrame);
        list_->setWordWrap(true);
        list_->setTextElideMode(Qt::ElideRight);
        list_->setUniformItemSizes(false);
        list_->setContextMenuPolicy(Qt::CustomContextMenu);
        Layout->addWidget(list_, 1);
        auto* Empty = new QLabel(tr("실행한 문이 여기에 남습니다. 키를 넣는 PRAGMA 는 남기지 않습니다."), this);
        Empty->setWordWrap(true);
        Empty->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        Empty->setContentsMargins(8, 16, 8, 8);
        empty_ = Empty;
        Layout->addWidget(Empty, 1);
        list_->hide();
        connect(filter_, &QLineEdit::textChanged, this, &HistoryPanel::applyFilter);
        connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* Item) { if (Item) emit InsertRequested(Item->data(SqlRole).toString()); });
        connect(list_, &QListWidget::customContextMenuRequested, this, &HistoryPanel::showMenu);
        connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, &HistoryPanel::applyTheme);
        applyTheme();
    }

    QListWidgetItem* HistoryPanel::makeItem(const HistoryEntry& Entry) const
    {
        const auto& Colors = fm::style::themeColorsFor(this);
        auto* Item = new QListWidgetItem;
        QString Meta = QLocale().toString(Entry.at_, QStringLiteral("MM-dd HH:mm")) + QStringLiteral(" · %1 ms").arg(Entry.ms_);
        if (!Entry.ok_) Meta += QStringLiteral(" · ") + Entry.error_;
        else if (Entry.changes_ >= 0) Meta += tr(" · %1행 바뀜").arg(QLocale().toString(Entry.changes_));
        else if (Entry.rows_ >= 0) Meta += tr(" · %1행%2").arg(QLocale().toString(Entry.rows_), Entry.moreRows_ ? QStringLiteral("+") : QString());
        Item->setText(Summary(Entry.sql_, 120) + QLatin1Char('\n') + Meta);
        Item->setToolTip(Entry.sql_);
        Item->setData(SqlRole, Entry.sql_);
        Item->setIcon(Glyph(Entry.ok_ ? QStringLiteral("ok") : QStringLiteral("err"), Colors[Entry.ok_ ? T::Ok : T::Danger], 14));
        return Item;
    }

    void HistoryPanel::SetEntries(const QList<HistoryEntry>& Entries)
    {
        entries_ = Entries;
        list_->clear();
        for (const auto& Entry : entries_) list_->addItem(makeItem(Entry));
        applyFilter();
    }

    void HistoryPanel::Prepend(const QList<HistoryEntry>& Entries, int Limit)
    {
        // 실행 순서대로 받은 것을 새것이 위로 오게.
        for (const auto& Entry : Entries) {
            if (Entry.sql_.trimmed().isEmpty() || ProfileStore::Sensitive(Entry.sql_)) continue;
            for (int Index = int(entries_.size()) - 1; Index >= 0; --Index)
                if (entries_[Index].sql_ == Entry.sql_) { entries_.removeAt(Index); delete list_->takeItem(Index); }
            entries_.prepend(Entry);
            list_->insertItem(0, makeItem(Entry));
        }
        while (Limit > 0 && entries_.size() > Limit) { entries_.removeLast(); delete list_->takeItem(list_->count() - 1); }
        applyFilter();
    }

    void HistoryPanel::SetMonoFont(const QFont& Font) { list_->setFont(Font); }

    void HistoryPanel::applyFilter()
    {
        const auto Text = filter_->text().trimmed();
        int Shown = 0;
        for (int Row = 0; Row < list_->count(); ++Row) {
            auto* Item = list_->item(Row);
            const bool Visible = Text.isEmpty() || Item->data(SqlRole).toString().contains(Text, Qt::CaseInsensitive);
            Item->setHidden(!Visible);
            Shown += Visible;
        }
        list_->setVisible(list_->count() > 0);
        empty_->setVisible(list_->count() == 0);
        Q_UNUSED(Shown);
    }

    void HistoryPanel::applyTheme()
    {
        auto Palette = empty_->palette();
        Palette.setColor(QPalette::WindowText, fm::style::themeColorsFor(this)[T::Fg3]);
        empty_->setPalette(Palette);
    }

    void HistoryPanel::changeEvent(QEvent* Event)
    {
        QWidget::changeEvent(Event);
        if (Event->type() == QEvent::PaletteChange || Event->type() == QEvent::StyleChange) applyTheme();
    }

    void HistoryPanel::showMenu(const QPoint& Position)
    {
        auto* Item = list_->itemAt(Position);
        QMenu Menu(this);
        if (Item) {
            const auto Sql = Item->data(SqlRole).toString();
            Menu.addAction(tr("쿼리 탭에 넣기"), this, [this, Sql] { emit InsertRequested(Sql); });
            Menu.addAction(tr("새 쿼리 탭에서 열기"), this, [this, Sql] { emit OpenRequested(Sql); });
            Menu.addAction(tr("복사"), this, [Sql] { QApplication::clipboard()->setText(Sql); });
            Menu.addSeparator();
        }
        Menu.addAction(tr("이 데이터베이스의 기록 지우기…"), this, &HistoryPanel::ClearRequested)->setEnabled(list_->count() > 0);
        Menu.exec(list_->viewport()->mapToGlobal(Position));
    }
}
