#pragma once

// Ally cover spacing: the data and the rules (docs/cover_spacing.md).
//
// Stalkers used to share cover knowledge only inside one agent manager (team + squad + group), and only for
// teammates that already held a cover. This file keeps one level-wide list of the stalkers that are alive and of
// the spot each of them has claimed, in a 4 m grid, so "is an ally heading for a spot near this one" is a look
// into a few grid cells and not a walk over every stalker.
//
// This header has no engine includes on purpose: tests/cover_spacing_test.cpp compiles it alone.
// The engine side (who claims what, the console vars, the log lines) is cover_spacing_engine.h/.cpp.

#include <vector>
#include <cmath>

namespace cover_spacing
{
	const float k_cell = 4.f; // grid cell side, metres
	// the vanilla tie rule (agent_location_manager.cpp): a teammate keeps a spot when he is as close to his spot as I
	// am to mine, give or take this much (square metres)
	const float k_tie_sqr = 2.f;
	// soft penalty of a spot with an ally right on it. Cover quality runs 0 (best) to 1, +10 for a spot with no
	// wall towards the enemy. 2 at the spot itself, falling to 0 at the spacing distance: an ally inside half the
	// spacing costs more than the whole quality range, so any free walled spot wins; and it stays far under 10, so
	// a crowded spot behind a wall still beats a free one in the open
	const float k_penalty = 2.f;
	const float k_penalty_cap = 6.f;
	const float k_side_step_back = 1.f; // the fallback side-step stands this far behind the teammate's line
	const unsigned k_body_refresh_ms = 200; // the body grid is at most this stale (a sprint covers ~1.2 m)

	struct vec
	{
		float x, y, z;
	};

	inline vec make_vec(float x, float y, float z)
	{
		vec v = {x, y, z};
		return v;
	}

	inline float dist_sqr(const vec& a, const vec& b)
	{
		const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
		return dx * dx + dy * dy + dz * dz;
	}

	// ---- the rules, as plain arithmetic ----

	// A spot is refused when an ally's claimed cover is within `spacing` of it and that ally is at least as close
	// to his cover as I am to this spot (the vanilla rule, with the vanilla tie margin). spacing 0 = rule off.
	inline bool claim_refuses(float claim_to_spot_sqr, float spacing, float owner_to_claim_sqr, float me_to_spot_sqr)
	{
		if (spacing <= 0.f)
			return false;
		if (claim_to_spot_sqr > spacing * spacing)
			return false;
		return owner_to_claim_sqr <= me_to_spot_sqr + k_tie_sqr;
	}

	// A spot is refused when an ally with no cover stands within `spacing` of it and is nearer to it than I am.
	// "Nearer than I am" keeps a man who already sits on his cover from losing it to somebody walking past.
	inline bool body_refuses(float body_to_spot_sqr, float spacing, float me_to_spot_sqr)
	{
		if (spacing <= 0.f)
			return false;
		if (body_to_spot_sqr > spacing * spacing)
			return false;
		return body_to_spot_sqr < me_to_spot_sqr;
	}

	// soft penalty of one ally (his claim or his body) at this distance from the spot
	inline float crowd_penalty(float to_spot_sqr, float spacing)
	{
		if (spacing <= 0.f)
			return 0.f;
		if (to_spot_sqr >= spacing * spacing)
			return 0.f;
		return k_penalty * (1.f - sqrtf(to_spot_sqr) / spacing);
	}

	// The fallback side-step: `offset` metres to one side of the teammate, across the line from him to the enemy,
	// and k_side_step_back behind that line (away from the enemy). The height is the teammate's.
	// false when the enemy is on top of the teammate (no line to stand beside).
	inline bool side_step_target(const vec& mate, const vec& enemy, float offset, bool left, vec& result)
	{
		float dx = enemy.x - mate.x, dz = enemy.z - mate.z;
		const float length = sqrtf(dx * dx + dz * dz);
		if (length < 0.01f)
			return false;
		dx /= length;
		dz /= length;
		const float side = left ? -offset : offset;
		// (dz, -dx) is the line turned a quarter to the right, seen from above
		result.x = mate.x + dz * side - dx * k_side_step_back;
		result.y = mate.y;
		result.z = mate.z - dx * side - dz * k_side_step_back;
		return true;
	}

	// ---- a grid of points, hashed by cell ----

	class point_grid
	{
		enum { k_buckets = 1024 };

