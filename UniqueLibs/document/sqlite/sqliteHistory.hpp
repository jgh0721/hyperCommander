#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QWidget>

class QListWidget;
class QListWidgetItem;
namespace fm::ui { class FindBox; }

namespace hc::sqlite
{
    struct HistoryEntry
    {
        QString sql_;
        QDateTime at_;
        qint64 ms_ = 0;
        qint64 rows_ = -1;          // 읽은 행(결과가 있는 문). 더 있으면 moreRows_
        qint64 changes_ = -1;       // 바뀐 행(쓰기 문)
        bool ok_ = true, moreRows_ = false;
        QString error_;
    };

    // 데이터베이스 파일마다의 프로필 자료(설계 5.5): 실행 기록과 쿼리 탭 보관. <프로필>/sqlite/{history,tabs}/<경로 해시>.json.
    // HyperViewer · HyperEditor · 빠른 보기가 같은 파일을 쓰므로 QLockFile 로 쓰기를 막는다.
    // 키를 넣는 PRAGMA(key · hexkey · textkey · rekey …)와 키가 든 URI · ATTACH … KEY 는 어디에도 남기지 않는다(설계 8.3).
    class ProfileStore
    {
    public:
        ProfileStore(const QString& Directory, const QString& DatabasePath);
        bool IsValid() const { return !directory_.isEmpty(); }

        QList<HistoryEntry> LoadHistory() const;            // 새것이 앞
        bool AppendHistory(const QList<HistoryEntry>& Entries, int Limit);
        bool ClearHistory();

        QJsonObject LoadTabs() const;
        bool SaveTabs(const QJsonObject& State);

        static bool Sensitive(const QString& Sql);
        // 여러 문 가운데 키를 넣는 문만 뺀다(쿼리 탭 보관).
        static QString WithoutSensitive(const QString& Text);
        // 설정 › SQLite › 실행 · 기록 › ‘기록 지우기’: 모든 데이터베이스의 기록.
        static bool ClearAllHistory(const QString& Directory);
        QString HistoryFile() const;
        QString TabsFile() const;
    private:
        QJsonObject read(const QString& File) const;
        bool write(const QString& File, const QJsonObject& Object);
        QString directory_, path_, key_;
    };

    // 스키마 패널의 ‘기록’ 탭. 새것이 위, 문 요약(고정폭) + 시각 · 걸린 시간 · 행 수(또는 오류).
    // Enter · 두 번 누르기는 지금 쿼리 탭의 커서 자리에 넣는다(쿼리 탭이 아니면 새 쿼리 탭).
    class HistoryPanel final : public QWidget
    {
        Q_OBJECT
    public:
        explicit HistoryPanel(QWidget* Parent = nullptr);
        void SetEntries(const QList<HistoryEntry>& Entries);
        void Prepend(const QList<HistoryEntry>& Entries, int Limit);
        void SetMonoFont(const QFont& Font);
        QListWidget* List() const { return list_; }
    signals:
        void InsertRequested(const QString& Sql);
        void OpenRequested(const QString& Sql);
        void ClearRequested();
    protected:
        void changeEvent(QEvent* Event) override;
    private:
        QListWidgetItem* makeItem(const HistoryEntry& Entry) const;
        void applyFilter();
        void applyTheme();
        void showMenu(const QPoint& Position);

        fm::ui::FindBox* filter_ = nullptr;
        QListWidget* list_ = nullptr;
        QWidget* empty_ = nullptr;
        QList<HistoryEntry> entries_;
    };
}
