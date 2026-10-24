#include "stdafx.h"
#include "readingSettings.hpp"

/// Platform/framework headers
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <cmath>

namespace hc::epub::poc
{
    namespace
    {
        constexpr qint64 MaxStoreBytes = 2 * 1024 * 1024;
        void validateProfile(const QJsonObject& Profile)
        {
            const QHash<QString, QPair<double, double>> Limits{
                {"size", {12, 40}}, {"weight", {400, 700}}, {"letter", {-0.05, 0.2}}, {"word", {0, 0.8}},
                {"line", {1, 3}}, {"before", {0, 3}}, {"after", {0, 3}}, {"indent", {0, 3}},
                {"margin", {8, 80}}, {"width", {400, 1200}}};
            for(auto It = Profile.begin(); It != Profile.end(); ++It)
            {
                const auto Key = It.key(); const auto Value = It.value();
                bool Valid = false;
                if(Limits.contains(Key))
                    Valid = Value.isDouble() && std::isfinite(Value.toDouble())
                        && Value.toDouble() >= Limits[Key].first && Value.toDouble() <= Limits[Key].second;
                else if(Key == "override") Valid = Value.isBool();
                else if(Key == "theme") Valid = Value.isString() && QStringList{"light", "dark", "sepia", "green"}.contains(Value.toString());
                else if(Key == "align") Valid = Value.isString() && QStringList{"left", "center", "right", "justify"}.contains(Value.toString());
                else if(Key == "font")
                {
                    Valid = Value.isString() && !Value.toString().trimmed().isEmpty() && Value.toString().size() <= 128;
                    for(const auto Character : Value.toString()) if(Character.category() == QChar::Other_Control) Valid = false;
                }
                if(!Valid) throw QStringLiteral("설정 값 또는 버전이 올바르지 않습니다: ") + Key;
            }
        }
        bool validBook(const QString& Book)
        { return QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(Book).hasMatch(); }
        QJsonObject readRoot(const QString& Path)
        {
            if(!QFileInfo::exists(Path)) return {{"schemaVersion", 1}, {"global", QJsonObject{}}, {"books", QJsonObject{}}};
            QFile File(Path);
            if(!File.open(QIODevice::ReadOnly) || File.size() > MaxStoreBytes) throw QStringLiteral("설정 파일을 읽을 수 없거나 크기 제한을 초과했습니다.");
            const auto Bytes = File.read(MaxStoreBytes + 1);
            QJsonParseError Error;
            const auto Doc = QJsonDocument::fromJson(Bytes, &Error);
            if(Bytes.size() > MaxStoreBytes || Error.error != QJsonParseError::NoError || !Doc.isObject())
                throw QStringLiteral("설정 파일이 손상되었습니다. 기존 파일을 유지합니다.");
            const auto Root = Doc.object();
            if(Root["schemaVersion"] != QJsonValue(1) || !Root["global"].isObject() || !Root["books"].isObject())
                throw QStringLiteral("지원하지 않는 설정 파일입니다. 기존 파일을 유지합니다.");
            validateProfile(Root["global"].toObject());
            const auto Books = Root["books"].toObject();
            if(Books.size() > 4096) throw QStringLiteral("책별 설정 수 제한을 초과했습니다.");
            for(auto It = Books.begin(); It != Books.end(); ++It)
            {
                if(!validBook(It.key()) || !It.value().isObject()) throw QStringLiteral("책별 설정 식별자가 올바르지 않습니다.");
                validateProfile(It.value().toObject());
            }
            return Root;
        }
        ReadingSnapshot snapshot(const QJsonObject& Root, const QString& Book)
        { return {Root["global"].toObject(), Root["books"].toObject()[Book].toObject(), {}}; }
    }

    QJsonObject ReadingStore::Defaults()
    {
        return {{"theme", "light"}, {"font", "Malgun Gothic"}, {"size", 19}, {"weight", 400},
            {"letter", 0}, {"word", 0}, {"line", 1.8}, {"before", 0}, {"after", 0.8}, {"indent", 0},
            {"align", "left"}, {"margin", 28}, {"width", 760}, {"override", true}};
    }
    QJsonObject ReadingStore::Resolve(const QJsonObject& Global, const QJsonObject& Book)
    {
        auto Result = Defaults();
        for(const auto& Layer : {Global, Book})
            for(auto It = Layer.begin(); It != Layer.end(); ++It) Result[It.key()] = It.value();
        return Result;
    }
    ReadingSnapshot ReadingStore::Load(const QString& Path, const QString& Book)
    {
        try
        {
            if(!validBook(Book)) throw QStringLiteral("책의 SHA-256 식별자가 올바르지 않습니다.");
            return snapshot(readRoot(Path), Book);
        }
        catch(const QString& Error) { return {{}, {}, Error}; }
    }
    ReadingSnapshot ReadingStore::Save(const QString& Path, const QString& Book, bool Global,
        const QJsonObject& Base, const QJsonObject& Draft)
    {
        try
        {
            if(!validBook(Book)) throw QStringLiteral("책의 SHA-256 식별자가 올바르지 않습니다.");
            validateProfile(Base); validateProfile(Draft);
            if(!QDir().mkpath(QFileInfo(Path).absolutePath())) throw QStringLiteral("설정 폴더를 만들 수 없습니다.");
            QLockFile Lock(Path + ".lock");
            if(!Lock.tryLock(0)) throw QStringLiteral("다른 창에서 저장 중입니다. 잠시 후 다시 적용하세요.");
            auto Root = readRoot(Path);
            auto Books = Root["books"].toObject();
            auto Current = Global ? Root["global"].toObject() : Books[Book].toObject();
            QSet<QString> Keys;
            for(auto It = Base.begin(); It != Base.end(); ++It) Keys.insert(It.key());
            for(auto It = Draft.begin(); It != Draft.end(); ++It) Keys.insert(It.key());
            for(const auto& Key : Keys)
            {
                if(Base[Key] == Draft[Key]) continue;
                if(Current.value(Key) != Base[Key] && Current.value(Key) != Draft[Key])
                    throw QStringLiteral("다른 창에서 같은 항목을 변경했습니다. 취소 후 다시 불러오세요: ") + Key;
                if(Draft.contains(Key)) Current[Key] = Draft[Key]; else Current.remove(Key);
            }
            if(Global) Root["global"] = Current;
            else
            {
                if(Current.isEmpty()) Books.remove(Book); else Books[Book] = Current;
                if(Books.size() > 4096) throw QStringLiteral("책별 설정 수 제한을 초과했습니다.");
                Root["books"] = Books;
            }
            const auto Bytes = QJsonDocument(Root).toJson(QJsonDocument::Indented);
            if(Bytes.size() > MaxStoreBytes) throw QStringLiteral("설정 파일 크기 제한을 초과했습니다.");
            QSaveFile File(Path); File.setDirectWriteFallback(false);
            if(!File.open(QIODevice::WriteOnly) || File.write(Bytes) != Bytes.size() || !File.commit())
                throw QStringLiteral("설정을 저장할 수 없습니다. 마지막 적용 값을 유지합니다.");
            return snapshot(Root, Book);
        }
        catch(const QString& Error) { return {{}, {}, Error}; }
    }
}