		struct node
		{
			vec p;
			int cx, cz;
			int next;
			bool used;
		};

		std::vector<node> m_nodes; // by slot
		int m_head[k_buckets];
		unsigned m_count;

		static int cell(float v) { return int(floorf(v / k_cell)); }

		static unsigned bucket(int cx, int cz)
		{
			return (unsigned(cx) * 73856093u ^ unsigned(cz) * 19349663u) & (k_buckets - 1);
		}

	public:
		point_grid() { clear(); }

		void clear()
		{
			for (int i = 0; i < k_buckets; ++i)
				m_head[i] = -1;
			for (size_t i = 0; i < m_nodes.size(); ++i)
				m_nodes[i].used = false;
			m_count = 0;
		}

		unsigned count() const { return m_count; }
		bool has(unsigned slot) const { return slot < m_nodes.size() && m_nodes[slot].used; }

		void remove(unsigned slot)
		{
			if (!has(slot))
				return;
			node& n = m_nodes[slot];
			int* link = &m_head[bucket(n.cx, n.cz)];
			while (*link != -1 && *link != int(slot))
				link = &m_nodes[*link].next;
			if (*link == int(slot))
				*link = n.next;
			n.used = false;
			--m_count;
		}

		void insert(unsigned slot, const vec& p)
		{
			remove(slot);
			if (slot >= m_nodes.size())
			{
				node empty = {{0.f, 0.f, 0.f}, 0, 0, -1, false};
				m_nodes.resize(slot + 1, empty);
			}
			node& n = m_nodes[slot];
			n.p = p;
			n.cx = cell(p.x);
			n.cz = cell(p.z);
			const unsigned b = bucket(n.cx, n.cz);
			n.next = m_head[b];
			n.used = true;
			m_head[b] = int(slot);
			++m_count;
		}

		// calls f(slot, distance squared) for every point within `radius` of p (3D distance, cells by x and z)
		template <typename F>
		void query(const vec& p, float radius, F f) const
		{
			if (!m_count || radius <= 0.f)
				return;
			const float radius_sqr = radius * radius;
			const int x0 = cell(p.x - radius), x1 = cell(p.x + radius);
			const int z0 = cell(p.z - radius), z1 = cell(p.z + radius);
			for (int cx = x0; cx <= x1; ++cx)
				for (int cz = z0; cz <= z1; ++cz)
					for (int i = m_head[bucket(cx, cz)]; i != -1; i = m_nodes[i].next)
					{
						const node& n = m_nodes[i];
						if (n.cx != cx || n.cz != cz)
							continue; // another cell that shares the bucket
						const float d = dist_sqr(n.p, p);
						if (d <= radius_sqr)
							f(unsigned(i), d);
					}
		}
	};

	// ---- the level-wide list ----

	enum claim_kind
	{
		claim_none = 0,
		claim_cover, // a cover point he holds or is running to
		claim_side, // the fallback side-step spot (no cover was found)
	};

	enum note_reason
	{
		note_ally_claim = 0,
		note_body,
		note_side_step,
		note_count
	};

	template <typename Owner>
	class registry
	{
	public:
		struct member
		{
			int team, squad, group;
			Owner* owner;
			claim_kind kind;
			vec claim;
			unsigned note_time[note_count];
			unsigned note_skipped[note_count];
			bool used;
		};

	private:
		std::vector<member> m_members; // by slot
		std::vector<int> m_free;
		point_grid m_claims; // claim positions, by slot
		point_grid m_bodies; // positions of members with no claim, by slot
		unsigned m_bodies_time;
		bool m_bodies_dirty;
		unsigned m_count;

		bool valid(int slot) const
		{
			return slot >= 0 && size_t(slot) < m_members.size() && m_members[slot].used;
		}

	public:
		registry() : m_bodies_time(0), m_bodies_dirty(true), m_count(0)
		{
		}

		// forgets everybody. Slots handed out before are dead: remove() and the claim calls ignore them
		void clear()
		{
			m_members.clear();
			m_free.clear();
			m_claims.clear();
			m_bodies.clear();
			m_bodies_dirty = true;
			m_count = 0;
		}

		unsigned count() const { return m_count; }
		unsigned claim_count() const { return m_claims.count(); }

		const member* find(int slot) const
		{
			return valid(slot) ? &m_members[slot] : 0;
		}

