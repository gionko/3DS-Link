// Page: the credits of 3DS Link, opened from About. One row for each thing 3DS Link uses or has copied (the name at the left, its license in a word at the right) and, on the
// top screen, the note of the selected row: what was taken from it, who made it, which license, where it lives. The list is about_text.hpp's kCredits, and AzaharLink/CREDITS.md
// says the same (Tests/settings_test/about_test.cpp compares the two). The list scrolls like every list of the app (D-pad up and down, or the stylus).
#include "about_text.hpp"
#include "pages.hpp"

using namespace ui;

namespace {

class CreditsPage : public ListPage {
protected:
    const char* Title() const override {
        return "Credits";
    }

    // The rows only tell: no A button.
    const char* ActionLabel() const override {
        return "";
    }

    void BuildRows(std::vector<Row>& rows) override {
        if (!rows.empty()) {
            return; // (built once: the list does not change while the page is up)
        }
        Row thanks;
        thanks.kind = Kind::Info;
        thanks.label = about::kThanksLabel;
        thanks.note_title = about::kThanksLabel;
        thanks.note = about::kThanksNote;
        rows.push_back(thanks);

        for (int i = 0; i < about::kCreditCount; ++i) {
            const about::Credit& credit = about::kCredits[i];
            Row row;
            row.kind = Kind::Info;
            row.label = credit.name;
            row.value = credit.value;
            row.note_title = credit.name;
            row.note = about::CreditNote(credit);
            rows.push_back(row);
        }
    }

    void Activate(const Row& row, int step) override {
        (void)row;
        (void)step; // the rows only tell
    }
};

} // namespace

namespace pages {

ui::Page* NewCreditsPage() {
    return new CreditsPage;
}

} // namespace pages
