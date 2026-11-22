// Standalone regression for the per-light draw graph of R1 (light::GMLight, CLightR_Manager); not an engine build.
//
// Part 1 models what CLightR_Manager::render_point / render_spot do to one light's graph over several frames, with
// real heap objects, and checks (under AddressSanitizer when built with -fsanitize=address) that a light never draws
// a packet whose texture list was freed after the packet was captured.
// Part 2 reads the engine sources and checks that the fixed protocol is what is written there.
//
//   g++ -std=c++17 -g -fsanitize=address tests/r1_light_graph_test.cpp -o /tmp/light_test && /tmp/light_test .
// The argument is the repository root (default "."); a second argument replaces LightPPA.cpp (mutation runs).
#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct TextureList // STextureList: owned by a shader pass, freed with the shader
{
    int* slots = new int(3);
    ~TextureList() { delete slots; }
};

struct Packet // RenderPacket: raw pointers, no reference held
{
    TextureList* textures;
};

struct LightGraph // CDSGraphManager of one light
{
    bool mask[2] = {true, false}; // light.h: deferred, not forward
    std::vector<Packet> queue[2]; // mapStaticPasses / mapDynamicPasses by priority
    int drawn = 0;

    void capture(const std::vector<TextureList*>& scene)
    {
        // every shader a light draws with has priority 0 here; the mask drops the others
        for (TextureList* t : scene)
            if (mask[0])
                queue[0].push_back({t});
    }

    void render(unsigned priority)
    {
        for (Packet& p : queue[priority])
            drawn += *p.textures->slots; // CBackend::set_Textures: reads freed memory if the list is gone
        queue[priority].clear();
    }
};

// CLightR_Manager::render(priority) for one lit light. fixed = the engine change.
static void light_pass(LightGraph& g, unsigned priority, const std::vector<TextureList*>& scene, bool fixed)
{
    if (fixed)
    {
        if (!g.mask[priority])
            return;
        g.queue[0].clear();
        g.queue[1].clear();
    }
    g.capture(scene);
    g.render(priority);
}

static void frame(LightGraph& g, const std::vector<TextureList*>& scene, bool fixed)
{
    light_pass(g, 0, scene, fixed); // L_Dynamic->render(0)
    light_pass(g, 1, scene, fixed); // L_Dynamic->render(1)
}

static std::string slurp(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    assert(f && "cannot open source file");
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static size_t count(const std::string& s, const std::string& what)
{
    size_t n = 0;
    for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + what.size()))
        ++n;
    return n;
}

int main(int argc, char** argv)
{
    std::string root = argc > 1 ? argv[1] : ".";
    std::string light_path = argc > 2 ? argv[2] : root + "/src/Layers/xrRenderPC_R1/LightPPA.cpp";

    // Part 1: fixed protocol. The light is lit for two frames, goes dark, a shader is swapped (its texture list is
    // freed), and the light is lit again.
    {
        LightGraph g;
        TextureList* wall = new TextureList;
        TextureList* body = new TextureList;
        frame(g, {wall, body}, true);
        frame(g, {wall, body}, true);
        assert(g.drawn == 12); // each of the two, once a frame
        assert(g.queue[0].empty() && g.queue[1].empty());
        delete body; // set_shader on the model, or the model is gone
        frame(g, {wall}, true); // would read "body" through a packet left behind
        assert(g.drawn == 15);
        delete wall;
        std::puts("model, fixed protocol: ok");
    }
    // The old protocol leaves the second capture of every frame queued, and draws it with the next frame's (only
    // checked structurally: letting it draw a freed list would be a use-after-free).
    {
        LightGraph g;
        TextureList* wall = new TextureList;
        frame(g, {wall}, false);
        assert(g.queue[0].size() == 1 && g.queue[0][0].textures == wall);
        frame(g, {wall}, false);
        assert(g.drawn == 3 + 6); // the second frame drew the wall twice
        delete wall;
        std::puts("model, old protocol: leaves a capture queued in the light (the bug)");
    }

    // Part 2: the sources.
    bool ok = true;
    const std::string src = slurp(light_path);
    const char* fns[2] = {"void CLightR_Manager::render_point(u32 _priority)", "void CLightR_Manager::render_spot(u32 _priority)"};
    for (const char* fn : fns)
    {
        size_t a = src.find(fn);
        assert(a != std::string::npos);
        size_t b = src.find("\nvoid CLightR_Manager::", a + 1);
        const std::string body = src.substr(a, b == std::string::npos ? b : b - a);

        const size_t skip = body.find("if (!light_graph_takes(L, _priority)) continue;");
        const size_t clear = body.find("L->GMLight.RGraph.clear<false>();");
        const size_t traverse = body.find("L->GMLight.traverse(");
        const size_t capture = body.find("L->GMLight.r_dsgraph_capture(");
        const size_t draw = body.find("L->GMLight.r_dsgraph_render_graph(_priority);");
        if (traverse == std::string::npos || capture == std::string::npos || draw == std::string::npos)
        {
            std::printf("FAIL: %s: capture or draw not found\n", fn);
            ok = false;
            continue;
        }
        if (skip == std::string::npos || skip > traverse)
        {
            std::printf("FAIL: %s captures for a priority its graph does not take\n", fn);
            ok = false;
        }
        if (clear == std::string::npos || clear > traverse)
        {
            std::printf("FAIL: %s does not empty the light's graph before the capture\n", fn);
            ok = false;
        }
        if (count(body, "L->GMLight.r_dsgraph_capture(") != 1)
        {
            std::printf("FAIL: %s captures more than once\n", fn);
            ok = false;
        }
    }
    if (src.find("return L->GMLight.i_mask[_priority];") == std::string::npos)
    {
        std::puts("FAIL: light_graph_takes does not ask the graph's own mask");
        ok = false;
    }
    // the mask the model above assumes
    const std::string light_h = slurp(root + "/src/Layers/xrRender/light.h");
    if (light_h.find("{ true,false,false,false,true,false,false });") == std::string::npos)
    {
        std::puts("FAIL: light.h: GMLight's mask changed, check that render_point / render_spot still match it");
        ok = false;
    }
    const std::string workflow = slurp(root + "/.github/workflows/eft-parts.yml");
    if (workflow.find("r1_light_graph_test") == std::string::npos)
    {
        std::puts("FAIL: the workflow does not run this test");
        ok = false;
    }
    if (!ok) return 1;
    std::puts("sources: ok");
    return 0;
}
