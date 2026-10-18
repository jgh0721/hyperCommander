#pragma once
#include "columnContracts.hpp"
#include <QCollator>
#include <numeric>
#include <functional>

namespace hc::columns
{
    struct SortRow { QString id_; bool parent_ = false; bool directory_ = false; QVector<CellValue> values_; };
    // Comparison never invokes a provider. Missing values stay last in either
    // direction; parent/folder grouping and stable identity precede tie breaking.
    inline auto TextCompare(bool Locale = true, bool Numeric = true, Qt::CaseSensitivity Case = Qt::CaseInsensitive)
    {
        QCollator Collator; Collator.setNumericMode(Numeric); Collator.setCaseSensitivity(Case);
        return [=](const QString& A, const QString& B) {
            if (Locale) return Collator.compare(A, B);
            if (!Numeric) return QString::compare(A, B, Case);
            // Locale-independent natural order; arbitrary-length digit runs never overflow.
            int I = 0, J = 0;
            while (I < A.size() && J < B.size()) {
                if (A[I].isDigit() && B[J].isDigit()) {
                    int AE = I, BE = J; while (AE < A.size() && A[AE].isDigit()) ++AE; while (BE < B.size() && B[BE].isDigit()) ++BE;
                    auto AN = A.mid(I, AE - I), BN = B.mid(J, BE - J);
                    while (AN.size() > 1 && AN[0] == '0') AN.remove(0, 1); while (BN.size() > 1 && BN[0] == '0') BN.remove(0, 1);
                    if (AN.size() != BN.size()) return AN.size() < BN.size() ? -1 : 1;
                    if (const auto Order = QString::compare(AN, BN, Qt::CaseSensitive); Order) return Order;
                    I = AE; J = BE;
                } else { const int Order = QString::compare(A.mid(I++, 1), B.mid(J++, 1), Case); if (Order) return Order; }
            }
            return (I < A.size()) - (J < B.size());
        };
    }
    inline auto SortLess(const QVector<Qt::SortOrder>& Orders,
        bool Locale = true, bool Numeric = true, Qt::CaseSensitivity Case = Qt::CaseInsensitive)
    {
        const auto CompareText = TextCompare(Locale, Numeric, Case);
        return [=](const SortRow& Left, const SortRow& Right) {
            if (Left.parent_ != Right.parent_) return Left.parent_;
            if (Left.directory_ != Right.directory_) return Left.directory_;
            for (int Index = 0; Index < Orders.size(); ++Index) {
                const auto LV = Left.values_.value(Index), RV = Right.values_.value(Index);
                const bool LR = LV.State() == ValueState::Ready, RR = RV.State() == ValueState::Ready;
                if (LR != RR) return LR;
                if (!LR) continue;
                int Order = 0;
                if (LV.Type() == FieldType::Text && RV.Type() == FieldType::Text) Order = CompareText(LV.Raw().toString(), RV.Raw().toString());
                else { const auto Compared = QVariant::compare(LV.Raw(), RV.Raw()); Order = Compared < 0 ? -1 : Compared > 0 ? 1 : 0; }
                if (Order) return Orders[Index] == Qt::AscendingOrder ? Order < 0 : Order > 0;
            }
            return QString::compare(Left.id_, Right.id_, Qt::CaseSensitive) < 0;
        };
    }
    inline QVector<int> SortRanks(const QVector<SortRow>& Rows, const QVector<Qt::SortOrder>& Orders,
        bool Locale = true, bool Numeric = true, Qt::CaseSensitivity Case = Qt::CaseInsensitive)
    {
        const auto Less = SortLess(Orders, Locale, Numeric, Case);
        QVector<int> Indices(Rows.size()); std::iota(Indices.begin(), Indices.end(), 0);
        std::stable_sort(Indices.begin(), Indices.end(), [&](int A, int B) { return Less(Rows[A], Rows[B]); });
        QVector<int> Result(Rows.size()); for (int Rank = 0; Rank < Indices.size(); ++Rank) Result[Indices[Rank]] = Rank;
        return Result;
    }
}
