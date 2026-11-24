//
// DESCRIPTION:
//	Karp priority system: a stack of goals. The top one is worked on.
//
//	Under -autoplay every level starts with one goal, GO TO EXIT. A goal
//	is a condition (out of the level, holding a blue key, sector 45 moved)
//	and the places it can be met: its candidates (every exit line, every
//	blue key lying in the level, every line that moves sector 45). Each
//	time a goal is planned, all its candidates are scored by route and the
//	player goes for the best one it can reach. If it can reach none, the
//	candidate whose route is blocked by the most fixable crossings wins,
//	and the fix for the first of them goes on the stack, in front: GET KEY
//	for a locked door, USE TRIGGER for a sector a switch or walk-over line
//	moves, KILL EVERY <kind> for one a boss's death moves (E1M8's barons).
//	When that is done it comes off, and the goal below is planned
//	again from wherever the player is: the world has changed.
//
//	A goal is done when its condition holds, however that came about. A
//	candidate that fails is not chosen again for a while; a goal whose
//	candidates all fail fails, is not pushed again for a while, and the
//	goal below looks for another way. Nothing is given up for good: GO TO
//	EXIT never fails, and with no way it waits and plans again. A click on
//	the console's map pushes a GOTO on top; when it ends, the rest resumes.
//
//	-karpexit has GO TO EXIT go for one kind of exit, secret or normal,
//	until it stays out of reach (EXIT_IDLE_MOST). An explorer
//	(-karpattitude) also goes to the places off its way, as to items: the
//	areas not visited yet, the secrets not found yet (PLACE_AREA_REACH).
//

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "doomdef.h"
#include "doomstat.h"
#include "m_misc.h"
#include "r_state.h"

#include "karp.h"
#include "k_area.h"
#include "k_brain.h"
#include "k_combat.h"
#include "k_condition.h"
#include "k_items.h"
#include "k_jump.h"
#include "k_log.h"
#include "k_nav.h"
#include "k_salt.h"
#include "k_sight.h"
#include "k_specials.h"

#define MAX_GOALS 8
#define MAX_FAILED 32
#define MAX_CANDIDATES 64
#define MAX_TRIED 16

// What a blocked crossing adds to a route: one the brain can get past (a
// key lies in the level, some line moves the sector) less than one it
// cannot, so routes go through the fixable ones.
#define FIXABLE_COST 1.0e6
#define UNFIXABLE_COST 1.0e8

// A sector moved by a line: the time it gets to start moving after a
// walk-over line is crossed, and to stop again (perpetual lifts never
// do). After MAX_TRIGGERS times in a level, a line is not worked again:
// e.g. the switch of a door that closes before the player gets there.
// Per line, not per sector: Doom II MAP21 raises a staircase 16 units a
// press, by four switches in a row.
#define START_TICS (2 * TICRATE)
#define MOVE_TICS (15 * TICRATE)
#define MAX_TRIGGERS 3

// Waiting for it to stop, standing on a floor that hurts, no suit on, the
// player goes to the nearest dry spot (StayDry): out to DRY_REACH, in rings
// DRY_RING apart, DRY_WAYS ways round, the one with the shortest way there
// (the user, E4M2: switch 166 pressed from the dry floor before it, a step
// out of an imp's fireball took the bot into the nukage of the ring beside
// it, and it stood there 7 s as the octagon rose, from 111 health and
// armour to 48 - "it STAYED on the damaging floor, doing something"). Looked
// for again DRY_LOOK after none was found.
#define DRY_REACH 256.0
#define DRY_RING 32.0
#define DRY_WAYS 16
#define DRY_LOOK TICRATE

// A lift or a door that goes back by itself (FixPasses), its line just
// worked (the user, E3M9: lift 147 lowered three times, each time a
// cacodemon fought or held short of, or a box of rockets gone for, while it
// went back up; the third use used the switch up, and every goal behind it
// failed for 2 minutes): until it goes back, PASS_TICS at most, no item is
// gone for but health at CRITICAL; and a window a fight or a hold took
// counts neither as a use of the line (MAX_TRIGGERS) nor, for PASS_TICS
// after, as the way shut on the goal going through (MAX_SHUT): the player
// had no fair try, and works the line again. Fights go on as ever: run as
// for an item (escape), passing broke off what keeps the player from a
// Cyberdemon, and E3M9 died 3 runs of 3 by lift 27, under its rockets.
#define PASS_TICS (10 * TICRATE)

// A goal whose way shut while the player was on it (a door closing before
// it got through) is planned again, which pushes the fix, up to this many
// times before the candidate counts as not working out.
#define MAX_SHUT 3

// No way to any candidate while sectors are still moving (stairs being
// built, a floor going down): plan again after RETRY_TICS, for up to
// MAX_WAIT_TICS in all before the goal fails.
#define RETRY_TICS TICRATE
#define MAX_WAIT_TICS (15 * TICRATE)

// Nothing is given up for good (follow-up 11, the user): something out of
// the player's hands may still open a way - a monster crossing a walk-over
// line, a slow floor, a door bounced off a monster - or a second try go
// better than the first. A goal that failed may go on the stack again after
// FAIL_TICS, twice that after failing again, and so on up to MAX_FAIL_TICS;
// a candidate that did not work out is tried again after TRIED_TICS; a
// line's uses (MAX_TRIGGERS) count for LINE_USES_TICS after the last one.
// GO TO EXIT never fails: with no way, it is planned again every IDLE_TICS
// (Doom II MAP02: stuck by a barrel for 8 s, the blue key was given up for
// good, then the exit, and the bot stood idle). While it waits so, with
// nothing else to do, a goal that failed is tried again after IDLE_TICS,
// twice that after failing again, and so on up to 8 times (the user, Doom
// II MAP14: every goal failed at once from the spot of a shotgun picked up,
// a moving floor in the way, and the bot stood idle for 30 s).
#define FAIL_TICS (30 * TICRATE)
#define MAX_FAIL_TICS (240 * TICRATE)
#define TRIED_TICS (30 * TICRATE)
#define LINE_USES_TICS (30 * TICRATE)
#define IDLE_TICS (5 * TICRATE)

// The exit wanted (-karpexit, k_brain.h): GO TO EXIT goes for exits of that
// kind only - a floor that ends the level is a normal one - and no route
// crosses a walk-over exit of the other kind (K_Path_SetKeepOff). Drawn for
// each level (random) from the salt, or else from the map; a map with no
// exit of the kind is left by any (E2M8: the Cyberdemon's death). It gives
// way to any exit for the rest of the level (the user: a designer's trick
// may shut a secret exit for good, after 30 s or a line crossed) after
// EXIT_IDLE_MOST in all with no way to it and nothing to fix, or EXIT_STALL
// with no progress toward it - no fix on the way done, the way left never
// EXIT_GAIN shorter than its best so far - counted out of a fight, with no
// detour on and no sweep before the exit.
#define EXIT_IDLE_MOST (30 * TICRATE)
#define EXIT_STALL (3 * 60 * TICRATE)
#define EXIT_GAIN 64.0
#define EXIT_ANY -1
#define EXIT_UNDECIDED -2

// A door that leaves less headroom than the player's height is shut; a
// floor more than STEP_UP higher is a step it does not climb (P_TryMove).
#define PLAYER_HEIGHT 56.0
#define STEP_UP 24.0

// A barrel in the way (plan, 7.3) goes off with its chain: the barrels under
// BARREL_CHAIN apart that see each other, up to MAX_CHAIN. Any of them may be
// the one shot (the user: MAP23's are placed so that shooting the one in
// the way hurts; the user shot one on a ledge from outside the yellow door,
// and the rest went off out of sight). It is shot from a spot BARREL_RINGS
// from it, in BARREL_WAYS directions, that the player can stand at, in reach
// of the shots, and safe from every barrel of the chain: BARREL_SAFE off, or
// out of its sight (a blast hurts only what can see it; a dead barrel is a
// quarter of its height, BLAST_HEIGHT). Shooting gives up after
// BARREL_SHOOT_TICS out of a fight (combat shoots until it goes off, or
// misses too often). Once it is gone, the player goes on only when no shot
// barrel within BARREL_CHAIN_WAIT of it or of the player is still to go off.
#define BARREL_WAYS 16
#define BARREL_SAFE 160.0
#define BARREL_CHAIN 118.0
#define MAX_CHAIN 32
#define BARREL_SHOOT_TICS (10 * TICRATE)
#define BARREL_CHAIN_WAIT 400.0
#define GUN_HEIGHT 36.0
#define EYE_HEIGHT 42.0
#define BARREL_HEIGHT 42.0
#define BLAST_HEIGHT 10.0

static const double barrel_rings[] = { 192.0, 256.0, 320.0 };

// A Cyberdemon or a spider mastermind the level needs dead (KILL EVERY
// <kind>) is fought from a firing spot, not walked up to (the user, E4M6:
// the route led to the Cyberdemon, met at the door of its room 130 to 250
// off - too near for rockets or cover, dead every time; peek from cover,
// then fight it from where rockets reach it): one FIRE_RINGS from it - as
// near as it is kept off, no further than aim suffers - in FIRE_WAYS
// directions, on floor the player can stand on that does not hurt, a
// radiation suit worn or not (the user, E4M6: at 512 to 768, a spot down in
// the nukage, the suit on, far off for the rockets to find it; a dry ledge
// over it was better), the boss in sight from the gun - the FIRE_MOST
// nearest the player as the crow flies are planned to, the nearest by route
// gone to. Got there, the player stays while combat fights it
// (K_Combat_Needed: fought as far as it targets); a new spot once it has
// moved KILL_DRIFT, or after FIRE_IDLE out of a fight. Only while it can be
// fought: met as hopeless, it is gone for itself as before - kept off,
// passed, what beats it fetched (E4M8: from spot after spot in its sight,
// no fight there, the spider mastermind shot the bot dead in 4 salts of 4;
// head killed it in all four). With no open way to any spot, the boss
// itself is gone for, as before, until it moves. Spots with cover beside
// them go first (the user, E3M8: "cover from the spider" - spots in the
// open arena, the bot shot down there; combat fights from beside cover,
// k_combat.c: COVER_REACH): a spot out of the boss's sight within FIRE_COVER,
// in one of FIRE_COVER_WAYS directions every FIRE_COVER_STEP, on a floor
// within a step, nothing in the way at the waist; a spot without counts
// FIRE_BARE further off, and is dropped while any with cover is kept.
#define FIRE_WAYS 24
#define FIRE_MOST 16
#define FIRE_IDLE (3 * TICRATE)
#define FIRE_COVER 96.0
#define FIRE_COVER_STEP 32.0
#define FIRE_COVER_WAYS 8
#define FIRE_BARE 1024.0

static const double fire_rings[] = { 512.0, 576.0, 640.0 };

// Items worth a detour (k_items.h, plan 7.5) are looked for every
// ITEM_CHECK_TICS while the player follows a route, the ITEM_ROUTES most
// promising by worth and distance looked at by route (MAX_ITEM_ROUTES when
// health is CRITICAL, any detour then being worth it, and in a fight, where
// the nearest may lie beyond the monsters - GoesFirst); one with no open way
// there, or none on from it, is not looked at again for ITEM_SKIP_TICS. The
// distance is as the crow flies, or twice that from the route within
// ROUTE_AHEAD ahead if less: an item by the route ahead costs a step aside,
// however far off it is (Doom II MAP31: the clips of two SS just killed lay
// by the route 340 off, past the 256 they were worth, and were run past).
// They are ranked as if none paid more than RANK_REACH: one any detour pays
// for (the ammo running out) does not crowd out what lies near (the user,
// E2M2: a green armour 240 off, unguarded, at 0 armour, never looked at -
// every second's looks went to shells 2000 off, any detour paying, which
// then had no way there).
#define ITEM_CHECK_TICS TICRATE
#define ITEM_ROUTES 2
#define MAX_ITEM_ROUTES 4
#define ITEM_SKIP_TICS (10 * TICRATE)
#define ROUTE_AHEAD 1024.0
#define RANK_REACH 3000.0

// A sweep before the exit (k_items.c: SWEEP_UNITS; the user: not the
// detours changed, but a guess just before leaving whether too much ammo
// was left lying - then even the longest detours for it, cells and rockets,
// armour - or "nah, we probably have enough for the next level"): once a
// level, GO TO EXIT the only goal and the way left to the exit no longer
// than SWEEP_NEAR, while the arsenal goes on to a next level
// (K_Condition_ArsenalGoesOn) and the level has monsters - when something
// lying about is worth one (K_Items_Sweepable). Over once nothing is, or
// after SWEEP_MOST (the user: limits, but generous - never an endless run);
// the route to the exit goes on all the while, so with nothing in reach the
// player just leaves.
#define SWEEP_NEAR 1024.0
#define SWEEP_MOST (5 * 60 * TICRATE)

// Some items go first (GoesFirst): a weapon at a pistol start (k_items.h:
// first; the user: a health bonus far off was gone for, a shotgun by the
// route left, with only the pistol in hand), and health when CRITICAL. They
// are looked at by route before any other item, the nearest first
// (FIRST_SCORE puts them ahead of any score by worth and distance), and go
// before the others when in reach. Of two such, the one in hand soonest -
// the shorter way there, the work at a fix counted - not the smaller detour
// nor the most to spare (the user: time to acquire; Doom II MAP10: a
// chaingun on the way to the key, behind a switch and a lift, was gone for
// over a shotgun back by the start, a 465-unit run, and the bot fought with
// the pistol for 20 s; at 23 health, a berserk 5322 units off over a
// medikit near, and the bot died on the way); the monsters inside the areas
// on the way there count DAMAGE_WAY a point of health (WayThrough). One
// already gone for gives way to another only when the other's way there is
// REPLACE_MARGIN shorter, so counted, or the two would flip.
#define FIRST_SCORE 1.0e6
#define REPLACE_MARGIN 256.0

// In a fight only items worth breaking it off for are looked for (k_items.h:
// urgent), and but for those worth leaving it for however far (escape:
// health when LOW or CRITICAL, ammo when LOW), only within FIGHT_REACH of
// detour: a dash, shooting on the way, not a trip. Going for one, the fight
// is broken off once the item is within FIGHT_REACH (UpdateEscape) - but
// only while no monster the player can see is within DASH_CLEAR: through
// monsters close by, the fight comes first (the user, Doom II MAP02: dashing
// for a super shotgun, the bot walked into a room of zombiemen and shotgun
// guys 40 to 90 off, fighting none of them, and died). Stopped so, the dash
// waits until none is within DASH_FREE (E3M3: a dash for the shotgun broke a
// fight off and began it again 7 times in 15 s, imps and demons coming in
// and out of DASH_CLEAR as the fight backed off them and the run turned back
// toward them) - or, once a fight began, until it is over: the fight backs
// off to fire rockets, and loses sight of its target round corners, beyond
// DASH_FREE (the user, E3M2: a dash for a box of rockets past a cacodemon
// broke the fight off and began it again 7 times in 17 s, the rocket
// launcher brought up at 383 off and put away for the pistol as the run
// turned back toward it).
#define FIGHT_REACH 400.0
#define DASH_CLEAR 128.0
#define DASH_FREE 384.0

// An invulnerability sphere run for, a boss on the player (SphereRun), is
// gone for with its way there no longer than SPHERE_RUN - some 7 s at a run;
// its 30 s begin when it is picked up (Doom II MAP10: by the windows where
// the bot met the Cyberdemon, the nearer sphere was 3412 off by route, round
// the rooms between). One with no open way there that short is not looked
// at again for SPHERE_SKIP (MAP10: with the far sphere taken for one, the
// health gone for at 14 was dropped every second).
#define SPHERE_RUN 4096.0
#define SPHERE_SKIP (30 * TICRATE)

// A boss chased while invulnerable (ConsiderChase) only within CHASE_REACH
// by route (Doom II MAP10, salt 7: the Cyberdemon teleported to the other
// room, 3062 off, and the bot spent 16 s of the 22 running after it).
#define CHASE_REACH 1536.0

// Stacked detours (plan, 7.8: detours are sticky; the user, E1M8: going for
// the first shotgun, the bot ran up the stairs 140 off the soulsphere behind
// the secret door there and never went back for it, one detour at a time):
// while an item is gone for - on top, the route ending at it - another whose
// extra way to it is no more than STACK_SHARE of its own reach, and
// STACK_MOST (the soulsphere 140 off as the crow flies was 1178 out of the
// way: round by the stairs to its door, a second to open, counted as 560),
// goes on top of it, out of a fight, through no fix; the one below is gone
// for again once it is picked up. On top of one that goes first (GoesFirst:
// a first weapon, the health or ammo that is short), only health and armour,
// what keeps the player alive (E1M8: a box of bullets 1052 out of the way
// went on top of the first shotgun, which came at 52 s, not 29). STACK_DEPTH
// items on the chain at the most.
#define STACK_SHARE 0.5
#define STACK_MOST 1536.0
#define STACK_DEPTH 2

// Out of a fight, an item behind one fix the brain knows - barrels in the
// way, a door or bars a switch or walk-over line opens; never a key - is
// gone for through it: the detour counts the way to the fix's nearest spot,
// on to the item and on, and FIX_WORK for the work there (the user, Doom II
// MAP04: the shotguns dropped behind barrels in a doorway, and a super
// shotgun behind a door a switch opens, were left with the shells gone).
// Of the fix's spots, the FIX_SPOTS with the least way through them are
// looked at; going for the fix, every one (MAP25 without monsters: a
// megasphere behind a chain of barrels costed through all 64 spots round
// them, 128 route searches, 80 ms every second).
#define FIX_WORK 300.0
#define FIX_SPOTS 8

// Telefrags (the user: map designers shut a monster in on a teleporter's
// landing spot for the player to kill by teleporting onto it - E4M2's
// Cyberdemon, caged with the BFG; P_TeleportMove kills whatever the player
// lands on): a tough monster (K_Combat_OnSpot; a weaker one is telefragged
// only by chance) on the landing spot of a teleporter the player can get to
// now, through no fix, the way there ending by it and a way on from there to
// where the route goes - TELEFRAG goes on top, a detour through the
// teleporter. No more than TELEFRAG_REACH out of the way; any length for the
// Cyberdemon, the spider mastermind and the arch-vile, too big a threat to
// fight (the user). Looked for every TELEFRAG_CHECK_TICS while a route is
// followed, in a fight too (a standoff with the very monster); a teleporter
// looked at is not again for TELEFRAG_SKIP. Done on landing, within
// TELEFRAG_LANDED of the spot. With nothing tough on the spot for
// TELEFRAG_OFF (it stepped off, or died), it fails: not again for 30 s,
// then longer (the user: never replanning in a loop).
#define TELEFRAG_REACH 512.0
#define TELEFRAG_CHECK_TICS TICRATE
#define TELEFRAG_SKIP (10 * TICRATE)
#define TELEFRAG_OFF TICRATE
#define TELEFRAG_LANDED 32.0

// The floors that hurt on a detour (k_path.h: damage) - there and on, less
// what the route takes anyway - count against it, each point of health as
// DAMAGE_WAY units of detour, as the pathfinder counts them; and health is
// gone for only when they take no more than HEALTH_NET of what it gives
// (the user: on levels with large pools of nukage, stimpacks chased across
// it cost more health than they gave).
#define DAMAGE_WAY 100.0
#define HEALTH_NET 0.5

// So do the monsters inside the areas a detour goes through that the route
// left does not (k_area.h: inside, the asleep at half) - not the item's own
// area, whose guards its worth counts already (k_items.h: danger) - each
// point of health as DAMAGE_WAY units of detour (the user: detours took the
// bot into fresh fights on many fronts, waking monsters). An item any detour
// pays for (k_items.h: reach - health LOW, the ammo running out) pays for
// no more than RISKY_REACH while they come to RISKY_ANY or more (MAP10: a
// stimpack at LOW health, 18062 units out of the way through 125 health of
// monsters); but for health CRITICAL, when staying costs as much.
#define ANY_REACH 1.0e5
#define RISKY_ANY 20.0
#define RISKY_REACH 3000.0

// Routes (k_path.h: K_Path_SetEnterCost): going into an area costs
// ROUTE_DANGER_WAY units for each point of what the awake monsters that
// could hit the player there would cost (k_area.h: awake_danger) - a way
// round a Cyberdemon in sight when it is not much longer; soft only, no
// route is ever refused (the user).
#define ROUTE_DANGER_WAY 20.0

// Falling back (the user: fight from safety). In a fight going badly -
// FALL_LOSS health or more lost over the last FALL_TICS with FALL_PRESSURE
// or more of awake monsters in sight (what fights with them would cost), or
// those costing more than the health left - in an area that is not safe,
// the player falls back to the nearest safe area (k_area.h; of the
// FALL_CANDIDATES nearest, within FALL_REACH of route, those visited before
// counted VISITED_SHARE as far), no floor that hurts in it or on the way
// (E1M3: fallen back into a nukage pit, a step of 120 out, the bot burned),
// whose way there goes through no other area
// with monsters inside - open now, or through one switch or walk-over line
// that opens it, its work counted FIX_WORK (FallBackWay): a GO TO on top, the
// fix pushed on it, the fight broken off, shooting on the way (UpdateEscape);
// there, holding short of the way back in
// (k_combat.c) lets the monsters come. Only against a crowd - FALL_CROWD
// monsters or more on the player - or gunmen worth FALL_GUNMEN or more,
// whom getting out of sight answers: a few tough ones that chase follow the
// player anywhere, and running gives up the shots (E1M8: falling back from
// the two barons, it died 3 s later, where it had stood and won before). Not
// while holding already (MAP10: holding short of ten monsters as they came,
// the bot fell back 1331 units and died on the way), nor invulnerable for a
// chase (ConsiderChase), nor further than
// FALL_REACH; not again for FALL_AGAIN; with no such area, looked for again
// in a second.
#define FALL_LOSS 25
#define FALL_TICS (3 * TICRATE)
#define FALL_PRESSURE 15.0
#define FALL_CROWD 3
#define FALL_GUNMEN 10.0
#define FALL_REACH 1024.0
#define FALL_CANDIDATES 6
#define VISITED_SHARE 0.75
#define FALL_AGAIN (8 * TICRATE)

// Fallen back with health not OK, the player rests there (the user, E4M2:
// fallen back from the barons and lost souls let out of the NE arm's closet
// into the corridor behind door 9 at 37 health, the bot set off again at
// once, the medikits all gone, back out to the sill in the barons' sight -
// 10 health, fell back again, set off again, and died there): it stands,
// fighting what comes after it - the fight from safety the fall back was for
// - and looking for health as ever (items), no longer falling back (the
// fight is not broken off). On once nothing was fought, came at it or was
// felt (no health lost over FALL_TICS) for REST_QUIET, or after REST_MOST,
// or with health OK again, or something to go for first in reach.
#define REST_QUIET (5 * TICRATE)
#define REST_MOST (30 * TICRATE)

// A way back (the user, Doom II MAP15: teleported into the tower room, the
// stairs out are built by a switch beside the landing - better pressed before
// the fight than in it): brought somewhere by a teleporter, with monsters
// about - alive, awake or not, seen or not, within WAY_BACK_DANGER, worth
// WAY_BACK_RISK or more - the player looks at the way back to the cell it
// stood on before. Blocked by what a switch or walk-over line fixes (never a
// key) and keeps fixed (no lift, no door that shuts again), that one's
// nearest spot within WAY_BACK_REACH, it is worked first - once no awake
// monster is in sight, within WAY_BACK_TICS of getting there, out of a fight,
// following a route. Not after a drop (Doom II MAP04: dropped into a room,
// the bars back out pressed open for nothing).
#define WAY_BACK_REACH 512.0
#define WAY_BACK_DANGER 1024.0
#define WAY_BACK_RISK 30.0
#define WAY_BACK_TICS (10 * TICRATE)
#define TELEPORT_JUMP 64.0

// Ways count the waits at doors, lifts and teleporters on them, as the
// pathfinder expects them (k_path.h: wait), each tic as WAIT_WAY units, the
// way the player could run meanwhile: an item behind a slow lift is further
// off than its length.
#define WAIT_WAY 16.0

// An item whose way there waits more than ITEM_WAIT_MOST at doors and
// lifts is not gone for, for now (E1M6: at LOW health, any detour paying,
// the bot stood 25.6 s at a door shut on a timer for a stimpack behind it,
// and went for a medikit 1200 units off right after).
#define ITEM_WAIT_MOST (8 * TICRATE)

// A radiation suit (k_items.h: suit) is also worth the detour when going on
// by it - the way there, then on with the suit worn - costs SUIT_GAIN less
// than going on without, the floors counted as above: those it would keep
// out on the route ahead, or a way round a pool cut short through it (the
// user: worth a detour for long stretches of nukage, and lying in the
// nukage more often than not). About 5 health: the reckoning is rough, and
// a little saved is no trip (Doom II MAP29: 353 saved, and a lift that took
// 24 s); MAP24's suit by the nukage run below the ledge saves 768, and the
// bot keeps 100 health through it (70 without). Not while one is worn with
// SUIT_KEEP tics left.
#define SUIT_GAIN 500.0
#define SUIT_KEEP (20 * TICRATE)

// Held up by a barrel where the map shows room to pass (the user, E1M5:
// barrels in a doorway held the bot for seconds before it squeezed past):
// following a route, with nothing to work and no fight, the player kept
// within HOLD_RADIUS of one spot for HOLD_TICS, a standing barrel within
// HOLD_GAP of its box - that barrel is shot from a safe spot, as one in the
// way (GOAL_BARREL).
#define HOLD_TICS (5 * TICRATE / 2)
#define HOLD_RADIUS 48.0
#define HOLD_GAP 24.0
#define PLAYER_RADIUS 16.0

// Got to an item or a key in the air, falling onto it (Doom II MAP03: a box
// of shells at the foot of a drop): the engine touches things only as the
// player moves across (P_TryMove), and one landed on straight down, nav
// done, stays lying (the user, E3M3: the cell pack at the foot of a drop of
// 48, the bot stood on it for good, nothing logged). Landed with it still
// there, the player steps STEP_OFF aside, to a spot it fits at with a way
// back - the first step touches it; with no such spot, the spot did not
// work out.
#define STEP_OFF 32.0

// Going round in a loop (the user: whatever the cause, break it; Doom II
// MAP15 pushed and dropped a key for 13 minutes): a goal planned LOOP_PLANS
// times within LOOP_TICS - a wait for moving sectors plans once a second,
// a loop several times - fails, so the goal below looks for another way.
// GO TO EXIT never fails: the player wanders to an open spot WANDER_MIN to
// WANDER_MOST off by route, through no floor that hurts, picked at random
// (salt, k_salt.h) out of WANDER_TRIES, and plans again from there; with
// none, it waits IDLE_TICS.
#define LOOP_PLANS 20
#define LOOP_TICS (5 * TICRATE)
#define WANDER_MIN 256.0
#define WANDER_MOST 1024.0
#define WANDER_TRIES 32

// Exploring (-karpattitude explorer; the user: a natural exploration while
// the bot runs through the level, the sweep before the exit for the rest):
// places off the way are gone to as items are (ConsiderItems), a detour on
// top of the goal followed - an area not visited yet (k_area.h; no door or
// lift, PLACE_AREA_MIN of floor at least, not mostly one that hurts) within
// PLACE_AREA_REACH of detour, a secret not found yet (K_Specials_SecretSector)
// within PLACE_SECRET_REACH, PLACE_SWEEP_REACH in the sweep before the exit.
// The monsters inside the areas on the way and the place's own count against
// it, as for an item (DAMAGE_WAY). Only out of a fight, health OK and ammo
// not LOW, no boss on the player, no other place on; on top of an item gone
// for, but one that goes first (GoesFirst), within STACK_SHARE of its reach,
// in the sweep too (E1M2: items gone for one after another, a place looked
// at 8 times in 90 s; E1M5: PlaceReach); never into the area of a live
// Cyberdemon or spider mastermind. The
// PLACE_ROUTES most promising (PLACE_SWEEP_ROUTES in the sweep: E1M2, the
// two nearest had no way there, and the exit was reached before the next;
// none whose way could not be shorter than the nearest found)
// are looked at by route every PLACE_CHECK_TICS, one too far for now not
// again for ITEM_SKIP_TICS. One with no way there, or that did not work out
// (the user: decoys, secrets a map's bugs put out of reach), is set aside
// for PLACE_SKIP, and dropped for the level after PLACE_TRIES such - but not
// one looked at on top of an item, where no fix counts: not again for
// ITEM_SKIP_TICS only (E4M1: the secret room, past lift 75 lowered, looked at
// in the sweep while health bonuses were gone for, set aside for 60 s, and
// the level left). A secret's reach is wide (the user: "E4M1 is a very
// compact map, so the bot should be willing to go anywhere" - the secret
// room 6000 to 6500 out of the way, through the fix and the nukage). No more
// places after EXPLORE_MOST spent on them in a level. None within EXIT_CLEAR
// of a walk-over exit line: passed on the way out anyway, and gone to, the
// player may run on over it (E3M5: an area 44 off the exit line, the level
// ended the tic after it was reached). An explorer's sweep
// before the exit is for every level, and for the places not seen yet too,
// any way off (PLACE_SWEEP_REACH; E1M2: those left lay 8000 to 13000 off,
// the other half of the map), the nearest by the way there first, not the
// least out of the way to the exit, and one with no way back to where the
// player is (a drop, a door that opens from one side) only once no other is
// left (the user, E3M4: "not that much exploration"; the area by the exit,
// on the way, was gone to first, through door 1041 that opens one way only,
// and 26 places were out of reach behind it), the waits at doors and lifts on
// the way up to PLACE_WAIT_MOST (an item's ITEM_WAIT_MOST: E1M2, the rooms
// left were 9 s of doors and lifts away and more): begun once GO TO EXIT is all
// there is left to do and has a way, however far (E1M2: begun 339 off the
// exit, a drop of 88 on the way there left every place behind out of
// reach); over when none is left, after EXPLORE_MOST, or with nothing new
// reached for EXPLORE_STALL. Places and items are looked for every tic
// within SWEEP_HOLD of the exit, not every second, and places while passing
// a lift too (PASS_TICS): the level is not left for want of a look (E1M2:
// the lift ridden for an area kept the sweep off for 7 s, and the exit was
// pressed with 38 places not seen). An area looked into counts as visited:
// its spot in sight within PLACE_SEEN_REACH, not only once the player stood
// in it (the user, E3M1: the yard by the start, in pieces, each gone a step
// into one after another, a different way each time - the bot looked to
// move erratically); a secret is found only by going in.
#define PLACE_AREA_REACH 2048.0
#define PLACE_SEEN_REACH 1024.0
#define PLACE_SECRET_REACH 32768.0
#define PLACE_SWEEP_REACH ANY_REACH
#define PLACE_AREA_MIN 4096.0
#define PLACE_ROUTES 2
#define PLACE_SWEEP_ROUTES 16
#define PLACE_WAIT_MOST (30 * TICRATE)
#define PLACE_CHECK_TICS TICRATE
#define SWEEP_HOLD 256.0
#define PLACE_SKIP (60 * TICRATE)
#define PLACE_TRIES 3
#define EXPLORE_MOST (15 * 60 * TICRATE)
#define EXPLORE_STALL (2 * 60 * TICRATE)
#define EXIT_CLEAR 128.0
#define MAX_BOSSES 8

