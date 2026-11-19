// Standalone regression for the script look speed factor (src/xrGame/look_speed.h, docs/look_speed.md); not an
// engine build.
//
// Part 1 runs the header the engine includes: the limits of what a script can set, and the reset.
// Part 2 reads the engine sources and checks the wiring: the factor scales the actor's mouse look once and the look
// keys once, a new CLevel resets it, scripts get the two level.* calls, and it is not a console var (so it cannot be
// saved to user.ltx) and nothing but the setter and the reset writes it.
//
//   g++ -std=c++17 -g -Wall -fsanitize=address,undefined tests/look_speed_test.cpp -o /tmp/look_speed_test
//   /tmp/look_speed_test .
// The argument is the repository root (default ".").
#include "../src/xrGame/look_speed.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

float g_look_speed_factor = 1.f; // the engine's is in ActorInput.cpp

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static std::string read(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) { std::printf("FAIL cannot read %s\n", path.c_str()); ++g_fail; return ""; }
	std::stringstream s;
	s << f.rdbuf();
	return s.str();
}
static int count(const std::string& text, const std::string& what)
{
	int n = 0;
	for (size_t p = text.find(what); p != std::string::npos; p = text.find(what, p + what.size())) ++n;
	return n;
}
// the text of a function: from its signature to the first line that is a lone closing brace
static std::string body(const std::string& text, const std::string& signature)
{
	const size_t a = text.find(signature);
	if (a == std::string::npos) return "";
	size_t b = text.find("\n}", a);
	return text.substr(a, b == std::string::npos ? b : b - a);
}

int main(int argc, char** argv)
{
	const std::string root = argc > 1 ? argv[1] : ".";

	// ---- part 1: the header
	const float inf = std::numeric_limits<float>::infinity();
	CHECK(look_speed::k_min == 0.2f && look_speed::k_max == 1.f);
	CHECK(look_speed::sanitize(1.f) == 1.f);
	CHECK(look_speed::sanitize(0.84f) == 0.84f);
	CHECK(look_speed::sanitize(0.2f) == 0.2f);
	CHECK(look_speed::sanitize(0.1f) == 0.2f);
	CHECK(look_speed::sanitize(0.f) == 0.2f);
	CHECK(look_speed::sanitize(-3.f) == 0.2f);
	CHECK(look_speed::sanitize(-inf) == 0.2f);
	CHECK(look_speed::sanitize(1.5f) == 1.f);
	CHECK(look_speed::sanitize(inf) == 1.f);
	CHECK(look_speed::sanitize(std::nanf("")) == 1.f);

	CHECK(set_look_speed_factor(0.84f) == 0.84f && g_look_speed_factor == 0.84f);
	CHECK(set_look_speed_factor(0.f) == 0.2f && g_look_speed_factor == 0.2f);
	CHECK(set_look_speed_factor(7.f) == 1.f && g_look_speed_factor == 1.f);
	set_look_speed_factor(0.5f);
	reset_look_speed_factor();
	CHECK(g_look_speed_factor == 1.f);

	// ---- part 2: the wiring
	const std::string game = root + "/src/xrGame/";
	const std::string input = read(game + "ActorInput.cpp");
	const std::string level = read(game + "Level.cpp");
	const std::string script = read(game + "level_script.cpp");

	// the mouse: once, on the scale the camera is turned by, after the holder and the hud tuning have returned
	const std::string mouse = body(input, "void CActor::IR_OnMouseMove(int dx, int dy)");
	CHECK(count(mouse, "g_look_speed_factor") == 1);
	CHECK(count(mouse, "scale *= g_look_speed_factor;") == 1);
	CHECK(mouse.find("m_holder->OnMouseMove") < mouse.find("g_look_speed_factor"));
	CHECK(mouse.find("scale *= g_look_speed_factor;") < mouse.find("cam_Active()->Move"));
	// the look keys: once, on the divisor the camera's key turn uses
	const std::string keys = body(input, "void CActor::IR_OnKeyboardHold(int cmd)");
	CHECK(count(keys, "g_look_speed_factor") == 1);
	CHECK(count(keys, "float LookFactor = GetLookFactor() / g_look_speed_factor;") == 1);
	// nowhere else in the actor's input, and it starts at 1
	CHECK(count(input, "g_look_speed_factor") == 3);
	CHECK(count(input, "float g_look_speed_factor = 1.f;") == 1);

	// every new level, and the way out of one
	CHECK(count(body(level, "CLevel::CLevel()"), "reset_look_speed_factor();") == 1);
	CHECK(count(body(level, "CLevel::~CLevel()"), "reset_look_speed_factor();") == 1);

	// scripts
	CHECK(count(script, "def(\"set_look_speed_factor\", script_set_look_speed_factor)") == 1);
	CHECK(count(script, "def(\"get_look_speed_factor\", script_get_look_speed_factor)") == 1);
	CHECK(count(script, "return set_look_speed_factor(factor);") == 1);

	// not a console var, and no other writer: these files hold every console command of the game and the engine,
	// and the other receivers of mouse look (holders, the spectator, the demo camera) do not know the factor
	const char* const others[] = {
		"src/xrGame/console_commands.cpp", "src/xrGame/console_commands_mp.cpp", "src/xrEngine/xr_ioc_cmd.cpp",
		"src/xrGame/CarInput.cpp", "src/xrGame/CarNew.cpp", "src/xrGame/WeaponStatMgunIR.cpp",
		"src/xrGame/HolderEntityObject.cpp", "src/xrGame/Spectator.cpp", "src/xrEngine/FDemoRecord.cpp",
		"src/xrEngine/Xr_input.cpp",
	};
	for (const char* f : others)
	{
		const std::string text = read(root + "/" + f);
		CHECK(!text.empty());
		CHECK(count(text, "look_speed") == 0);
	}
	CHECK(count(level, "g_look_speed_factor") == 0);
	CHECK(count(script, "g_look_speed_factor =") == 0);

	if (g_fail) { std::printf("look_speed_test: %d FAILED\n", g_fail); return 1; }
	std::printf("look_speed_test: ok\n");
	return 0;
}
