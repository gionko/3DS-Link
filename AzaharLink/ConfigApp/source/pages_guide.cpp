// Page: the guide "How to get boot9.bin" (ConfigApp/guide: GodMode9's steps, a picture of its screen and a few words under each). The viewer is a screen of its own
// (guide.hpp): this page forwards the keys and the two draws to it and does nothing else until it says it is closed. L / R and the D-pad's up and down move
// between the steps (the steps are a vertical list), B closes.
//
// It is a page pushed over the one that offered it, so the key that opened it is gone by the time it is first updated (a page is updated once before it is drawn,
// and in a LATER frame than the one that pushed it): the Y of a footer or the D-pad's down that moved a list to the row that opens the guide cannot be taken for a
// step. The page under it is not updated while the guide is up, and is shown again, as it was, when B closes it.
#include "../guide/guide.hpp"
#include "pages.hpp"

using namespace ui;

namespace {

class GuidePage : public Page {
public:
    bool Open() {
        return viewer_.Open();
    }

    void Update(const Input& in) override {
        // The keys of the frame and the place the stylus went down this frame (the footer's three buttons do what their keys do).
        if (!viewer_.Update(in.down, in.touch_pressed ? in.tx : -1, in.touch_pressed ? in.ty : -1)) {
            viewer_.Close();
            PopPage();
        }
    }

    void DrawTop() override {
        viewer_.DrawTop();
    }

    void DrawBottom() override {
        viewer_.DrawBottom();
    }

private:
    guide::Viewer viewer_;
};

} // namespace

namespace pages {

bool OpenBoot9Guide() {
    GuidePage* page = new GuidePage;
    if (!page->Open()) {
        delete page; // (the pictures are not in this build: the caller says so)
        return false;
    }
    PushPage(page);
    return true;
}

} // namespace pages
