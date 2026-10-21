#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

namespace hc::passwords
{
    // Secret values remain DPAPI-protected in drafts and session caches.
    struct Entry
    {
        QString id_, name_;
        QByteArray protectedValue_;
        bool automatic_ = true;
    };

    class Store
    {
    public:
        explicit Store(QString Path = {});
        bool Load();
        bool ReloadIfChanged();
        bool Save();
        bool Unlock(const QString& Master);
        void Lock();
        bool SetMaster(const QString& Master);
        bool Add(const QString& Name, const QString& Value, bool Automatic = true);
        bool Edit(int Row, const QString& Name, const QString& Value, bool Automatic);
        bool Remove(int Row);
        QString Value(int Row) const;
        const QVector<Entry>& Entries() const { return entries_; }
        bool Locked() const { return locked_; }
        bool HasMaster() const { return master_; }
        bool Automatic() const { return automatic_; }
        void SetAutomatic(bool On) { automatic_ = On; }
        const QString& Error() const { return error_; }
        const QString& Path() const { return path_; }
        static QByteArray Protect(const QByteArray& Plain);
        static QByteArray Reveal(const QByteArray& Protected);
    private:
        bool fail(const QString& Message);
        bool decode(QByteArray Plain);
        QByteArray encode() const;
        QString path_, error_;
        QByteArray revision_, envelope_, salt_, protectedKey_;
        QVector<Entry> entries_;
        bool master_ = false, locked_ = false, automatic_ = true;
    };
}
