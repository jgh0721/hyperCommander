#pragma once
#include <QList>
#include <cmath>

namespace hc::document {
// A line change made after the preview was built: the 0-based line of the edit and the lines it
// added (negative when removed). Recorded in edit order.
struct LineShift { int line_ = 0, delta_ = 0; };

// Editor line -> line of the previewed revision, undoing the shifts newest first. Lines are 1-based
// and keep their fraction; lines inside an insertion map to the line where it was made.
inline double ToRevisionLine(double Line, const QList<LineShift>& Shifts) {
    double Value = Line - 1;
    for (auto At = Shifts.crbegin(); At != Shifts.crend(); ++At) {
        const double Whole = std::floor(Value);
        if (At->delta_ > 0) { if (Whole > At->line_ + At->delta_) Value -= At->delta_; else if (Whole > At->line_) Value = At->line_; }
        else if (Whole > At->line_) Value -= At->delta_;
    }
    return Value + 1;
}

// Previewed revision line -> editor line, applying the shifts oldest first. Removed lines map to the
// line that absorbed them.
inline double FromRevisionLine(double Line, const QList<LineShift>& Shifts) {
    double Value = Line - 1;
    for (const auto& Shift : Shifts) {
        const double Whole = std::floor(Value);
        if (Shift.delta_ > 0) { if (Whole > Shift.line_) Value += Shift.delta_; }
        else if (Whole > Shift.line_ - Shift.delta_) Value += Shift.delta_;
        else if (Whole > Shift.line_) Value = Shift.line_;
    }
    return Value + 1;
}
}
