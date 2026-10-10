// Standalone regression for the EFT parts alt aim (CWeapon::SetAltAimScript, API bit 512); not an engine build.
//
// Part 1 models the zoom-type state of CWeapon that the setter touches (SwitchZoomType with the separate launcher key,
// ToggleGrenadeLauncher / PerformSwitchGL with aimmode_remember, UpdateUIScope -> UpdateZoomParams, the HUD index and the
// offset that UpdateHudAdditional takes for it) with the setter's real rules, and checks that the rifle never sits on the
// unset index 3 of a HUD section without an alt aim of its own.
// Part 2 reads the engine sources and checks that the change is what is written there (it fails on 617f96b's sources).
//
//   g++ -std=c++17 -g -fsanitize=address tests/eft_altaim_test.cpp -o /tmp/altaim_test && /tmp/altaim_test .
// The argument is the repository root (default "."); a second argument is a directory holding Weapon.cpp, Weapon.h and
// WeaponAK74.cpp to check instead of the ones under src/xrGame (e.g. the old sources from git show).
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// ---------------------------------------------------------------------------------------------------------------- Part 1
struct V3 { float x = 0, y = 0, z = 0; bool operator==(const V3& o) const { return x == o.x && y == o.y && z == o.z; } };
static const float PI_ = 3.1415926535897932384626433832795f;
static bool valid(float f) { return std::isfinite(f); }

struct Wpn
{
    // section
    bool m_altAimPos = false;            // use_alt_aim_hud
    float section_alt_zoom = 0.f;        // scope_zoom_factor_alt
    float section_main_zoom = 0.f;       // scope_zoom_factor
    V3 hud_alt_pos, hud_alt_rot;         // m_hands_offset[0/1][3] (zero when the HUD section has none)
    V3 hud_aim_pos{0.f, 0.f, -0.2f}, hud_aim_rot; // m_hands_offset[0/1][1]
    // state
    unsigned char m_zoomtype = 0;
    unsigned char zoomTypeBeforeLauncher = 0;
    bool isGrenadeLauncherActive = false;
    bool zoomed = false;
    float scope_zoom = 0.f;              // m_fScopeZoomFactor
    float current_zoom = 0.f;            // GetZoomFactor() while zoomed
    // EFT
    bool eft = false;
    V3 eft_pos, eft_rot;
    float eft_zoom = 0.f;

    bool SectionHasAltAim() const { return m_altAimPos; }
    bool GetAltAim() const { return eft; }
    bool HasAltAim() const { return m_altAimPos || eft; }

    void UpdateZoomParams()
    {
        if (m_zoomtype == 2) scope_zoom = 0.f;
        else if (m_zoomtype == 1)
        {
            scope_zoom = section_alt_zoom;
            if (eft && eft_zoom > 0.f) scope_zoom = eft_zoom;
        }
        else scope_zoom = section_main_zoom;
        if (zoomed) current_zoom = scope_zoom;
    }
    void UpdateUIScope() { UpdateZoomParams(); }
    void SetZoomTypeAndParams(unsigned char t) { m_zoomtype = t; }
    void SwitchZoomType() // useSeparateUBGLKeybind (the default)
    {
        if (m_zoomtype == 0 && HasAltAim()) SetZoomTypeAndParams(1);
        else if (m_zoomtype != 0) SetZoomTypeAndParams(0);
        UpdateUIScope();
    }
    void ToggleGrenadeLauncher() // + the eSwitch that ends in PerformSwitchGL (aimmode_remember = 1)
    {
        if (!isGrenadeLauncherActive) zoomTypeBeforeLauncher = m_zoomtype;
        isGrenadeLauncherActive = !isGrenadeLauncherActive;
        m_zoomtype = isGrenadeLauncherActive ? 2 : zoomTypeBeforeLauncher;
        UpdateUIScope();
    }
    void ZoomIn() { zoomed = true; current_zoom = scope_zoom; }
    void ZoomOut() { zoomed = false; }
    int HudIdx() const { return !zoomed ? 0 : m_zoomtype == 2 ? 2 : m_zoomtype == 1 ? 3 : 1; }
    // the target UpdateHudAdditional interpolates to (non-modular, not in adjust mode)
    void HudTarget(V3& pos, V3& rot) const
    {
        int i = HudIdx();
        if (i == 3 && eft) { pos = eft_pos; rot = eft_rot; }
        else if (i == 3) { pos = hud_alt_pos; rot = hud_alt_rot; }
        else if (i == 1) { pos = hud_aim_pos; rot = hud_aim_rot; }
        else { pos = V3{}; rot = V3{}; }
    }
    bool OnUnsetIndex3() const { return HudIdx() == 3 && !eft && !SectionHasAltAim(); }

