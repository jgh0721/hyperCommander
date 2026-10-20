#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

// Dedicated child-process fixture. Tests pass only temporary paths, and the
// executable must receive the streaming invocation, never offset/limit calls.
inline int EverythingStreamFixture(int Argc, char** Argv)
{
    QCoreApplication App(Argc, Argv);
    QFile Source(qEnvironmentVariable("HC_EVERYTHING_TEST_INPUT"));
    if (!Source.open(QIODevice::ReadOnly)) return 2;
    const auto Paths = QJsonDocument::fromJson(Source.readAll()).array();
    QFile Output, Input;
    if (!Output.open(stdout, QIODevice::WriteOnly) || !Input.open(stdin, QIODevice::ReadOnly)) return 3;
    int Sequence = 0;
    const auto Send = [&](QJsonObject Message) {
        Message["v"] = 1; Message["seq"] = Sequence++;
        const auto Bytes = QJsonDocument(Message).toJson(QJsonDocument::Compact) + '\n';
        // Exercise a UTF-8/JSON token split across reads.
        Output.write(Bytes.left(Bytes.size() / 2)); Output.flush();
        Output.write(Bytes.mid(Bytes.size() / 2)); Output.flush();
    };
    Send({{"type", "started"}});
    Send({{"type", "snapshot"}, {"total", QString::number(Paths.size())}});
    if (qEnvironmentVariable("HC_EVERYTHING_TEST_MODE") == "bad-sequence") ++Sequence;
    for (int Row = 0; Row < Paths.size();) {
        QJsonArray Frame;
        const int Count = Row == 0 ? 1 : 64;
        for (int I = 0; I < Count && Row < Paths.size(); ++I, ++Row) Frame.append(Paths[Row]);
        const auto Sent = Sequence; Send({{"type", "items"}, {"paths", Frame}});
        const auto Ack = QJsonDocument::fromJson(Input.readLine(1024)).object();
        if (Ack["type"] != "ack" || Ack["seq"].toInt(-1) != Sent) return 4;
        if (qEnvironmentVariable("HC_EVERYTHING_TEST_MODE") == "slow" && Row == 1) QThread::msleep(1200);
    }
    if (qEnvironmentVariable("HC_EVERYTHING_TEST_MODE") == "missing-end") return 0;
    Send({{"type", "end"}, {"emitted", QString::number(Paths.size())}, {"complete", true}});
    return 0;
}
