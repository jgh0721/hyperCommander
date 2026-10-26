#pragma once

#include <QObject>
#include <QPrinter>
#include <QStringList>

class QBaseView;

/// 인쇄 관리자 — 프린터 필터링 및 인쇄 미리보기
class PrintManager : public QObject
{
    Q_OBJECT

public:
    explicit PrintManager(QObject* parent = nullptr);

    void printView(QBaseView* view, QPrinter* printer);
    bool showPrintDialog(QBaseView* view, QWidget* parent = nullptr);
    void showPreview(QBaseView* view, QWidget* parent = nullptr);

    // 프린터 블랙리스트
    void            setBlacklist(const QStringList& patterns);
    QStringList     blacklist() const;
    QStringList     allowedPrinters() const;

private:
    QStringList m_blacklist;
};