		// A member is known by the slot this returns (the engine keeps it in CMemberOrder): a stalker's object id
		// is not set yet when he is registered during his spawn, so the id cannot be the key.
		int add(int team, int squad, int group, Owner* owner)
		{
			int s;
			if (!m_free.empty())
			{
				s = m_free.back();
				m_free.pop_back();
			}
			else
			{
				s = int(m_members.size());
				m_members.push_back(member());
			}
			member& m = m_members[s];
			m.team = team;
			m.squad = squad;
			m.group = group;
			m.owner = owner;
			m.kind = claim_none;
			m.claim = make_vec(0.f, 0.f, 0.f);
			for (int i = 0; i < note_count; ++i)
			{
				m.note_time[i] = 0;
				m.note_skipped[i] = 0;
			}
			m.used = true;
			m_bodies_dirty = true;
			++m_count;
			return s;
		}

		// `owner` guards against a slot from before a clear(): only the member that was added with it is removed
		void remove(int slot, const Owner* owner)
		{
			if (!valid(slot) || m_members[slot].owner != owner)
				return;
			m_claims.remove(unsigned(slot));
			m_bodies.remove(unsigned(slot));
			m_members[slot].used = false;
			m_members[slot].owner = 0;
			m_free.push_back(slot);
			--m_count;
		}

		// true when the claim changed
		bool set_claim(int slot, claim_kind kind, const vec& position)
		{
			if (!valid(slot) || kind == claim_none)
				return false;
			member& m = m_members[slot];
			if (m.kind == kind && m.claim.x == position.x && m.claim.y == position.y && m.claim.z == position.z)
				return false;
			m.kind = kind;
			m.claim = position;
			m_claims.insert(unsigned(slot), position);
			m_bodies.remove(unsigned(slot)); // from now on he counts by where he is heading
			return true;
		}

		// only = claim_none clears any claim; otherwise only a claim of that kind. true when a claim was cleared
		bool clear_claim(int slot, claim_kind only = claim_none)
		{
			if (!valid(slot))
				return false;
			member& m = m_members[slot];
			if (m.kind == claim_none || (only != claim_none && m.kind != only))
				return false;
			m.kind = claim_none;
			m_claims.remove(unsigned(slot));
			m_bodies_dirty = true; // he counts by his body again
			return true;
		}

		// scope 0: the same team, squad and group. scope 1: the same team.
		static bool in_scope(const member& m, int team, int squad, int group, int scope)
		{
			if (m.team != team)
				return false;
			return scope != 0 || (m.squad == squad && m.group == group);
		}

		// f(member, distance squared) for every claim within `radius` of p. f must not change the registry.
		template <typename F>
		void claims_near(const vec& p, float radius, F f) const
		{
			m_claims.query(p, radius, [&](unsigned s, float d) { f(m_members[s], d); });
		}

		// Brings the body grid up to date when it is stale: position_of(owner, vec&) gives where a member stands
		// now. Only ever called from a cover search, so a frame with no search does no work here.
		template <typename P>
		void refresh_bodies(unsigned now_ms, P position_of)
		{
			if (!m_bodies_dirty && (now_ms - m_bodies_time) < k_body_refresh_ms)
				return;
			m_bodies.clear();
			for (size_t s = 0; s < m_members.size(); ++s)
			{
				const member& m = m_members[s];
				if (!m.used || m.kind != claim_none)
					continue;
				vec p;
				position_of(m.owner, p);
				m_bodies.insert(unsigned(s), p);
			}
			m_bodies_time = now_ms;
			m_bodies_dirty = false;
		}

		// f(member, distance squared) for every member with no claim that stood within `radius` of p at the last
		// refresh_bodies
		template <typename F>
		void bodies_near(const vec& p, float radius, F f) const
		{
			m_bodies.query(p, radius, [&](unsigned s, float d) { f(m_members[s], d); });
		}

		// Rate limit for the debug lines: true at most once per `interval_ms` for one member and one reason;
		// `skipped` is how many notes were swallowed since the last line.
		bool note(int slot, note_reason reason, unsigned now_ms, unsigned interval_ms, unsigned& skipped)
		{
			if (!valid(slot))
				return false;
			member& m = m_members[slot];
			if (m.note_time[reason] && (now_ms - m.note_time[reason]) < interval_ms)
			{
				++m.note_skipped[reason];
				return false;
			}
			skipped = m.note_skipped[reason];
			m.note_skipped[reason] = 0;
			m.note_time[reason] = now_ms ? now_ms : 1;
			return true;
		}
	};
}