typedef enum
{
    GOAL_GOTO,              // a point (a click)
    GOAL_EXIT,              // any exit line
    GOAL_KEY,               // any key of a colour
    GOAL_TRIGGER,           // any line that moves a sector
    GOAL_ITEM,              // an item worth a detour, on top of the chain
    GOAL_BARREL,            // a barrel in the way, shot from a safe spot
    GOAL_KILL,              // every monster of a kind dead: a map's bosses
    GOAL_TELEFRAG,          // through a teleporter, onto a monster on its
                            // landing spot (TELEFRAG_REACH)
    GOAL_PLACE,             // a place off the way, for an explorer: an area
                            // to visit, a secret to find (PLACE_AREA_REACH)
} goal_type_t;

// A place a goal can be met, and what to do there.
typedef struct
{
    k_vec2_t at;
    k_goalkind_t kind;
    int line;               // the line to work, -1 for a key or a point
    k_vec2_t aim;           // GOAL_BARREL: the barrel to shoot from there;
                            // GOAL_KILL: where the monster stood
    int door;               // GOAL_BARREL: the shut door it sees the barrel
                            // through once open (BarrelSpots), else -1
    char what[64];          // "exit switch at line 162", "blue keycard"...
} candidate_t;

typedef struct
{
    goal_type_t type;
    int param;              // GOAL_KEY: the key; GOAL_TRIGGER: the sector;
                            // GOAL_ITEM: the item (K_Items_Get); GOAL_BARREL:
                            // the barrel's block (k_block_t); GOAL_KILL: the
                            // kind (mobjtype_t); GOAL_TELEFRAG: the
                            // teleporter (k_graph.h: teleports)
    k_vec2_t point;         // GOAL_GOTO
    int victim;             // GOAL_TELEFRAG: the kind on the spot, and the
    int off;                // tics nothing tough has stood there, in a row

    // Not planned until a candidate is chosen; planned again when it
    // fails, or a goal in front of this one ends.
    boolean planned;
    candidate_t chosen;
    char note[K_LOG_LINE_LEN];  // what it is doing or waiting for

    k_vec2_t tried[MAX_TRIED];  // candidates that failed
    int tried_until[MAX_TRIED]; // and when each may be tried again
    int num_tried;
    int shut;                   // times its way shut on the way (MAX_SHUT)
    boolean idle;               // GO TO EXIT: no way for now, waiting
    boolean landing;            // GOAL_KEY, GOAL_ITEM: got there in the air,
                                // falling onto it (STEP_OFF)
    boolean staying;            // GO TO EXIT: on the exit floor, waiting for
                                // it to end the level
    boolean shooting;           // GOAL_BARREL: at the spot, combat shooting
                                // (wait counts the tics); GOAL_KILL: at the
                                // firing spot, combat fighting
    boolean fixable;            // GOAL_ITEM: gone for through a fix in its
                                // way (FIX_WORK), pushed in front of it
    boolean fallback;           // GOAL_GOTO: falling back to a safe area out
                                // of a fight going badly (FALL_LOSS)
    boolean resting;            // and there, hurt, resting (REST_QUIET): wait
                                // counts the tics, quiet those in a row quiet
    boolean way_back;           // GOAL_TRIGGER: opening the way back from
                                // where a teleporter brought the player
                                // (WAY_BACK_REACH)
    boolean wander;             // GOAL_GOTO: out of a loop GO TO EXIT went
                                // round (BreakLoop)
    boolean chase;              // GOAL_KILL: a boss chased while invulnerable
                                // (ConsiderChase)
    boolean near;               // GOAL_KILL: no way to a firing spot - the
                                // boss itself gone for (FIRE_RINGS)
    int quiet;                  // GOAL_KILL: tics at the firing spot out of a
                                // fight (FIRE_IDLE); GOAL_GOTO: resting
    int plans;                  // times planned since plans_since, within
    int plans_since;            // LOOP_TICS (BreakLoop)

    // GOAL_TRIGGER: the sector's heights when its line was gone for; or,
    // if it was busy then (settling: K_Graph_SectorBusy, a door closing by
    // itself), when it stopped.
    double floor, ceiling;
    boolean settling;
    boolean used;           // a walk-over line was crossed
    boolean moving;
    boolean changed;        // the heights have differed from those since
    int wait;
    int dry_look;           // moving, on a floor that hurts: when a dry spot
                            // may be looked for (StayDry)

    // GOAL_TRIGGER: the blocked crossing it was pushed for (crossed out of
    // cell from), -1 if none. Once that opens, even if only after a wait
    // (a door opening, a lift on its way down), the goal is done: the
    // player goes on and waits there, not at the switch. want: the moves
    // of the sector that would open it (k_move_t); only lines that make one
    // of them are candidates. For a gap inside a cell, portal is -1, and
    // pinch_cell and pinch_at say where (else pinch_cell is -1): done once
    // the gap is wide enough.
    int portal, from;
    int want;
    int pinch_cell;
    k_vec2_t pinch_at;

    // Found no way while sectors were moving: not planned again before
    // leveltime retry_tic; waited is how long it has waited so far.
    int retry_tic;
    int waited;
} goal_t;

typedef struct
{
    goal_type_t type;
    int param;
    int count;              // times it failed this level
    int since;              // when it failed last
    int until;              // when it may go on the stack again
    char text[K_LOG_LINE_LEN];
} failed_t;

static goal_t goals[MAX_GOALS];
static int depth;           // goals[depth - 1] is worked on
static failed_t failed[MAX_FAILED];
static int num_failed;
static boolean idling;      // GO TO EXIT waits with no way, nothing to do
static boolean exit_wanted; // GO TO EXIT goes on an empty list
static k_exitchoice_t exit_choice;  // -karpexit
static boolean explorer;            // -karpattitude explorer
static int exit_kind;       // this level: the kind wanted, 1 secret, 0 normal;
                            // EXIT_ANY; EXIT_UNDECIDED until GO TO EXIT
static int exit_idle;       // tics with no way to it (EXIT_IDLE_MOST)
static int exit_stall;      // tics with no progress toward it (EXIT_STALL)
static double exit_best;    // the shortest way left to it so far

// An explorer's places (PLACE_AREA_REACH), listed on level load.
typedef struct
{
    int area;               // an area to visit, or -1
    int sector;             // a secret to find, or -1
    int in_area;            // the area its spot lies in, -1 if none
    k_vec2_t spot;          // where to go
    int skip;               // not looked at before this
    int tries;              // times set aside (PLACE_TRIES)
    int later;              // no way back from it, other places first: not
                            // looked at before this (PLACE_SWEEP_REACH)
    boolean seen;           // an area: its spot was in sight (PLACE_SEEN_REACH)
} place_t;

static place_t *places;
static int num_places;
static int place_seen_next;     // the place to look for in sight next
static int place_check_tic;     // when to look for places next
static int explore_tics;        // spent on places this level (EXPLORE_MOST)
static int secrets_found;       // the player's secrets, as last logged

// Refreshed when planning: the keys lying in the level (up to
// MAX_CANDIDATES of each colour known by place), and where the player was.
static int keys_lying[K_NUM_KEYS];
static k_vec2_t keys_at[K_NUM_KEYS][MAX_CANDIDATES];
static k_vec2_t planned_from;
static unsigned char *line_uses;    // per line, as a trigger goal's choice
static int *line_last;              // per line, when it was last chosen
static k_path_t scratch;
static k_path_t probe;              // routes looked at while scratch is in use
static k_path_t fix_probe;          // routes to and from a fix an item is behind

// Areas (k_area.h) the route left goes through, marked route_stamp; those a
// detour has counted already, marked risk_stamp; those the way to an item
// has, through_stamp.
static int *route_marks;
static int *risk_marks;
static int *through_marks;
static int marks_size;
static int route_stamp;
static int risk_stamp;
static int through_stamp;

// The player's health over the last FALL_TICS, -1 where not known yet;
// and when falling back may be looked at again.
static int health_ring[FALL_TICS];
static int fall_again;
static candidate_t fix_candidates[MAX_CANDIDATES];
static int fix_order[MAX_CANDIDATES];       // least way through them first
static double fix_least[MAX_CANDIDATES];    // the least way through each

static char outcome[K_LOG_LINE_LEN];    // how the list last emptied
static char status[K_LOG_LINE_LEN * 2];
static int item_check_tic;              // when to look for items next
static boolean was_critical;            // health was CRITICAL at the last look
static int dashing = -1;                // the item fights are broken off for
static boolean dash_held;               // its dash stopped for a monster close
static boolean dash_fought;             // by (DASH_FREE), and a fight begun
static int run_waited = -1;             // the item whose run waited, logged
static int *item_skip;                  // per item: not looked at before this
static int item_skip_size;
static k_vec2_t hold_at;                // where the player has kept near, and
static int hold_since;                  // since when (HOLD_TICS)
static k_pose_t last_pose;              // the player last tic, if known
static boolean last_known;
static int back_cell = -1;              // where the player stood before a
static int back_until;                  // teleporter brought it here, looked
                                        // at until then (WAY_BACK_TICS)
static int telefrag_check_tic;          // when to look for telefrags next
static int *telefrag_skip;              // per teleporter: not looked at before
static int telefrag_skip_size;          // this (TELEFRAG_SKIP)
static int pass_portal = -1;            // the crossing a lift or door opens,
static int pass_from;                   // its line just worked (PASS_TICS),
static int pass_sector;                 // crossed out of pass_from; the
static int pass_line;                   // mover, the line, and until when
static int pass_until;
static boolean pass_routed;             // the route has gone through it
static boolean pass_past;               // the player has got past it
static boolean pass_fought;             // a fight took the window
static int chase_kind = -1;             // the boss last on the player
static int chase_check_tic;             // when to look at the chase next
static int pass_excused = -1;           // when one missed so: the way shut
                                        // is not counted (MAX_SHUT)
static int sweep_since;                 // when the sweep before the exit
                                        // began; 0 before, -1 once over
static int sweep_found;                 // when it last reached something
                                        // (EXPLORE_STALL)

//
// Names
//

static void GoalName(const goal_t *goal, char *buf, int len)
{
    switch (goal->type)
    {
        case GOAL_EXIT:
            M_StringCopy(buf, "GO TO EXIT", len);
            break;

        case GOAL_KEY:
            M_snprintf(buf, len, "GET %s KEY", goal->param == K_KEY_BLUE ? "BLUE"
                       : goal->param == K_KEY_YELLOW ? "YELLOW" : "RED");
            break;

        case GOAL_ITEM:
            M_snprintf(buf, len, "GET %s", K_Items_Get(goal->param)->name);
            M_ForceUppercase(buf);
            break;

        case GOAL_BARREL:
            M_snprintf(buf, len, "SHOOT BARREL at (%.0f,%.0f)",
                       K_Map_Get()->blocks[goal->param].at.x,
                       K_Map_Get()->blocks[goal->param].at.y);
            break;

        case GOAL_KILL:
            M_snprintf(buf, len, "%s %s", goal->chase ? "CHASE THE" : "KILL EVERY",
                       K_Combat_KindName(goal->param));
            M_ForceUppercase(buf);
            break;

        case GOAL_TELEFRAG:
            M_snprintf(buf, len, "TELEFRAG THE %s", K_Combat_KindName(goal->victim));
            M_ForceUppercase(buf);
            break;

        case GOAL_PLACE:
            if (places[goal->param].sector >= 0)
            {
                M_snprintf(buf, len, "FIND SECRET in sector %d", places[goal->param].sector);
            }
            else
            {
                M_snprintf(buf, len, "EXPLORE AREA %d", places[goal->param].area);
            }
            break;

        // The line's tag: a stair builder's steps after the first are not
        // tagged.
        case GOAL_TRIGGER:
            // By the special at level load: an open-and-stay door loses its
            // own once used.
            if (goal->planned && goal->chosen.kind == K_GOAL_USE_LINE
             && K_Specials_ManualDoor(K_Map_Get()->lines[goal->chosen.line].special))
            {
                M_snprintf(buf, len, "OPEN DOOR line %d (sector %d)", goal->chosen.line,
                           goal->param);
            }
            else if (goal->planned && goal->chosen.kind == K_GOAL_USE_LINE)
            {
                M_snprintf(buf, len, "USE SWITCH line %d (tag %d)", goal->chosen.line,
                           lines[goal->chosen.line].tag);
            }
            else if (goal->planned && goal->chosen.kind == K_GOAL_SHOOT_LINE)
            {
                M_snprintf(buf, len, "SHOOT SWITCH line %d (tag %d)", goal->chosen.line,
                           lines[goal->chosen.line].tag);
            }
            else if (goal->planned)
            {
                M_snprintf(buf, len, "CROSS LINE %d (tag %d)", goal->chosen.line,
                           lines[goal->chosen.line].tag);
            }
            else
            {
                M_snprintf(buf, len, "USE TRIGGER for sector %d (tag %d)", goal->param,
                           sectors[goal->param].tag);
            }
            break;

        default:
            M_snprintf(buf, len, "%s (%.0f, %.0f)", goal->fallback ? "FALL BACK TO"
                       : goal->wander ? "WANDER TO" : "GO TO", goal->point.x, goal->point.y);
            break;
    }
}

//
// The stack
//

static failed_t *FailedEntry(goal_type_t type, int param)
{
    int i;

    for (i = 0; i < num_failed; ++i)
    {
        if (failed[i].type == type && failed[i].param == param)
        {
            return &failed[i];
        }
    }

    return NULL;
}

// Failed, and not yet due to be tried again - sooner while idling.
static boolean IsFailed(goal_type_t type, int param)
{
    const failed_t *entry = FailedEntry(type, param);

    return entry != NULL
        && leveltime < (idling ? SDL_min(entry->until, entry->since + (IDLE_TICS
                                         << SDL_min(SDL_max(entry->count - 1, 0), 3)))
                               : entry->until);
}

static boolean OnStack(goal_type_t type, int param)
{
    int i;

    for (i = 0; i < depth; ++i)
    {
        if (goals[i].type == type && goals[i].param == param)
        {
            return true;
        }
    }

    return false;
}

// Whether a goal may go on the stack: not one that failed, not one already
// there (it would wait for itself), and not too deep. While fixes_stacked,
// one already there counts: a detour on is measured through the fixes
// pushed for it (ConsiderItems).
static boolean fixes_stacked;

static boolean Allowed(goal_type_t type, int param)
{
    return depth < MAX_GOALS && !IsFailed(type, param)
        && (fixes_stacked || !OnStack(type, param));
}

// How many times a line was chosen lately: the count starts over
// LINE_USES_TICS after the last time.
static int LineUses(int line)
{
    if (line_uses == NULL)
    {
        return 0;
    }

    if (leveltime - line_last[line] >= LINE_USES_TICS)
    {
        line_uses[line] = 0;
    }

    return line_uses[line];
}

// Whether a line can still be worked: it has its special (one-shot lines
// lose it once used) and has not been worked MAX_TRIGGERS times lately.
static boolean LineWorks(int line)
{
    return K_Specials_MoverTrigger(K_Specials_LineSpecial(line)) != K_TRIGGER_NONE
        && LineUses(line) < MAX_TRIGGERS;
}

// Whether a line still works and moves its sectors one of the ways wanted
// (k_move_t flags).
static boolean LineHelps(int line, int want)
{
    return LineWorks(line) && (K_Specials_MoverMoves(K_Specials_LineSpecial(line)) & want);
}

static goal_t *Push(goal_type_t type, int param)
{
    goal_t *goal = &goals[depth++];

    memset(goal, 0, sizeof(*goal));
    goal->type = type;
    goal->param = param;
    goal->chosen.line = -1;
    goal->portal = -1;
    goal->from = -1;
    goal->pinch_cell = -1;
    return goal;
}

// The goal below the top is planned again: the world has changed. A click
// that ends on an empty list lets GO TO EXIT start over, failures
// forgotten: the player may stand somewhere that changes things.
static void Pop(void)
{
    --depth;

    if (depth > 0)
    {
        goals[depth - 1].planned = false;
    }
    else if (goals[0].type == GOAL_GOTO)
    {
        exit_wanted = K_AutoplayEnabled();
        num_failed = 0;
    }

    K_Nav_Clear();
}

static void DoneGoal(const char *how)
{
    const goal_t *goal = &goals[depth - 1];
    char name[64];

    GoalName(goal, name, sizeof(name));
    K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s done - %s", name, how);

    // Something reached in the sweep before the exit (EXPLORE_STALL).
    if (goal->type == GOAL_ITEM || goal->type == GOAL_PLACE)
    {
        sweep_found = leveltime;
    }

    // A fix on the way to the exit: progress toward it (EXIT_STALL).
    if (goals[0].type == GOAL_EXIT
     && (goal->type == GOAL_KEY || goal->type == GOAL_TRIGGER || goal->type == GOAL_BARREL
         || (goal->type == GOAL_KILL && !goal->chase)))
    {
        exit_stall = 0;
    }

    if (depth == 1)
    {
        M_snprintf(outcome, sizeof(outcome), "%s done - %s", name, how);
    }

    Pop();
}

// A place set aside for PLACE_SKIP; dropped for the level after PLACE_TRIES.
static void SetPlaceAside(int p)
{
    ++places[p].tries;
    places[p].skip = leveltime + PLACE_SKIP;
}

// Not pushed again for a while: FAIL_TICS, doubled each time it fails
// again, up to MAX_FAIL_TICS. Never GO TO EXIT (PlanGoal waits instead). A
// place is set aside instead (PLACE_SKIP).
static void FailGoal(const char *why)
{
    goal_t *goal = &goals[depth - 1];
    failed_t *entry = FailedEntry(goal->type, goal->param);
    char name[64];
    int tics, i;

    if (goal->type == GOAL_PLACE)
    {
        SetPlaceAside(goal->param);
        GoalName(goal, name, sizeof(name));
        K_Log(K_LOG_BRAIN, K_LOG_WARN, "%s failed - %s; %s", name, why,
              places[goal->param].tries < PLACE_TRIES ? "set aside for a while"
                                                      : "given up for the level");
        Pop();
        return;
    }

    // A new entry; when the list is full, in place of the one due first.
    if (entry == NULL && num_failed < MAX_FAILED)
    {
        entry = &failed[num_failed++];
        entry->count = 0;
    }
    else if (entry == NULL)
    {
        for (entry = &failed[0], i = 1; i < num_failed; ++i)
        {
            if (failed[i].until < entry->until)
            {
                entry = &failed[i];
            }
        }

        entry->count = 0;
    }

    entry->type = goal->type;
    entry->param = goal->param;
    tics = FAIL_TICS << SDL_min(entry->count, 3);
    tics = SDL_min(tics, MAX_FAIL_TICS);
    entry->since = leveltime;
    entry->until = leveltime + tics;
    ++entry->count;

    GoalName(goal, name, sizeof(name));
    K_Log(K_LOG_BRAIN, K_LOG_WARN, "%s failed - %s; trying again in %d s", name, why,
          tics / TICRATE);
    M_snprintf(entry->text, sizeof(entry->text), "%s - %s", name, why);

    if (depth == 1)
    {
        M_snprintf(outcome, sizeof(outcome), "%s failed - %s", name, why);
    }

    Pop();
}

// A candidate tried at the point, -1 if none.
static int TriedIndex(const goal_t *goal, k_vec2_t at)
{
    int i;

    for (i = 0; i < goal->num_tried; ++i)
    {
        if (fabs(goal->tried[i].x - at.x) < 1.0 && fabs(goal->tried[i].y - at.y) < 1.0)
        {
            return i;
        }
    }

    return -1;
}

// The chosen candidate did not work out: plan again without it, for
// TRIED_TICS.
static void CandidateFailed(goal_t *goal, const char *why)
{
    char name[64];
    int i = TriedIndex(goal, goal->chosen.at);

    GoalName(goal, name, sizeof(name));
    K_Log(K_LOG_BRAIN, K_LOG_WARN, "%s: the %s did not work out - %s", name,
          goal->chosen.what, why);

    // Its own entry again, else a new one, else the one due first.
    if (i < 0 && goal->num_tried < MAX_TRIED)
    {
        i = goal->num_tried++;
    }
    else if (i < 0)
    {
        int k;

        for (i = 0, k = 1; k < goal->num_tried; ++k)
        {
            if (goal->tried_until[k] < goal->tried_until[i])
            {
                i = k;
            }
        }
    }

    goal->tried[i] = goal->chosen.at;
    goal->tried_until[i] = leveltime + TRIED_TICS;
    goal->planned = false;
    K_Nav_Clear();
}

//
// Candidates
//

// Tried lately and did not work out.
static boolean WasTried(const goal_t *goal, k_vec2_t at)
{
    int i = TriedIndex(goal, at);

    return i >= 0 && leveltime < goal->tried_until[i];
}

// The floor a block stands on.
static double BlockFloor(int block)
{
    const k_block_t *b = &K_Map_Get()->blocks[block];
    int cell = K_Map_CellAt(b->at.x, b->at.y);

    return cell >= 0 ? K_Map_CellFloor(cell) : 0.0;
}

static double AxisDistance(k_vec2_t a, k_vec2_t b)
{
    return SDL_max(fabs(b.x - a.x), fabs(b.y - a.y));
}

// The standing barrels that go off with the barrel (BARREL_CHAIN), itself
// first. Returns how many.
static int BarrelChain(int block, int *chain)
{
    const k_map_t *map = K_Map_Get();
    int count = 1;
    int i, b, k;

    chain[0] = block;

    for (i = 0; i < count; ++i)
    {
        const k_block_t *bomb = &map->blocks[chain[i]];
        double bomb_floor = BlockFloor(chain[i]);

        for (b = 0; b < map->num_blocks && count < MAX_CHAIN; ++b)
        {
            const k_block_t *other = &map->blocks[b];

            for (k = 0; k < count && chain[k] != b; ++k)
            {
            }

            // P_RadiusAttack: the barrel hit must see the one going off.
            if (k < count || !other->barrel || !K_Map_BlockStands(b)
             || AxisDistance(bomb->at, other->at) >= BARREL_CHAIN
             || !K_Sight_Check(other->at.x, other->at.y,
                               BlockFloor(b) + BARREL_HEIGHT * 0.75, bomb->at.x, bomb->at.y,
                               bomb_floor, bomb_floor + BLAST_HEIGHT))
            {
                continue;
            }

            chain[count++] = b;
        }
    }

    return count;
}

// Spots to set off a barrel in the way from (BARREL_RINGS): round any
// barrel of its chain, on floor the player can stand on, that barrel in
// reach of the shots, every barrel of the chain BARREL_SAFE off or out of
// sight. Also one that sees the barrel only through a shut door, as it
// would once the door is open, the door opened first (PlanGoal; Doom II
// MAP23: the barrel at (1184,-320) narrows the only ways to the spots
// beyond it, and the only others, west of door 31, see the chain through
// that door; they were got to only because the door was taken for one that
// narrowed a way, 110 units off). Returns how many.
static int BarrelSpots(int block, candidate_t *out, int max)
{
    const k_map_t *map = K_Map_Get();
    int chain[MAX_CHAIN];
    int length = BarrelChain(block, chain);
    int count = 0;
    int m, r, k, c;

    for (m = 0; m < length; ++m)
    {
        const k_block_t *barrel = &map->blocks[chain[m]];
        double bottom = BlockFloor(chain[m]);

        for (r = 0; r < arrlen(barrel_rings); ++r)
        {
            for (k = 0; k < BARREL_WAYS && count < max; ++k)
            {
                double angle = k * 2.0 * 3.14159265358979323846 / BARREL_WAYS;
                k_vec2_t spot = { barrel->at.x + cos(angle) * barrel_rings[r],
                                  barrel->at.y + sin(angle) * barrel_rings[r] };
                int cell = K_Map_CellAt(spot.x, spot.y);
                boolean safe = true;
                int door = -1;
                double floor;

                if (cell < 0 || K_Graph_Clearance(spot, 17.0) < 17.0)
                {
                    continue;
                }

                floor = K_Map_CellFloor(cell);

                if (!K_Sight_Aim(spot.x, spot.y, floor + GUN_HEIGHT, barrel->at.x, barrel->at.y,
                                 bottom, bottom + BARREL_HEIGHT))
                {
                    door = K_Sight_ShutDoor();

                    if (door < 0 || !K_Graph_IsDoor(door))
                    {
                        continue;
                    }

                    K_Sight_AssumeOpen(door, K_Graph_DoorTop(door));
                    safe = K_Sight_Aim(spot.x, spot.y, floor + GUN_HEIGHT, barrel->at.x,
                                       barrel->at.y, bottom, bottom + BARREL_HEIGHT);
                }

                for (c = 0; c < length && safe; ++c)
                {
                    const k_block_t *other = &map->blocks[chain[c]];
                    double other_floor = BlockFloor(chain[c]);

                    safe = AxisDistance(spot, other->at) >= BARREL_SAFE
                        || !K_Sight_Check(spot.x, spot.y, floor + EYE_HEIGHT, other->at.x,
                                          other->at.y, other_floor, other_floor + BLAST_HEIGHT);
                }

                K_Sight_AssumeOpen(-1, 0.0);

                if (!safe)
                {
                    continue;
                }

                out[count].at = spot;
                out[count].kind = K_GOAL_POINT;
                out[count].line = -1;
                out[count].aim = barrel->at;
                out[count].door = door;

                if (m == 0)
                {
                    M_snprintf(out[count].what, sizeof(out[count].what),
                               "spot %.0f units from it", barrel_rings[r]);
                }
                else
                {
                    M_snprintf(out[count].what, sizeof(out[count].what),
                               "spot %.0f units from the barrel at (%.0f,%.0f) of its chain",
                               barrel_rings[r], barrel->at.x, barrel->at.y);
                }

                ++count;
            }
        }
    }

    return count;
}

// Whether the KILL goal is planned to a firing spot (FIRE_RINGS): a
// Cyberdemon or a spider mastermind the level needs dead, not chased, none
// of them hopeless, a spot to get to.
static boolean FiresFrom(const goal_t *goal)
{
    return goal->type == GOAL_KILL && !goal->chase && !goal->near
        && (goal->param == MT_CYBORG || goal->param == MT_SPIDER)
        && !K_Combat_Hopeless(goal->param);
}

// Whether a KILL goal's candidate is a firing spot, not the monster itself.
static boolean IsSpot(const candidate_t *candidate)
{
    return hypot(candidate->at.x - candidate->aim.x, candidate->at.y - candidate->aim.y) > 1.0;
}

// Whether the KILL goal went for a firing spot.
static boolean SpotChosen(const goal_t *goal)
{
    return IsSpot(&goal->chosen);
}

// Whether a firing spot on the floor at `floor` has cover beside it from an
// eye at `eye` over `boss` (FIRE_COVER).
static boolean SpotCovered(k_vec2_t spot, double floor, k_vec2_t boss, double eye)
{
    int k, s;

    for (k = 0; k < FIRE_COVER_WAYS; ++k)
    {
        double angle = k * 2.0 * 3.14159265358979323846 / FIRE_COVER_WAYS;

        for (s = 1; s * FIRE_COVER_STEP <= FIRE_COVER; ++s)
        {
            k_vec2_t at = { spot.x + cos(angle) * FIRE_COVER_STEP * s,
                            spot.y + sin(angle) * FIRE_COVER_STEP * s };
            int cell = K_Map_CellAt(at.x, at.y);
            double at_floor;

            if (cell < 0)
            {
                break;
            }

            at_floor = K_Map_CellFloor(cell);

            if (fabs(at_floor - floor) > STEP_UP
             || !K_Sight_Check(spot.x, spot.y, floor + PLAYER_HEIGHT * 0.5, at.x, at.y,
                               at_floor + PLAYER_HEIGHT * 0.25, at_floor + PLAYER_HEIGHT * 0.75))
            {
                break;
            }

            if (!K_Sight_Check(boss.x, boss.y, eye, at.x, at.y, at_floor, at_floor + PLAYER_HEIGHT))
            {
                return true;
            }
        }
    }

    return false;
}

