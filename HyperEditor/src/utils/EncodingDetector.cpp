#include "utils/EncodingDetector.h"

#include <QStringConverter>

// ── BOM 감지 ──
QString EncodingDetector::detectBOM(const QByteArray& data)
{
    if (data.size() >= 3 &&
        static_cast<uint8_t>(data[0]) == 0xEF &&
        static_cast<uint8_t>(data[1]) == 0xBB &&
        static_cast<uint8_t>(data[2]) == 0xBF)
        return QStringLiteral("UTF-8");

    if (data.size() >= 2) {
        uint8_t b0 = data[0], b1 = data[1];
        if (b0 == 0xFF && b1 == 0xFE) return QStringLiteral("UTF-16LE");
        if (b0 == 0xFE && b1 == 0xFF) return QStringLiteral("UTF-16BE");
    }

    if (data.size() >= 4) {
        uint8_t b0 = data[0], b1 = data[1], b2 = data[2], b3 = data[3];
        if (b0 == 0xFF && b1 == 0xFE && b2 == 0x00 && b3 == 0x00)
            return QStringLiteral("UTF-32LE");
        if (b0 == 0x00 && b1 == 0x00 && b2 == 0xFE && b3 == 0xFF)
            return QStringLiteral("UTF-32BE");
    }

    return {};
}

// ── UTF-8 유효성 검사 ──
bool EncodingDetector::isValidUtf8(const QByteArray& data)
{
    int remaining = 0;
    for (uint8_t byte : data) {
        if (remaining > 0) {
            if ((byte & 0xC0) != 0x80) return false;
            --remaining;
        } else if ((byte & 0x80) == 0) {
            continue; // ASCII
        } else if ((byte & 0xE0) == 0xC0) {
            remaining = 1;
        } else if ((byte & 0xF0) == 0xE0) {
            remaining = 2;
        } else if ((byte & 0xF8) == 0xF0) {
            remaining = 3;
        } else {
            return false;
        }
    }
    return remaining == 0;
}

// ── 휴리스틱 감지 ──
QString EncodingDetector::detectHeuristic(const QByteArray& data)
{
    if (isValidUtf8(data)) {
        // 비-ASCII 바이트가 있으면 UTF-8, 없으면 ASCII (UTF-8 호환)
        for (uint8_t b : data) {
            if (b & 0x80) return QStringLiteral("UTF-8");
        }
        return QStringLiteral("UTF-8");
    }

    // CP949 (EUC-KR) 감지 시도 — 한국어 범위
    bool couldBeCP949 = true;
    for (int i = 0; i < data.size(); ++i) {
        uint8_t b = data[i];
        if (b >= 0x80) {
            if (i + 1 >= data.size()) { couldBeCP949 = false; break; }
            uint8_t b2 = data[i + 1];
            // EUC-KR 범위: 첫 바이트 0x81~0xFE, 둘째 바이트 0x41~0xFE
            if (b < 0x81 || b2 < 0x41) { couldBeCP949 = false; break; }
            ++i;
        }
    }
    if (couldBeCP949)
        return QStringLiteral("EUC-KR");

    // 폴백
    return QStringLiteral("System");
}

// ── 공개 API ──
QString EncodingDetector::detect(const QByteArray& rawData)
{
    auto bom = detectBOM(rawData);
    return bom.isEmpty() ? detectHeuristic(rawData) : bom;
}

QString EncodingDetector::detectFromSample(const QByteArray& sampleData)
{
    return detect(sampleData);
}

QString EncodingDetector::decode(const QByteArray& rawData, const QString& encoding)
{
    auto enc = QStringConverter::encodingForName(encoding.toUtf8().constData());
    if (enc) {
        QStringDecoder decoder(*enc);
        return decoder(rawData);
    }
    // 폴백: UTF-8
    return QString::fromUtf8(rawData);
}

QByteArray EncodingDetector::encode(const QString& text, const QString& encoding)
{
    return encode(text, encoding, BomPolicy::Never);
}

QByteArray EncodingDetector::encode(const QString& text, const QString& encoding, BomPolicy bomPolicy)
{
    auto enc = QStringConverter::encodingForName(encoding.toUtf8().constData());
    QByteArray encoded;
    if (enc) {
        QStringEncoder encoder(*enc);
        encoded = encoder(text);
    } else {
        encoded = text.toUtf8();
    }

    const QByteArray bom = bomBytesForEncoding(encoding);
    if (!bom.isEmpty()) {
        if (bomPolicy == BomPolicy::Always) {
            if (!encoded.startsWith(bom))
                encoded.prepend(bom);
        } else if (encoded.startsWith(bom)) {
            encoded.remove(0, bom.size());
        }
    }

    return encoded;
}

QStringList EncodingDetector::availableEncodings()
{
    return {
        "UTF-8", "UTF-16LE", "UTF-16BE",
        "EUC-KR", "System",
        "ISO-8859-1", "Shift_JIS", "GB18030"
    };
}

bool EncodingDetector::hasBom(const QByteArray& rawData, const QString& encoding)
{
    if (rawData.isEmpty())
        return false;

    const QString normalized = encoding.trimmed().toUpper();
    const QString bomEncoding = detectBOM(rawData).toUpper();

    if (normalized.isEmpty())
        return !bomEncoding.isEmpty();

    if (normalized == QStringLiteral("UTF-8"))
        return bomEncoding == QStringLiteral("UTF-8");
    if (normalized == QStringLiteral("UTF-16") || normalized == QStringLiteral("UTF-16LE"))
        return bomEncoding == QStringLiteral("UTF-16LE");
    if (normalized == QStringLiteral("UTF-16BE"))
        return bomEncoding == QStringLiteral("UTF-16BE");

    return false;
}

bool EncodingDetector::supportsBom(const QString& encoding)
{
    return !bomBytesForEncoding(encoding).isEmpty();
}

QByteArray EncodingDetector::bomBytesForEncoding(const QString& encoding)
{
    const QString normalized = encoding.trimmed().toUpper();
    if (normalized == QStringLiteral("UTF-8"))
        return QByteArray::fromHex("EFBBBF");
    if (normalized == QStringLiteral("UTF-16") || normalized == QStringLiteral("UTF-16LE"))
        return QByteArray::fromHex("FFFE");
    if (normalized == QStringLiteral("UTF-16BE"))
        return QByteArray::fromHex("FEFF");
    return {};
}

