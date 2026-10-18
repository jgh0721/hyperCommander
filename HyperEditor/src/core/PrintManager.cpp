#include "core/PrintManager.h"
#include "core/QBaseView.h"

#include <QApplication>
#include <QPrintDialog>
#include <QPrintPreviewDialog>
#include <QPrinterInfo>

PrintManager::PrintManager(QObject* parent)
    : QObject(parent)
{}

void PrintManager::printView(QBaseView* view, QPrinter* printer)
{
    if (!view || !printer) return;
    if (!view->isPrinterAllowed(printer->printerName())) return;
    view->print(printer);
}

bool PrintManager::showPrintDialog(QBaseView* view, QWidget* parent)
{
    if (!view)
        return false;

    const int totalPages = qMax(1, view->printablePageCount());
    QPrinter printer(QPrinter::HighResolution);
    QPrintDialog dlg(&printer, parent);
    dlg.setOption(QAbstractPrintDialog::PrintToFile, true);
    dlg.setOption(QAbstractPrintDialog::PrintPageRange, true);
    dlg.setMinMax(1, totalPages);
    dlg.setFromTo(1, totalPages);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    if (!view->isPrinterAllowed(printer.printerName()))
        return false;

    view->print(&printer);
    return true;
}

void PrintManager::showPreview(QBaseView* view, QWidget* parent)
{
    if (!view) return;

    QApplication::setOverrideCursor(Qt::WaitCursor);

    QPrinter printer(QPrinter::HighResolution);
    QPrintPreviewDialog dlg(&printer, parent);

    connect(&dlg, &QPrintPreviewDialog::paintRequested, view,
            [view](QPrinter* p) { view->print(p); });

    QApplication::restoreOverrideCursor();
    dlg.exec();
}

void PrintManager::setBlacklist(const QStringList& patterns) { m_blacklist = patterns; }
QStringList PrintManager::blacklist() const                    { return m_blacklist; }

QStringList PrintManager::allowedPrinters() const
{
    QStringList result;
    for (const auto& info : QPrinterInfo::availablePrinters()) {
        bool blocked = false;
        for (const auto& pat : m_blacklist) {
            if (info.printerName().contains(pat, Qt::CaseInsensitive)) {
                blocked = true;
                break;
            }
        }
        if (!blocked)
            result.append(info.printerName());
    }
    return result;
}