// Spots to fight the bosses of the kind from (FIRE_RINGS), the nearest to
// the player first - those with cover beside them (FIRE_COVER) only, if
// any. Returns how many.
static int FireSpots(int kind, candidate_t *out, int max)
{
    const k_map_t *map = K_Map_Get();
    k_vec2_t at[MAX_CANDIDATES];
    double height = (double) mobjinfo[kind].height / FRACUNIT;
    double away[MAX_CANDIDATES];
    boolean covered[MAX_CANDIDATES];
    k_pose_t player;
    int bosses = SDL_min(K_Specials_FindMonsters(kind, at, MAX_CANDIDATES), MAX_CANDIDATES);
    int count = 0, kept = 0;
    int m, r, k, i;

    max = SDL_min(max, MAX_CANDIDATES);

    if (!K_Map_GetPlayer(&player))
    {
        return 0;
    }

    for (m = 0; m < bosses; ++m)
    {
        int boss_cell = K_Map_CellAt(at[m].x, at[m].y);
        double bottom = boss_cell >= 0 ? K_Map_CellFloor(boss_cell) : 0.0;

        for (r = 0; r < arrlen(fire_rings); ++r)
        {
            for (k = 0; k < FIRE_WAYS; ++k)
            {
                double angle = k * 2.0 * 3.14159265358979323846 / FIRE_WAYS;
                k_vec2_t spot = { at[m].x + cos(angle) * fire_rings[r],
                                  at[m].y + sin(angle) * fire_rings[r] };
                int cell = K_Map_CellAt(spot.x, spot.y);
                double floor, distance;
                boolean cover;
                int worst;

                if (cell < 0 || K_Graph_Clearance(spot, 17.0) < 17.0
                 || K_Map_SuitFloorDamage(map->subsectors[map->cells[cell].subsector].sector,
                                          false) > 0.0)
                {
                    continue;
                }

                floor = K_Map_CellFloor(cell);

                if (boss_cell < 0
                 || !K_Sight_Aim(spot.x, spot.y, floor + GUN_HEIGHT, at[m].x, at[m].y, bottom,
                                 bottom + height))
                {
                    continue;
                }

                // Kept the FIRE_MOST nearest the player, those without cover
                // FIRE_BARE further off.
                distance = hypot(spot.x - player.x, spot.y - player.y);
                worst = -1;

                if (count >= FIRE_MOST || count >= max)
                {
                    for (worst = 0, i = 1; i < count; ++i)
                    {
                        if (away[i] > away[worst])
                        {
                            worst = i;
                        }
                    }

                    if (distance >= away[worst])
                    {
                        continue;
                    }
                }

                cover = SpotCovered(spot, floor, at[m], bottom + height * 0.75);
                distance += cover ? 0.0 : FIRE_BARE;

                if (worst < 0)
                {
                    worst = count++;
                }
                else if (distance >= away[worst])
                {
                    continue;
                }

                away[worst] = distance;
                covered[worst] = cover;
                out[worst].at = spot;
                out[worst].kind = K_GOAL_POINT;
                out[worst].line = -1;
                out[worst].aim = at[m];
                M_snprintf(out[worst].what, sizeof(out[worst].what),
                           "spot (%.0f,%.0f), %.0f from the %s at (%.0f,%.0f)", spot.x, spot.y,
                           fire_rings[r], K_Combat_KindName(kind), at[m].x, at[m].y);
            }
        }
    }

    // With cover beside any, those without are dropped.
    for (i = 0; i < count; ++i)
    {
        if (covered[i])
        {
            out[kept++] = out[i];
        }
    }

    return kept > 0 ? kept : count;
}

// Where to go to pick up a key lying at `at` (K_Graph_PickupSpot); with no
// spot to stand at, the key itself: the route finds the way or not. A key
// on a ledge is also a candidate from the floors around it that reach it,
// MAX_KEY_FLOORS at the most (K_Graph_PickupSpotsOff): the route picks.
#define MAX_KEY_FLOORS 4

static k_vec2_t KeySpot(k_vec2_t at)
{
    k_vec2_t spot;

    return K_Graph_PickupSpot(at, &spot) ? spot : at;
}

static int LineCandidate(int line, k_trigger_t trigger, const char *what,
                         candidate_t *candidate)
{
    candidate->kind = trigger == K_TRIGGER_USE ? K_GOAL_USE_LINE
                    : trigger == K_TRIGGER_SHOOT ? K_GOAL_SHOOT_LINE : K_GOAL_CROSS_LINE;
    candidate->line = line;
    M_snprintf(candidate->what, sizeof(candidate->what), "%s at line %d", what, line);
    return K_Nav_LineGoalPoint(line, candidate->kind, &candidate->at.x,
                               &candidate->at.y);
}

// Which side of the line through a and b the point p is on.
static double Side(k_vec2_t a, k_vec2_t b, k_vec2_t p)
{
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

// Whether segments ab and cd cross.
static boolean Crosses(k_vec2_t a, k_vec2_t b, k_vec2_t c, k_vec2_t d)
{
    return (Side(c, d, a) > 0.0) != (Side(c, d, b) > 0.0)
        && (Side(a, b, c) > 0.0) != (Side(a, b, d) > 0.0);
}

// The key that a door between a switch and the spot it is pressed from
// needs, if the player lacks it, else -1; door gets the door's line. Use
// stops at the first line with a special on its way (PTR_UseTraverse), so
// a locked door in front of a switch takes the press (Doom II MAP27: switch
// 727 at the back of a closet behind the blue door). A one-shot door loses
// its special once opened, and then the press gets through.
static int DoorKeyInTheWay(const candidate_t *candidate, int *door)
{
    const k_map_t *map = K_Map_Get();
    const k_line_t *target = &map->lines[candidate->line];
    k_vec2_t middle = { (target->v1.x + target->v2.x) * 0.5,
                        (target->v1.y + target->v2.y) * 0.5 };
    int i;

    for (i = 0; i < map->num_lines; ++i)
    {
        int key;

        if (i == candidate->line
         || !Crosses(candidate->at, middle, map->lines[i].v1, map->lines[i].v2))
        {
            continue;
        }

        key = K_Specials_LockKey(K_Specials_LineSpecial(i));

        if (key >= 0 && !K_Specials_HasKey(key))
        {
            *door = i;
            return key;
        }
    }

    return -1;
}

static int Candidates(const goal_t *goal, candidate_t *out, int max)
{
    const k_map_t *map = K_Map_Get();
    int count = 0;
    int i;

    switch (goal->type)
    {
        // Of the kind wanted, if one is (EXIT_IDLE_MOST).
        case GOAL_EXIT:
            for (i = 0; i < map->num_lines && count < max; ++i)
            {
                int secret;
                k_trigger_t trigger = K_Specials_ExitTrigger(map->lines[i].special, &secret);

                if (trigger != K_TRIGGER_NONE && (exit_kind < 0 || (secret != 0) == exit_kind)
                 && LineCandidate(i, trigger, trigger == K_TRIGGER_USE
                                  ? (secret ? "secret exit switch" : "exit switch")
                                  : (secret ? "secret exit line" : "exit line"),
                                  &out[count]))
                {
                    ++count;
                }
            }

            // A floor that ends the level (E1M8): any of its cells, the
            // route picks one it reaches (E1M8's is a box teleported into,
            // the floor round it void).
            for (i = 0; exit_kind != 1 && i < map->num_cells && count < max; ++i)
            {
                const k_cell_t *cell = &map->cells[i];
                int sector = map->subsectors[cell->subsector].sector;

                if (K_Specials_ExitSector(sector)
                 && K_Graph_Clearance(cell->centroid, PLAYER_RADIUS + 1.0) > PLAYER_RADIUS)
                {
                    out[count].at = cell->centroid;
                    out[count].kind = K_GOAL_POINT;
                    out[count].line = -1;
                    M_snprintf(out[count].what, sizeof(out[count].what),
                               "exit floor in sector %d at (%.0f,%.0f)", sector,
                               cell->centroid.x, cell->centroid.y);
                    ++count;
                }
            }
            break;

        case GOAL_KEY:
            {
                k_keything_t keys[MAX_CANDIDATES];
                int found = SDL_min(K_Specials_FindKeys(goal->param, keys, max), max);

                // From where it lies, or from a floor below (KeySpot).
                for (i = 0; i < found && count < max; ++i)
                {
                    k_vec2_t spots[1 + MAX_KEY_FLOORS];
                    int num_spots, k;

                    spots[0] = KeySpot(keys[i].at);
                    num_spots = 1 + K_Graph_PickupSpotsOff(keys[i].at, spots + 1,
                                                           MAX_KEY_FLOORS);

                    for (k = 0; k < num_spots && count < max; ++k)
                    {
                        out[count].at = spots[k];
                        out[count].kind = K_GOAL_POINT;
                        out[count].line = -1;
                        M_snprintf(out[count].what, sizeof(out[count].what), "%s %s at "
                                   "(%.0f,%.0f)", K_Specials_KeyName(goal->param),
                                   keys[i].skull ? "skull key" : "keycard",
                                   keys[i].at.x, keys[i].at.y);
                        ++count;
                    }
                }
            }
            break;

        case GOAL_ITEM:
            {
                const k_item_t *item = K_Items_Get(goal->param);

                // From where it lies, or from a floor around (k_items.h: off).
                for (i = -1; item->present && item->reachable && i < item->num_off
                             && count < max; ++i)
                {
                    out[count].at = i < 0 ? item->spot : item->off[i];
                    out[count].kind = K_GOAL_POINT;
                    out[count].line = -1;
                    M_snprintf(out[count].what, sizeof(out[count].what), "%s at (%.0f,%.0f)",
                               item->name, item->at.x, item->at.y);
                    ++count;
                }
            }
            break;

        case GOAL_BARREL:
            count = BarrelSpots(goal->param, out, max);
            break;

        case GOAL_TELEFRAG:
            {
                const k_teleport_t *teleport = &K_Graph_Get()->teleports[goal->param];

                out[0].at = teleport->to;
                out[0].kind = K_GOAL_POINT;
                out[0].line = -1;
                M_snprintf(out[0].what, sizeof(out[0].what), "%s on the landing spot of the "
                           "teleporter (tag %d) at (%.0f,%.0f)", K_Combat_KindName(goal->victim),
                           teleport->tag, teleport->to.x, teleport->to.y);
                count = 1;
            }
            break;

        // Where each one is now: once in sight, combat fights it. A boss
        // from a firing spot (FIRE_RINGS), if there is any.
        case GOAL_KILL:
            {
                k_vec2_t at[MAX_CANDIDATES];

                if (FiresFrom(goal) && (count = FireSpots(goal->param, out, max)) > 0)
                {
                    break;
                }

                count = SDL_min(K_Specials_FindMonsters(goal->param, at, max), max);

                for (i = 0; i < count; ++i)
                {
                    out[i].at = at[i];
                    out[i].aim = at[i];
                    out[i].kind = K_GOAL_POINT;
                    out[i].line = -1;
                    M_snprintf(out[i].what, sizeof(out[i].what), "%s at (%.0f,%.0f)",
                               K_Combat_KindName(goal->param), at[i].x, at[i].y);
                }
            }
            break;

        // A gun switch from further off too (K_Nav_ShotSpot): the route
        // picks.
        case GOAL_TRIGGER:
            for (i = 0; i < K_Specials_TriggerCount(goal->param) && count < max; ++i)
            {
                int line = K_Specials_TriggerLine(goal->param, i);
                k_trigger_t trigger = K_Specials_MoverTrigger(K_Specials_LineSpecial(line));
                const char *what = K_Specials_ManualDoor(K_Specials_LineSpecial(line)) ? "door"
                                 : trigger == K_TRIGGER_USE ? "switch"
                                 : trigger == K_TRIGGER_SHOOT ? "gun switch"
                                 : "walk-over line";

                if (!LineHelps(line, goal->want))
                {
                    continue;
                }

                if (LineCandidate(line, trigger, what, &out[count]))
                {
                    ++count;
                }

                if (trigger == K_TRIGGER_SHOOT && count < max
                 && K_Nav_ShotSpot(line, &out[count].at.x, &out[count].at.y))
                {
                    out[count].kind = K_GOAL_SHOOT_LINE;
                    out[count].line = line;
                    M_snprintf(out[count].what, sizeof(out[count].what), "%s at line %d",
                               what, line);
                    ++count;
                }
            }
            break;

        case GOAL_PLACE:
            out[0].at = places[goal->param].spot;
            out[0].kind = K_GOAL_POINT;
            out[0].line = -1;
            M_snprintf(out[0].what, sizeof(out[0].what), "%s %d at (%.0f,%.0f)",
                       places[goal->param].sector >= 0 ? "secret sector" : "area",
                       places[goal->param].sector >= 0 ? places[goal->param].sector
                                                       : places[goal->param].area,
                       out[0].at.x, out[0].at.y);
            count = 1;
            break;

        default:
            out[0].at = goal->point;
            out[0].kind = K_GOAL_POINT;
            out[0].line = -1;
            M_snprintf(out[0].what, sizeof(out[0].what), "point (%.0f,%.0f)",
                       goal->point.x, goal->point.y);
            count = 1;
            break;
    }

    // Only a barrel's spots wait on a door (BarrelSpots).
    for (i = 0; goal->type != GOAL_BARREL && i < count; ++i)
    {
        out[i].door = -1;
    }

    return count;
}

//
// Getting past blocked crossings
//

// What opens a way with too little headroom, or too narrow: a floor going
// down, a ceiling going up.
#define OPENING_MOVES (K_MOVE_FLOOR_DOWN | K_MOVE_CEILING_UP)

// Whether some line can still move the sector one of the ways wanted
// (LineHelps).
static boolean LiveTrigger(int sector, int want)
{
    int i;

    for (i = 0; i < K_Specials_TriggerCount(sector); ++i)
    {
        if (LineHelps(K_Specials_TriggerLine(sector, i), want))
        {
            return true;
        }
    }

    return false;
}

// How far the player goes to be able to use a line: nowhere if it needs no
// key or the player has it, else to the nearest key and on to the line
// (as the crow flies); -1 if there is no key to get.
static double KeyTrip(int line)
{
    const k_line_t *l = &K_Map_Get()->lines[line];
    k_vec2_t middle = { (l->v1.x + l->v2.x) * 0.5, (l->v1.y + l->v2.y) * 0.5 };
    int key = K_Specials_LockKey(K_Specials_LineSpecial(line));
    double best = -1.0;
    int i;

    if (key < 0 || K_Specials_HasKey(key))
    {
        return 0.0;
    }

    if (keys_lying[key] <= 0 || !Allowed(GOAL_KEY, key))
    {
        return -1.0;
    }

    for (i = 0; i < SDL_min(keys_lying[key], MAX_CANDIDATES); ++i)
    {
        k_vec2_t at = keys_at[key][i];
        double trip = hypot(at.x - planned_from.x, at.y - planned_from.y)
                    + hypot(middle.x - at.x, middle.y - at.y);

        if (best < 0.0 || trip < best)
        {
            best = trip;
        }
    }

    return best;
}

// What opening a sector that narrows a gap takes: nothing if a line still
// moves it out of the way (bars that lower), or its door opens from a face
// with no key or one in hand; else the trip to a key and on to the door
// (KeyTrip); -1 if nothing opens it.
static double OpeningCost(int sector)
{
    double best = -1.0;
    int i;

    for (i = 0; i < K_Specials_TriggerCount(sector); ++i)
    {
        int line = K_Specials_TriggerLine(sector, i);
        double cost;

        if (LineHelps(line, OPENING_MOVES))
        {
            return 0.0;
        }

        if (!LineHelps(line, K_MOVE_DOOR))
        {
            continue;
        }

        cost = KeyTrip(line);

        if (cost >= 0.0 && (best < 0.0 || cost < best))
        {
            best = cost;
        }
    }

    return best;
}

// Of the sectors narrowing a gap, the one the player can open soonest
// (OpeningCost), as an index; -1 if none can be.
static int OpenSoonest(const int *narrowing, int count)
{
    int best = -1;
    double best_cost = 0.0;
    int i;

    for (i = 0; i < count; ++i)
    {
        double cost = OpeningCost(narrowing[i]);

        if (cost >= 0.0 && Allowed(GOAL_TRIGGER, narrowing[i])
         && (best < 0 || cost < best_cost))
        {
            best = i;
            best_cost = cost;
        }
    }

    return best;
}

// The kind of boss whose death moves the sector one of the ways wanted
// (k_specials.h: A_BossDeath; E1M8: the barons lower the star-shaped wall
// before the teleporter to the exit floor), with some still alive and the
// goal allowed; -1 if none. With tag 0 (sector -1): one whose death ends
// the level (E2M8, E3M8).
static int BossFor(int sector, int want)
{
    k_boss_t bosses[4];
    int count = K_Specials_Bosses(bosses, arrlen(bosses));
    int i;

    for (i = 0; i < count; ++i)
    {
        if ((sector < 0 ? bosses[i].tag == 0
                        : bosses[i].tag != 0 && bosses[i].tag == sectors[sector].tag
                          && (bosses[i].moves & want))
         && K_Specials_FindMonsters(bosses[i].type, NULL, 0) > 0
         && Allowed(GOAL_KILL, bosses[i].type))
        {
            return bosses[i].type;
        }
    }

    return -1;
}

// Of the sectors narrowing a gap, the kind of boss whose death opens one
// (BossFor), for when no line can be worked for any (OpenSoonest); -1 if
// none. E4M6: the bars before the exit open as the Cyberdemon dies, or by
// switch 1331 - in a closet whose door opens from inside only; every
// switch tried and failed, the bot stood there for minutes.
static int BossOpens(const int *narrowing, int count)
{
    int i, boss;

    for (i = 0; i < count; ++i)
    {
        if ((boss = BossFor(narrowing[i], OPENING_MOVES | K_MOVE_DOOR)) >= 0)
        {
            return boss;
        }
    }

    return -1;
}

// What would get the player through a gap inside a cell: as for a gap too
// narrow at a crossing, opening a sector that narrows it (Doom II MAP11: a
// fence of pillars 16 apart, the gaps between them 24 long, behind the red
// door; walk-over lines open or lower every other pillar for a while), or
// killing the boss whose death opens one.
static boolean FixForPinch(int cell, k_vec2_t at, goal_type_t *type, int *param, int *want)
{
    int narrowing[8];
    int count = K_Graph_PinchSectors(cell, at, narrowing, arrlen(narrowing));
    int best = OpenSoonest(narrowing, count);
    int barrel = K_Graph_PinchBarrel(cell, at);
    int boss;

    // A barrel narrowing it: shot.
    if (barrel >= 0 && Allowed(GOAL_BARREL, barrel))
    {
        *type = GOAL_BARREL;
        *param = barrel;
        *want = 0;
        return true;
    }

    if (best < 0 && (boss = BossOpens(narrowing, count)) >= 0)
    {
        *type = GOAL_KILL;
        *param = boss;
        *want = 0;
        return true;
    }

    if (best < 0)
    {
        return false;
    }

    *type = GOAL_TRIGGER;
    *param = narrowing[best];
    *want = OPENING_MOVES | K_MOVE_DOOR;
    return true;
}

// What would get the player past a blocked crossing, if anything: the key
// for a locked door; for a step too high, a line that lowers the floor
// beyond or raises the one the player is on, never one that lowers it (Doom
// II MAP27: lowering the arm it stood on, to climb the pillar beside it,
// burned a dozen walk-over lines); for too little headroom, a line that
// lowers a floor or raises a ceiling on either side; for a gap too narrow,
// one that does so to a sector narrowing it (bars that lower), or a door
// narrowing it, opened from its face, the one the player can open soonest
// (OpeningCost); or else a line that lowers a floor or raises a ceiling on
// either side (a lift the player rides down to where the way on has
// headroom, Doom II MAP05). One sector at a time: if the gap is still too
// narrow once it moved, the next one is pushed (Doom II MAP19: three key
// bars, two of which must be open). For a jump that does not work now, a
// line that raises its takeoff floor back, or lowers its landing. Where no
// line does it, the map's bosses whose death does (BossFor). want gets the
// moves that would help.
static boolean FixFor(int portal, int from, k_crossing_t crossing, goal_type_t *type,
                      int *param, int *want)
{
    const k_map_t *map = K_Map_Get();
    const k_portal_t *p = &K_Graph_Get()->portals[portal];
    int to = p->cell[0] == from ? p->cell[1] : p->cell[0];
    int from_sector = map->subsectors[map->cells[from].subsector].sector;
    int to_sector = map->subsectors[map->cells[to].subsector].sector;
    int sectors_[8];
    int wants[8];
    int count = 2;
    int barrel, i;

    *want = 0;

    // A barrel standing in the way, or narrowing it: shot (Doom II MAP23).
    if ((crossing == K_CROSS_THING || crossing == K_CROSS_NARROW)
     && (barrel = K_Graph_BarrelInTheWay(portal, from)) >= 0 && Allowed(GOAL_BARREL, barrel))
    {
        *type = GOAL_BARREL;
        *param = barrel;
        return true;
    }

    if (crossing == K_CROSS_LOCKED)
    {
        int key = K_Graph_LockKey(portal);

        if (key >= 0 && keys_lying[key] > 0 && Allowed(GOAL_KEY, key))
        {
            *type = GOAL_KEY;
            *param = key;
            return true;
        }

        return false;
    }

    if (crossing == K_CROSS_NARROW)
    {
        int narrowing = K_Graph_NarrowSectors(portal, from, sectors_, arrlen(sectors_));
        int best = OpenSoonest(sectors_, narrowing);
        int boss;

        if (best >= 0)
        {
            *type = GOAL_TRIGGER;
            *param = sectors_[best];
            *want = OPENING_MOVES | K_MOVE_DOOR;
            return true;
        }

        if ((boss = BossOpens(sectors_, narrowing)) >= 0)
        {
            *type = GOAL_KILL;
            *param = boss;
            return true;
        }

        // Narrowed by a floor too high to step onto beside it: the floor
        // beyond raised to within a step of it opens it too (E2M8: three
        // strips 16 deep up to an alcove 32 high, the last too shallow to
        // stand in while it was down; the stairs a pillar face builds raise
        // them 8, 16 and 24, and the way to the alcove's door line was never
        // looked for).
        wants[1] = OPENING_MOVES;

        for (i = 0; i < narrowing; ++i)
        {
            double to_floor, to_ceiling, floor, ceiling;

            K_Map_SectorHeights(to_sector, &to_floor, &to_ceiling);
            K_Map_SectorHeights(sectors_[i], &floor, &ceiling);

            if (floor - to_floor > STEP_UP)
            {
                wants[1] |= K_MOVE_FLOOR_UP;
            }
        }

        sectors_[0] = from_sector;
        wants[0] = OPENING_MOVES;
        sectors_[1] = to_sector;
    }
    else if (crossing == K_CROSS_STEP)
    {
        sectors_[0] = to_sector;
        wants[0] = K_MOVE_FLOOR_DOWN;
        sectors_[1] = from_sector;
        wants[1] = K_MOVE_FLOOR_UP;
    }
    else if (crossing == K_CROSS_HEADROOM)
    {
        sectors_[0] = to_sector;
        wants[0] = OPENING_MOVES;
        sectors_[1] = from_sector;
        wants[1] = OPENING_MOVES;
    }
    else if (crossing == K_CROSS_GAP && p->jump >= 0)
    {
        // A jump found as if a sector had moved: move it there (Doom II
        // MAP09: switch 126 raises the floor a jump lands on, switch 157
        // opens the door another starts from). One found with a door open
        // that is closed: open it (MAP09: switch 455 opens the door at the
        // top of the stairs). A jump that worked when it was found: its
        // takeoff floor has gone down since, or its landing up (Doom II
        // MAP08: walking over line 599 lowers the floor the jump back over
        // the slime starts from, and switch 572 raises it again).
        const k_jump_t *jump = K_Jump_Get(p->jump);
        double floor, ceiling;

        count = 0;

        if (jump->as_if_sector >= 0)
        {
            K_Map_SectorHeights(jump->as_if_sector, &floor, &ceiling);

            if (floor < jump->as_if_floor - 0.5)
            {
                sectors_[count] = jump->as_if_sector;
                wants[count++] = K_MOVE_FLOOR_UP;
            }
            else if (floor > jump->as_if_floor + 0.5)
            {
                sectors_[count] = jump->as_if_sector;
                wants[count++] = K_MOVE_FLOOR_DOWN;
            }

            if (ceiling < jump->as_if_ceiling - 0.5)
            {
                sectors_[count] = jump->as_if_sector;
                wants[count++] = K_MOVE_CEILING_UP;
            }
        }

        if (jump->door >= 0)
        {
            K_Map_SectorHeights(jump->door, &floor, &ceiling);

            if (ceiling - floor < PLAYER_HEIGHT)
            {
                sectors_[count] = jump->door;
                wants[count++] = K_MOVE_CEILING_UP;
            }
        }

        K_Map_SectorHeights(from_sector, &floor, &ceiling);

        if (floor < jump->from_floor - 0.5)
        {
            sectors_[count] = from_sector;
            wants[count++] = K_MOVE_FLOOR_UP;
        }

        K_Map_SectorHeights(jump->land_sector, &floor, &ceiling);

        if (floor > jump->land_floor + 0.5)
        {
            sectors_[count] = jump->land_sector;
            wants[count++] = K_MOVE_FLOOR_DOWN;
        }
    }
    else
    {
        return false;
    }

    for (i = 0; i < count; ++i)
    {
        if (sectors_[i] >= 0 && LiveTrigger(sectors_[i], wants[i])
         && Allowed(GOAL_TRIGGER, sectors_[i]))
        {
            *type = GOAL_TRIGGER;
            *param = sectors_[i];
            *want = wants[i];
            return true;
        }
    }

    for (i = 0; i < count; ++i)
    {
        int boss = sectors_[i] >= 0 ? BossFor(sectors_[i], wants[i]) : -1;

        if (boss >= 0)
        {
            *type = GOAL_KILL;
            *param = boss;
            return true;
        }
    }

    return false;
}

// Whether a route crosses a door that needs a key the player lacks.
static boolean LockedOnRoute(const k_path_t *path)
{
    int i;

    for (i = 0; i < path->num_cells - 1; ++i)
    {
        if (K_Graph_CanCross(path->portals[i], path->cells[i]) == K_CROSS_LOCKED)
        {
            return true;
        }
    }

    return false;
}

// Whether the player can get to work a trigger fix without a key it lacks:
// some line it can work, with a route there, open or blocked, through no
// door that needs a key it lacks (Doom II MAP14: the fix further on was a
// switch behind the red door the bot was fetching the key for).
static boolean FixWithoutKey(goal_type_t type, int param, int want)
{
    static candidate_t candidates[MAX_CANDIDATES];
    goal_t goal;
    k_pose_t player;
    k_vec2_t start;
    int count, i, door;

    if (type != GOAL_TRIGGER || !K_Map_GetPlayer(&player))
    {
        return false;
    }

    memset(&goal, 0, sizeof(goal));
    goal.type = type;
    goal.param = param;
    goal.want = want;
    count = Candidates(&goal, candidates, MAX_CANDIDATES);
    start.x = player.x;
    start.y = player.y;

    for (i = 0; i < count; ++i)
    {
        int key = K_Specials_LockKey(K_Specials_LineSpecial(candidates[i].line));

        if ((key >= 0 && !K_Specials_HasKey(key))
         || (candidates[i].kind == K_GOAL_USE_LINE && DoorKeyInTheWay(&candidates[i], &door) >= 0))
        {
            continue;
        }

        K_Path_Find(&probe, start, player.cell, candidates[i].at);

        if (probe.result == K_PATH_FOUND
         || (probe.result == K_PATH_BLOCKED && !LockedOnRoute(&probe)))
        {
            return true;
        }
    }

    return false;
}

// Whether what a trigger fix does lasts only a while: a lift comes back up,
// and a door that only lines closing it again open shuts (Doom II MAP04:
// door 32, open 4 s).
static boolean FixPasses(goal_type_t type, int sector, int want)
{
    boolean any = false;
    int i;

    if (type != GOAL_TRIGGER)
    {
        return false;
    }

    if (K_Graph_IsLift(sector))
    {
        return true;
    }

    for (i = 0; i < K_Specials_TriggerCount(sector); ++i)
    {
        int line = K_Specials_TriggerLine(sector, i);

        if (!LineHelps(line, want))
        {
            continue;
        }

        if (!K_Specials_DoorCloses(K_Specials_LineSpecial(line)))
        {
            return false;
        }

        any = true;
    }

    return any;
}

// The key a trigger fix needs, if every line that would do it needs one the
// player lacks and can get (Doom II MAP04: door 302, opened by switches for
// the blue key), else -1.
static int FixKey(goal_type_t type, int sector, int want)
{
    int key = -1;
    int i;

    if (type != GOAL_TRIGGER)
    {
        return -1;
    }

    for (i = 0; i < K_Specials_TriggerCount(sector); ++i)
    {
        int line = K_Specials_TriggerLine(sector, i);
        int needs = K_Specials_LockKey(K_Specials_LineSpecial(line));

        if (!LineHelps(line, want))
        {
            continue;
        }

        if (needs < 0 || K_Specials_HasKey(needs) || (key >= 0 && needs != key))
        {
            return -1;
        }

        key = needs;
    }

    return key >= 0 && keys_lying[key] > 0 && Allowed(GOAL_KEY, key) ? key : -1;
}

// The fix for a blocked route: for its first blocked crossing, or none.
// But a lift comes back up, and some doors close again: if that fix works
// one, and a crossing further on is fixed some other way, that one first,
// so the lift is not back up by the time the player returns to it (Doom II
// MAP09: lift 123 lowered, then door 130 opened again further on, and the
// lift worked a second time), nor the door shut (MAP04: door 32 opened, then
// floor 84 lowered back the way the bot came). And a key comes first,
// wherever its door is on the route, or a door opened only by switches
// that need it: keys stay, while what a switch does may not last until the
// player gets there (a lift goes back up while it fetches the key, Doom II
// MAP12). Sets portal and from to the crossing fixed, later if it is not the
// first one, and key_door to that door's portal, else -1.
static boolean RouteFix(const k_path_t *path, goal_type_t *type, int *param, int *want,
                        int *portal, int *from, boolean *later, int *key_door)
{
    goal_type_t key_type, later_type;
    int key_param, key_want, later_param, later_want;
    int i;

    *key_door = -1;
    *portal = path->blocked_portal;
    *from = path->blocked_from;
    *later = false;

    // A gap inside a cell is no crossing's: no portal, but the cell (and the
    // path's blocked_at).
    if (path->blocked_pinch)
    {
        *portal = -1;

        if (!FixForPinch(path->blocked_from, path->blocked_at, type, param, want))
        {
            return false;
        }
    }
    else if (!FixFor(path->blocked_portal, path->blocked_from, path->blocked_by, type, param,
                     want))
    {
        return false;
    }

    // A crossing further on that nothing fixes now gets the fixes before it
    // nowhere (Doom II MAP14: the switch for a sector narrowing the way was
    // behind the red door, whose key the bot was out fetching; it opened
    // the doors in front, closing again, over and over).
    for (i = 0; i < path->num_cells - 1; ++i)
    {
        k_crossing_t crossing = K_Graph_CanCross(path->portals[i], path->cells[i]);

        if (!K_Graph_Passable(crossing)
         && !FixFor(path->portals[i], path->cells[i], crossing, &later_type, &later_param,
                    &later_want))
        {
            return false;
        }
    }

    for (i = 0; i < path->num_cells - 1 && FixPasses(*type, *param, *want); ++i)
    {
        k_crossing_t crossing = K_Graph_CanCross(path->portals[i], path->cells[i]);

        if (path->portals[i] != path->blocked_portal && !K_Graph_Passable(crossing)
         && FixFor(path->portals[i], path->cells[i], crossing, &later_type, &later_param,
                   &later_want)
         && !FixPasses(later_type, later_param, later_want)
         && FixWithoutKey(later_type, later_param, later_want))
        {
            *type = later_type;
            *param = later_param;
            *want = later_want;
            *portal = path->portals[i];
            *from = path->cells[i];
            *later = true;
        }
    }

    for (i = 0; i < path->num_cells - 1 && *type != GOAL_KEY; ++i)
    {
        k_crossing_t crossing = K_Graph_CanCross(path->portals[i], path->cells[i]);

        if (crossing == K_CROSS_LOCKED
         && FixFor(path->portals[i], path->cells[i], K_CROSS_LOCKED, &key_type, &key_param,
                   &key_want))
        {
            *type = key_type;
            *param = key_param;
            *want = key_want;
            *key_door = path->portals[i];
        }
        else if (!K_Graph_Passable(crossing)
              && FixFor(path->portals[i], path->cells[i], crossing, &later_type, &later_param,
                        &later_want)
              && (key_param = FixKey(later_type, later_param, later_want)) >= 0)
        {
            *type = GOAL_KEY;
            *param = key_param;
            *want = 0;
            *key_door = path->portals[i];
        }
    }

    return true;
}

// Set as the pathfinder's block cost, for every route planned (nav's
// replans too).
static double BlockCost(int portal, int from, k_crossing_t crossing)
{
    goal_type_t type;
    int param, want;

    return FixFor(portal, from, crossing, &type, &param, &want) ? FIXABLE_COST
                                                                : UNFIXABLE_COST;
}

// Set as the pathfinder's cost for a gap inside a cell.
static double PinchCost(int cell, k_vec2_t at)
{
    goal_type_t type;
    int param, want;

    return FixForPinch(cell, at, &type, &param, &want) ? FIXABLE_COST : UNFIXABLE_COST;
}

static void CountKeys(k_vec2_t from)
{
    k_keything_t keys[MAX_CANDIDATES];
    int key, i;

    planned_from = from;

    for (key = 0; key < K_NUM_KEYS; ++key)
    {
        keys_lying[key] = K_Specials_FindKeys(key, keys, MAX_CANDIDATES);

        for (i = 0; i < SDL_min(keys_lying[key], MAX_CANDIDATES); ++i)
        {
            keys_at[key][i] = keys[i].at;
        }
    }
}

//
// Planning
//

static void Go(goal_t *goal, const candidate_t *candidate, double length,
               int reachable, int total)
{
    char name[64];
    int ok;

    goal->chosen = *candidate;
    goal->planned = true;
    goal->used = false;
    goal->moving = false;
    goal->changed = false;
    goal->wait = 0;
    goal->waited = 0;
    goal->idle = false;
    goal->landing = false;
    goal->shooting = false;
    idling = false;

    if (goal->type == GOAL_TRIGGER)
    {
        K_Map_SectorHeights(goal->param, &goal->floor, &goal->ceiling);
        goal->settling = K_Graph_SectorBusy(goal->param);

        if (line_uses != NULL && candidate->line >= 0)
        {
            line_uses[candidate->line] = LineUses(candidate->line) + 1;
            line_last[candidate->line] = leveltime;
        }
    }

    GoalName(goal, name, sizeof(name));
    M_snprintf(goal->note, sizeof(goal->note), "%s, %.0f units (%d of %d reachable)",
               candidate->what, length, reachable, total);
    K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: going for the %s", name, goal->note);

    ok = candidate->kind == K_GOAL_POINT
       ? K_Nav_SetGoal(candidate->at.x, candidate->at.y)
       : K_Nav_SetLineGoal(candidate->line, candidate->kind, candidate->at.x, candidate->at.y);

    if (!ok)
    {
        CandidateFailed(goal, K_Nav_GetStatus()->problem);
    }
}

// Why each candidate was given up on, while planning a goal: logged one
// by one when it fails (the failure itself names only the last).
#define MAX_REASONS 8

static char reasons[MAX_REASONS][K_LOG_LINE_LEN];
static int num_reasons;

static void KeepReason(const char *reason)
{
    if (num_reasons < MAX_REASONS)
    {
        M_StringCopy(reasons[num_reasons++], reason, K_LOG_LINE_LEN);
    }
}

static void LogReasons(const char *name)
{
    int i;

    for (i = 0; i < num_reasons && num_reasons > 1; ++i)
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: %s", name, reasons[i]);
    }
}

