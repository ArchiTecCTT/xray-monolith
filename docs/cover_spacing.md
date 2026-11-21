# Ally cover spacing

NPCs of one side no longer pile onto the same cover. Each stalker's cover is known to every ally on the
level, a spot too close to an ally's is refused or costs extra, and a stalker that finds no cover at all
steps to the side of a teammate and no longer queues behind him.

Everything is switched by console vars. All of them are saved in `user.ltx`, and `0` gives the behaviour
of the older exe for that rule.

## The vars

| var | default | range | what it does |
|---|---|---|---|
| `ai_cover_ally_spacing` | 4.0 | 0 .. 10 m | A cover this close to a cover an ally holds, or is running to, is refused. The ally keeps his when he is at least as close to it as we are to ours; when we are clearly closer we take ours and he looks for another. |
| `ai_cover_body_spacing` | 2.5 | 0 .. 10 m | A cover this close to a teammate who fights with no cover yet is refused, when the spot is nearer to him than to us. Also how far a coverless ally "pushes" in the soft cost below. |
| `ai_cover_fallback_side_offset` | 3.5 | 0 .. 10 m | No cover found: stand this far to the left or right of the nearest teammate that has an enemy. `0` = the old way, 1.2 m behind the first such teammate. |
| `ai_cover_spacing_scope` | 1 | 0 or 1 | Whose spots count. `1` = every ally of the same team (faction) on the level. `0` = the stalker's own engine group only. |
| `ai_cover_spacing_debug` | 0 | 0 or 1 | `1` = `[COVER]` lines in the log (below). |
| `ai_sudden_attack_break_on_fire` | 1 | 0 or 1 | Not about cover: a stalker sneaking up on an enemy that has not seen him stops sneaking when he is hit or a bullet lands within 5 m of him. `0` = old. |

They are on the options page Modded Exes > Gameplay > Stalkers, with the engine game data of this build.

