#pragma once
#include "largeTextView.hpp"
namespace hc::compare
{
    class LargeTextSourcePane;
    class LargeTextMultiView final : public LargeTextView
    {
    public:
        LargeTextMultiView(Request Input, std::shared_ptr<LargeTextComparison> Value, QWidget* Parent);
        ~LargeTextMultiView() override;
        bool Busy() const override;
        int ExitCode() const override;
        void Cancel() override;
        bool CanSave() const override { return (!all_ || merging()) && LargeTextView::CanSave(); }
        void SaveActive() override { if (CanSave()) LargeTextView::SaveActive(); }
        void SetTextFont(const QFont& Font) override;
        void SetDark(bool Dark) override;
    protected:
        void find(bool Backwards) override;
    private:
        void changed();
        void setAll(bool All);
        void applySource(LargeTextSourcePane* Pane);
        QComboBox* layoutMode_ = nullptr;
        std::vector<LargeTextSourcePane*> sources_;
        std::shared_ptr<const LargeTextSession> displayed_;
        std::uint64_t resultAnchor_ = 0;
        LargeTextSourcePane* searching_ = nullptr;
        bool all_ = true, synchronizing_ = false;
    };
}