    // the real rules of CWeapon::SetAltAimScript
    bool SetAltAim(bool on, V3 pos, V3 rot, float zoom)
    {
        if (on)
        {
            if (!valid(pos.x) || !valid(pos.y) || !valid(pos.z) || !valid(rot.x) || !valid(rot.y) || !valid(rot.z) ||
                !valid(zoom) || zoom < 0.f) return false;
            if (std::fabs(pos.x) > 1.f || std::fabs(pos.y) > 1.f || std::fabs(pos.z) > 1.f) return false;
            if (std::fabs(rot.x) > PI_ || std::fabs(rot.y) > PI_ || std::fabs(rot.z) > PI_) return false;
            bool zoom_changed = !eft || zoom != eft_zoom;
            eft = true; eft_pos = pos; eft_rot = rot; eft_zoom = zoom;
            if (m_zoomtype == 1 && zoom_changed) UpdateUIScope();
            return true;
        }
        if (!eft) return true;
        eft = false;
        if (!SectionHasAltAim())
        {
            if (zoomTypeBeforeLauncher == 1) zoomTypeBeforeLauncher = 0;
            if (m_zoomtype == 1) SetZoomTypeAndParams(0);
        }
        if (m_zoomtype != 2) UpdateUIScope();
        return true;
    }
};

static Wpn rifle() { Wpn w; w.section_main_zoom = 0.f; w.section_alt_zoom = 0.f; return w; }
static const V3 P{-0.05f, -0.12f, -0.2133f}, R{0.0155f, 0.0181f, 0.7837f}, Z{};

