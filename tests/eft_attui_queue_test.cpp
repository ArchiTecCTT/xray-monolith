// Standalone regression for the R1 attachment-UI queue (g_pGamePersistent->AttachmentUIsToRender); not an engine build.
//
// Part 1 models the frame protocol of CRender::Render() (R1) with real heap objects and checks, under AddressSanitizer
// when built with -fsanitize=address, that no frame ever draws the UI of an attachment deleted since it was queued.
// Part 2 reads the engine sources and checks that the fixed protocol is what is written there: the queue is emptied
// after the last pass that can queue (L_Dynamic->render(1)), on the discarded first frame after a reset, and in ~CLevel.
//
//   g++ -std=c++17 -g -fsanitize=address tests/eft_attui_queue_test.cpp -o /tmp/attui_test && /tmp/attui_test .
// The argument is the repository root (default "."); a second argument replaces FStaticRender.cpp (mutation runs).
#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct Att
{
    int* kin = new int(7); // stands for m_kinematics
    int drawn = 0;
    ~Att() { delete kin; }
    void RenderUI() { drawn += *kin; } // reads freed memory if the attachment (or its kinematics) was deleted
};

static std::vector<Att*> queue_; // AttachmentUIsToRender

// One CRender::Render() frame. fixed = clear at the end of Render (the engine change).
static void frame(const std::vector<Att*>& visible, const std::vector<Att*>& lit_only_pass2, bool fixed)
{
    for (Att* a : visible) queue_.push_back(a);          // Calculate(): capture
    for (Att* a : visible) queue_.push_back(a);          // L_Dynamic->render(0): per-light capture
    for (Att* a : queue_) a->RenderUI();                  // Render_R1_Attachment_UI()
    queue_.clear();
    for (Att* a : lit_only_pass2) queue_.push_back(a);   // L_Dynamic->render(1): per-light capture, after the clear
    if (fixed) queue_.clear();                           // fixed: end of Render()
}

static bool contains_in_order(const std::string& s, const std::string& a, const std::string& b)
{
    size_t pa = s.find(a);
    size_t pb = s.rfind(b);
    return pa != std::string::npos && pb != std::string::npos && pa < pb;
}

static std::string slurp(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    assert(f && "cannot open source file");
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv)
{
    std::string root = argc > 1 ? argv[1] : ".";
    std::string render_path = argc > 2 ? argv[2] : root + "/src/Layers/xrRenderPC_R1/FStaticRender.cpp";

    // Part 1: fixed protocol, level destroyed between frames (all attachments deleted), then the first frame of the new level.
    {
        Att* a = new Att;
        Att* b = new Att;
        frame({a}, {b}, true);
        delete a;
        delete b; // level unload / remove_attachment
        Att* c = new Att;
        frame({c}, {}, true); // would touch a and b through the stale queue if the protocol left them behind
        assert(c->drawn == 14);
        queue_.clear();
        delete c;
        std::puts("model, fixed protocol: ok");
    }
    // The old protocol leaves b queued after frame 1 (only checked structurally: running it would be a use-after-free).
    {
        Att* a = new Att;
        Att* b = new Att;
        frame({a}, {b}, false);
        assert(queue_.size() == 1 && queue_[0] == b);
        queue_.clear();
        delete a;
        delete b;
        std::puts("model, old protocol: leaves the pass-2 capture queued (the bug)");
    }

    // Part 2: the sources.
    std::string r = slurp(render_path);
    size_t fn = r.find("void CRender::Render()");
    assert(fn != std::string::npos);
    std::string body = r.substr(fn, r.find("\nvoid CRender::ApplyBlur4", fn) - fn);
    const std::string clear = "AttachmentUIsToRender.clear_not_free();";
    size_t early = body.find("m_bFirstFrameAfterReset)");
    size_t early_end = body.find("return;", early);
    bool ok = true;
    if (body.substr(early, early_end - early).find(clear) == std::string::npos)
    {
        std::puts("FAIL: the discarded first frame after a reset does not empty the queue");
        ok = false;
    }
    if (!contains_in_order(body, "L_Dynamic->render(1)", clear))
    {
        std::puts("FAIL: the queue is not emptied after L_Dynamic->render(1)");
        ok = false;
    }
    std::string lvl = slurp(root + "/src/xrGame/Level.cpp");
    size_t d = lvl.find("CLevel::~CLevel()");
    size_t dm = lvl.find("delete_data(m_script_attachments);", d);
    size_t dc = lvl.find(clear, d);
    if (dc == std::string::npos || dm == std::string::npos || dc > dm)
    {
        std::puts("FAIL: ~CLevel does not empty the queue before it deletes attachments");
        ok = false;
    }
    if (!ok) return 1;
    std::puts("sources: ok");
    return 0;
}