// Whether some candidate of a goal is open to the player now: one it can
// work (no key it lacks, none needed for a door in front of a switch, no
// shut door between it and a barrel), with an open route there.
static boolean HasOpenWay(const goal_t *goal, const k_pose_t *player)
{
    static candidate_t candidates[MAX_CANDIDATES];
    k_vec2_t start = { player->x, player->y };
    int count = Candidates(goal, candidates, MAX_CANDIDATES);
    int i, door;

    for (i = 0; i < count; ++i)
    {
        if (WasTried(goal, candidates[i].at) || candidates[i].door >= 0)
        {
            continue;
        }

        if (candidates[i].line >= 0)
        {
            int key = K_Specials_LockKey(K_Specials_LineSpecial(candidates[i].line));

            if ((key >= 0 && !K_Specials_HasKey(key))
             || (candidates[i].kind == K_GOAL_USE_LINE && DoorKeyInTheWay(&candidates[i], &door) >= 0))
            {
                continue;
            }
        }

        K_Path_Find(&scratch, start, player->cell, candidates[i].at);

        if (scratch.result == K_PATH_FOUND)
        {
            return true;
        }
    }

    return false;
}

// Before the goal on top is planned: if a goal further down has an open way
// now, the goals above it were pushed for a way that is no longer needed,
// and are dropped (Doom II MAP14: switch 416 opened the door on the way to
// the red key, while the goal on top was still a switch for another way
// round, and from there the bot looped). The lowest such goal wins. An item
// is a detour, not a fix for the goals below it, which have their way: only
// the item and the fixes pushed for it count (Doom II MAP04: the switch for
// the bars before a shotgun was dropped with the shotgun, every second); so
// is a telefrag, a fall back, a way back opened (ConsiderWayBack), a wander
// out of a loop (BreakLoop), and a chase (ConsiderChase). Each goal is looked at with the stack as
// it would be once those above it are gone, as it is planned then: a fix on
// the stack is not allowed again (Allowed), and the crossing it is for costs
// a route as one nothing fixes (Doom II MAP15: at 10 health, with GET RED KEY
// pushed, the red door counted so, and the way to switch 594 went open over
// floors that hurt; dropped, the door was a fix again, the way through it
// cheaper, and GET RED KEY was pushed again - 4842 times, for 13 minutes).
// Returns true if goals were dropped.
static boolean DropNeedless(void)
{
    k_pose_t player;
    char name[64];
    int base = 0;
    int top = depth;
    boolean open;
    int i;

    for (i = depth - 1; i >= 0; --i)
    {
        if (goals[i].type == GOAL_ITEM || goals[i].type == GOAL_TELEFRAG
         || goals[i].type == GOAL_PLACE || goals[i].fallback || goals[i].way_back
         || goals[i].wander || goals[i].chase)
        {
            base = i;
            break;
        }
    }

    if (depth < 2 || !K_Map_GetPlayer(&player))
    {
        return false;
    }

    for (i = base; i < top - 1; ++i)
    {
        if (goals[i].type == GOAL_GOTO)
        {
            continue;
        }

        depth = i + 1;
        open = HasOpenWay(&goals[i], &player);
        depth = top;

        if (!open)
        {
            continue;
        }

        GoalName(&goals[i], name, sizeof(name));
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: there is an open way now - dropping the %d "
              "goals above it", name, depth - 1 - i);
        depth = i + 1;
        goals[i].planned = false;
        K_Nav_Clear();
        return true;
    }

    return false;
}

// A cheap hash for a spot picked at random: the same game picks the same,
// but for salt (k_salt.h).
static unsigned int Hash(unsigned int x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// An open spot to wander to out of a loop (LOOP_PLANS), or false.
static boolean WanderSpot(k_vec2_t *spot)
{
    const k_map_t *map = K_Map_Get();
    k_pose_t player;
    k_vec2_t start;
    int i;

    if (map->num_cells == 0 || !K_Map_GetPlayer(&player))
    {
        return false;
    }

    start.x = player.x;
    start.y = player.y;

    for (i = 0; i < WANDER_TRIES; ++i)
    {
        const k_cell_t *cell = &map->cells[Hash(K_Salt_Mix((unsigned int) leveltime * 31u + i))
                                           % map->num_cells];

        if (K_Graph_Clearance(cell->centroid, PLAYER_RADIUS + 1.0) <= PLAYER_RADIUS)
        {
            continue;
        }

        K_Path_Find(&scratch, start, player.cell, cell->centroid);

        if (scratch.result == K_PATH_FOUND && scratch.damage <= 0.0
         && scratch.length >= WANDER_MIN && scratch.length <= WANDER_MOST)
        {
            *spot = cell->centroid;
            return true;
        }
    }

    return false;
}

// Before the goal on top is planned: if it has been planned LOOP_PLANS times
// within LOOP_TICS, the stack is going round in a loop, and it is broken.
// Returns true if it was.
static boolean BreakLoop(goal_t *goal)
{
    char name[64], why[K_LOG_LINE_LEN];
    k_vec2_t spot;
    goal_t *wander;
    int took;

    if (leveltime - goal->plans_since >= LOOP_TICS)
    {
        goal->plans_since = leveltime;
        goal->plans = 0;
    }

    if (++goal->plans < LOOP_PLANS)
    {
        return false;
    }

    took = leveltime - goal->plans_since;
    goal->plans = 0;
    goal->plans_since = leveltime;
    M_snprintf(why, sizeof(why), "planned %d times in %.1f s, going round in a loop",
               LOOP_PLANS, (double) took / TICRATE);

    if (goal->type != GOAL_EXIT)
    {
        FailGoal(why);
        return true;
    }

    GoalName(goal, name, sizeof(name));

    if (depth < MAX_GOALS && WanderSpot(&spot))
    {
        wander = Push(GOAL_GOTO, -1);
        wander->point = spot;
        wander->wander = true;
        K_Log(K_LOG_BRAIN, K_LOG_WARN, "%s: %s - added WANDER TO (%.0f, %.0f), to plan again "
              "from there", name, why, spot.x, spot.y);
        return true;
    }

    K_Log(K_LOG_BRAIN, K_LOG_WARN, "%s: %s - nowhere open to wander to; waiting %d s", name, why,
          IDLE_TICS / TICRATE);
    M_snprintf(goal->note, sizeof(goal->note), "waiting - %s", why);
    goal->retry_tic = leveltime + IDLE_TICS;
    K_Nav_Clear();
    return true;
}

//
// The exit wanted
//

static const char *ExitKindName(int kind)
{
    return kind ? "secret" : "normal";
}

// The kind of exit this level is left by (EXIT_IDLE_MOST), once GO TO EXIT
// goes on the list.
static void ChooseExit(void)
{
    const k_map_t *map = K_Map_Get();
    int have[2] = { 0, 0 };
    int i, secret;

    exit_kind = exit_choice == K_EXIT_SECRET ? 1
              : exit_choice == K_EXIT_NORMAL ? 0
              : exit_choice == K_EXIT_RANDOM
              ? (int) (Hash(K_Salt_Mix(0x5ec7e700u + gameepisode * 100u + gamemap)) & 1)
              : EXIT_ANY;

    if (exit_kind == EXIT_ANY)
    {
        return;
    }

    for (i = 0; i < map->num_lines; ++i)
    {
        if (K_Specials_ExitTrigger(map->lines[i].special, &secret) != K_TRIGGER_NONE)
        {
            ++have[secret != 0];
        }
    }

    for (i = 0; i < map->num_sectors; ++i)
    {
        have[0] += K_Specials_ExitSector(i);
    }

    if (have[exit_kind] == 0)
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "GO TO EXIT: no %s exit on this map - any exit",
              ExitKindName(exit_kind));
        exit_kind = EXIT_ANY;
        return;
    }

    K_Log(K_LOG_BRAIN, K_LOG_INFO, "GO TO EXIT: the %s exit wanted (%s)", ExitKindName(exit_kind),
          exit_choice == K_EXIT_RANDOM ? "drawn for this level" : "-karpexit");
}

// The exit wanted gives way to any for the rest of the level.
static void AnyExit(const char *why)
{
    K_Log(K_LOG_BRAIN, K_LOG_WARN, "GO TO EXIT: %s - any exit now, not only the %s one", why,
          ExitKindName(exit_kind));
    exit_kind = EXIT_ANY;

    if (depth == 1)
    {
        goals[0].planned = false;
        goals[0].retry_tic = 0;
        K_Nav_Clear();
    }
}

// Each tic: progress toward the exit wanted (EXIT_STALL). Not in a fight,
// nor on a detour, nor in the sweep before the exit.
static void UpdateExit(void)
{
    const k_navstatus_t *nav = K_Nav_GetStatus();
    int i;

    if (exit_kind < 0 || depth == 0 || goals[0].type != GOAL_EXIT || sweep_since > 0
     || K_Combat_GetStatus()->battle)
    {
        return;
    }

    for (i = 1; i < depth; ++i)
    {
        if (goals[i].type == GOAL_ITEM || goals[i].type == GOAL_TELEFRAG
         || goals[i].type == GOAL_PLACE || goals[i].fallback || goals[i].way_back
         || goals[i].chase)
        {
            return;
        }
    }

    if (depth == 1 && goals[0].planned && nav->state == K_NAV_DRIVING
     && nav->remaining < exit_best - EXIT_GAIN)
    {
        exit_best = nav->remaining;
        exit_stall = 0;
    }
    else if (++exit_stall >= EXIT_STALL)
    {
        AnyExit("no nearer to it for 3 minutes");
    }
}

// Walk-over exits no route crosses (K_Path_SetKeepOff): of the kind not
// wanted; for an explorer, any but the one gone over (the level is not
// ended on the way to something else).
static int KeepOff(int line)
{
    int secret;

    return K_Specials_ExitTrigger(K_Specials_LineSpecial(line), &secret) == K_TRIGGER_CROSS
        && (explorer || (exit_kind >= 0 && (secret != 0) != exit_kind));
}

static void PlanGoal(goal_t *goal)
{
    static candidate_t candidates[MAX_CANDIDATES];
    int count = Candidates(goal, candidates, MAX_CANDIDATES);
    int best_open = -1, best_blocked = -1, reachable = 0;
    int waitable = 0;   // candidates with no way now, which a move may open
    int moving, boss;
    double open_length = 0.0, blocked_score = 0.0;
    goal_type_t fix_type = GOAL_GOTO;
    int fix_param = 0, fix_want = 0, fix_portal = -1, fix_from = -1;
    k_vec2_t fix_pinch_at = { 0.0, 0.0 };
    char reason[K_LOG_LINE_LEN], blocker[K_LOG_LINE_LEN] = "";
    char name[64];
    k_pose_t player;
    k_vec2_t start;
    int i;

    if (!K_Map_GetPlayer(&player))
    {
        return;
    }

    start.x = player.x;
    start.y = player.y;
    CountKeys(start);
    GoalName(goal, name, sizeof(name));
    num_reasons = 0;

    // No spot sees the boss now: the boss itself, until it moves.
    if (FiresFrom(goal) && count > 0 && !IsSpot(&candidates[0]))
    {
        goal->near = true;
    }

    M_snprintf(reason, sizeof(reason), "%s",
               goal->type == GOAL_EXIT ? "the map has no exit line or floor"
             : goal->type == GOAL_KEY ? "no such key lies in the level"
             : goal->type == GOAL_TRIGGER ? "no line moves the sector any more"
             : goal->type == GOAL_ITEM ? "it is gone, or no spot touches it"
             : goal->type == GOAL_BARREL ? "no safe spot to shoot it from"
             : goal->type == GOAL_TELEFRAG ? "no way through the teleporter now"
             : "no way there");

    for (i = 0; i < count; ++i)
    {
        char text[K_LOG_LINE_LEN];
        goal_type_t type;
        int param, want, portal, from, key_door;
        boolean later;

        if (WasTried(goal, candidates[i].at))
        {
            M_snprintf(reason, sizeof(reason), "the %s did not work out",
                       candidates[i].what);
            KeepReason(reason);
            continue;
        }

        K_Path_Find(&scratch, start, player.cell, candidates[i].at);
        K_Path_Describe(&scratch, text, sizeof(text));

        // A switch that needs a key (red bars, say), or stands behind a door
        // that does: nothing happens until the player has it.
        if (candidates[i].line >= 0)
        {
            int key = K_Specials_LockKey(K_Specials_LineSpecial(candidates[i].line));
            int door = -1;
            char needs[64];

            if ((key < 0 || K_Specials_HasKey(key))
             && candidates[i].kind == K_GOAL_USE_LINE)
            {
                key = DoorKeyInTheWay(&candidates[i], &door);
            }

            if (key >= 0 && !K_Specials_HasKey(key))
            {
                if (door >= 0)
                {
                    M_snprintf(needs, sizeof(needs), "is behind a door at line %d that "
                               "needs the %s key", door, K_Specials_KeyName(key));
                }
                else
                {
                    M_snprintf(needs, sizeof(needs), "needs the %s key",
                               K_Specials_KeyName(key));
                }

                if (scratch.result != K_PATH_FOUND && scratch.result != K_PATH_BLOCKED)
                {
                    M_snprintf(reason, sizeof(reason), "no way to the %s: %s",
                               candidates[i].what, text);
                    KeepReason(reason);
                    ++waitable;
                }
                else if (keys_lying[key] > 0 && Allowed(GOAL_KEY, key))
                {
                    double score = (scratch.blocked_count + 1) * FIXABLE_COST
                                 + scratch.length;

                    if (best_blocked < 0 || score < blocked_score)
                    {
                        best_blocked = i;
                        blocked_score = score;
                        fix_type = GOAL_KEY;
                        fix_param = key;
                        fix_want = 0;
                        M_snprintf(blocker, sizeof(blocker), "the %s %s",
                                   candidates[i].what, needs);
                    }
                }
                else
                {
                    M_snprintf(reason, sizeof(reason), "the %s %s, and there is none to "
                               "get", candidates[i].what, needs);
                    KeepReason(reason);
                }

                continue;
            }
        }

        // A spot that sees the barrel only through a shut door (BarrelSpots),
        // an open way there: the door opened first, a fix on the way.
        if (candidates[i].door >= 0)
        {
            if (scratch.result == K_PATH_FOUND
             && LiveTrigger(candidates[i].door, OPENING_MOVES | K_MOVE_DOOR)
             && Allowed(GOAL_TRIGGER, candidates[i].door))
            {
                double score = FIXABLE_COST + scratch.length;

                if (best_blocked < 0 || score < blocked_score)
                {
                    best_blocked = i;
                    blocked_score = score;
                    fix_type = GOAL_TRIGGER;
                    fix_param = candidates[i].door;
                    fix_want = OPENING_MOVES | K_MOVE_DOOR;
                    fix_portal = -1;
                    fix_from = -1;
                    M_snprintf(blocker, sizeof(blocker), "the %s sees it only through the "
                               "door of sector %d, shut", candidates[i].what,
                               candidates[i].door);
                }
            }
            else
            {
                M_snprintf(reason, sizeof(reason), "the %s sees it only through the door of "
                           "sector %d, shut, and %s", candidates[i].what, candidates[i].door,
                           scratch.result != K_PATH_FOUND ? "there is no open way there"
                                                          : "nothing opens it");
                KeepReason(reason);
            }

            continue;
        }

        if (scratch.result == K_PATH_FOUND)
        {
            ++reachable;

            if (best_open < 0 || scratch.length < open_length)
            {
                best_open = i;
                open_length = scratch.length;
            }
        }
        else if (scratch.result == K_PATH_BLOCKED
              && RouteFix(&scratch, &type, &param, &want, &portal, &from, &later, &key_door))
        {
            double score = scratch.blocked_count * FIXABLE_COST + scratch.length;

            if (best_blocked < 0 || score < blocked_score)
            {
                best_blocked = i;
                blocked_score = score;
                fix_type = type;
                fix_param = param;
                fix_want = want;
                fix_portal = portal;
                fix_from = from;
                fix_pinch_at = scratch.blocked_at;

                if (key_door >= 0)
                {
                    M_snprintf(blocker, sizeof(blocker), "the %s is blocked by %s, and "
                               "further on by a door that needs the %s key - the key "
                               "first", candidates[i].what, text,
                               K_Specials_KeyName(param));
                }
                else if (later)
                {
                    M_snprintf(blocker, sizeof(blocker), "the %s is blocked by a lift or a "
                               "door that shuts again (%s) - first the crossing further on "
                               "(%s)", candidates[i].what, text,
                               K_Graph_CrossingName(K_Graph_CanCross(portal, from)));
                }
                else
                {
                    M_snprintf(blocker, sizeof(blocker), "the %s is blocked by %s",
                               candidates[i].what, text);
                }
            }
        }
        else
        {
            M_snprintf(reason, sizeof(reason), "no way to the %s: %s",
                       candidates[i].what, text);
            KeepReason(reason);
            ++waitable;
        }
    }

    if (best_open >= 0)
    {
        Go(goal, &candidates[best_open], open_length, reachable, count);
    }
    else if (FiresFrom(goal) && count > 0 && IsSpot(&candidates[0]))
    {
        // No open way to a spot to fight it from: the boss itself, planned
        // next tic, fixes and all.
        goal->near = true;
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: no open way to any of %d spots to fight it from - "
              "going for the %s itself", name, count, K_Combat_KindName(goal->param));
    }
    else if (((goal->type == GOAL_ITEM || goal->type == GOAL_PLACE)
              && (!(goal->fixable || goal->shut > 0) || best_blocked < 0 || fix_type == GOAL_KEY))
          || goal->type == GOAL_TELEFRAG)
    {
        // A detour, not a step on the way: never worth a wait, nor a fix
        // unless it was gone for through one (FIX_WORK), or its way shut on
        // the way (E2M8: a door shut behind the player going for the rocket
        // launcher, the cyberdemon too much without it; the launcher was
        // given up for 30 s, and the bot walked up to the cyberdemon and
        // died), and never a key; so is a place. A telefrag never through a
        // fix.
        FailGoal(reason);
    }
    else if (best_blocked >= 0)
    {
        goal_t *fix;
        char fix_name[64];

        M_StringCopy(goal->note, blocker, sizeof(goal->note));
        fix = Push(fix_type, fix_param);

        if (fix_type == GOAL_TRIGGER)
        {
            fix->portal = fix_portal;
            fix->from = fix_from;
            fix->want = fix_want;

            if (fix_portal < 0)
            {
                fix->pinch_cell = fix_from;
                fix->pinch_at = fix_pinch_at;
            }
        }

        GoalName(fix, fix_name, sizeof(fix_name));
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: %s - added %s", name, blocker, fix_name);
    }
    else if (goal->type == GOAL_EXIT && (boss = BossFor(-1, 0)) >= 0)
    {
        // The level ends when the last of the map's bosses dies (E2M8,
        // E3M8: no exit line).
        goal_t *fix;
        char fix_name[64];

        M_snprintf(goal->note, sizeof(goal->note), "the level ends when the last %s dies",
                   K_Combat_KindName(boss));
        fix = Push(GOAL_KILL, boss);
        GoalName(fix, fix_name, sizeof(fix_name));
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: %s - added %s", name, goal->note, fix_name);
    }
    else if (waitable > 0 && (moving = K_Graph_MovingSector()) >= 0
          && goal->waited < MAX_WAIT_TICS)
    {
        // The way may still open (stairs being built, a floor going down,
        // Doom II MAP15 and MAP13): stand still and plan again shortly.
        // Not for candidates already tried: they stay given up on, so
        // nothing that moves brings them back (MAP17 stood 15 s for a
        // walk-over line it could not get to).
        if (goal->waited == 0)
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: no way yet (%s), but sector %d is "
                  "moving - waiting", name, reason, moving);
        }

        M_snprintf(goal->note, sizeof(goal->note), "waiting while sectors move - %s",
                   reason);
        goal->retry_tic = leveltime + RETRY_TICS;
        goal->waited += RETRY_TICS;
        K_Nav_Clear();
    }
    else if (goal->type == GOAL_EXIT && exit_kind >= 0 && exit_idle >= EXIT_IDLE_MOST)
    {
        // No way to the exit wanted for long (EXIT_IDLE_MOST): any, now.
        char why[K_LOG_LINE_LEN];

        LogReasons(name);
        M_snprintf(why, sizeof(why), "no way to it for %d s (%s)", exit_idle / TICRATE, reason);
        AnyExit(why);
    }
    else if (goal->type == GOAL_EXIT)
    {
        // Never given up: the goals that failed on the way come back after
        // a while, and something may open a way meanwhile.
        if (exit_kind >= 0)
        {
            exit_idle += IDLE_TICS;
        }

        if (!goal->idle)
        {
            LogReasons(name);
            K_Log(K_LOG_BRAIN, K_LOG_WARN, "%s: no way for now - %s; waiting, planning again "
                  "every %d s", name, reason, IDLE_TICS / TICRATE);
            goal->idle = true;
        }

        M_snprintf(goal->note, sizeof(goal->note), "no way for now, waiting - %s", reason);
        goal->retry_tic = leveltime + IDLE_TICS;
        goal->waited = 0;
        idling = true;
        K_Nav_Clear();
    }
    else
    {
        LogReasons(name);
        FailGoal(reason);
    }
}

// The player sets off past a lift or a door that goes back by itself, its
// line just worked (PASS_TICS).
static void StartPassing(const goal_t *goal, k_crossing_t crossing)
{
    pass_portal = goal->portal;
    pass_from = goal->from;
    pass_sector = goal->param;
    pass_line = goal->chosen.line;
    pass_until = leveltime + PASS_TICS;
    pass_routed = false;
    pass_past = false;
    pass_fought = false;
    K_Log(K_LOG_BRAIN, K_LOG_INFO, "passing the %s (sector %d) before it goes back (%s) - no "
          "detour meanwhile", K_Graph_IsLift(pass_sector) ? "lift" : "door", pass_sector,
          K_Graph_CrossingName(crossing));
}

// Every tic, until the mover goes back, or the time is up: whether a fight
// or a hold has come meanwhile, and whether the player is past it - off the
// mover, in a cell of the route after the crossing (stepping onto a lift
// lowered and off it on the near side is no pass). Past it, the window is
// kept all the same: E3M9, over lift 27, a strip 8 deep, the bot fought a
// cacodemon and retreated from it back over the lift still down, which was
// up again when the fight was over. Riding the mover, kept until the time
// is up. Ended at once if the route, which went through the crossing, goes
// through it no more (another way).
static void UpdatePassing(void)
{
    const k_map_t *map = K_Map_Get();
    const k_path_t *path = K_Nav_GetPath();
    k_pose_t player;
    boolean on, open;
    int k = -1, i;

    if (pass_portal < 0 || !K_Map_GetPlayer(&player) || player.cell < 0)
    {
        return;
    }

    on = map->subsectors[map->cells[player.cell].subsector].sector == pass_sector;
    open = K_Graph_Passable(K_Graph_CanCross(pass_portal, pass_from));
    pass_fought |= K_Combat_GetStatus()->battle || K_Combat_GetStatus()->holding;

    for (i = 0; path != NULL && i < path->num_cells - 1 && k < 0; ++i)
    {
        if (path->portals[i] == pass_portal)
        {
            k = i;
        }
    }

    for (i = k + 1; k >= 0 && i < path->num_cells && !on && !pass_past; ++i)
    {
        pass_past = path->cells[i] == player.cell;
    }

    pass_routed |= k >= 0;

    if (on ? leveltime < pass_until
           : leveltime < pass_until && open && (k >= 0 || !pass_routed || pass_past))
    {
        return;
    }

    // The route goes another way: nothing to give back.
    if (!on && open && leveltime < pass_until)
    {
        pass_portal = -1;
        return;
    }

    // Over: a fight or a hold took the window, the player perhaps carried
    // back over the mover with it (a retreat), so no fair try.
    if (pass_fought && line_uses != NULL && line_uses[pass_line] > 0)
    {
        --line_uses[pass_line];
    }

    pass_excused = pass_fought ? leveltime : -1;

    if (!on && !pass_past)
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "missed the %s (sector %d) - %s%s",
              K_Graph_IsLift(pass_sector) ? "lift" : "door", pass_sector,
              leveltime >= pass_until ? "out of time" : "it went back",
              pass_fought ? "; a fight took the way, so neither the use of its line nor the "
                            "way shut counts" : "");
    }

    pass_portal = -1;
}