static void part1()
{
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();

    { // no part: V does nothing
        Wpn w = rifle();
        w.SwitchZoomType();
        CHECK(w.m_zoomtype == 0);
    }
    { // set while in zoom 0, then V, aim, the HUD takes the script's numbers and zoom
        Wpn w = rifle();
        CHECK(w.SetAltAim(true, P, R, 60.f));
        CHECK(w.m_zoomtype == 0 && w.GetAltAim());
        w.SwitchZoomType(); w.ZoomIn();
        CHECK(w.m_zoomtype == 1 && w.HudIdx() == 3);
        V3 p, r; w.HudTarget(p, r);
        CHECK(p == P && r == R);
        CHECK(w.current_zoom == 60.f);
        // V again: back to the main aim
        w.SwitchZoomType();
        CHECK(w.m_zoomtype == 0 && w.HudIdx() == 1);
    }
    { // zoom 0 in the setter = the section's alt zoom
        Wpn w = rifle(); w.section_alt_zoom = 45.f;
        w.SetAltAim(true, P, R, 0.f); w.SwitchZoomType(); w.ZoomIn();
        CHECK(w.current_zoom == 45.f);
    }
    for (int aimed = 0; aimed < 2; ++aimed) { // clear while in zoom 1 (aimed and not aimed) -> zoom 0, never on index 3
        Wpn w = rifle(); w.section_main_zoom = 30.f;
        w.SetAltAim(true, P, R, 60.f); w.SwitchZoomType();
        if (aimed) w.ZoomIn();
        CHECK(w.m_zoomtype == 1);
        CHECK(w.SetAltAim(false, Z, Z, 0.f));
        CHECK(!w.GetAltAim() && w.m_zoomtype == 0);
        CHECK(!w.OnUnsetIndex3());
        if (aimed) CHECK(w.HudIdx() == 1 && w.current_zoom == 30.f);
        w.ZoomIn();
        CHECK(w.HudIdx() == 1 && !w.OnUnsetIndex3());
        w.SwitchZoomType(); // V without the part: nothing
        CHECK(w.m_zoomtype == 0);
    }
    { // clear while in zoom 0 changes no zoom type
        Wpn w = rifle();
        w.SetAltAim(true, P, R, 60.f);
        CHECK(w.SetAltAim(false, Z, Z, 0.f));
        CHECK(w.m_zoomtype == 0 && !w.GetAltAim());
        CHECK(w.SetAltAim(false, Z, Z, 0.f)); // clear again: true, nothing
    }
    { // launcher round trip: V aim, launcher, part removed in the launcher, back -> zoom 0, not index 3
        Wpn w = rifle();
        w.SetAltAim(true, P, R, 60.f); w.SwitchZoomType(); w.ZoomIn();
        w.ToggleGrenadeLauncher();
        CHECK(w.m_zoomtype == 2 && w.zoomTypeBeforeLauncher == 1);
        w.SetAltAim(false, Z, Z, 0.f);
        CHECK(w.m_zoomtype == 2 && w.zoomTypeBeforeLauncher == 0);
        w.ToggleGrenadeLauncher();
        CHECK(w.m_zoomtype == 0 && !w.OnUnsetIndex3());
        // without the fix (no reset of zoomTypeBeforeLauncher) the rifle would be on the zero vector of index 3
        Wpn u = rifle();
        u.SetAltAim(true, P, R, 60.f); u.SwitchZoomType(); u.ZoomIn(); u.ToggleGrenadeLauncher();
        u.eft = false; u.ToggleGrenadeLauncher();
        CHECK(u.OnUnsetIndex3());
    }
    { // launcher round trip with the part kept: back in the V aim with the script's numbers
        Wpn w = rifle();
        w.SetAltAim(true, P, R, 60.f); w.SwitchZoomType(); w.ZoomIn();
        w.ToggleGrenadeLauncher(); w.ToggleGrenadeLauncher();
        V3 p, r; w.HudTarget(p, r);
        CHECK(w.m_zoomtype == 1 && p == P && r == R && w.current_zoom == 60.f);
    }
    { // refusals change nothing
        Wpn w = rifle();
        w.SetAltAim(true, P, R, 60.f); w.SwitchZoomType(); w.ZoomIn();
        const V3 bad_pos[] = {{nan, 0, 0}, {0, inf, 0}, {0, 0, 1.5f}, {-1.5f, 0, 0}};
        const V3 bad_rot[] = {{nan, 0, 0}, {0, 0, -inf}, {0, 0, 4.f}, {0, -4.f, 0}};
        for (const V3& b : bad_pos) CHECK(!w.SetAltAim(true, b, R, 50.f));
        for (const V3& b : bad_rot) CHECK(!w.SetAltAim(true, P, b, 50.f));
        CHECK(!w.SetAltAim(true, P, R, nan));
        CHECK(!w.SetAltAim(true, P, R, inf));
        CHECK(!w.SetAltAim(true, P, R, -1.f));
        V3 p, r; w.HudTarget(p, r);
        CHECK(w.GetAltAim() && w.m_zoomtype == 1 && p == P && r == R && w.eft_zoom == 60.f && w.current_zoom == 60.f);
        // a refusal while off leaves it off
        Wpn o = rifle();
        CHECK(!o.SetAltAim(true, V3{0, 0, 1.5f}, R, 0.f));
        CHECK(!o.GetAltAim());
        // limits themselves are accepted
        CHECK(o.SetAltAim(true, V3{1.f, -1.f, 1.f}, V3{PI_, -PI_, 0.f}, 0.f));
    }
    { // re-set while in zoom 1 (aimed): new numbers on the next frame, new zoom at once
        Wpn w = rifle();
        w.SetAltAim(true, P, R, 60.f); w.SwitchZoomType(); w.ZoomIn();
        const V3 P2{-0.04f, -0.11f, -0.246f}, R2{0.f, 0.f, -0.78f};
        CHECK(w.SetAltAim(true, P2, R2, 50.f));
        V3 p, r; w.HudTarget(p, r);
        CHECK(p == P2 && r == R2 && w.current_zoom == 50.f && w.m_zoomtype == 1);
    }
    { // a section with its own alt keeps zoom 1 after a clear, and gets its own zoom and offset back
        Wpn w = rifle(); w.m_altAimPos = true; w.section_alt_zoom = 45.f; w.hud_alt_pos = V3{0.1f, 0.f, -0.3f};
        w.SwitchZoomType(); w.ZoomIn();
        CHECK(w.current_zoom == 45.f);
        w.SetAltAim(true, P, R, 60.f);
        CHECK(w.current_zoom == 60.f); // switched on while in zoom 1: the zoom changes at once
        w.ToggleGrenadeLauncher();
        w.SetAltAim(false, Z, Z, 0.f);
        CHECK(w.zoomTypeBeforeLauncher == 1);
        w.ToggleGrenadeLauncher();
        CHECK(w.m_zoomtype == 1 && w.current_zoom == 45.f);
        V3 p, r; w.HudTarget(p, r);
        CHECK(p == w.hud_alt_pos);
        w.SetAltAim(true, P, R, 60.f);
        w.SetAltAim(false, Z, Z, 0.f);
        CHECK(w.m_zoomtype == 1 && w.current_zoom == 45.f);
    }
}

