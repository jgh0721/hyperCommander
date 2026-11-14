#include "stdafx.h"
#include "sqliteFormat.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFile>
#include <QFileInfo>

namespace hc::sqlite
{
    namespace
    {
        constexpr char Magic[16] = {'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f', 'o', 'r', 'm', 'a', 't', ' ', '3', '\0'};

        int Byte(const QByteArray& Data, int At) { return At < Data.size() ? int(quint8(Data[At])) : -1; }

        // 16–17: 페이지 크기(빅 엔디언). 1 은 65536. 그 밖은 512–32768 의 2의 거듭제곱.
        int PageSize(const QByteArray& Data)
        {
            const int High = Byte(Data, 16), Low = Byte(Data, 17);
            if (High < 0 || Low < 0) return 0;
            const int Value = (High << 8) | Low;
            if (Value == 1) return 65536;
            if (Value < 512 || Value > 32768 || (Value & (Value - 1)) != 0) return 0;
            return Value;
        }
    }

    namespace { std::atomic_bool Sniffing{true}; }
    void SetHeaderSniffing(bool Enabled) { Sniffing.store(Enabled); }
    bool HeaderSniffing() { return Sniffing.load(); }

    bool HasPlainHeader(const QByteArray& Header)
    {
        return Header.size() >= 16 && std::memcmp(Header.constData(), Magic, 16) == 0;
    }

    HeaderInfo InspectHeader(const QByteArray& Header, qint64 FileSize)
    {
        HeaderInfo Info;
        Info.fileSize_ = FileSize;
        if (FileSize == 0 || (FileSize < 0 && Header.isEmpty())) { Info.kind_ = FileKind::Empty; return Info; }
        Info.pageSize_ = PageSize(Header);
        Info.reservedBytes_ = qMax(0, Byte(Header, 20));
        Info.wal_ = Byte(Header, 18) == 2 && Byte(Header, 19) == 2;
        if (HasPlainHeader(Header)) { Info.kind_ = FileKind::Plain; return Info; }
        // SEE 는 0–15 바이트를 암호화하고 16–23 바이트를 평문으로 둔다. 쓰기 · 읽기 형식(1 · 2)과
        // 페이로드 비율(64 · 32 · 32)은 SQLite 가 고정한 값이라 우연히 맞기 어렵다.
        const bool Versions = (Byte(Header, 18) == 1 || Byte(Header, 18) == 2) && (Byte(Header, 19) == 1 || Byte(Header, 19) == 2);
        const bool Fractions = Byte(Header, 21) == 64 && Byte(Header, 22) == 32 && Byte(Header, 23) == 32;
        const bool SizeFits = FileSize < 0 || (Info.pageSize_ > 0 && FileSize % Info.pageSize_ == 0);
        Info.kind_ = Info.pageSize_ > 0 && Versions && Fractions && SizeFits ? FileKind::SeeEncrypted : FileKind::NotSqlite;
        return Info;
    }

    HeaderInfo InspectFile(const QString& Path)
    {
        QFile File(Path);
        if (!File.open(QIODevice::ReadOnly)) {
            HeaderInfo Info;
            Info.kind_ = FileKind::Unreadable;
            Info.error_ = File.errorString();
            return Info;
        }
        return InspectHeader(File.read(100), File.size());
    }
}