// A trigger goal is done once the crossing it was pushed for opens: a door
// opening or a lift coming down is a crossing to wait at, so the player
// sets off for it at once, as a human would. Judged before the goal is
// planned too: the way may have opened while a goal in front of it was
// worked on (Doom II MAP24: the red key platform rising with the player),
// and planning would send the player off for nothing and count a use of
// the line. Returns true if the goal is done.
static boolean DoneIfOpen(const goal_t *goal)
{
    k_crossing_t crossing;
    char how[96];

    if (goal->type == GOAL_TRIGGER && goal->pinch_cell >= 0)
    {
        if (K_Graph_PinchNarrow(goal->pinch_cell, goal->pinch_at))
        {
            return false;
        }

        DoneGoal("the gap is wide enough now, or widening");
        return true;
    }

    if (goal->type != GOAL_TRIGGER || goal->portal < 0)
    {
        return false;
    }

    crossing = K_Graph_CanCross(goal->portal, goal->from);

    if (!K_Graph_Passable(crossing))
    {
        return false;
    }

    M_snprintf(how, sizeof(how), "the way on is open, or opening (%s)",
               K_Graph_CrossingName(crossing));

    if (goal->planned && goal->chosen.line >= 0 && FixPasses(goal->type, goal->param, goal->want))
    {
        StartPassing(goal, crossing);
    }

    DoneGoal(how);
    return true;
}

// The nearest dry spot to go to from a floor that hurts (DRY_REACH): in the
// first ring with any, the one with the shortest way there.
static boolean DrySpot(const k_pose_t *player, k_vec2_t *spot)
{
    const k_map_t *map = K_Map_Get();
    k_vec2_t start = { player->x, player->y };
    double best = 1e30, radius;
    int i;

    for (radius = DRY_RING; radius <= DRY_REACH && best >= 1e30; radius += DRY_RING)
    {
        for (i = 0; i < DRY_WAYS; ++i)
        {
            double angle = i * 2.0 * 3.14159265358979323846 / DRY_WAYS;
            k_vec2_t at = { start.x + cos(angle) * radius, start.y + sin(angle) * radius };
            int cell = K_Map_CellAt(at.x, at.y);

            if (cell < 0
             || K_Map_FloorDamage(map->subsectors[map->cells[cell].subsector].sector) > 0.0
             || K_Graph_Clearance(at, PLAYER_RADIUS + 1.0) <= PLAYER_RADIUS)
            {
                continue;
            }

            K_Path_Find(&probe, start, player->cell, at);

            if (probe.result == K_PATH_FOUND && probe.length <= 2.0 * radius
             && probe.length < best)
            {
                best = probe.length;
                *spot = at;
            }
        }
    }

    return best < 1e30;
}

// Waiting for a sector set going to stop (UpdateTrigger), on a floor that
// hurts, no suit on: to the nearest dry spot (DrySpot), nav driving there;
// logged as it sets off.
static void StayDry(goal_t *goal)
{
    const k_map_t *map = K_Map_Get();
    k_pose_t player;
    k_vec2_t spot;
    char name[64];
    int sector;

    if (K_Nav_GetStatus()->state == K_NAV_DRIVING)
    {
        K_Nav_Tick();
        return;
    }

    if (leveltime < goal->dry_look || K_Map_SuitTics() > 0 || !K_Map_GetPlayer(&player)
     || player.cell < 0)
    {
        return;
    }

    // Damage comes from the floor under the player's middle, stood on.
    sector = map->subsectors[map->cells[player.cell].subsector].sector;

    if (K_Map_FloorDamage(sector) <= 0.0 || fabs(K_Map_CellFloor(player.cell) - player.floor) > 1.0)
    {
        return;
    }

    goal->dry_look = leveltime + DRY_LOOK;

    if (DrySpot(&player, &spot) && K_Nav_SetGoal(spot.x, spot.y))
    {
        GoalName(goal, name, sizeof(name));
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: waiting for sector %d to stop, on a floor that hurts "
              "- to dry floor at (%.0f,%.0f)", name, goal->param, spot.x, spot.y);
    }
}

// A sector moved by a line: once the line is used, wait for the sector to
// start moving, then to stop (unless the way opens first: DoneIfOpen) - off
// a floor that hurts meanwhile (StayDry). Returns true while it handles the
// goal.
static boolean UpdateTrigger(goal_t *goal)
{
    double floor, ceiling;
    k_mover_t mover = K_Graph_SectorMover(goal->param);
    char how[96];

    // Busy when its line was gone for: what it does until it stops is not
    // the line's doing, and nav waits at the switch meanwhile (Doom II
    // MAP02: switch 102, gone for as door 30 closed by itself, counted as
    // having moved it, twice in 0.1 s, then once more, using it up).
    if (goal->settling)
    {
        if (K_Graph_SectorBusy(goal->param))
        {
            return false;
        }

        goal->settling = false;
        K_Map_SectorHeights(goal->param, &goal->floor, &goal->ceiling);

        // A walk-over line crossed meanwhile set nothing off: crossed again
        // (E3M4: line 491 crossed as the bars it opens were closing, counted
        // as not working, twice; the third time every goal behind them
        // failed).
        if (goal->used)
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "crossed line %d as sector %d was still moving - "
                  "crossing it again", goal->chosen.line, goal->param);
            goal->planned = false;
            return true;
        }
    }

    K_Map_SectorHeights(goal->param, &floor, &ceiling);

    if (floor != goal->floor || ceiling != goal->ceiling)
    {
        goal->changed = true;
    }

    if (!goal->moving)
    {
        if (goal->changed || mover != K_MOVER_NONE)
        {
            goal->moving = true;
            goal->wait = 0;
            M_snprintf(goal->note, sizeof(goal->note), "sector %d is moving",
                       goal->param);
            K_Nav_Clear();
            return true;
        }

        if (goal->used && ++goal->wait > START_TICS)
        {
            CandidateFailed(goal, "crossing it moved nothing");
            return true;
        }

        return false;
    }

    if (mover == K_MOVER_NONE || mover == K_MOVER_WAITING || ++goal->wait > MOVE_TICS)
    {
        // A mover set going can have nowhere to go: a lift already at the
        // bottom waits there at once (Doom II MAP28), and counting that as
        // moved, the brain worked its switches round and round.
        if (!goal->changed)
        {
            CandidateFailed(goal, "using it moved nothing");
            return true;
        }

        M_snprintf(how, sizeof(how), "sector %d moved (floor %.0f, ceiling %.0f)",
                   goal->param, floor, ceiling);
        DoneGoal(how);
        return true;
    }

    StayDry(goal);
    return true;
}

static boolean OnExitFloor(void)
{
    const mobj_t *mo = players[consoleplayer].mo;

    return mo != NULL && K_Specials_ExitSector((int) (mo->subsector->sector - sectors));
}

// Gone for the floor that ends the level (E1M8): once on it, the player
// stays put, whatever its health (no detour: ConsiderItems), until the
// floor has taken it down to 10 and the level ends; pushed off, it goes back.
// Returns true while it handles the goal.
static boolean UpdateExitFloor(goal_t *goal)
{
    if (goal->chosen.line >= 0)
    {
        return false;
    }

    if (!OnExitFloor())
    {
        if (goal->staying)
        {
            goal->staying = false;
            goal->planned = false;
            K_Nav_Clear();
            return true;
        }

        return false;
    }

    if (!goal->staying)
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "GO TO EXIT: on the exit floor - staying while it takes "
              "the health down to 10, which ends the level");
        M_StringCopy(goal->note, "on the exit floor, waiting for it to end the level",
                     sizeof(goal->note));
        goal->staying = true;
        K_Nav_Clear();
    }

    return true;
}

// The monster of the kind gone for has moved KILL_DRIFT from where it stood
// then (looked at every KILL_LOOK_TICS, not in a fight): planned again - a
// firing spot too, the boss itself gone for no more (FIRE_RINGS); so is a
// boss that can be fought now, gone for itself as hopeless, or one hopeless
// now, gone for from a spot. Returns true if it was.
#define KILL_DRIFT 256.0
#define KILL_LOOK_TICS TICRATE

static boolean UpdateKill(goal_t *goal)
{
    k_vec2_t at[MAX_CANDIDATES];
    double nearest = -1.0;
    int count, i;

    if (++goal->wait < KILL_LOOK_TICS || K_Combat_GetStatus()->battle)
    {
        return false;
    }

    goal->wait = 0;
    count = SDL_min(K_Specials_FindMonsters(goal->param, at, MAX_CANDIDATES), MAX_CANDIDATES);

    for (i = 0; i < count; ++i)
    {
        double distance = hypot(at[i].x - goal->chosen.aim.x, at[i].y - goal->chosen.aim.y);

        if (nearest < 0.0 || distance < nearest)
        {
            nearest = distance;
        }
    }

    if (nearest >= 0.0 && nearest < KILL_DRIFT && SpotChosen(goal) == FiresFrom(goal))
    {
        return false;
    }

    goal->planned = false;
    goal->shooting = false;
    goal->near = false;
    K_Nav_Clear();
    return true;
}

// The FALL BACK on the stack, -1 if none: only the fixes pushed for it can
// be above it (ConsiderItems looks for none meanwhile). Not one there,
// resting (REST_QUIET).
static int FallingBack(void)
{
    int i;

    for (i = depth - 1; i >= 0; --i)
    {
        if (goals[i].type == GOAL_GOTO && goals[i].fallback && !goals[i].resting)
        {
            return i;
        }
    }

    return -1;
}

// The way back being opened (ConsiderWayBack), on the stack, -1 if none:
// no item detour meanwhile - dropping one would drop it too, above it (Doom
// II MAP15: the stairs switch added on landing, gone with the BFG dropped
// for health the next tic).
static int OpeningWayBack(void)
{
    int i;

    for (i = depth - 1; i >= 0; --i)
    {
        if (goals[i].way_back)
        {
            return i;
        }
    }

    return -1;
}

// The chase on the stack (ConsiderChase), -1 if none.
static int ChaseOn(void)
{
    int i;

    for (i = depth - 1; i >= 0; --i)
    {
        if (goals[i].chase)
        {
            return i;
        }
    }

    return -1;
}

// Whether the item (by its index) is gone for already, somewhere on the
// chain.
static boolean OnChain(int item)
{
    int i;

    for (i = 0; i < depth; ++i)
    {
        if (goals[i].type == GOAL_ITEM && goals[i].param == item)
        {
            return true;
        }
    }

    return false;
}

// Whether the item goes before the others (FIRST_SCORE): one worth leaving a
// fight for however far (k_items.h: escape) - but when CRITICAL only health
// (Doom II MAP15: at 9 health a clip for the chaingun, firepower LOW, was
// added over the medikits and dropped for them every second), and
// invulnerability with a boss on the player - and but when CRITICAL a weapon
// at a pistol start.
static boolean GoesFirst(const k_item_t *item, boolean critical)
{
    return (item->escape && (item->kind == K_ITEM_HEALTH || item->invuln || !critical))
        || (item->first && !critical);
}

// Whether something worth leaving a fight for (k_items.h: escape) lies in
// reach: health, when LOW or CRITICAL; but for health CRITICAL, ammo when
// ammo is LOW or firepower CRITICAL. *health tells which. Invulnerability is
// not counted: it goes before either (SphereRun).
static boolean EscapeFor(boolean critical, boolean *health)
{
    boolean ammo = false;
    int i;

    *health = false;

    for (i = 0; i < K_Items_Count(); ++i)
    {
        const k_item_t *item = K_Items_Get(i);

        if (item->escape && item->present && item->reach > 0.0 && !item->invuln)
        {
            *health |= item->kind == K_ITEM_HEALTH;
            ammo |= item->kind != K_ITEM_HEALTH && !critical;
        }
    }

    return *health || ammo;
}

// Whether an invulnerability sphere is worth a run out of the fight, a boss
// on the player (k_items.h: escape; K_Combat_Boss), with an open way there
// no longer than SPHERE_RUN - not one given up on lately; one with none is
// not looked at for SPHERE_SKIP. It goes before any other item: one gone for
// is dropped for it, and none is gone for over it.
static boolean SphereRun(void)
{
    k_pose_t player;
    k_vec2_t start;
    int i;

    if (!K_Map_GetPlayer(&player))
    {
        return false;
    }

    start.x = player.x;
    start.y = player.y;

    for (i = 0; i < K_Items_Count(); ++i)
    {
        const k_item_t *item = K_Items_Get(i);

        if (!item->invuln || !item->escape || !item->present || item->reach <= 0.0
         || (i < item_skip_size && leveltime < item_skip[i]) || IsFailed(GOAL_ITEM, i))
        {
            continue;
        }

        K_Path_Find(&probe, start, player.cell, item->spot);

        if (probe.result == K_PATH_FOUND && probe.length + probe.wait * WAIT_WAY <= SPHERE_RUN)
        {
            return true;
        }

        if (i < item_skip_size)
        {
            item_skip[i] = leveltime + SPHERE_SKIP;
        }
    }

    return false;
}

// How far out of its way the player may go for an item now: in a fight, a
// dash at most, but for one worth leaving the fight for (escape). For the
// sphere run for (SphereRun), how far its way there may be.
static double ItemReach(const k_item_t *item, boolean fighting)
{
    if (item->invuln && item->escape && item->reach > 0.0)
    {
        return SPHERE_RUN;
    }

    if (fighting && !item->escape)
    {
        return SDL_min(item->reach, FIGHT_REACH);
    }

    return item->reach;
}

// How far a point lies from the route ahead, within ROUTE_AHEAD along it
// from the player (start), the next waypoint `from`.
static double RouteDistance(const k_path_t *path, int from, k_vec2_t start, k_vec2_t at)
{
    double best = 1.0e9, left = ROUTE_AHEAD;
    k_vec2_t a = start;
    int k;

    for (k = SDL_max(from, 0); k < path->num_points && left > 0.0; ++k)
    {
        k_vec2_t b = path->points[k];
        double dx = b.x - a.x, dy = b.y - a.y;
        double length = sqrt(dx * dx + dy * dy);
        double t = length > 0.0 ? ((at.x - a.x) * dx + (at.y - a.y) * dy) / (length * length)
                                : 0.0;
        double ox, oy;

        t = SDL_max(0.0, SDL_min(1.0, t));
        ox = a.x + dx * t - at.x;
        oy = a.y + dy * t - at.y;
        best = SDL_min(best, sqrt(ox * ox + oy * oy));
        left -= length;
        a = b;
    }

    return best;
}

// A route that is open, or (fixed) blocked where the fix of that type and
// param opens it: the fix for it (RouteFix) is that one. However many
// crossings it is: the bars of one sector are a crossing each (Doom II
// MAP04: twelve, the shotguns behind them left).
static boolean OpenOrFixed(const k_path_t *path, boolean fixed, goal_type_t type, int param)
{
    goal_type_t fix_type;
    int fix_param, want, portal, from, key_door;
    boolean later;

    return path->result == K_PATH_FOUND
        || (fixed && path->result == K_PATH_BLOCKED
            && RouteFix(path, &fix_type, &fix_param, &want, &portal, &from, &later, &key_door)
            && fix_type == type && fix_param == param);
}

// Marks the areas the route goes through from its route cell `from` on
// (route_marks).
static void MarkRouteAreas(const k_path_t *path, int from)
{
    int i;

    if (marks_size < K_Area_Get()->num_areas)
    {
        marks_size = K_Area_Get()->num_areas * 2;
        route_marks = realloc(route_marks, marks_size * sizeof(*route_marks));
        risk_marks = realloc(risk_marks, marks_size * sizeof(*risk_marks));
        through_marks = realloc(through_marks, marks_size * sizeof(*through_marks));
        memset(route_marks, 0, marks_size * sizeof(*route_marks));
        memset(risk_marks, 0, marks_size * sizeof(*risk_marks));
        memset(through_marks, 0, marks_size * sizeof(*through_marks));
    }

    ++route_stamp;

    for (i = SDL_max(from, 0); path != NULL && i < path->num_cells; ++i)
    {
        int area = K_Area_Of(path->cells[i]);

        if (area >= 0)
        {
            route_marks[area] = route_stamp;
        }
    }
}

// What the monsters inside the areas a way goes through would cost (k_area.h:
// inside), but for the areas of the route (MarkRouteAreas), the area `skip`
// and those counted already for this detour (risk_marks).
static double WayRisk(const k_path_t *path, int skip)
{
    double risk = 0.0;
    int i;

    for (i = 0; i < path->num_cells; ++i)
    {
        int area = K_Area_Of(path->cells[i]);

        if (area < 0 || area >= marks_size || area == skip || route_marks[area] == route_stamp
         || risk_marks[area] == risk_stamp)
        {
            continue;
        }

        risk_marks[area] = risk_stamp;
        risk += K_Area_Get()->areas[area].inside;
    }

    return risk;
}

// What the monsters inside the areas a way goes through would cost, the
// route's and the item's own too, but for the area `own` (the player's: met
// whichever way it goes) and those counted already for this way
// (through_marks).
static double WayThrough(const k_path_t *path, int own)
{
    double through = 0.0;
    int i;

    for (i = 0; i < path->num_cells; ++i)
    {
        int area = K_Area_Of(path->cells[i]);

        if (area < 0 || area >= marks_size || area == own || through_marks[area] == through_stamp)
        {
            continue;
        }

        through_marks[area] = through_stamp;
        through += K_Area_Get()->areas[area].inside;
    }

    return through;
}

// The doors opened by hand on a way that stay open (not K_Specials_DoorCloses):
// open for a way on from its end, the player having gone through them
// (K_Graph_AssumeOpen).
static void LeaveOpen(const k_path_t *path)
{
    const k_map_t *map = K_Map_Get();
    const k_graph_t *graph = K_Graph_Get();
    int i;

    for (i = 0; i + 1 < path->num_cells; ++i)
    {
        int line = graph->portals[path->portals[i]].line;
        int special = line >= 0 ? K_Specials_LineSpecial(line) : 0;

        if (K_Specials_ManualDoor(special) && !K_Specials_DoorCloses(special))
        {
            K_Graph_AssumeOpen(map->subsectors[map->cells[path->cells[i + 1]].subsector].sector);
        }
    }
}

// The fix's spots (fix_candidates) by the least way from start through each
// on to spot (K_Path_LeastWay), least first: fix_order, fix_least.
static void OrderFixCandidates(k_vec2_t start, k_vec2_t spot, int count)
{
    int i, k;

    for (i = 0; i < count; ++i)
    {
        double least = K_Path_LeastWay(start, fix_candidates[i].at)
                     + K_Path_LeastWay(fix_candidates[i].at, spot);

        for (k = i; k > 0 && fix_least[fix_order[k - 1]] > least; --k)
        {
            fix_order[k] = fix_order[k - 1];
        }

        fix_order[k] = i;
        fix_least[i] = least;
    }
}

// The extra way an item takes: there and on to where the route goes, less
// the way left; the way there (the work at a fix counted); and the health
// the floors take there and on, less what they take on the way left. Ways
// with their waits (WAIT_WAY). With fixes allowed, a way there blocked by
// one fix the brain knows (FIX_WORK) is counted through it, and fixed set.
// risk, if not NULL, gets what the monsters in the areas the detour goes
// through and the route does not would cost (WayRisk; not on the way to a
// fix); through, if not NULL, what those on the way there would cost, the
// route's too (WayThrough; 0 through a fix: never in a fight). Returns
// false if there is no way there, or none on, or the waits there come to
// more than wait_most. The fix's spots are looked at by the least way
// through them first (OrderFixCandidates), FIX_SPOTS at most, and none
// that could not beat the best way found (the user, E1M9: a box behind
// barrels looked at through all 48 spots round them, two route searches
// each, every second, the game stalling).
static boolean ItemDetour(k_vec2_t start, int cell, k_vec2_t spot, k_vec2_t end,
                          double remaining, double remaining_damage, boolean fixes,
                          double wait_most, double *detour, double *way, double *damage,
                          boolean *fixed, double *risk, double *through)
{
    goal_type_t type = GOAL_GOTO;
    int param = -1, want, portal, from, key_door, count, i;
    int spot_area = K_Area_Of(K_Map_CellAt(spot.x, spot.y));
    boolean later;
    double there = -1.0, there_damage = 0.0, there_risk = 0.0, there_through = 0.0;
    goal_t fix;

    *fixed = false;
    ++risk_stamp;
    ++through_stamp;
    K_Path_Find(&probe, start, cell, spot);

    if (probe.result == K_PATH_FOUND)
    {
        if (probe.wait > wait_most)
        {
            return false;
        }

        there = probe.length + probe.wait * WAIT_WAY;
        there_damage = probe.damage;
        there_risk = WayRisk(&probe, spot_area);
        there_through = WayThrough(&probe, K_Area_Of(cell));
    }
    else if (fixes && probe.result == K_PATH_BLOCKED
          && RouteFix(&probe, &type, &param, &want, &portal, &from, &later, &key_door)
          && (type == GOAL_BARREL || type == GOAL_TRIGGER) && key_door < 0 && !later)
    {
        // The fix as PlanGoal would push it, and its nearest spot.
        memset(&fix, 0, sizeof(fix));
        fix.type = type;
        fix.param = param;
        fix.want = want;
        fix.portal = portal;
        fix.from = from;
        fix.pinch_cell = -1;

        if (type == GOAL_TRIGGER && portal < 0)
        {
            fix.pinch_cell = from;
            fix.pinch_at = probe.blocked_at;
        }

        count = Candidates(&fix, fix_candidates, MAX_CANDIDATES);
        OrderFixCandidates(start, spot, count);

        for (i = 0; i < count && i < FIX_SPOTS; ++i)
        {
            k_vec2_t at = fix_candidates[fix_order[i]].at;
            double to_fix, to_fix_damage, to_fix_wait;

            if (there >= 0.0 && fix_least[fix_order[i]] + FIX_WORK >= there)
            {
                break;
            }

            // Not a barrel's spot past a shut door: a fix of its own first.
            if (fix_candidates[fix_order[i]].door >= 0)
            {
                continue;
            }

            K_Path_Find(&fix_probe, start, cell, at);

            if (fix_probe.result != K_PATH_FOUND)
            {
                continue;
            }

            to_fix = fix_probe.length + fix_probe.wait * WAIT_WAY;
            to_fix_damage = fix_probe.damage;
            to_fix_wait = fix_probe.wait;

            if (there >= 0.0 && to_fix + K_Path_LeastWay(at, spot) + FIX_WORK >= there)
            {
                continue;
            }

            K_Path_Find(&fix_probe, at, -1, spot);

            if (OpenOrFixed(&fix_probe, true, type, param)
             && to_fix_wait + fix_probe.wait <= wait_most
             && (there < 0.0
                 || to_fix + fix_probe.length + fix_probe.wait * WAIT_WAY + FIX_WORK < there))
            {
                there = to_fix + fix_probe.length + fix_probe.wait * WAIT_WAY + FIX_WORK;
                there_damage = to_fix_damage + fix_probe.damage;
                there_through = 0.0;
            }
        }

        *fixed = there >= 0.0;
    }

    if (there < 0.0)
    {
        return false;
    }

    // The way on from the spot: through the doors opened on the way there
    // that stay open (K_Graph_AssumeOpen).
    if (!*fixed)
    {
        LeaveOpen(&probe);
    }

    K_Path_Find(&probe, spot, -1, end);
    K_Graph_AssumeOpen(-1);

    if (!OpenOrFixed(&probe, *fixed, type, param))
    {
        return false;
    }

    *detour = SDL_max(0.0, there + probe.length + probe.wait * WAIT_WAY - remaining);
    *way = there;
    *damage = SDL_max(0.0, there_damage + probe.damage - remaining_damage);

    if (risk != NULL)
    {
        *risk = there_risk + WayRisk(&probe, spot_area);
    }

    if (through != NULL)
    {
        *through = there_through;
    }

    return true;
}

// ItemDetour for an item from the best of its spots: where it lies, and the
// floors around that touch it (k_items.h: off) - the route picks, as for a
// key on a ledge.
static boolean ItemSpotDetour(const k_item_t *item, k_vec2_t start, int cell, k_vec2_t end,
                              double remaining, double remaining_damage, boolean fixes,
                              double *detour, double *way, double *damage, boolean *fixed,
                              double *risk, double *through)
{
    boolean found = false;
    int k;

    for (k = -1; k < item->num_off; ++k)
    {
        double d, w, dm, r = 0.0, t = 0.0;
        boolean f;

        if (ItemDetour(start, cell, k < 0 ? item->spot : item->off[k], end, remaining,
                       remaining_damage, fixes, ITEM_WAIT_MOST, &d, &w, &dm, &f,
                       risk != NULL ? &r : NULL, through != NULL ? &t : NULL)
         && (!found || d < *detour))
        {
            found = true;
            *detour = d;
            *way = w;
            *damage = dm;
            *fixed = f;

            if (risk != NULL)
            {
                *risk = r;
            }

            if (through != NULL)
            {
                *through = t;
            }
        }
    }

    return found;
}

// Going on by a radiation suit: what the way there costs, and the way on
// from it with a fresh suit worn, the waits and the floors counted
// (WAIT_WAY, DAMAGE_WAY); the way there; and the extra way, less the way
// left (remaining). Returns false if there is no open way there, or none
// on.
static boolean SuitDetour(k_vec2_t start, int cell, k_vec2_t spot, k_vec2_t end,
                          double remaining, double *cost, double *way, double *detour)
{
    K_Path_Find(&probe, start, cell, spot);

    if (probe.result != K_PATH_FOUND || probe.wait > ITEM_WAIT_MOST)
    {
        return false;
    }

    *way = probe.length + probe.wait * WAIT_WAY;
    *cost = *way + probe.damage * DAMAGE_WAY;
    K_Path_AssumeSuit(IRONTICS);
    K_Path_Find(&probe, spot, -1, end);
    K_Path_AssumeSuit(-1.0);

    if (probe.result != K_PATH_FOUND)
    {
        return false;
    }


    *cost += probe.length + probe.wait * WAIT_WAY + probe.damage * DAMAGE_WAY;
    *detour = SDL_max(0.0, *way + probe.length + probe.wait * WAIT_WAY - remaining);
    return true;
}

