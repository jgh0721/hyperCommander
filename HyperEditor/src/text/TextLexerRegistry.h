#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

struct TextLexerProfile
{
    QString     languageId;
    QString     displayName;
    QString     lexerKey;
    QStringList extensions;
    QStringList fileNames;
    QStringList keywords;
    QHash<int, QString> styles;
};

class TextLexerRegistry
{
public:
    static const TextLexerRegistry& instance();

    QString languageForExtension(const QString& extension) const;
    QString languageForFileName(const QString& fileName) const;
    QString lexerKeyForDisplayName(const QString& displayName) const;
    QStringList displayNames() const;
    QStringList supportedExtensions() const;
    const TextLexerProfile* profileForDisplayName(const QString& displayName) const;
    bool loadedFromJson() const { return m_loadedFromJson; }
    QString sourceDescription() const { return m_sourceDescription; }

private:
    static TextLexerRegistry load();
    static QVector<TextLexerProfile> fallbackProfiles();
    void buildIndexes();
    void addAvailableLexers();

    QVector<TextLexerProfile> m_profiles;
    QHash<QString, int>       m_profileIndexByName;
    QHash<QString, QString>   m_languageByExtension;
    QHash<QString, QString>   m_languageByFileName;
    bool                      m_loadedFromJson = false;
    QString                   m_sourceDescription;
};