Test shortcuts (`appdata\`, next to `user.ltx`; type the line in the console):

| console | does |
|---|---|
| `cfg_load cover_on` | the defaults above |
| `cfg_load cover_off` | the three spacing vars at 0: the old behaviour, for comparing |
| `cfg_load cover_debug` | the defaults, and the log lines on |

## What the engine does

**Claims.** Every living stalker has one record in a level-wide list. When his cover is set (any search:
combat, danger, a smart cover) the cover's position becomes his claim; it is dropped when his cover is
cleared, when he leaves combat, when he dies, when he goes offline. The claims sit in a 4 m grid, so
"is an ally's spot near this one" looks into a few grid cells and does not walk over the stalkers.

**Refusing a spot** (`CAgentLocationManager::suitable`, asked for every cover a search looks at):

1. The vanilla rule stays: inside the stalker's own group a cover within 5 m of a teammate's cover is
   refused when that teammate is as close to his cover as we are to this one (+2 m² margin).
2. New: the same question for every ally's claim in the scope, at `ai_cover_ally_spacing`, with the same
   "who is closer" rule. Taking a cover takes away the cover of an ally whose claim is too close to it
   (he lost the "who is closer" comparison), as vanilla does inside the group.
3. New: a teammate that is in combat and has no cover was skipped. Now a spot within
   `ai_cover_body_spacing` of him, and nearer to him than to us, is his. "Nearer to him than to us"
   means a man sitting on his cover does not lose it to somebody walking past.

**A soft cost** (`CCoverEvaluatorBest::evaluate_cover`). The earlier fork code added +1000 to a cover
with a teammate's body within 1.5 m. The evaluator starts from a best value of 1000, so that cover was
refused outright, and in a crowd the search often came back with nothing. Now a spot costs up to 2 for
each ally heading to a cover near it (falling to 0 at `ai_cover_ally_spacing`), and for each ally with
no cover standing near it (0 at `ai_cover_body_spacing`), 6 at most. Cover quality runs from 0 (best) to
1, plus 10 for a spot with no wall towards the enemy. So an ally inside half the spacing makes a spot
lose to any free walled spot, and a crowded spot behind a wall still beats a free spot in the open.
An ally that has a cover counts by the cover, not by where he stands: a man running to his cover does
not spoil the spots along his way. An ally with no cover only counts against spots he is nearer to than
we are, so a teammate walking past does not make the cover we sit on worse.

**No cover found** (`CStalkerActionTakeCover::execute`). The stalker goes to a spot
`ai_cover_fallback_side_offset` to the side of a teammate (of his own group, within 30 m) that has an
enemy: across that teammate's line to the enemy, 1 m back, left or right by the stalker's place in the
group's list. The spot has to be on the AI map, walkable in a straight line from the teammate, allowed
by the stalker's restrictions, and not within `ai_cover_body_spacing` of where another ally stands or
is heading. If the first side fails the other is tried, then the same beside the next nearest teammate,
four teammates at most; if all fail he stays where he is, as before. A man standing beside somebody
has an enemy too, so the next man can stand beside him: with little cover a squad forms a line across
the enemy's direction. The spot is his claim while he uses it. It is checked every 2 s and kept for as
long as no other ally has come to stand on it; when none was found the search runs again after half a
second. A real cover is still looked for on every update, and replaces the spot when one turns up.

## The log lines

With `ai_cover_spacing_debug 1`, at most one line per stalker, reason and second:

```
[COVER] id=1234 reason=ally_claim what=refuse other=1250 dist=2.8 value=0.00 more=17 name=sim_default_duty_2 time=512345
```

| field | meaning |
|---|---|
| `id`, `name` | the stalker that was looking for cover |
| `reason` | `ally_claim` = an ally's cover was too close. `body` = an ally with no cover (or his side-step spot) was too close. `side_step` = the no-cover fallback ran |
| `what` | `refuse` = the spot was refused. `penalty` = it was made more expensive, `value` says by how much. For `side_step`: `left` / `right` = the side taken, `none` = no side was free and he stayed |
| `other` | the ally's id (for `side_step`: the teammate he stands beside) |
| `dist` | metres between the spot and the ally's cover or body (for `side_step`: to the teammate) |
| `more` | how many more of this kind happened for this stalker since his last line and were not printed |

Counting a session: `ally_claim` and `body` lines (plus their `more`) are spots turned down for
spacing; `side_step what=left|right` are side-steps taken; `side_step what=none` are stalkers left with
no cover and no free side.

## Cost

- A frame with no cover search does no new work. A stalker that is already taking cover re-checks his
  held cover every brain update, as he did before; the cost part of that re-check walked over all his
  teammates and is now two looks into the grid.
- A search looks into the grid three times per cover point it considers (one for the refusal, two for
  the cost). In the test's 200 stalkers on a 100 x 100 m field a cover point has less than one ally's
  spot or body near it on average, so each look is a handful of compares, whatever the head count.
- The positions of the stalkers that have no claim are collected at most once per 200 ms, and only when
  a search asks.
- The vanilla in-group loop in `suitable()` is untouched and still walks the group's members for every
  cover point.

## Limits, as built

- "Same team" is the engine's team id. In Anomaly that is the faction (the community's team number), and
  the simulation gives every stalker squad 0 and group 0, so normally one faction on a level is already
  one engine group: scope 0 and scope 1 then see the same allies. Scope 1 matters for stalkers that
  were given their own squad or group ids.
- Cover spacing is by faction, not by relation: two friendly factions do not space from each other.
- A script can keep a stalker on the cover he holds (`npc_on_best_cover_repick`, `flags.allow = false`).
  The engine asks before it replaces a held cover, so such a veto also overrules spacing: a stalker
  that already sits too close to an ally stays there for as long as the script says no. First picks
  (no cover held yet) are never vetoed and always spaced.
- Monsters and the player are not in the list. NPCs do not keep away from the player's cover.
- Smart covers are claimed like any cover, but the smart cover evaluation itself is unchanged.
- A group of more than 64 stalkers overflows the engine's 64-bit member mask (old engine limit, only
  reached by spawning a crowd of one faction). "Is in combat" answers can then be wrong for the members
  past the 64th, which makes rule 3 treat some of them by the vanilla 5 m rule. Nothing breaks.

## Files

| file | what |
|---|---|
| `src/xrGame/cover_spacing.h` | the list, the grid and the rules; no engine includes |
| `src/xrGame/cover_spacing_engine.h/.cpp` | who claims what, the vars, the log lines |
| `src/xrGame/member_order.h`, `member_order_inline.h` | a stalker's record joins and leaves the list; the cover setter updates the claim |
| `src/xrGame/agent_location_manager.cpp` | `suitable()`, `make_suitable()` |
| `src/xrGame/cover_evaluators.cpp` | the soft cost |
| `src/xrGame/stalker_combat_actions.cpp/.h` | the side-step; the sudden attack switch |
| `src/xrGame/stalker_combat_planner.cpp` | leaving combat frees the claim |
| `src/xrGame/sound_memory_manager.*` | the time a bullet last landed near a stalker |
| `tests/cover_spacing_test.cpp` | runs the rules, a squad on a row of covers, 200 stalkers, and reads the wiring from the sources |