// Pickups (plan, 7.5): an item worth a detour - one whose extra way, there
// and on to where the route goes, is within how far out of its way the
// player may go for it now (k_items.h) - goes on top of the chain, and
// when it is picked up the goal below is planned again from there. Looked
// for only while the player follows a route to the goal on top: not while
// working a door, lift or switch, running a jump, going where a click sent
// it, or waiting for a switch's sector. In a fight, only what is worth
// breaking it off for (k_items.h: urgent), within FIGHT_REACH but for
// health when CRITICAL (UpdateEscape). Out of a fight, an item behind one
// fix is gone for through it (FIX_WORK). An item the player could not get
// on from (down in a pit, past a one-way drop) is not gone for. Returns
// true if one was added.
static boolean ConsiderItems(goal_t *goal)
{
    const k_navstatus_t *nav = K_Nav_GetStatus();
    const k_path_t *path = K_Nav_GetPath();
    boolean critical = K_Condition_Get()->health == K_NEED_CRITICAL;
    boolean fighting = K_Combat_GetStatus()->battle;
    int routes = critical || fighting ? MAX_ITEM_ROUTES : ITEM_ROUTES;
    int order[MAX_ITEM_ROUTES];
    double order_score[MAX_ITEM_ROUTES];
    int found = 0, best = -1;
    double best_slack = 0.0, best_detour = 0.0, best_way = 0.0, best_damage = 0.0;
    double best_risk = 0.0, best_cost = 0.0, best_through = 0.0;
    double remaining_way = 0.0, remaining_damage = 0.0, remaining_cost = 0.0;
    boolean best_fixed = false, best_first = false, best_suit = false, suits = false;
    boolean best_sphere = false;
    const k_item_t *chosen;
    k_pose_t player;
    k_vec2_t start, end;
    goal_t *item_goal;
    char name[64], guarded[64] = "", floors[64] = "", risky[64] = "", below[96] = "";
    int detour_at = -1;     // the detour on that goes first, which a nearer one may replace
    boolean stacking = false;   // a small one may go on top of the item gone for
    boolean stackable, best_stack = false;
    boolean lives_only;         // on top only health and armour (STACK_SHARE)
    int on_items = 0;
    int alt = -1;           // the best of those that would go on top, if a
    double alt_slack = 0.0, alt_detour = 0.0, alt_way = 0.0, alt_damage = 0.0;
    double alt_risk = 0.0, alt_cost = 0.0, alt_through = 0.0;   // replacement is refused
    boolean alt_first = false;
    double on_way = 1.0e9, on_through = 0.0;    // the detour on, to be replaced
    int i, k;

    // Health just went CRITICAL: looked at now, not within the second (Doom
    // II MAP02: a demon bit the bot 0.9 s after, before the next look).
    if (critical && !was_critical)
    {
        item_check_tic = 0;
    }

    was_critical = critical;

    // A goal not planned yet (the one below an item just picked up) does not
    // use the look up: it comes once it is. Nothing takes the player off the
    // exit floor (E1M8: health is what it takes), nor off a fall back, the
    // fix in its way included, nor off a way back being opened, nor past a
    // lift or a door that goes back but for health at CRITICAL (PASS_TICS),
    // nor off a chase (ConsiderChase).
    if (leveltime < item_check_tic || !goal->planned || goal->staying || FallingBack() >= 0
     || OpeningWayBack() >= 0 || (pass_portal >= 0 && !critical) || ChaseOn() >= 0)
    {
        return false;
    }

    item_check_tic = leveltime + ITEM_CHECK_TICS;

    for (i = 0; i < depth; ++i)
    {
        on_items += goals[i].type == GOAL_ITEM;
    }

    stackable = !fighting && on_items > 0 && on_items < STACK_DEPTH && goal->type == GOAL_ITEM;
    lives_only = stackable && GoesFirst(K_Items_Get(goal->param), critical);

    // One detour at a time, but for small ones on top (STACK_SHARE): not
    // while one is on, nor its fix in front of it
    // (Doom II MAP04: the shotgun behind bars was added again over the
    // switch pushed for it, every second). But health LOW or CRITICAL goes
    // before any other, while some health worth leaving a fight for is there
    // (k_items.h: escape): that one is dropped, with the fixes pushed for it,
    // and health looked for once the goal below is planned again (MAP10: an
    // armour bonus behind a gun switch kept the bot fighting at 20 health, a
    // medikit in reach, until it died). So does the ammo when ammo is LOW or
    // firepower CRITICAL, but for health CRITICAL (MAP10: out of shells, the
    // shotguns dropped beside the bot were passed for a cell charge 2000
    // off, and the bot fought on with the pistol). When LOW, not one that
    // goes first itself (GoesFirst: a weapon at a pistol start, that ammo);
    // that one gives way only to another that goes first, its way there
    // REPLACE_MARGIN shorter (E2M5: a chaingun behind a walk-over line 2788
    // off was gone for at the start; a shotgun guy then dropped his shotgun
    // beside the bot, never looked at, and the bot fought demons with the
    // pistol).
    for (i = 0; i < depth && detour_at < 0; ++i)
    {
        const k_item_t *on;
        boolean for_health;

        if (goals[i].type != GOAL_ITEM)
        {
            continue;
        }

        on = K_Items_Get(goals[i].param);

        // Running for the sphere, nothing else meanwhile; any other gives way
        // to it (SphereRun).
        if (on->invuln && on->escape)
        {
            return false;
        }

        if (i > 0 && !on->invuln && SphereRun())
        {
            GoalName(&goals[i], name, sizeof(name));
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s dropped - the %s on the player, the "
                  "invulnerability goes first", name, K_Combat_KindName(K_Combat_Boss()));
            depth = i;
            goals[depth - 1].planned = false;
            K_Nav_Clear();
            item_check_tic = 0;
            return true;
        }

        if (i > 0 && GoesFirst(on, critical) && (on->kind == K_ITEM_HEALTH || !critical))
        {
            detour_at = i;
            continue;
        }

        if (i == 0 || on->kind == K_ITEM_HEALTH || (!critical && GoesFirst(on, critical))
         || !EscapeFor(critical, &for_health))
        {
            if (!stackable)
            {
                return false;
            }

            stacking = true;
            break;
        }

        GoalName(&goals[i], name, sizeof(name));

        if (for_health)
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s dropped - health %s, health goes first", name,
                  critical ? "CRITICAL" : "LOW");
        }
        else
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s dropped - %s, ammo goes first", name,
                  K_Condition_Get()->firepower == K_NEED_CRITICAL ? "firepower CRITICAL"
                                                                  : "ammo LOW");
        }

        if (goals[i].param < item_skip_size)
        {
            item_skip[goals[i].param] = leveltime + ITEM_SKIP_TICS;
        }

        depth = i;
        goals[depth - 1].planned = false;
        K_Nav_Clear();
        item_check_tic = 0;
        return true;
    }

    // On top of one that goes first, too, as well as in its place.
    stacking |= detour_at >= 0 && stackable;

    // In a fight nav waits: it was free to when the fight began.
    if (depth >= MAX_GOALS || !goal->planned
     || goal->type == GOAL_GOTO || (goal->type == GOAL_TRIGGER && (goal->used || goal->moving))
     || nav->state != K_NAV_DRIVING || path == NULL || path->num_points < 1
     || (!fighting && (nav->action != K_ACTION_NONE || !K_Nav_CanPause()))
     || !K_Map_GetPlayer(&player))
    {
        return false;
    }

    start.x = player.x;
    start.y = player.y;
    end = path->points[path->num_points - 1];

    if (item_skip_size < K_Items_Count())
    {
        item_skip = realloc(item_skip, K_Items_Count() * 2 * sizeof(*item_skip));
        memset(item_skip + item_skip_size, 0,
               (K_Items_Count() * 2 - item_skip_size) * sizeof(*item_skip));
        item_skip_size = K_Items_Count() * 2;
    }

    // The most promising few, by worth and distance: as the crow flies, or
    // twice that from the route ahead.
    for (i = 0; i < K_Items_Count(); ++i)
    {
        const k_item_t *item = K_Items_Get(i);
        double distance = sqrt((item->at.x - start.x) * (item->at.x - start.x)
                             + (item->at.y - start.y) * (item->at.y - start.y));
        double reach = ItemReach(item, fighting);
        double score;

        suits |= item->suit && item->present;

        // With one on, in its place one that goes first, or on top a small
        // one (STACK_SHARE).
        if (stacking && !(detour_at >= 0 && GoesFirst(item, critical)))
        {
            reach = SDL_min(reach * STACK_SHARE, STACK_MOST);
        }

        if (item->present && reach > 0.0 && distance > reach)
        {
            distance = SDL_min(distance, 2.0 * RouteDistance(path, nav->target, start, item->at));
        }

        if (!item->present || reach <= 0.0 || distance > reach
         || leveltime < item_skip[i] || IsFailed(GOAL_ITEM, i) || OnChain(i)
         || (fighting && !item->urgent)
         || (detour_at >= 0 && !stacking && !GoesFirst(item, critical))
         || (stacking && lives_only && !(detour_at >= 0 && GoesFirst(item, critical))
             && item->kind != K_ITEM_HEALTH && item->kind != K_ITEM_ARMOUR))
        {
            continue;
        }

        // The sphere run for before even those that go first (SphereRun).
        score = item->invuln && item->escape ? 2.0 * FIRST_SCORE - distance
              : GoesFirst(item, critical) ? FIRST_SCORE - distance
              : SDL_min(reach, RANK_REACH) / (distance + 64.0);

        // Kept best first.
        if (found == routes && score <= order_score[routes - 1])
        {
            continue;
        }

        for (k = found < routes ? found++ : routes - 1;
             k > 0 && order_score[k - 1] < score; --k)
        {
            order[k] = order[k - 1];
            order_score[k] = order_score[k - 1];
        }

        order[k] = i;
        order_score[k] = score;
    }

    suits &= !fighting && detour_at < 0 && !stacking && K_Map_SuitTics() < SUIT_KEEP;

    // The way left as the pathfinder sees it now, with its waits, and the
    // floors that hurt on it; as nav has it if there is none.
    remaining_way = nav->remaining;

    if (found > 0 || suits)
    {
        K_Path_Find(&probe, start, player.cell, end);

        if (probe.result == K_PATH_FOUND)
        {
            remaining_way = probe.length + probe.wait * WAIT_WAY;
            remaining_damage = probe.damage;
            MarkRouteAreas(&probe, 0);
        }
        else
        {
            MarkRouteAreas(path, nav->route_cell);
        }

        remaining_cost = remaining_way + remaining_damage * DAMAGE_WAY;
    }

    for (k = 0; k < found; ++k)
    {
        const k_item_t *item = K_Items_Get(order[k]);
        double reach = ItemReach(item, fighting);
        boolean first = GoesFirst(item, critical);
        boolean stacks = stacking && !(detour_at >= 0 && first);
        boolean sphere = item->invuln && item->escape;
        double detour, way, damage, risk, through, cost;
        boolean fixed;

        if (stacks)
        {
            reach = SDL_min(reach * STACK_SHARE, STACK_MOST);
        }

        if (!ItemSpotDetour(item, start, player.cell, end, remaining_way, remaining_damage,
                            !fighting && !stacks, &detour, &way, &damage, &fixed, &risk, &through))
        {
            item_skip[order[k]] = leveltime + ITEM_SKIP_TICS;
            continue;
        }

        // Health the floors on the way would take most of back: left.
        if (item->kind == K_ITEM_HEALTH && damage > item->together * HEALTH_NET)
        {
            item_skip[order[k]] = leveltime + ITEM_SKIP_TICS;
            continue;
        }

        if (reach > ANY_REACH && risk >= RISKY_ANY && !(critical && item->kind == K_ITEM_HEALTH))
        {
            reach = RISKY_REACH;
        }

        // The sphere run for, a boss on the player (SphereRun): the way there
        // is what counts, whatever is on it - the boss is worse, and nothing
        // hurts the player once it is in hand.
        if (sphere)
        {
            detour = way;
            risk = 0.0;
            through = 0.0;
        }

        detour += (damage + risk) * DAMAGE_WAY;

        if (sphere && detour > reach)
        {
            item_skip[order[k]] = leveltime + ITEM_SKIP_TICS;
            continue;
        }

        // Of those that go first, the soonest in hand, the monsters inside
        // the areas on the way there counted too, the route's as well: the
        // run leaves it (the user, E2M9: out of plasma and shells, the bot
        // held short of three barons with the pistol for 20 s, the rocket
        // launcher beyond them 534 off, a chaingun behind it clear). But the
        // sphere run for before any.
        cost = way + through * DAMAGE_WAY;

        if (detour <= reach
         && (best < 0 || (sphere && !best_sphere)
             || (!best_sphere
                 && ((first && !best_first)
                     || (first && best_first && cost < best_cost)
                     || (!first && !best_first && reach - detour > best_slack)))))
        {
            best = order[k];
            best_slack = reach - detour;
            best_detour = detour;
            best_way = way;
            best_cost = cost;
            best_through = through;
            best_damage = damage;
            best_risk = risk;
            best_fixed = fixed;
            best_first = first;
            best_stack = stacks;
            best_sphere = sphere;
        }

        if (stacks && detour <= reach && (alt < 0 || reach - detour > alt_slack))
        {
            alt = order[k];
            alt_slack = reach - detour;
            alt_detour = detour;
            alt_way = way;
            alt_cost = cost;
            alt_through = through;
            alt_damage = damage;
            alt_risk = risk;
            alt_first = first;
        }
    }

    // A radiation suit, by what going on by it saves (SUIT_GAIN); not over a
    // weapon at a pistol start, nor health when CRITICAL.
    for (i = 0; suits && !best_first && i < K_Items_Count(); ++i)
    {
        const k_item_t *item = K_Items_Get(i);
        double cost, way, detour;

        if (!item->suit || !item->present || item->reach <= 0.0 || leveltime < item_skip[i]
         || IsFailed(GOAL_ITEM, i)
         || !SuitDetour(start, player.cell, item->spot, end, remaining_way, &cost, &way, &detour)
         || remaining_cost - cost <= SUIT_GAIN || (best >= 0 && remaining_cost - cost <= best_slack))
        {
            continue;
        }

        best = i;
        best_slack = remaining_cost - cost;
        best_detour = detour;
        best_way = way;
        best_cost = way;
        best_through = 0.0;
        best_damage = 0.0;
        best_risk = 0.0;
        best_fixed = false;
        best_suit = true;
    }

    if (best < 0)
    {
        return false;
    }

    // In place of the detour on, only when clearly sooner in hand; it and
    // the fixes pushed for it go, the goal below it staying as it was. When
    // not, the best that would go on top of it (E1M8: the soulsphere, 1178
    // out of the way of the first shotgun, lost to the other shotgun, which
    // was then no better than the one gone for).
    if (!best_stack && detour_at >= 0)
    {
        const k_item_t *on = K_Items_Get(goals[detour_at].param);
        double on_detour, on_damage;
        boolean on_fixed;

        // Through the fixes pushed for it, which are on the stack (E2M5:
        // walking to line 389 for the box of shells behind the door it
        // opens, the door's fix was not Allowed, being there, the box
        // counted as out of reach, the shells won - and the box the next
        // second, for 12 s). The monsters on the way counted, as above.
        fixes_stacked = true;
        ItemSpotDetour(on, start, player.cell, end, remaining_way, remaining_damage, true,
                       &on_detour, &on_way, &on_damage, &on_fixed, NULL, &on_through);
        fixes_stacked = false;

        if (best_cost + REPLACE_MARGIN >= on_way + on_through * DAMAGE_WAY)
        {
            if (alt < 0)
            {
                return false;
            }

            best = alt;
            best_slack = alt_slack;
            best_detour = alt_detour;
            best_way = alt_way;
            best_cost = alt_cost;
            best_through = alt_through;
            best_damage = alt_damage;
            best_risk = alt_risk;
            best_fixed = false;
            best_first = alt_first;
            best_suit = false;
            best_stack = true;
        }
    }

    if (!best_stack && detour_at >= 0)
    {
        const k_item_t *on = K_Items_Get(goals[detour_at].param);

        GoalName(&goals[detour_at], name, sizeof(name));

        if (on_way < 1.0e9 && (best_through >= 0.5 || on_through >= 0.5))
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s dropped - the %s is the better way, %.0f units "
                  "and %.0f health at risk on it against %.0f and %.0f", name,
                  K_Items_Get(best)->name, best_way, best_through, on_way, on_through);
        }
        else if (on_way < 1.0e9)
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s dropped - the %s is in hand sooner, %.0f units "
                  "against %.0f", name, K_Items_Get(best)->name, best_way, on_way);
        }
        else
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s dropped - the %s is in hand sooner, %.0f units "
                  "(no way from here to the %s)", name, K_Items_Get(best)->name, best_way,
                  on->name);
        }
        depth = detour_at;
        K_Nav_Clear();
    }

    // On top of the item gone for (STACK_SHARE): it stays, gone for again
    // once this one is picked up.
    else if (best_stack)
    {
        GoalName(goal, name, sizeof(name));
        M_snprintf(below, sizeof(below), ", on the way to %s", name);
    }

    // A switch not worked yet: going for it again after the item is not
    // another use of its line (MAX_TRIGGERS). Already so for one in place
    // of a detour.
    else if (goal->type == GOAL_TRIGGER && line_uses != NULL && goal->chosen.line >= 0
          && line_uses[goal->chosen.line] > 0)
    {
        --line_uses[goal->chosen.line];
    }

    if (best_sphere && K_Combat_Boss() >= 0)
    {
        M_snprintf(below, sizeof(below), " (the way there: a run for it, the %s on the player)",
                   K_Combat_KindName(K_Combat_Boss()));
    }

    item_goal = Push(GOAL_ITEM, best);
    item_goal->fixable = best_fixed;
    GoalName(item_goal, name, sizeof(name));
    chosen = K_Items_Get(best);

    if (chosen->danger >= 0.5)
    {
        M_snprintf(guarded, sizeof(guarded), ", guarded: %.0f health at risk", chosen->danger);
    }

    if (best_suit)
    {
        M_snprintf(floors, sizeof(floors), ", going on by it saves %.0f in way and floors "
                   "that hurt", best_slack);
    }
    else if (best_damage >= 0.5)
    {
        M_snprintf(floors, sizeof(floors), ", %.0f health off floors that hurt", best_damage);
    }

    if (best_risk >= 0.5)
    {
        M_snprintf(risky, sizeof(risky), ", %.0f health at risk in the areas on the way",
                   best_risk);
    }

    K_Log(K_LOG_BRAIN, K_LOG_INFO, "added %s at (%.0f,%.0f) - %.0f units out of the way%s%s%s%s, "
          "worth %.0f (%.0f with those beside it)%s", name, chosen->at.x, chosen->at.y,
          best_detour, best_fixed ? " through what blocks it" : "", below, floors, risky,
          chosen->worth, chosen->together, guarded);
    return true;
}

// A spot the player fits at, in the cell, or false.
static boolean RoomAt(int cell, k_vec2_t *spot)
{
    *spot = K_Map_Get()->cells[cell].centroid;
    return K_Graph_Clearance(*spot, PLAYER_RADIUS + 1.0) > PLAYER_RADIUS
        && K_Map_CellAt(spot->x, spot->y) == cell;
}

// Whether a point lies within EXIT_CLEAR of a walk-over exit line.
static boolean NearExitLine(k_vec2_t at)
{
    const k_map_t *map = K_Map_Get();
    int i, secret;

    for (i = 0; i < map->num_lines; ++i)
    {
        const k_line_t *line = &map->lines[i];
        double dx = line->v2.x - line->v1.x, dy = line->v2.y - line->v1.y;
        double length2 = dx * dx + dy * dy, t;

        if (K_Specials_ExitTrigger(line->special, &secret) != K_TRIGGER_CROSS)
        {
            continue;
        }

        t = length2 > 0.0 ? ((at.x - line->v1.x) * dx + (at.y - line->v1.y) * dy) / length2 : 0.0;
        t = SDL_max(0.0, SDL_min(1.0, t));

        if (hypot(line->v1.x + dx * t - at.x, line->v1.y + dy * t - at.y) < EXIT_CLEAR)
        {
            return true;
        }
    }

    return false;
}

// An explorer's places, on level load: the areas worth a visit and the
// secrets (PLACE_AREA_REACH), each with a spot to go to - an area's largest
// cell's middle, else any of its cells' the player fits at; a secret's
// largest cell the player fits in.
static void BuildPlaces(void)
{
    const k_map_t *map = K_Map_Get();
    const k_areamap_t *areamap = K_Area_Get();
    int a, i;

    free(places);
    places = calloc(areamap->num_areas + SDL_max(map->num_sectors, 1), sizeof(*places));
    num_places = 0;

    if (!explorer)
    {
        return;
    }

    for (a = 0; a < areamap->num_areas; ++a)
    {
        const k_area_t *area = &areamap->areas[a];
        place_t *place = &places[num_places];
        int cell = K_Map_CellAt(area->spot.x, area->spot.y);
        boolean fits = cell >= 0 && K_Area_Of(cell) == a && RoomAt(cell, &place->spot);

        if (area->passage || area->floor_area < PLACE_AREA_MIN || area->hurts >= 0.5)
        {
            continue;
        }

        for (i = 0; i < area->num_cells && !fits; ++i)
        {
            fits = RoomAt(areamap->cells[area->first_cell + i], &place->spot);
        }

        if (fits && !NearExitLine(place->spot))
        {
            place->area = a;
            place->sector = -1;
            place->in_area = a;
            ++num_places;
        }
    }

    for (i = 0; i < map->num_sectors; ++i)
    {
        place_t *place = &places[num_places];
        double largest = 0.0;
        k_vec2_t spot;
        int c;

        if (!K_Specials_SecretSector(i))
        {
            continue;
        }

        for (c = 0; c < map->num_cells; ++c)
        {
            if (map->subsectors[map->cells[c].subsector].sector == i
             && map->cells[c].area > largest && RoomAt(c, &spot))
            {
                largest = map->cells[c].area;
                place->spot = spot;
            }
        }

        if (largest > 0.0 && !NearExitLine(place->spot))
        {
            place->area = -1;
            place->sector = i;
            place->in_area = K_Area_Of(K_Map_CellAt(place->spot.x, place->spot.y));
            ++num_places;
        }
    }

    K_Log(K_LOG_BRAIN, K_LOG_INFO, "explorer: %d places - areas to visit and secrets to find",
          num_places);
}

// Whether a place has been visited or looked into, or its secret found.
static boolean PlaceDone(int p)
{
    return places[p].sector >= 0 ? !K_Specials_SecretSector(places[p].sector)
                                 : K_Area_Get()->areas[places[p].area].visited || places[p].seen;
}

// Whether an area's spot is in sight of the player, within
// PLACE_SEEN_REACH (PlaceDone).
static void LookAtPlace(int p, const k_pose_t *player)
{
    place_t *place = &places[p];
    int cell = K_Map_CellAt(place->spot.x, place->spot.y);
    double floor;

    if (place->seen || place->sector >= 0 || cell < 0
     || hypot(place->spot.x - player->x, place->spot.y - player->y) > PLACE_SEEN_REACH)
    {
        return;
    }

    floor = K_Map_CellFloor(cell);
    place->seen = K_Sight_Check(player->x, player->y, player->floor + EYE_HEIGHT,
                                place->spot.x, place->spot.y, floor, floor + PLAYER_HEIGHT);
}

// At the level's end: how much of it an explorer saw.
static void LogExplored(void)
{
    int areas = 0, visited = 0, dropped = 0, p;

    for (p = 0; explorer && p < num_places; ++p)
    {
        areas += places[p].area >= 0;
        visited += places[p].area >= 0 && PlaceDone(p);
        dropped += places[p].tries >= PLACE_TRIES && !PlaceDone(p);
    }

    if (explorer)
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "explorer: %d of %d areas visited or looked into, %d "
              "places given up on, %.1f s spent on places off the way", visited, areas, dropped,
              (double) explore_tics / TICRATE);
    }
}

// How many places an explorer may still go to in the sweep before the exit
// (PLACE_SWEEP_REACH), there and back as the crow flies.
static int PlacesSweepable(void)
{
    k_pose_t player;
    int count = 0, p;

    for (p = 0; explorer && p < num_places && K_Map_GetPlayer(&player); ++p)
    {
        count += !PlaceDone(p) && places[p].tries < PLACE_TRIES && leveltime >= places[p].skip
              && 2.0 * hypot(places[p].spot.x - player.x, places[p].spot.y - player.y)
                 <= PLACE_SWEEP_REACH;
    }

    return count;
}

// How far out of its way the player may go for a place: on top of an item
// gone for, STACK_SHARE of it, in the sweep too; else any way in the sweep.
// Stacked any way, the nearest place by the way there was gone to first and
// the item after, from wherever that was (the user, E1M5: the soulsphere 278
// off, the secret doors to it opened, then left for an area by the exit 3272
// off, "16989 units out of the way", and gone back for across the map).
static double PlaceReach(const place_t *place, boolean stacking)
{
    double reach = place->sector < 0 ? PLACE_AREA_REACH : PLACE_SECRET_REACH;

    return stacking ? reach * STACK_SHARE : sweep_since > 0 ? PLACE_SWEEP_REACH : reach;
}

// An explorer's places off the way (PLACE_AREA_REACH), looked for as items
// are (ConsiderItems). Returns true if one was added.
static boolean ConsiderPlaces(goal_t *goal)
{
    const k_navstatus_t *nav = K_Nav_GetStatus();
    const k_path_t *path = K_Nav_GetPath();
    const k_conditionstatus_t *condition = K_Condition_Get();
    int routes = sweep_since > 0 ? PLACE_SWEEP_ROUTES : PLACE_ROUTES;
    int order[PLACE_SWEEP_ROUTES];
    double order_score[PLACE_SWEEP_ROUTES];
    int boss_areas[2 * MAX_BOSSES];
    k_vec2_t at[MAX_BOSSES];
    int found = 0, best = -1, best_one_way = -1, num_boss = 0, count, i, k;
    double best_cost = 0.0, best_detour = 0.0, best_risk = 0.0;
    double one_way_cost = 0.0, one_way_detour = 0.0, one_way_risk = 0.0;
    double remaining_way, remaining_damage = 0.0;
    boolean best_fixed = false, one_way_fixed = false;
    boolean stacking;           // on top of an item gone for (STACK_SHARE)
    k_pose_t player;
    k_vec2_t start, end;
    goal_t *place_goal;
    char name[64], risky[64] = "", below[96] = "";

    if (!explorer || num_places == 0 || leveltime < place_check_tic
     || explore_tics >= EXPLORE_MOST)
    {
        return false;
    }

    place_check_tic = leveltime + PLACE_CHECK_TICS;

    // As for an item, and only out of a fight, in good shape, one detour at
    // a time; not on the way to a boss, nor anywhere near one.
    if (!goal->planned || goal->staying || FallingBack() >= 0 || OpeningWayBack() >= 0
     || (pass_portal >= 0 && sweep_since <= 0) || ChaseOn() >= 0 || depth >= MAX_GOALS
     || goal->type == GOAL_GOTO || goal->type == GOAL_TELEFRAG || goal->type == GOAL_BARREL
     || goal->type == GOAL_KILL || (goal->type == GOAL_TRIGGER && (goal->used || goal->moving))
     || K_Combat_GetStatus()->battle || K_Combat_Boss() >= 0
     || condition->health != K_NEED_OK || condition->ammo == K_NEED_LOW
     || nav->state != K_NAV_DRIVING || path == NULL || path->num_points < 1
     || nav->action != K_ACTION_NONE || !K_Nav_CanPause() || !K_Map_GetPlayer(&player))
    {
        return false;
    }

    for (i = 0; i < depth; ++i)
    {
        if (goals[i].type == GOAL_PLACE
         || (goals[i].type == GOAL_ITEM
             && (i < depth - 1 || GoesFirst(K_Items_Get(goals[i].param), false))))
        {
            return false;
        }
    }

    stacking = goal->type == GOAL_ITEM;
    count = SDL_min(K_Specials_FindMonsters(MT_CYBORG, at, MAX_BOSSES), MAX_BOSSES);

    for (i = 0; i < count; ++i)
    {
        boss_areas[num_boss++] = K_Area_Of(K_Map_CellAt(at[i].x, at[i].y));
    }

    count = SDL_min(K_Specials_FindMonsters(MT_SPIDER, at, MAX_BOSSES), MAX_BOSSES);

    for (i = 0; i < count; ++i)
    {
        boss_areas[num_boss++] = K_Area_Of(K_Map_CellAt(at[i].x, at[i].y));
    }

    start.x = player.x;
    start.y = player.y;
    end = path->points[path->num_points - 1];

    // The most promising few, as items are ranked.
    for (i = 0; i < num_places; ++i)
    {
        const place_t *place = &places[i];
        double reach = PlaceReach(place, stacking);
        double distance = hypot(place->spot.x - start.x, place->spot.y - start.y);
        double score;
        boolean boss = false;

        for (k = 0; k < num_boss; ++k)
        {
            boss |= boss_areas[k] >= 0 && boss_areas[k] == place->in_area;
        }

        if (place->tries >= PLACE_TRIES || leveltime < place->skip || leveltime < place->later
         || boss || PlaceDone(i))
        {
            continue;
        }

        if (distance > reach)
        {
            distance = SDL_min(distance,
                               2.0 * RouteDistance(path, nav->target, start, place->spot));
        }

        if (distance > reach)
        {
            continue;
        }

        score = reach / (distance + 64.0);

        if (found == routes && score <= order_score[routes - 1])
        {
            continue;
        }

        for (k = found < routes ? found++ : routes - 1;
             k > 0 && order_score[k - 1] < score; --k)
        {
            order[k] = order[k - 1];
            order_score[k] = order_score[k - 1];
        }

        order[k] = i;
        order_score[k] = score;
    }

    if (found == 0)
    {
        return false;
    }

    // The way left as the pathfinder sees it now.
    K_Path_Find(&probe, start, player.cell, end);

    if (probe.result == K_PATH_FOUND)
    {
        remaining_way = probe.length + probe.wait * WAIT_WAY;
        remaining_damage = probe.damage;
        MarkRouteAreas(&probe, 0);
    }
    else
    {
        remaining_way = nav->remaining;
        MarkRouteAreas(path, nav->route_cell);
    }

    for (k = 0; k < found; ++k)
    {
        place_t *place = &places[order[k]];
        double reach = PlaceReach(place, stacking);
        double detour, way, damage, risk, cost;
        boolean fixed, one_way = false;

        // In the sweep, none whose way there could not be shorter than the
        // nearest found (K_Path_LeastWay; the user, E1M9: 16 places looked
        // at by route at each turn of the sweep, up to 95 route searches in
        // a tic).
        if (sweep_since > 0 && best >= 0 && K_Path_LeastWay(start, place->spot) >= best_cost)
        {
            continue;
        }

        if (!ItemDetour(start, player.cell, place->spot, end, remaining_way, remaining_damage,
                        !stacking, sweep_since > 0 ? PLACE_WAIT_MOST : ITEM_WAIT_MOST, &detour,
                        &way, &damage, &fixed, &risk, NULL))
        {
            if (stacking)
            {
                place->skip = leveltime + ITEM_SKIP_TICS;
            }
            else
            {
                SetPlaceAside(order[k]);
            }

            continue;
        }

        // The monsters inside the place's own area, unless the route goes
        // through it anyway.
        if (place->in_area >= 0 && place->in_area < marks_size
         && route_marks[place->in_area] != route_stamp)
        {
            risk += K_Area_Get()->areas[place->in_area].inside;
        }

        detour += (damage + risk) * DAMAGE_WAY;

        if (detour > reach)
        {
            place->skip = leveltime + ITEM_SKIP_TICS;
            continue;
        }

        // In the sweep the nearest by the way there, the danger on it
        // counted; one with no way back only once nothing else is left
        // (PLACE_SWEEP_REACH). Else the least out of the way.
        if (sweep_since > 0)
        {
            K_Path_Find(&probe, place->spot, -1, start);
            one_way = probe.result != K_PATH_FOUND;
            cost = way + (damage + risk) * DAMAGE_WAY;
        }
        else
        {
            cost = detour - reach;
        }

        if (one_way)
        {
            if (best_one_way < 0 || cost < one_way_cost)
            {
                best_one_way = order[k];
                one_way_cost = cost;
                one_way_detour = detour;
                one_way_risk = risk;
                one_way_fixed = fixed;
            }

            continue;
        }

        if (best < 0 || cost < best_cost)
        {
            best = order[k];
            best_cost = cost;
            best_detour = detour;
            best_risk = risk;
            best_fixed = fixed;
        }
    }

    // Only places with no way back looked at: gone to when they are all
    // there is left (fewer than were looked at), or looked past once
    // already; else looked past for a while, for the others.
    if (best < 0 && best_one_way >= 0
     && (found < routes || places[best_one_way].later != 0))
    {
        best = best_one_way;
        best_detour = one_way_detour;
        best_risk = one_way_risk;
        best_fixed = one_way_fixed;
    }
    else if (best < 0)
    {
        for (k = 0; k < found; ++k)
        {
            if (places[order[k]].later == 0)
            {
                places[order[k]].later = leveltime + ITEM_SKIP_TICS;
            }
        }
    }

    if (best < 0)
    {
        return false;
    }

    // A switch not worked yet: going for it again after is not another use
    // of its line (MAX_TRIGGERS), as after an item.
    if (goal->type == GOAL_TRIGGER && line_uses != NULL && goal->chosen.line >= 0
     && line_uses[goal->chosen.line] > 0)
    {
        --line_uses[goal->chosen.line];
    }

    if (stacking)
    {
        GoalName(goal, name, sizeof(name));
        M_snprintf(below, sizeof(below), ", on the way to %s", name);
    }

    place_goal = Push(GOAL_PLACE, best);
    place_goal->fixable = best_fixed;
    GoalName(place_goal, name, sizeof(name));

    if (best_risk >= 0.5)
    {
        M_snprintf(risky, sizeof(risky), ", %.0f health at risk on the way", best_risk);
    }

    K_Log(K_LOG_BRAIN, K_LOG_INFO, "added %s at (%.0f,%.0f) - %.0f units out of the way%s%s%s",
          name, places[best].spot.x, places[best].spot.y, best_detour,
          best_fixed ? " through what blocks it" : "", below, risky);
    return true;
}