// ---------------------------------------------------------------------------------------------------------------- Part 2
static std::string slurp(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) { std::printf("FAIL cannot open %s\n", p.c_str()); ++g_fail; return ""; }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static size_t count(const std::string& s, const std::string& k)
{
    size_t n = 0;
    for (size_t p = s.find(k); p != std::string::npos; p = s.find(k, p + 1)) ++n;
    return n;
}
// the text from the line holding `head` to the next line that starts with '}' (a function body at column 0)
static std::string body(const std::string& s, const std::string& head)
{
    size_t a = s.find(head);
    if (a == std::string::npos) return "";
    size_t b = s.find("\n}", a);
    return s.substr(a, b == std::string::npos ? std::string::npos : b - a);
}
static bool in_order(const std::string& s, const char* a, const char* b)
{
    size_t pa = s.find(a), pb = pa == std::string::npos ? pa : s.find(b, pa);
    return pa != std::string::npos && pb != std::string::npos;
}

static void part2(const std::string& dir)
{
    const std::string cpp = slurp(dir + "/Weapon.cpp"), h = slurp(dir + "/Weapon.h"), ak = slurp(dir + "/WeaponAK74.cpp");

    // Weapon.h: fields, HasAltAim, the setter and getter
    CHECK(h.find("bool m_eft_alt_aim = false;") != std::string::npos);
    CHECK(h.find("Fvector m_eft_alt_pos") != std::string::npos && h.find("Fvector m_eft_alt_rot") != std::string::npos);
    CHECK(h.find("float m_eft_alt_zoom = 0.f;") != std::string::npos);
    CHECK(h.find("bool HasAltAim() const { return m_altAimPos || m_eft_alt_aim; }") != std::string::npos);
    CHECK(h.find("bool SetAltAimScript(bool on, Fvector pos, Fvector rot, float zoom);") != std::string::npos);
    CHECK(h.find("bool GetAltAimScript() const { return m_eft_alt_aim; }") != std::string::npos);

    // V gate: both SwitchZoomType paths ask HasAltAim(); m_altAimPos is left only at init, Load and SectionHasAltAim
    const std::string sw = body(cpp, "void CWeapon::SwitchZoomType()");
    CHECK(count(sw, "m_zoomtype == 0 && (HasAltAim() || g_player_hud->m_adjust_mode") == 2);
    CHECK(count(sw, "m_altAimPos") == 0);
    CHECK(count(cpp, "m_altAimPos") == 3);
    CHECK(body(cpp, "bool CWeapon::SectionHasAltAim() const").find("return m_altAimPos ||") != std::string::npos);

    // the setter: refusals, the clear rules
    const std::string st = body(cpp, "bool CWeapon::SetAltAimScript(bool on, Fvector pos, Fvector rot, float zoom)");
    CHECK(st.find("!_valid(pos) || !_valid(rot) || !_valid(zoom) || zoom < 0.f) return false;") != std::string::npos);
    CHECK(st.find("_abs(pos.x) > 1.f || _abs(pos.y) > 1.f || _abs(pos.z) > 1.f) return false;") != std::string::npos);
    CHECK(st.find("_abs(rot.x) > PI || _abs(rot.y) > PI || _abs(rot.z) > PI) return false;") != std::string::npos);
    CHECK(in_order(st, "m_eft_alt_zoom = zoom;", "if (m_zoomtype == 1 && zoom_changed)"));
    CHECK(in_order(st, "if (!SectionHasAltAim())", "if (zoomTypeBeforeLauncher == 1)"));
    CHECK(in_order(st, "zoomTypeBeforeLauncher = 0;", "if (m_zoomtype == 1)\n\t\t\tSetZoomTypeAndParams(0);"));
    CHECK(in_order(st, "SetZoomTypeAndParams(0);", "UpdateUIScope();"));

    // the index 3 override in UpdateHudAdditional: after the modular and adjust-mode branches, before the interpolation
    const std::string hud = body(cpp, "void CWeapon::UpdateHudAdditional(Fmatrix& trans)");
    CHECK(in_order(hud, "if ((idx == 1 || idx == 3) && m_modular_attachments)", "else if (g_player_hud->m_adjust_mode)"));
    CHECK(in_order(hud, "else if (g_player_hud->m_adjust_mode)", "else if (idx == 3 && m_eft_alt_aim) {"));
    CHECK(in_order(hud, "else if (idx == 3 && m_eft_alt_aim) {", "curr_offs = m_eft_alt_pos;"));
    CHECK(in_order(hud, "curr_offs = m_eft_alt_pos;", "curr_rot = m_eft_alt_rot;"));
    CHECK(in_order(hud, "curr_rot = m_eft_alt_rot;", "curr_offs = hi->m_measures.m_hands_offset[0][idx];"));
    CHECK(in_order(hud, "curr_offs = hi->m_measures.m_hands_offset[0][idx];", "InterpolateOffset(m_hud_offset[0], curr_offs, factor);"));

    // the zoom override in zoom type 1, after the section's value; no section overlay or scope radius for it
    const std::string zp = body(cpp, "void CWeapon::UpdateZoomParams()");
    CHECK(in_order(zp, "else if (m_zoomtype == 1) //Alt", "\"scope_zoom_factor_alt\""));
    CHECK(in_order(zp, "\"scope_zoom_factor_alt\"", "if (m_eft_alt_aim && m_eft_alt_zoom > 0.f && !g_player_hud->m_adjust_mode)"));
    CHECK(in_order(zp, "m_eft_alt_zoom > 0.f", "m_zoom_params.m_fScopeZoomFactor = m_eft_alt_zoom;"));
    CHECK(in_order(zp, "m_zoom_params.m_fScopeZoomFactor = m_eft_alt_zoom;", "else //Main Sight"));
    const std::string ui = body(cpp, "void CWeapon::UpdateUIScope()");
    CHECK(in_order(ui, "else if (m_zoomtype == 1)", "if (!m_eft_alt_aim)\n\t\t\tscope_tex_name = m_secondary_scope_tex_name;"));
    CHECK(body(cpp, "float CWeapon::SDS_Radius(bool alt)").find("if (alt && m_eft_alt_aim)\n\t\treturn 0.0;") != std::string::npos);

    // WeaponAK74.cpp: bit 512 and the two exports on CWeapon, next to SetHandPose
    // (later bits may follow 512: 1024 = SetAimSettleK)
    CHECK(ak.find("static int eft_weapon_api() { return 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512") != std::string::npos);
    CHECK(in_order(ak, "class_<CWeapon,", ".def(\"SetHandPose\", &CWeapon::SetHandPoseScript)"));
    CHECK(in_order(ak, ".def(\"SetHandPose\", &CWeapon::SetHandPoseScript)", ".def(\"SetAltAim\", &CWeapon::SetAltAimScript)"));
    CHECK(in_order(ak, ".def(\"SetAltAim\", &CWeapon::SetAltAimScript)", ".def(\"GetAltAim\", &CWeapon::GetAltAimScript)"));
}

int main(int argc, char** argv)
{
    std::string root = argc > 1 ? argv[1] : ".";
    std::string dir = argc > 2 ? argv[2] : root + "/src/xrGame";
    part1();
    int f1 = g_fail;
    std::printf("part 1 (model): %s\n", f1 ? "FAIL" : "PASS");
    part2(dir);
    std::printf("part 2 (sources in %s): %s\n", dir.c_str(), g_fail > f1 ? "FAIL" : "PASS");
    std::printf("%s\n", g_fail ? "FAILED" : "ALL PASS");
    return g_fail ? 1 : 0;
}