// Each tic: the secrets found, logged; the places in sight - one in turn,
// and the one gone to (PLACE_SEEN_REACH); the time spent on places
// (EXPLORE_MOST).
static void UpdateExplore(void)
{
    const player_t *player = &players[consoleplayer];
    k_pose_t pose;
    int i;

    if (player->secretcount != secrets_found)
    {
        secrets_found = player->secretcount;
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "a secret found - %d of %d", secrets_found, totalsecret);
    }

    if (explorer && num_places > 0 && K_Map_GetPlayer(&pose))
    {
        place_seen_next = (place_seen_next + 1) % num_places;
        LookAtPlace(place_seen_next, &pose);
    }

    for (i = 0; explorer && i < depth; ++i)
    {
        if (goals[i].type != GOAL_PLACE)
        {
            continue;
        }

        if (K_Map_GetPlayer(&pose))
        {
            LookAtPlace(goals[i].param, &pose);
        }

        if (++explore_tics == EXPLORE_MOST)
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "%d minutes spent on places off the way - no more "
                  "this level", EXPLORE_MOST / (60 * TICRATE));
        }

        break;
    }
}

// Whether the run for an item waits: its way goes into an area with
// monsters inside, not the player's own (K_Combat_MonstersAhead) - fought
// for there and held short of as on any way, not run into (E2M5: at
// firepower LOW the bot ran for a box of shells past three imps in a
// corridor, clawed; Doom II MAP14: a hold that had killed five was broken
// off for a medikit among the monsters held short of) - until the fight
// for it is over. Not for health at CRITICAL: worth any risk; nor for
// invulnerability, which makes the monsters there harmless; nor into an
// area with a boss inside, never held short of (k_combat.c: BossInside) -
// run past it, the boss's rockets left behind by a running player (the
// user, E2M8: at 36 health the soulsphere's way went into the Cyberdemon's
// area, "fought for, not run for", and the bot stood in its rockets until
// it died). Logged once a run.
static boolean RunWaits(const goal_t *goal, const k_item_t *item)
{
    char name[64];
    int area;

    if ((item->kind == K_ITEM_HEALTH && K_Condition_Get()->health == K_NEED_CRITICAL)
     || item->invuln)
    {
        return false;
    }

    area = K_Combat_MonstersAhead();

    if (area >= 0 && run_waited != goal->param)
    {
        GoalName(goal, name, sizeof(name));
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: the way goes into area %d, %d monsters inside - "
              "fought for, not run for", name, area, K_Area_Get()->areas[area].monsters);
        run_waited = goal->param;
    }

    return area >= 0;
}

// Fleeing (plan, 7.4), and dashes: while the goal on top is an item worth
// breaking a fight off for (k_items.h: urgent) - health when LOW or
// CRITICAL, and ammo when ammo is LOW or firepower CRITICAL, wherever it is
// (escape); a weapon the player lacks, or the ammo running out, from when it
// comes within FIGHT_REACH until it is picked up, while no monster is within
// DASH_CLEAR (DASH_FREE once one was) -
// combat starts no fight and breaks off the one going on; the player runs
// for it, shooting on the way at what it sees - but not while its way goes
// into a room of monsters ahead (RunWaits). The item may have fixes on
// top of it: working them is part of the run for it (MAP10: going for a
// medikit at LOW health, the bot fought an arachnotron by the walk-over line
// pushed for it, and died). With nothing to go for, fights go on as before:
// running to nowhere is no safer, and cover comes with dodging (C3).
static void UpdateEscape(void)
{
    const goal_t *goal = NULL;
    const k_item_t *item = NULL;
    char why[96];
    k_pose_t player;
    int i;

    // Falling back to a safe area, the fix in its way worked too: the fight
    // is broken off.
    if (FallingBack() >= 0)
    {
        dashing = -1;
        K_Combat_Escape("falling back to a safe area");
        return;
    }

    for (i = depth - 1; i >= 0 && goal == NULL; --i)
    {
        if (goals[i].type == GOAL_ITEM)
        {
            goal = &goals[i];
            item = K_Items_Get(goal->param);
        }
    }

    if (item == NULL || !item->urgent)
    {
        dashing = -1;
        run_waited = -1;
        K_Combat_Escape(NULL);
        return;
    }

    if (item->escape)
    {
        if (RunWaits(goal, item))
        {
            K_Combat_Escape(NULL);
            return;
        }

        if (item->invuln)
        {
            M_snprintf(why, sizeof(why), "the %s on the player, going for the %s",
                       K_Combat_Boss() >= 0 ? K_Combat_KindName(K_Combat_Boss()) : "boss",
                       item->name);
        }
        else
        {
            M_snprintf(why, sizeof(why), "%s, going for the %s",
                       item->kind != K_ITEM_HEALTH
                       ? (K_Condition_Get()->firepower == K_NEED_CRITICAL ? "firepower CRITICAL"
                                                                          : "ammo LOW")
                       : K_Condition_Get()->health == K_NEED_CRITICAL ? "health CRITICAL"
                       : "health LOW", item->name);
        }

        K_Combat_Escape(why);
        return;
    }

    if (dashing != goal->param && K_Map_GetPlayer(&player)
     && sqrt((item->at.x - player.x) * (item->at.x - player.x)
           + (item->at.y - player.y) * (item->at.y - player.y)) <= FIGHT_REACH)
    {
        dashing = goal->param;
        dash_held = false;
        dash_fought = false;
    }

    // Stopped for a monster close by, until none is within DASH_FREE - or,
    // once a fight began meanwhile, until it is over.
    if (dashing == goal->param)
    {
        if (K_Combat_MonstersWithin(DASH_CLEAR) > 0)
        {
            dash_fought |= dash_held && K_Combat_GetStatus()->battle;
            dash_held = true;
        }
        else if (dash_held)
        {
            dash_fought |= K_Combat_GetStatus()->battle;
            dash_held = dash_fought ? K_Combat_GetStatus()->battle
                                    : K_Combat_MonstersWithin(DASH_FREE) > 0;
        }

        dash_fought &= dash_held;
    }

    if (dashing == goal->param && !dash_held && !RunWaits(goal, item))
    {
        M_snprintf(why, sizeof(why), "going for the %s", item->name);
        K_Combat_Escape(why);
    }
    else
    {
        K_Combat_Escape(NULL);
    }
}

// Each tic: the sweep before the exit (SWEEP_NEAR) - judged, begun, over.
// An explorer's on every level, for the places not seen yet too
// (EXPLORE_STALL).
static void UpdateSweep(void)
{
    const k_navstatus_t *nav = K_Nav_GetStatus();
    int count, unseen;

    if (sweep_since == 0 && depth == 1 && goals[0].type == GOAL_EXIT && goals[0].planned
     && nav->state == K_NAV_DRIVING && (nav->remaining <= SWEEP_NEAR || explorer))
    {
        count = K_Items_Sweepable();
        unseen = PlacesSweepable();

        if (!explorer && (nomonsters || !K_Condition_ArsenalGoesOn()))
        {
            sweep_since = -1;
        }
        else if (count + unseen == 0)
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "GO TO EXIT: about to leave - nothing lying about "
                  "worth a sweep");
            K_Condition_LogArsenal("on leaving");
            sweep_since = -1;
        }
        else if (explorer)
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "GO TO EXIT: about to leave - %d items lying about "
                  "worth taking, %d places not seen yet, swept for", count, unseen);
            K_Condition_LogArsenal("before the sweep");
            sweep_since = SDL_max(leveltime, 1);
        }
        else
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "GO TO EXIT: about to leave - %d items lying about "
                  "worth taking on to the next level, swept for", count);
            K_Condition_LogArsenal("before the sweep");
            sweep_since = SDL_max(leveltime, 1);
        }

        // Looked for at once, not at the next second's look: the way out may
        // reach a lift or door first, and none is added at one (the user,
        // E3M1; E3M2: "14 places not seen yet, swept for", the lift to the
        // exit called 0.4 s later, ridden, and the exit line crossed).
        if (sweep_since > 0)
        {
            item_check_tic = 0;
            place_check_tic = 0;
        }

        sweep_found = leveltime;
    }
    else if (sweep_since > 0
          && ((count = K_Items_Sweepable() + PlacesSweepable()) == 0
              || leveltime - sweep_since >= (explorer ? EXPLORE_MOST : SWEEP_MOST)
              || (explorer && leveltime - sweep_found >= EXPLORE_STALL)))
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "GO TO EXIT: the sweep is over after %.1f s - %s",
              (double) (leveltime - sweep_since) / TICRATE,
              count == 0 ? "nothing left worth it"
              : explorer && leveltime - sweep_found >= EXPLORE_STALL
              ? "nothing new reached for 2 minutes" : "time up");
        K_Condition_LogArsenal("after the sweep");
        sweep_since = -1;
    }

    K_Items_Sweep(sweep_since > 0);
}

// The kind of monster the level needs dead for the goals on the chain (KILL
// EVERY <kind>): combat fights it with what the player holds once nothing
// that would beat it is left to fetch (K_Combat_Needed); -1 if none. Not a
// chase: the level does not need it dead.
static int NeededKind(void)
{
    int i;

    for (i = depth - 1; i >= 0; --i)
    {
        if (goals[i].type == GOAL_KILL && !goals[i].chase)
        {
            return goals[i].param;
        }
    }

    return -1;
}

// The way by route to the nearest live monster of the kind, -1 if there is
// no open way to any.
static double BossWay(int kind)
{
    k_vec2_t at[MAX_CANDIDATES];
    k_pose_t player;
    k_vec2_t start;
    double best = -1.0;
    int count, i;

    if (!K_Map_GetPlayer(&player))
    {
        return -1.0;
    }

    start.x = player.x;
    start.y = player.y;
    count = SDL_min(K_Specials_FindMonsters(kind, at, MAX_CANDIDATES), MAX_CANDIDATES);

    for (i = 0; i < count; ++i)
    {
        K_Path_Find(&probe, start, player.cell, at[i]);

        if (probe.result == K_PATH_FOUND && (best < 0.0 || probe.length < best))
        {
            best = probe.length;
        }
    }

    return best;
}

// Invulnerable (the user, Doom II MAP10: then chase the boss, as long as it
// lasts - the Cyberdemon teleports from room to room): while long enough
// for it (K_Combat_Chasing), the kind of boss last on the player is gone
// after, CHASE THE <kind> on top - a KILL EVERY <kind> as the level's bosses
// are, to where each one is now; once in sight combat closes in. Only while
// one is within CHASE_REACH by route, looked at every second. No item is
// gone for meanwhile, nor fallen back to safety. With not long enough left,
// or none near, it is dropped with the goals above it; combat keeps the
// boss off and hides from it again before it wears off. With two bosses and
// one sphere, the nearer by route. Returns true if the stack changed.
static boolean ConsiderChase(void)
{
    char name[64], why[96];
    goal_t *chase;
    double way;
    int on = ChaseOn();

    if (on >= 0 && !K_Combat_Chasing())
    {
        M_snprintf(why, sizeof(why), "the invulnerability runs out in %.0f s",
                   (double) players[consoleplayer].powers[pw_invulnerability] / TICRATE);
    }
    else if (leveltime < chase_check_tic || (on < 0 && (!K_Combat_Chasing() || chase_kind < 0
                                                  || depth >= MAX_GOALS
                                                  || !Allowed(GOAL_KILL, chase_kind))))
    {
        return false;
    }
    else
    {
        chase_check_tic = leveltime + TICRATE;
        way = BossWay(on >= 0 ? goals[on].param : chase_kind);

        if (on < 0 && way >= 0.0 && way <= CHASE_REACH)
        {
            chase = Push(GOAL_KILL, chase_kind);
            chase->chase = true;
            GoalName(chase, name, sizeof(name));
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "added %s - invulnerable for %.0f s more, %.0f off "
                  "by route", name,
                  (double) players[consoleplayer].powers[pw_invulnerability] / TICRATE, way);
            return true;
        }

        if (on < 0 || (way >= 0.0 && way <= CHASE_REACH))
        {
            return false;
        }

        if (way < 0.0)
        {
            M_StringCopy(why, "no way to it now", sizeof(why));
        }
        else
        {
            M_snprintf(why, sizeof(why), "%.0f off by route, too far to chase", way);
        }
    }

    GoalName(&goals[on], name, sizeof(name));
    K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s over - %s", name, why);
    depth = on;

    if (depth > 0)
    {
        goals[depth - 1].planned = false;
    }

    K_Nav_Clear();
    return true;
}

// The teleporter the topmost TELEFRAG goes through, -1 if none: combat
// leaves what stands on its landing spot alone (K_Combat_Telefrag).
static int TelefragOn(void)
{
    int i;

    for (i = depth - 1; i >= 0; --i)
    {
        if (goals[i].type == GOAL_TELEFRAG)
        {
            return goals[i].param;
        }
    }

    return -1;
}

// Whether a route ends by the teleporter (an index into teleports): its last
// crossing teleports the player to the spot, not walks it there.
static boolean LandsBy(const k_path_t *path, int teleport)
{
    int n = path->num_cells;

    return n >= 2 && K_Graph_Teleport(path->portals[n - 2], path->cells[n - 2]) == teleport;
}

// A telefrag (TELEFRAG_REACH), looked for as items are (ConsiderItems), in a
// fight too. Returns true if one was added.
static boolean ConsiderTelefrag(goal_t *goal)
{
    const k_navstatus_t *nav = K_Nav_GetStatus();
    const k_path_t *path = K_Nav_GetPath();
    const k_graph_t *graph = K_Graph_Get();
    boolean fighting = K_Combat_GetStatus()->battle;
    double remaining = -1.0, best_detour = 0.0, best_way = 0.0;
    int best = -1, best_kind = -1;
    boolean best_big = false;
    k_pose_t player;
    k_vec2_t start, end;
    goal_t *telefrag;
    char name[64];
    int t;

    if (leveltime < telefrag_check_tic || !goal->planned || goal->staying)
    {
        return false;
    }

    telefrag_check_tic = leveltime + TELEFRAG_CHECK_TICS;

    if (depth >= MAX_GOALS || goal->type == GOAL_GOTO || goal->type == GOAL_BARREL
     || TelefragOn() >= 0 || (goal->type == GOAL_TRIGGER && (goal->used || goal->moving))
     || nav->state != K_NAV_DRIVING || path == NULL || path->num_points < 1
     || (!fighting && (nav->action != K_ACTION_NONE || !K_Nav_CanPause()))
     || !K_Map_GetPlayer(&player))
    {
        return false;
    }

    if (telefrag_skip_size < graph->num_teleports)
    {
        telefrag_skip = realloc(telefrag_skip, graph->num_teleports * sizeof(*telefrag_skip));
        memset(telefrag_skip + telefrag_skip_size, 0,
               (graph->num_teleports - telefrag_skip_size) * sizeof(*telefrag_skip));
        telefrag_skip_size = graph->num_teleports;
    }

    start.x = player.x;
    start.y = player.y;
    end = path->points[path->num_points - 1];

    for (t = 0; t < graph->num_teleports; ++t)
    {
        const k_teleport_t *teleport = &graph->teleports[t];
        double there, detour;
        boolean big;
        int kind;

        if (teleport->cell < 0 || leveltime < telefrag_skip[t] || IsFailed(GOAL_TELEFRAG, t)
         || (kind = K_Combat_OnSpot(teleport->to.x, teleport->to.y)) < 0)
        {
            continue;
        }

        telefrag_skip[t] = leveltime + TELEFRAG_SKIP;
        big = kind == MT_CYBORG || kind == MT_SPIDER || kind == MT_VILE;

        // The way left, as the pathfinder sees it now.
        if (remaining < 0.0)
        {
            K_Path_Find(&probe, start, player.cell, end);
            remaining = probe.result == K_PATH_FOUND ? probe.length + probe.wait * WAIT_WAY
                                                     : nav->remaining;
        }

        K_Path_Find(&probe, start, player.cell, teleport->to);

        if (probe.result != K_PATH_FOUND || !LandsBy(&probe, t))
        {
            continue;
        }

        there = probe.length + probe.wait * WAIT_WAY;
        K_Path_Find(&probe, teleport->to, teleport->cell, end);

        if (probe.result != K_PATH_FOUND)
        {
            continue;
        }

        detour = SDL_max(0.0, there + probe.length + probe.wait * WAIT_WAY - remaining);

        if ((big || detour <= TELEFRAG_REACH)
         && (best < 0 || (big && !best_big) || (big == best_big && detour < best_detour)))
        {
            best = t;
            best_kind = kind;
            best_big = big;
            best_detour = detour;
            best_way = there;
        }
    }

    if (best < 0)
    {
        return false;
    }

    // A switch not worked yet: going for it again after is not another use
    // of its line (MAX_TRIGGERS), as after an item.
    if (goal->type == GOAL_TRIGGER && line_uses != NULL && goal->chosen.line >= 0
     && line_uses[goal->chosen.line] > 0)
    {
        --line_uses[goal->chosen.line];
    }

    telefrag = Push(GOAL_TELEFRAG, best);
    telefrag->victim = best_kind;
    GoalName(telefrag, name, sizeof(name));
    K_Log(K_LOG_BRAIN, K_LOG_INFO, "added %s - on the landing spot of the teleporter (tag %d) at "
          "(%.0f,%.0f), %.0f units there, %.0f out of the way%s", name, graph->teleports[best].tag,
          graph->teleports[best].to.x, graph->teleports[best].to.y, best_way, best_detour,
          best_detour > TELEFRAG_REACH ? " - too big a threat to fight" : "");
    return true;
}

// The telefrag on top: done once the player has landed on the spot; failed
// with nothing tough on it for TELEFRAG_OFF. Returns true if it ended.
static boolean UpdateTelefrag(goal_t *goal)
{
    const k_teleport_t *teleport = &K_Graph_Get()->teleports[goal->param];
    k_pose_t player;

    if (K_Map_GetPlayer(&player)
     && hypot(player.x - teleport->to.x, player.y - teleport->to.y) < TELEFRAG_LANDED)
    {
        DoneGoal("landed on the spot");
        return true;
    }

    goal->off = K_Combat_OnSpot(teleport->to.x, teleport->to.y) >= 0 ? 0 : goal->off + 1;

    if (goal->off >= TELEFRAG_OFF)
    {
        FailGoal("nothing tough stands on the landing spot now");
        return true;
    }

    return false;
}

// Held up by a barrel (HOLD_TICS): it goes on top, to be shot from a safe
// spot. Looked at every tic, to time the hold. Returns true if it was
// added.
// What going into a cell adds to a route's cost: the danger of the awake
// monsters in its area, if it is another (ROUTE_DANGER_WAY).
static double EnterCost(int cell, int from_cell)
{
    int area = K_Area_Of(cell);

    return area < 0 || area == K_Area_Of(from_cell)
         ? 0.0 : K_Area_Get()->areas[area].awake_danger * ROUTE_DANGER_WAY;
}

// Whether a way goes through an area with monsters inside, but `skip`.
static boolean ThroughMonsters(const k_path_t *path, int skip)
{
    int i;

    for (i = 0; i < path->num_cells; ++i)
    {
        int area = K_Area_Of(path->cells[i]);

        if (area >= 0 && area != skip && K_Area_Get()->areas[area].monsters > 0)
        {
            return true;
        }
    }

    return false;
}

// Whether a way can be walked now: no door or lift on it to work or wait
// for (E1M6: falling back through a door that opened by itself in 29 s,
// the bot stood at it 16 s, no fight started, and died there).
static boolean OpenNow(const k_path_t *path)
{
    int r;

    for (r = 1; r < path->num_cells; ++r)
    {
        k_crossing_t crossing = K_Graph_CanCross(path->portals[r - 1], path->cells[r - 1]);

        if (crossing != K_CROSS_OPEN && crossing != K_CROSS_JUMP
         && crossing != K_CROSS_TELEPORT)
        {
            return false;
        }
    }

    return true;
}

// The way to fall back to a spot (FALL_REACH): open now, through no area
// with monsters inside but `here`; or, with none, through one fix the brain
// knows - a switch, a walk-over line, never a key nor a barrel - the way to
// its nearest spot open now, and on from there blocked only by what it fixes
// (the user, Doom II MAP15: teleported into the tower room, its stairs out
// built by a switch beside the landing; the trap sprung, the only ways out
// went up the stairs, and the bot fought on at 65 health and died, 12 runs
// of 12). *length gets the way, *fixed whether it is through a fix.
static boolean FallBackWay(k_vec2_t start, int cell, k_vec2_t spot, int here, double *length,
                           boolean *fixed)
{
    goal_type_t type;
    int param, want, portal, from, key_door, count, i;
    boolean later;
    goal_t fix;

    *fixed = false;
    K_Path_Find(&probe, start, cell, spot);
    *length = probe.length;

    if (probe.result == K_PATH_FOUND)
    {
        return probe.length <= FALL_REACH && !ThroughMonsters(&probe, here) && OpenNow(&probe)
            && probe.damage < 1.0;
    }

    // Not one on its way already (Doom II MAP04: a gap narrowing the way to
    // a safe area, its sector moving; the fix done at once, the way still
    // shut, the fall back waited a second, the fight broken off, and failed).
    if (probe.result != K_PATH_BLOCKED
     || !RouteFix(&probe, &type, &param, &want, &portal, &from, &later, &key_door)
     || type != GOAL_TRIGGER || key_door >= 0 || later || portal < 0
     || K_Graph_SectorBusy(param) || !Allowed(GOAL_TRIGGER, param))
    {
        return false;
    }

    // The fix as PlanGoal would push it, and its nearest spot.
    memset(&fix, 0, sizeof(fix));
    fix.type = type;
    fix.param = param;
    fix.want = want;
    fix.portal = portal;
    fix.from = from;
    fix.pinch_cell = -1;
    count = Candidates(&fix, fix_candidates, MAX_CANDIDATES);

    for (i = 0; i < count; ++i)
    {
        double to_fix;

        K_Path_Find(&fix_probe, start, cell, fix_candidates[i].at);

        if (fix_probe.result != K_PATH_FOUND || ThroughMonsters(&fix_probe, here)
         || !OpenNow(&fix_probe))
        {
            continue;
        }

        to_fix = fix_probe.length;
        K_Path_Find(&fix_probe, fix_candidates[i].at, -1, spot);

        if (OpenOrFixed(&fix_probe, true, type, param) && !ThroughMonsters(&fix_probe, here)
         && to_fix + fix_probe.length <= FALL_REACH
         && (!*fixed || to_fix + fix_probe.length < *length))
        {
            *length = to_fix + fix_probe.length;
            *fixed = true;
        }
    }

    return *fixed;
}

// Falling back out of a fight going badly (FALL_LOSS). Looked at every tic;
// returns true if the goal was added.
static boolean ConsiderFallBack(void)
{
    const k_areamap_t *areas = K_Area_Get();
    int then = health_ring[(leveltime + 1) % FALL_TICS];
    int order[FALL_CANDIDATES];
    double order_distance[FALL_CANDIDATES];
    double pressure = 0.0, gunmen = 0.0, health = K_Map_PlayerHealth();
    double best_cost = 0.0, best_length = 0.0;
    int found = 0, best = -1, crowd = 0, here, a, i, k;
    boolean best_fixed = false;
    k_pose_t player;
    k_vec2_t start;
    goal_t *goal;

    if (!K_Combat_GetStatus()->battle || K_Combat_GetStatus()->holding
     || leveltime < fall_again || depth >= MAX_GOALS - 1 || K_Combat_Chasing()
     || FallingBack() >= 0 || !K_Map_GetPlayer(&player)
     || (here = K_Area_Of(player.cell)) < 0 || K_Area_Safe(here))
    {
        return false;
    }

    for (i = 0; i < K_Combat_MonsterCount(); ++i)
    {
        const k_monster_t *monster = K_Combat_Monster(i);

        if (monster->awake && monster->visible && monster->distance <= 1024.0
         && K_Combat_KindCost(monster->type) > 0.0)
        {
            pressure += K_Combat_KindCost(monster->type);
            gunmen += monster->hitscan ? K_Combat_KindCost(monster->type) : 0.0;
            ++crowd;
        }
    }

    if (!((then >= 0 && then - health >= FALL_LOSS && pressure >= FALL_PRESSURE)
          || pressure > health)
     || (crowd < FALL_CROWD && gunmen < FALL_GUNMEN))
    {
        return false;
    }

    start.x = player.x;
    start.y = player.y;

    // The nearest safe areas, as the crow flies.
    for (a = 0; a < areas->num_areas; ++a)
    {
        const k_area_t *area = &areas->areas[a];
        double distance = sqrt((area->spot.x - start.x) * (area->spot.x - start.x)
                             + (area->spot.y - start.y) * (area->spot.y - start.y));

        if (a == here || area->passage || !K_Area_Safe(a) || area->hurts > 0.0
         || distance > FALL_REACH
         || (found == FALL_CANDIDATES && distance >= order_distance[found - 1]))
        {
            continue;
        }

        for (k = found < FALL_CANDIDATES ? found++ : found - 1;
             k > 0 && order_distance[k - 1] > distance; --k)
        {
            order[k] = order[k - 1];
            order_distance[k] = order_distance[k - 1];
        }

        order[k] = a;
        order_distance[k] = distance;
    }

    for (k = 0; k < found; ++k)
    {
        const k_area_t *area = &areas->areas[order[k]];
        double cost, length;
        boolean fixed;

        if (!FallBackWay(start, player.cell, area->spot, here, &length, &fixed))
        {
            continue;
        }

        cost = (length + (fixed ? FIX_WORK : 0.0)) * (area->visited ? VISITED_SHARE : 1.0);

        if (best < 0 || cost < best_cost)
        {
            best = order[k];
            best_cost = cost;
            best_length = length;
            best_fixed = fixed;
        }
    }

    if (best < 0)
    {
        fall_again = leveltime + TICRATE;
        return false;
    }

    fall_again = leveltime + FALL_AGAIN;
    goal = Push(GOAL_GOTO, -1);
    goal->point = areas->areas[best].spot;
    goal->fallback = true;
    K_Log(K_LOG_BRAIN, K_LOG_INFO, "added FALL BACK TO (%.0f, %.0f) - area %d, safe, %.0f units "
          "away%s: %d health lost in %d s, %.0f health's worth of monsters on the player",
          goal->point.x, goal->point.y, best, best_length,
          best_fixed ? " through what blocks it" : "", then >= 0 ? then - (int) health : 0,
          FALL_TICS / TICRATE, pressure);
    PlanGoal(goal);
    return true;
}

static const k_item_t *FirstInReach(k_vec2_t at);

// Fallen back, and there: with health not OK, and nothing to go for first
// (FirstInReach), resting (REST_QUIET); else done.
static void FallenBack(goal_t *goal)
{
    k_need_t health = K_Condition_Get()->health;
    const k_item_t *first;
    k_pose_t player;
    k_vec2_t at;
    char name[64], how[96];

    if (health == K_NEED_OK || !K_Map_GetPlayer(&player))
    {
        DoneGoal("arrived");
        return;
    }

    at.x = player.x;
    at.y = player.y;

    if ((first = FirstInReach(at)) != NULL)
    {
        M_snprintf(how, sizeof(how), "arrived - the %s to go for first", first->name);
        DoneGoal(how);
        return;
    }

    goal->resting = true;
    goal->wait = 0;
    goal->quiet = 0;
    M_StringCopy(goal->note, "resting, hurt - fighting what comes", sizeof(goal->note));
    GoalName(goal, name, sizeof(name));
    K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: there, health %s (%.0f) - resting, fighting what comes; "
          "on once nothing has for %d s", name, health == K_NEED_CRITICAL ? "CRITICAL" : "LOW",
          K_Map_PlayerHealth(), REST_QUIET / TICRATE);
}

// Something that goes before the others (GoesFirst: health, with health not
// OK) within its reach as the crow flies, not skipped nor failed lately: one
// ConsiderItems would weigh, were there a route to weigh it on; NULL if none.
static const k_item_t *FirstInReach(k_vec2_t at)
{
    boolean critical = K_Condition_Get()->health == K_NEED_CRITICAL;
    int i;

    for (i = 0; i < K_Items_Count(); ++i)
    {
        const k_item_t *item = K_Items_Get(i);

        if (item->present && GoesFirst(item, critical)
         && sqrt((item->at.x - at.x) * (item->at.x - at.x) + (item->at.y - at.y) * (item->at.y - at.y))
            <= ItemReach(item, false)
         && !(i < item_skip_size && leveltime < item_skip[i]) && !IsFailed(GOAL_ITEM, i))
        {
            return item;
        }
    }

    return NULL;
}

// Each tic while resting after a fall back: nothing to plan; over with
// health OK, or something to go for first (FirstInReach: ConsiderItems weighs
// items only on a route, which a rest has none of - E4M2: a soulsphere 800
// off left 5 s), once quiet for REST_QUIET - no fight, nothing coming at the
// player (k_threat.h), no health lost over FALL_TICS: not one awake in
// sight that is not fought, nor comes (E4M2: a cacodemon 500 off, never
// nearer, held the bot 14 s) - or after REST_MOST.
static void UpdateRest(goal_t *goal)
{
    const k_combatstatus_t *combat = K_Combat_GetStatus();
    int then = health_ring[(leveltime + 1) % FALL_TICS];
    const k_item_t *first;
    k_pose_t player;
    k_vec2_t at = { 0.0, 0.0 };
    char how[96];

    goal->planned = true;
    ++goal->wait;
    goal->quiet = combat->battle || combat->threats > 0
               || (then >= 0 && then > (int) K_Map_PlayerHealth()) ? 0 : goal->quiet + 1;

    if (K_Map_GetPlayer(&player))
    {
        at.x = player.x;
        at.y = player.y;
    }

    if (K_Condition_Get()->health == K_NEED_OK)
    {
        M_snprintf(how, sizeof(how), "rested %.1f s - health OK again",
                   (double) goal->wait / TICRATE);
        DoneGoal(how);
    }
    else if ((first = FirstInReach(at)) != NULL)
    {
        M_snprintf(how, sizeof(how), "rested %.1f s - the %s to go for first",
                   (double) goal->wait / TICRATE, first->name);
        DoneGoal(how);
    }
    else if (goal->quiet >= REST_QUIET)
    {
        M_snprintf(how, sizeof(how), "rested %.1f s - no fight, nothing coming, nothing felt "
                   "for %d s", (double) goal->wait / TICRATE, REST_QUIET / TICRATE);
        DoneGoal(how);
    }
    else if (goal->wait >= REST_MOST)
    {
        M_snprintf(how, sizeof(how), "rested %d s, long enough", REST_MOST / TICRATE);
        DoneGoal(how);
    }
}

// Each tic: brought somewhere by a teleporter, the cell stood on before is
// looked at (ConsiderWayBack).
static void TrackArrival(void)
{
    k_pose_t pose;

    if (!K_Map_GetPlayer(&pose))
    {
        last_known = false;
        return;
    }

    if (last_known && last_pose.cell >= 0
     && hypot(pose.x - last_pose.x, pose.y - last_pose.y) > TELEPORT_JUMP)
    {
        back_cell = last_pose.cell;
        back_until = leveltime + WAY_BACK_TICS;
    }

    last_pose = pose;
    last_known = true;
}

// The way back from where a teleporter brought the player (WAY_BACK_REACH):
// blocked by what one switch or walk-over line near fixes, that fix goes on
// top, before anything else. Returns true if it was added.
static boolean ConsiderWayBack(goal_t *goal)
{
    const k_navstatus_t *nav = K_Nav_GetStatus();
    k_vec2_t back, start;
    goal_type_t type;
    int param, want, portal, from, key_door, count, i;
    boolean later, near = false;
    double about = 0.0;
    k_pose_t player;
    goal_t fix, *added;
    char name[64], text[K_LOG_LINE_LEN];

    if (back_cell >= 0 && leveltime > back_until)
    {
        back_cell = -1;
    }

    if (back_cell < 0 || !goal->planned || depth >= MAX_GOALS || goal->type == GOAL_GOTO
     || goal->type == GOAL_BARREL || (goal->type == GOAL_TRIGGER && (goal->used || goal->moving))
     || nav->state != K_NAV_DRIVING || nav->action != K_ACTION_NONE || !K_Nav_CanPause()
     || K_Combat_GetStatus()->battle || K_Combat_GetStatus()->awake > 0
     || !K_Map_GetPlayer(&player))
    {
        return false;
    }

    // Looked at once, with monsters about that a fight may come from.
    back = K_Map_Get()->cells[back_cell].centroid;
    back_cell = -1;

    for (i = 0; i < K_Combat_MonsterCount(); ++i)
    {
        const k_monster_t *monster = K_Combat_Monster(i);

        about += monster->distance <= WAY_BACK_DANGER ? K_Combat_KindCost(monster->type) : 0.0;
    }

    if (about < WAY_BACK_RISK)
    {
        return false;
    }

    start.x = player.x;
    start.y = player.y;
    K_Path_Find(&probe, start, player.cell, back);

    if (probe.result != K_PATH_BLOCKED
     || !RouteFix(&probe, &type, &param, &want, &portal, &from, &later, &key_door)
     || type != GOAL_TRIGGER || key_door >= 0 || later || !Allowed(GOAL_TRIGGER, param))
    {
        return false;
    }

    K_Path_Describe(&probe, text, sizeof(text));
    memset(&fix, 0, sizeof(fix));
    fix.type = type;
    fix.param = param;
    fix.want = want;
    fix.portal = portal;
    fix.from = from;
    fix.pinch_cell = -1;

    if (portal < 0)
    {
        fix.pinch_cell = from;
        fix.pinch_at = probe.blocked_at;
    }

    count = Candidates(&fix, fix_candidates, MAX_CANDIDATES);

    // Near, and keeping the way open once worked: no lift, no door that
    // shuts again (Doom II MAP03: a walk-over line beside the landing lowered
    // a lift, which carried the bot back where it came from - 16 s).
    for (i = 0; i < count && !near; ++i)
    {
        int line = fix_candidates[i].line;
        double floor, ceiling;

        if (line < 0 || !K_Specials_MoverTarget(line, param, &floor, &ceiling)
         || K_Specials_DoorCloses(K_Specials_LineSpecial(line)))
        {
            continue;
        }

        K_Path_Find(&fix_probe, start, player.cell, fix_candidates[i].at);
        near = fix_probe.result == K_PATH_FOUND
            && fix_probe.length + fix_probe.wait * WAIT_WAY <= WAY_BACK_REACH;
    }

    if (!near)
    {
        return false;
    }

    // A switch not worked yet: going for it again after is not another use
    // of its line (MAX_TRIGGERS), as after an item.
    if (goal->type == GOAL_TRIGGER && line_uses != NULL && goal->chosen.line >= 0
     && line_uses[goal->chosen.line] > 0)
    {
        --line_uses[goal->chosen.line];
    }

    added = Push(GOAL_TRIGGER, param);
    added->want = fix.want;
    added->portal = fix.portal;
    added->from = fix.from;
    added->pinch_cell = fix.pinch_cell;
    added->pinch_at = fix.pinch_at;
    added->way_back = true;
    GoalName(added, name, sizeof(name));
    K_Log(K_LOG_BRAIN, K_LOG_INFO, "the way back to (%.0f,%.0f), where the player stood before, is "
          "blocked by %s - opened first: added %s", back.x, back.y, text, name);
    return true;
}

static boolean ConsiderHeldUp(const goal_t *goal)
{
    const k_navstatus_t *nav = K_Nav_GetStatus();
    const k_map_t *map = K_Map_Get();
    double vx, vy, best_gap = HOLD_GAP, reach = PLAYER_RADIUS + HOLD_GAP + 32.0;
    int found[16], count, best = -1, i;
    k_pose_t player;
    k_vec2_t me, min, max;
    char name[64];

    if (!K_Map_GetPlayer(&player))
    {
        return false;
    }

    me.x = player.x;
    me.y = player.y;

    // Following a route only: not fighting or stepping away (combat moves
    // the player), not at a door or lift, not shooting a barrel already.
    if (!goal->planned || goal->type == GOAL_BARREL || nav->state != K_NAV_DRIVING
     || nav->action != K_ACTION_NONE || K_Combat_Move(&vx, &vy)
     || AxisDistance(me, hold_at) > HOLD_RADIUS)
    {
        hold_at = me;
        hold_since = leveltime;
        return false;
    }

    if (leveltime - hold_since < HOLD_TICS)
    {
        return false;
    }

    hold_at = me;
    hold_since = leveltime;
    min.x = me.x - reach;
    min.y = me.y - reach;
    max.x = me.x + reach;
    max.y = me.y + reach;
    count = K_Map_BlocksNear(min, max, found, arrlen(found));

    for (i = 0; i < count; ++i)
    {
        const k_block_t *block = &map->blocks[found[i]];
        double gap = AxisDistance(me, block->at) - block->radius - PLAYER_RADIUS;

        if (block->barrel && gap < best_gap && Allowed(GOAL_BARREL, found[i]))
        {
            best = found[i];
            best_gap = gap;
        }
    }

    if (best < 0)
    {
        return false;
    }

    GoalName(Push(GOAL_BARREL, best), name, sizeof(name));
    K_Log(K_LOG_BRAIN, K_LOG_INFO, "held up by the barrel at (%.0f,%.0f) for %.1f s - added %s",
          map->blocks[best].at.x, map->blocks[best].at.y, (double) HOLD_TICS / TICRATE, name);
    return true;
}

// Landed on the item or key gone for, still lying there (STEP_OFF): a step
// aside goes on top. Returns false if there is no spot for it.
static boolean StepOff(void)
{
    static const double ways[8][2] =
    {
        { 1.0, 0.0 }, { 0.7071, 0.7071 }, { 0.0, 1.0 }, { -0.7071, 0.7071 },
        { -1.0, 0.0 }, { -0.7071, -0.7071 }, { 0.0, -1.0 }, { 0.7071, -0.7071 },
    };
    k_pose_t player;
    k_vec2_t start, spot;
    goal_t *step;
    int i;

    if (depth >= MAX_GOALS || !K_Map_GetPlayer(&player))
    {
        return false;
    }

    start.x = player.x;
    start.y = player.y;

    for (i = 0; i < 8; ++i)
    {
        spot.x = start.x + ways[i][0] * STEP_OFF;
        spot.y = start.y + ways[i][1] * STEP_OFF;

        if (K_Map_CellAt(spot.x, spot.y) < 0
         || K_Graph_Clearance(spot, PLAYER_RADIUS + 1.0) <= PLAYER_RADIUS)
        {
            continue;
        }

        K_Path_Find(&probe, start, player.cell, spot);

        if (probe.result != K_PATH_FOUND || probe.length > 2.0 * STEP_OFF || probe.damage > 0.0)
        {
            continue;
        }

        K_Path_Find(&probe, spot, -1, start);

        if (probe.result != K_PATH_FOUND || probe.length > 2.0 * STEP_OFF)
        {
            continue;
        }

        step = Push(GOAL_GOTO, -1);
        step->point = spot;
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "landed on it, untouched - added GO TO (%.0f, %.0f), a "
              "step aside", spot.x, spot.y);
        return true;
    }

    return false;
}

// At the spot for a barrel in the way, combat shoots it.
static void UpdateShooting(void)
{
    const goal_t *goal = depth > 0 ? &goals[depth - 1] : NULL;

    if (goal != NULL && goal->type == GOAL_BARREL && goal->shooting)
    {
        K_Combat_ShootBarrel(1, goal->chosen.aim.x, goal->chosen.aim.y);
    }
    else
    {
        K_Combat_ShootBarrel(0, 0.0, 0.0);
    }
}

//
// Interface
//

void K_Brain_SetExitChoice(k_exitchoice_t choice)
{
    exit_choice = choice;
}

void K_Brain_SetExplorer(int on)
{
    explorer = on;
}

void K_Brain_Reset(void)
{
    int i;

    if (depth > 0)
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "%d goals dropped - new level", depth);
    }

    depth = 0;
    num_failed = 0;
    outcome[0] = '\0';
    exit_wanted = K_AutoplayEnabled();
    item_check_tic = 0;
    was_critical = false;
    idling = false;
    dashing = -1;
    dash_held = false;
    dash_fought = false;
    run_waited = -1;
    hold_since = 0;
    hold_at.x = hold_at.y = 1.0e9;
    last_known = false;
    back_cell = -1;
    telefrag_check_tic = 0;
    pass_portal = -1;
    pass_excused = -1;
    chase_kind = -1;
    chase_check_tic = 0;
    sweep_since = 0;
    K_Items_Sweep(0);
    exit_kind = EXIT_UNDECIDED;
    exit_idle = 0;
    exit_stall = 0;
    exit_best = 1.0e9;
    place_check_tic = 0;
    place_seen_next = 0;
    explore_tics = 0;
    secrets_found = players[consoleplayer].secretcount;
    BuildPlaces();

    if (item_skip != NULL)
    {
        memset(item_skip, 0, item_skip_size * sizeof(*item_skip));
    }

    if (telefrag_skip != NULL)
    {
        memset(telefrag_skip, 0, telefrag_skip_size * sizeof(*telefrag_skip));
    }

    free(line_uses);
    line_uses = calloc(SDL_max(numlines, 1), 1);
    free(line_last);
    line_last = calloc(SDL_max(numlines, 1), sizeof(*line_last));

    K_Path_SetBlockCost(BlockCost);
    K_Path_SetPinchCost(PinchCost);
    K_Path_SetEnterCost(EnterCost);
    K_Path_SetKeepOff(KeepOff);

    for (i = 0; i < FALL_TICS; ++i)
    {
        health_ring[i] = -1;
    }

    fall_again = 0;
}

void K_Brain_RequestGoto(double x, double y)
{
    k_pose_t player;
    goal_t *goal;

    if (!K_Map_GetPlayer(&player))
    {
        K_Log(K_LOG_BRAIN, K_LOG_WARN, "click at (%.0f,%.0f) ignored - not in "
              "a level", x, y);
        return;
    }

    if (K_Map_CellAt(x, y) < 0)
    {
        K_Log(K_LOG_BRAIN, K_LOG_WARN, "click at (%.0f,%.0f) ignored - not on "
              "any floor", x, y);
        return;
    }

    // A new click replaces one still being gone to.
    if (depth > 0 && goals[depth - 1].type == GOAL_GOTO)
    {
        Pop();
    }

    if (depth >= MAX_GOALS)
    {
        K_Log(K_LOG_BRAIN, K_LOG_WARN, "click at (%.0f,%.0f) ignored - the goal "
              "list is full", x, y);
        return;
    }

    goal = Push(GOAL_GOTO, -1);
    goal->point.x = x;
    goal->point.y = y;
    K_Log(K_LOG_BRAIN, K_LOG_INFO, "added GO TO (%.0f, %.0f) - user click", x, y);

    // Planned now, so the route shows even when nothing ticks (viewer
    // mode).
    PlanGoal(goal);
}

void K_Brain_Tick(void)
{
    goal_t *goal;
    const k_item_t *item;
    int telefrag;

    health_ring[leveltime % FALL_TICS] = (int) K_Map_PlayerHealth();
    TrackArrival();
    UpdatePassing();
    UpdateEscape();
    UpdateSweep();
    UpdateExit();
    UpdateExplore();

    if (K_Combat_Boss() >= 0)
    {
        chase_kind = K_Combat_Boss();
    }
    UpdateShooting();
    K_Combat_Needed(NeededKind());
    telefrag = TelefragOn();
    K_Combat_Telefrag(telefrag >= 0, telefrag >= 0 ? K_Graph_Get()->teleports[telefrag].to.x : 0.0,
                      telefrag >= 0 ? K_Graph_Get()->teleports[telefrag].to.y : 0.0);

    if (depth == 0 && exit_wanted)
    {
        if (exit_kind == EXIT_UNDECIDED)
        {
            ChooseExit();
        }

        Push(GOAL_EXIT, 0);
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "added GO TO EXIT");
    }

    if (depth == 0)
    {
        return;
    }

    goal = &goals[depth - 1];

    if ((goal->type == GOAL_KEY && K_Specials_HasKey(goal->param))
     || (goal->type == GOAL_ITEM && !K_Items_Get(goal->param)->present))
    {
        // The next item looked for at once: drops lie together after a fight.
        if (goal->type == GOAL_ITEM)
        {
            item_check_tic = 0;
        }

        DoneGoal("picked it up");
        return;
    }

    // What their death moves is on its way (A_BossDeath runs as the last
    // one dies).
    if (goal->type == GOAL_KILL && K_Specials_FindMonsters(goal->param, NULL, 0) == 0)
    {
        DoneGoal("every one is dead");
        return;
    }

    // Gone, and the chain over: a blast pushes a barrel off its square
    // before it goes off itself (MAP23: the bot set off for the way on, and
    // the barrel in it hit it for 48).
    if (goal->type == GOAL_BARREL && !K_Map_BlockStands(goal->param))
    {
        const k_block_t *barrel = &K_Map_Get()->blocks[goal->param];
        k_pose_t player;

        if (K_Combat_BlastsPending(barrel->at.x, barrel->at.y, BARREL_CHAIN_WAIT) > 0
         || (K_Map_GetPlayer(&player)
             && K_Combat_BlastsPending(player.x, player.y, BARREL_CHAIN_WAIT) > 0))
        {
            M_StringCopy(goal->note, "waiting for the chain to go off", sizeof(goal->note));
            return;
        }

        DoneGoal("it went off");
        return;
    }

    // Something else filled the need on the way (another item, say).
    if (goal->type == GOAL_ITEM && K_Items_Get(goal->param)->worth <= 0.0)
    {
        DoneGoal("no longer needed");
        return;
    }

    // Monsters came, or woke, and guard it now for more than it is worth
    // (or a solid thing came to stand by it): left for a while.
    if (goal->type == GOAL_ITEM && (item = K_Items_Get(goal->param))->reach <= 0.0)
    {
        char name[64];

        GoalName(goal, name, sizeof(name));

        if (!item->reachable)
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s dropped - no spot touches it now", name);
        }
        else
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s dropped - guarded now, %.0f health at risk "
                  "for %.0f", name, item->danger, item->together);
        }

        if (goal->param < item_skip_size)
        {
            item_skip[goal->param] = leveltime + ITEM_SKIP_TICS;
        }

        Pop();
        return;
    }

    if (goal->type == GOAL_TELEFRAG && UpdateTelefrag(goal))
    {
        return;
    }

    if (goal->type == GOAL_PLACE && PlaceDone(goal->param))
    {
        DoneGoal(places[goal->param].sector >= 0 ? "found it" : "been there");
        return;
    }

    // An explorer's sweep near the exit looks every tic (SWEEP_HOLD).
    if (explorer && sweep_since > 0 && goal->type == GOAL_EXIT && goal->planned
     && K_Nav_GetStatus()->state == K_NAV_DRIVING && K_Nav_GetStatus()->remaining <= SWEEP_HOLD)
    {
        item_check_tic = 0;
        place_check_tic = 0;
    }

    if (DoneIfOpen(goal) || ConsiderHeldUp(goal) || ConsiderChase() || ConsiderFallBack()
     || ConsiderWayBack(goal) || ConsiderTelefrag(goal) || ConsiderItems(goal)
     || ConsiderPlaces(goal))
    {
        return;
    }

    if (goal->resting)
    {
        UpdateRest(goal);
        return;
    }

    if (!goal->planned)
    {
        if (leveltime >= goal->retry_tic && !DropNeedless() && !BreakLoop(goal))
        {
            PlanGoal(goal);
        }

        return;
    }

    if (goal->type == GOAL_TRIGGER && UpdateTrigger(goal))
    {
        return;
    }

    if ((goal->type == GOAL_EXIT && UpdateExitFloor(goal))
     || (goal->type == GOAL_KILL && UpdateKill(goal)))
    {
        return;
    }

    // At the spot, combat shoots; a fight comes first, and does not count.
    if (goal->type == GOAL_BARREL && goal->shooting)
    {
        if (!K_Combat_GetStatus()->battle && ++goal->wait > BARREL_SHOOT_TICS)
        {
            CandidateFailed(goal, "the shots did not set it off");
        }

        return;
    }

    // At the firing spot, combat fights the boss; out of a fight too long,
    // a new spot (FIRE_IDLE).
    if (goal->type == GOAL_KILL && goal->shooting)
    {
        goal->quiet = K_Combat_GetStatus()->battle ? 0 : goal->quiet + 1;

        if (goal->quiet > FIRE_IDLE)
        {
            char name[64];

            GoalName(goal, name, sizeof(name));
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: no fight from the spot for %d s - another "
                  "spot", name, FIRE_IDLE / TICRATE);
            goal->planned = false;
            goal->shooting = false;
            K_Nav_Clear();
        }

        return;
    }

    // Fallen onto the item or key, landed, and it still lies there: stood on,
    // untouched (STEP_OFF).
    if (goal->landing)
    {
        k_pose_t pose;

        if (K_Map_GetPlayer(&pose) && pose.onground)
        {
            goal->landing = false;

            if (!StepOff())
            {
                CandidateFailed(goal, "landed on it, but did not pick it up");
            }
        }

        return;
    }

    switch (K_Nav_Tick())
    {
        case K_NAV_ARRIVED:
            if (goal->type == GOAL_GOTO && goal->fallback)
            {
                FallenBack(goal);
            }
            else if (goal->type == GOAL_GOTO)
            {
                DoneGoal("arrived");
            }
            else if (goal->type == GOAL_TRIGGER)
            {
                // A walk-over line: crossed on the way.
                goal->used = true;
                goal->wait = 0;
            }
            else if (goal->type == GOAL_KEY || goal->type == GOAL_ITEM)
            {
                k_pose_t pose;

                // Still falling onto it: looked at again on landing
                // (STEP_OFF).
                if (!K_Map_GetPlayer(&pose) || pose.onground)
                {
                    CandidateFailed(goal, "got there, but did not pick it up");
                }
                else
                {
                    goal->landing = true;
                }
            }
            else if (goal->type == GOAL_BARREL)
            {
                goal->shooting = true;
                goal->wait = 0;
                M_StringCopy(goal->note, "shooting it", sizeof(goal->note));
            }
            else if (goal->type == GOAL_KILL && SpotChosen(goal))
            {
                // At the firing spot: combat fights it from here.
                goal->shooting = true;
                goal->quiet = 0;
                M_StringCopy(goal->note, "fighting it from the spot", sizeof(goal->note));
            }
            else if (goal->type == GOAL_KILL)
            {
                // It has moved on: where to now, in a moment.
                goal->planned = false;
                goal->retry_tic = leveltime + TICRATE;
                K_Nav_Clear();
            }
            else if (goal->type == GOAL_TELEFRAG)
            {
                DoneGoal("landed on the spot");
            }
            else if (goal->type == GOAL_PLACE)
            {
                CandidateFailed(goal, "got there, but it did not count");
            }
            else
            {
                CandidateFailed(goal, "crossed it, but the level did not end");
            }
            break;

        case K_NAV_FAILED:
            // The way shut on the way, but is there, blocked (Doom II
            // MAP02: a fast door closed before the player got back to it,
            // and its only blue key was given up for good): planned again,
            // which pushes the fix (the door's switch), not given up. Not
            // counted when a fight took the way lately (PASS_TICS).
            if (K_Nav_GetStatus()->blocked
             && (goal->shut < MAX_SHUT
                 || (pass_excused >= 0 && leveltime - pass_excused <= PASS_TICS)))
            {
                char name[64];

                if (pass_excused < 0 || leveltime - pass_excused > PASS_TICS)
                {
                    ++goal->shut;
                }

                pass_excused = -1;
                GoalName(goal, name, sizeof(name));
                K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: the way shut on the way (%s) - planning "
                      "again (%d/%d)", name, K_Nav_GetStatus()->problem, goal->shut, MAX_SHUT);
                goal->planned = false;
                K_Nav_Clear();
            }
            else
            {
                CandidateFailed(goal, K_Nav_GetStatus()->problem);
            }
            break;

        default:
            break;
    }
}

void K_Brain_LevelCompleted(int secret)
{
    char how[96];

    M_snprintf(how, sizeof(how), "level finished in %.1f s through the %s exit, %.0f health "
               "left", (double) leveltime / TICRATE, secret ? "secret" : "normal",
               K_Map_PlayerHealth());

    if (depth > 0 && goals[0].type == GOAL_EXIT)
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "GO TO EXIT done - %s", how);
    }
    else
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s", how);
    }

    K_Log(K_LOG_BRAIN, K_LOG_INFO, "kills %d of %d, items %d of %d, secrets %d of %d",
          players[consoleplayer].killcount, totalkills, players[consoleplayer].itemcount,
          totalitems, players[consoleplayer].secretcount, totalsecret);
    LogExplored();
    depth = 0;
    exit_wanted = false;
}

// What a trigger goal's line does for the way, and how it is worked: by its
// special at level load (a one-shot line loses its own once used, and with
// it what K_Specials_MoverTarget could tell).
static void TriggerKind(const goal_t *goal, int *opens, int *worked)
{
    const k_map_t *map = K_Map_Get();
    int line = goal->planned ? goal->chosen.line : -1;
    int special = line >= 0 ? map->lines[line].special : 0;
    int moves = K_Specials_MoverMoves(special);

    *opens = goal->way_back ? K_OPENS_WAY_BACK
           : K_Specials_ManualDoor(special) || (moves & (K_MOVE_CEILING_UP | K_MOVE_DOOR))
           ? K_OPENS_DOOR
           : K_Specials_Lift(special) ? K_OPENS_LIFT
           : (moves & K_MOVE_FLOOR_UP) ? K_OPENS_FLOOR_UP
           : (moves & K_MOVE_FLOOR_DOWN) ? K_OPENS_FLOOR_DOWN
           : K_OPENS_WAY;
    *worked = line < 0 || K_Specials_ManualDoor(special) ? K_WORKED_WALK
            : goal->chosen.kind == K_GOAL_USE_LINE ? K_WORKED_PRESS
            : goal->chosen.kind == K_GOAL_SHOOT_LINE ? K_WORKED_SHOOT
            : K_WORKED_WALK;
}

// A goal as a purpose: the exit in the sweep before it as the sweep, an item
// fetched in it flagged so; an item run for out of a fight, an escape. A
// switch or barrels alone on the list, as themselves.
static void Purpose(const goal_t *goal, k_activity_t *activity)
{
    const k_item_t *item;

    switch (goal->type)
    {
        case GOAL_GOTO:
            activity->kind = goal->fallback ? K_ACT_FALLBACK : K_ACT_NONE;
            break;

        case GOAL_EXIT:
            activity->kind = sweep_since > 0 ? K_ACT_LOOT : K_ACT_EXIT;
            activity->detail = exit_kind == 1 ? 1 : exit_kind == 0 ? 0 : -1;
            break;

        case GOAL_ITEM:
            item = K_Items_Get(goal->param);
            activity->kind = item->escape ? K_ACT_ESCAPE : K_ACT_ITEM;
            activity->detail = item->kind;
            activity->flag = sweep_since > 0;
            activity->name = item->name;
            break;

        case GOAL_PLACE:
            activity->kind = places[goal->param].sector >= 0 ? K_ACT_SECRET : K_ACT_EXPLORE;
            break;

        case GOAL_KEY:
            activity->kind = K_ACT_KEY;
            activity->detail = goal->param;
            break;

        case GOAL_TRIGGER:
            activity->kind = K_ACT_TRIGGER;
            TriggerKind(goal, &activity->detail, &activity->flag);
            break;

        case GOAL_BARREL:
            activity->kind = K_ACT_BARREL;
            break;

        case GOAL_KILL:
            activity->kind = K_ACT_BOSS;
            activity->detail = goal->param;
            activity->flag = goal->chase;
            activity->name = K_Combat_KindName(goal->param);
            break;

        case GOAL_TELEFRAG:
            activity->kind = K_ACT_TELEFRAG;
            activity->detail = goal->victim;
            activity->name = K_Combat_KindName(goal->victim);
            break;
    }
}

// Whether a fix goal on top is worked now: a switch pressed or shot there,
// or a line's sector moving after it was worked; barrels shot at from the
// spot.
static boolean FixWorked(const goal_t *goal)
{
    const k_navstatus_t *nav = K_Nav_GetStatus();

    if (goal->type == GOAL_BARREL)
    {
        return goal->shooting;
    }

    return goal->type == GOAL_TRIGGER
        && (goal->used || goal->moving
            || nav->action == K_ACTION_USE || nav->action == K_ACTION_SHOOT);
}

void K_Brain_Activity(k_activity_t *activity)
{
    int i, top = -1;

    memset(activity, 0, sizeof(*activity));
    activity->kind = K_ACT_NONE;
    activity->detail = -1;
    activity->fix = K_ACT_NONE;

    for (i = depth - 1; i >= 0; --i)
    {
        const goal_t *goal = &goals[i];

        if (goal->type == GOAL_GOTO && !goal->fallback)
        {
            continue;
        }

        // The fix on top, if worked now; the purpose is under it.
        if (goal->type == GOAL_TRIGGER || goal->type == GOAL_BARREL)
        {
            if (top < 0)
            {
                top = i;

                if (FixWorked(goal))
                {
                    activity->fix = goal->type == GOAL_TRIGGER ? K_ACT_TRIGGER : K_ACT_BARREL;

                    if (goal->type == GOAL_TRIGGER)
                    {
                        TriggerKind(goal, &activity->fix_detail, &activity->fix_flag);
                    }
                }
            }

            continue;
        }

        Purpose(goal, activity);
        return;
    }

    // Only fixes: the one on top is the purpose.
    if (top >= 0)
    {
        Purpose(&goals[top], activity);
    }
}

int K_Brain_GoalCount(void)
{
    return depth;
}

void K_Brain_GoalText(int index, char *buf, int buf_len)
{
    const goal_t *goal = &goals[depth - 1 - index];
    char name[64];

    GoalName(goal, name, sizeof(name));

    if (goal->note[0] != '\0')
    {
        M_snprintf(buf, buf_len, "%s - %s", name, goal->note);
    }
    else
    {
        M_StringCopy(buf, name, buf_len);
    }
}

int K_Brain_FailedCount(void)
{
    return num_failed;
}

const char *K_Brain_FailedText(int index)
{
    static char text[K_LOG_LINE_LEN + 32];
    int left = failed[index].until - leveltime;

    if (left > 0)
    {
        M_snprintf(text, sizeof(text), "%s (again in %d s)", failed[index].text,
                   (left + TICRATE - 1) / TICRATE);
    }
    else
    {
        M_snprintf(text, sizeof(text), "%s (may be tried again)", failed[index].text);
    }

    return text;
}

const char *K_Brain_Status(void)
{
    const char *mode = K_AutoplayEnabled() ? "" : "VIEWER - ";
    const k_navstatus_t *nav = K_Nav_GetStatus();
    const k_path_t *path = K_Nav_GetPath();
    char top[K_LOG_LINE_LEN];

    if (depth > 0)
    {
        K_Brain_GoalText(0, top, sizeof(top));

        if (path != NULL && K_AutoplayEnabled() && nav->state == K_NAV_DRIVING)
        {
            M_snprintf(status, sizeof(status), "%s - %d waypoints left, %.0f units",
                       top, path->num_points - nav->target, nav->remaining);
        }
        else
        {
            M_snprintf(status, sizeof(status), "%s%s", mode, top);
        }
    }
    else if (outcome[0] != '\0')
    {
        M_snprintf(status, sizeof(status), "%sIDLE - %s", mode, outcome);
    }
    else if (!K_AutoplayEnabled())
    {
        M_snprintf(status, sizeof(status), "VIEWER - click the map to plan a "
                   "path; run with -autoplay to let the bot drive");
    }
    else
    {
        M_StringCopy(status, "IDLE", sizeof(status));
    }

    return status;
}
