//
// DESCRIPTION:
//	Karp combat, the first of it (karp/docs/plan.md, 7.2 and 7.3):
//	shooting on the way.
//
//	Each tic the monsters and projectiles are listed from the thinkers,
//	and Karp's own sight checks tell which monsters the player can see.
//	The target is the one scoring best: one within reach of its blows or in
//	the way first, then the most dangerous kinds (hitscanners; a pain
//	elemental before the lost souls it spits out), awake ones,
//	attacking ones, near ones, those that drop what the player wants (a
//	shotgun guy, out of shells); far ones are left alone (the user: chasing
//	a far hitscanner wastes time and ammo), and so are tough ones whose
//	shots can be sidestepped across a large room (a cacodemon, a baron).
//	One that cannot come at the player (behind bars, on a ledge) is shot
//	at on the way, a shot or two, and never fought - unless it is a gunman,
//	or the player is comfy, with health and ammo to spare.
//	The weapon is the one that would hurt it most a tic, counting no more
//	than kills it, brought up only when clearly better than the one in
//	hand (a switch takes about 0.9 s), and never instead of one whose next
//	shot should kill the target. The pistol is the last resort: for
//	monsters with little health sniped at from far off, not closing in,
//	with the area clear, and when nothing better can hurt the target; the
//	bare fist comes after it, and melee is for weak monsters only. Rockets
//	and cells are kept for the tough monsters left (barons, arch-viles,
//	mancubi, the Cyberdemon, the spider mastermind) while there are no more
//	than their health takes: the weaker ones get the other guns. Nav
//	faces the target wherever it
//	does not need the facing itself, and the weapon fires once the facing
//	after a command's turn is on the target.
//
//	Fight or avoid (plan, 7.3): a monster the guns and rounds held cannot
//	kill - too few, or too slow - is no target; one in sight near is kept
//	off, out of its sight, with nothing shot at it, and what would kill it
//	is gone for (K_Combat_Beats).
//
//	Infighting (plan, 7.8): a monster fighting another is no target - a shot
//	would turn it on the player - but kept off as any. Telefrags: what stands
//	where the brain goes to land by a teleporter is left alone, its death on
//	its way (K_Combat_Telefrag). Invulnerable, the player fights far
//	monsters too and begins no hold (INVULN_BOLD); a hard fight is what a
//	sphere is for (K_Combat_HardFight).
//
//	Pistol and chaingun shots stray only when the trigger is held through
//	a shot (P_GunShot is accurate while refire is 0): far off they are
//	tapped, fired only from the ready state. The plasma rifle is held: let
//	go, it pauses 20 tics after each ball. Rockets are fired only where
//	they would burst well clear of the player, as the blast measures it,
//	their path clear of walls and other monsters, no lost soul near; and
//	only at tough monsters, or bunched ones, unless the player is
//	invulnerable. A BFG ball's spray leaves from where the player stands
//	once it has burst: till then its burst is kept in sight (BFG_HOLD).
//
//	Barrels (plan, 7.3): never set off near the player, by a shot that could
//	graze one (spread counted, and those behind the target) or a blow; kept
//	away from in battle. Shot on purpose, from a safe distance, when the
//	blast is worth more than a shot at the target, when one by the route
//	ahead could be set off by monsters as the player passes (if cheap), or
//	when the brain wants one out of the way. One a monster's shot has hit
//	already is primed: kept away from as one going off, and popped first.
//
//	Battle mode (plan, 7.4), as a human who is not speedrunning plays (the
//	user): a monster near enough to matter, awake, or asleep but close,
//	is fought before going on. The route waits (K_Nav_Pause); the player
//	faces the target, sticks with it until it is dead (a wounded one
//	first), and strafes side to side near where the fight began, backing
//	off from monsters that come close - the tougher the kind, the further
//	it is kept off: a baron is shot from afar, the player backing off as
//	far as it takes, toward room to back off further, and pressed hard
//	where there is none, retreating to open floor nearby - never over a
//	drop, onto a floor that hurts, or over a walk-over line that does
//	something.
//	Once nothing has been worth shooting for a second, nav
//	plans its route again from where the player is. A target that has not
//	lost health for a while (out of reach of the shots) is left alone for
//	a time. Nav busy at a door, lift or switch, or running a jump, the
//	player shoots from where it is, as on the way. Hurt there by a monster
//	in reach of its blows that the shots cannot find (an imp clawing down
//	at a lift: vanilla's blows reach any height), the player steps away
//	from it, the route waiting.
//
//	Projectiles (plan, 7.4; k_threat.h): each way the player could move is
//	followed a second ahead against what flies at it or is about to - a
//	shot a monster winds up for is aimed at where the way puts the player
//	then - and costs the health its hits would take, a hit that could kill
//	far more. In battle that is one more part of each way's score; out of
//	a fight the route's way is judged, and when it would be hit the
//	cheapest way is taken: a sidestep on the way, or, if a hit could kill,
//	any way, the route waiting.
//
//	Escaping (plan, 7.4): while the brain goes for health with health
//	CRITICAL, or dashes for a weapon or ammo close by, no fight is started
//	and one going on is broken off; the player runs for it, shooting on the
//	way at what it sees - but not with a monster in reach of its blows,
//	which is fought first.
//
//	Holding (plan, 7.8: C3): walking the route out of an area with no
//	monsters inside (k_area.h) toward one with monsters inside, the route
//	waits short of the way in, the player facing it and fighting what
//	comes or shows itself - a door pressed, then stood back from as it
//	rises - and, with nothing worth fighting, creeping slowly on to the way
//	in and a little past it; once there with still nothing worth fighting,
//	or the area safe, it goes in. Runs for items wait while such an area
//	lies ahead (K_Combat_MonstersAhead).
//
//	Cover (plan, 7.8: C4): blows that need the monster to see the player -
//	an arch-vile's flame, an arachnotron's or the spider mastermind's
//	stream, a revenant's homing missile - are ducked out of sight of, just
//	in time, to the nearest spot it does not see that the player can get to
//	straight; and in battle each way the player could step counts what the
//	other monsters that would see it there cost: it fights from where the
//	fewest can shoot it.
//
//	Danger (plan, 7.5): what going to a spot would cost in health - the
//	monsters that could see the player there, each by what a fight with its
//	kind takes. A guarded medikit loses to a lonely one.
//

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "d_main.h"
#include "doomdef.h"
#include "doomstat.h"
#include "m_misc.h"
#include "p_local.h"

#include "k_area.h"
#include "k_combat.h"
#include "k_condition.h"
#include "k_graph.h"
#include "k_items.h"
#include "k_log.h"
#include "k_salt.h"
#include "k_sight.h"
#include "k_specials.h"
#include "k_threat.h"

#define K_PI 3.14159265358979323846
#define ANGLETURN_PER_RADIAN (65536.0 / (2.0 * K_PI))

// The blows ducked from (DUCK_EARLY).
extern void A_VileAttack();
extern void A_BspiAttack();
extern void A_SPosAttack();

// Monsters further off are not sight-checked: far beyond any target.
#define SIGHT_REACH 2048.0

// Targets: none further than TARGET_REACH, asleep ones only within
// ASLEEP_REACH. The current target scores TARGET_STICK times more, and a
// wounded one up to WOUNDED more again, nearly dead: enough to finish it,
// not so much that a monster coming close is left alone (the user: a
// zombieman walked up a corridor unshot while the bot kept to a wounded
// target further off).
#define TARGET_REACH 1024.0
#define ASLEEP_REACH 512.0
#define TARGET_STICK 1.3
#define WOUNDED 0.5

// Monsters with no bullets - their shots can be sidestepped - and more than
// LEAVE_HEALTH left are left alone beyond LEAVE_REACH (LEAVE_KEEP for the
// target already, or it flips at the edge): a cacodemon or a baron across a
// large room is little threat, and fighting it there spends the ammo the
// hitscanners further on need (the user: the last bullets spent on a
// cacodemon that was no threat, then death to a shotgun guy). It is fought
// once it comes close, or stands in the way. Not the arch-vile, whose flame
// no sidestep escapes, nor the pain elemental, whose lost souls keep coming.
#define LEAVE_REACH 512.0
#define LEAVE_KEEP 640.0
#define LEAVE_HEALTH 100

// A target lost and found again within REGAIN_TICS is no new one: the time
// it has gone unhurt and the shots that missed it count on (MAP03: a
// spectre in a pit, a sliver of it showing over the rim as it moved, was
// lost and found 42 times in 30 s, each time anew, and never left alone).
#define REGAIN_TICS 70

// A monster that drops what the player wants scores a whole time more for
// each DROP_WORTH its drop is worth (k_items.h), counting no more than
// DROP_MOST (the user, Doom II MAP03: out of shells, a shotgun guy is four
// shells for one; MAP04: with three times the score at a pistol start, a
// shotgun guy 445 off was shot at while a zombieman stood in the way).
#define DROP_WORTH 25.0
#define DROP_MOST 25.0

// A monster within CLOSE_REACH - in reach of its blows, or in the way -
// scores CLOSE_FIRST times more (plan 7.3: a monster within melee reach
// goes first; MAP04: a zombieman in the way held the bot 1.1 s, unshot).
#define CLOSE_REACH 96.0
#define CLOSE_FIRST 4.0

// While a pain elemental the shots can find is within TARGET_REACH, and not
// left alone, lost souls score SOUL_SHARE: it spits them out as fast as they
// die, so it goes first, even past souls within CLOSE_REACH, unless it is far
// off and they are on the player (the user; plan 7.3 point 4).
#define SOUL_SHARE 0.25

// Battle mode: for a target awake within BATTLE_REACH (asleep ones are
// targets only near anyway), in sight for SIGHTED_TICS in a row (a glimpse
// through a window is no fight: Doom II MAP02, monsters 420 units off
// showing for a tic or two, stopped the bot 58 times); over once there has
// been none for BATTLE_CALM tics. A target that loses no health for
// STALEMATE_TICS is left alone for IGNORE_TICS.
#define BATTLE_REACH 512.0
#define SIGHTED_TICS 8
#define BATTLE_CALM 35
#define STALEMATE_TICS 175
#define IGNORE_TICS 350
#define MAX_IGNORED 16

// A target MISS_SHOTS shots have not hurt is out of reach of them, as far
// as can be told (a sliver of it through a gap, the autoaim missing it): it
// is left alone for MISS_IGNORE_TICS (the user).
#define MISS_SHOTS 5
#define MISS_IGNORE_TICS 70

// Moving in battle: at STRAFE_SPEED, one way for STRAFE_MIN to STRAFE_MIN +
// STRAFE_SPREAD tics, then the other; within LEASH of where the fight began;
// away from awake monsters nearer than KEEP_AWAY - or than their kind's own
// distance, if further (kind_t: keep) and they can come at the player - by
// KEEP_PUSH for each KEEP_AWAY nearer than that. Each way is tried at
// MOVE_LOOK ahead, in MOVE_WAYS directions.
//
// Kept off (the user: a baron fought point blank cornered the bot against a
// wall and clawed it to death; another, in a cramped place, it could hardly
// step aside from, and it died to a demon in its way - a baron shot from a
// long way off, even with the shotgun, is easy): the further a monster
// keeps the player off the more dangerous its kind, and the nearer it is
// inside that, the harder it pushes. Nearer than its own distance it
// presses the player: there is no leash, where the fight began moving along
// as the player backs off, and each way scores ROOM_WEIGHT for each
// KEEP_AWAY it presses more as there is room on along it to back off
// further, out to ROOM_REACH (cornered, the player slips along the wall and
// round it, not into the corner). Rockets wanted - or the only gun but the
// pistol, against what they are for (RocketsFor) - keep the target
// ROCKET_ROOM beyond where rockets stop (ShotDamage), and each way scores
// ROCKET_TOWARD less as it goes toward it: running at it, rockets burst
// nearer (RocketGain). The target of a
// melee weapon (MeleeFor) is not backed off from: it must come close. And
// no step in a fight crosses a walk-over line that does something (Trips;
// as creeping, CreepStops): backing off goes further than strafing.
#define STRAFE_SPEED 10.0
#define STRAFE_MIN 20
#define STRAFE_SPREAD 30
#define LEASH 128.0
#define KEEP_AWAY 192.0
#define KEEP_PUSH 2.0
#define ROOM_REACH 160.0
#define ROOM_STEP 16.0
#define ROOM_WEIGHT 1.0
#define ROCKET_TOWARD 1.0
#define MAX_PUSHERS 32
#define MAX_TRIPS 64
#define MOVE_LOOK 32.0
#define MOVE_WAYS 16

// Retreating (the user, E3M9: barons fought in a cramped room, a bigger room
// nearby; E3M4 too, a baron's balls from 90 off, too near to step aside
// from; E3M9, teleported among three cacodemons, their balls from 60 to 100
// off): pressed hard - a monster RETREAT_PRESS times KEEP_AWAY or more
// inside its own distance - for RETREAT_AFTER, off open floor (a wall within
// RETREAT_OPEN), backing off does not work where the player is (a wall, a
// corner, a small room). Not by one that only bites: it follows the player
// anywhere, and a run from it is a run with it at the back (E3M5: a demon
// 74 off kept up with the retreat, biting, from 100 to dead in 2 s; backing
// off, the player shoots it). It retreats to open floor: the middle of a cell
// with no wall within RETREAT_OPEN, RETREAT_GAIN further from what presses
// it than the player is, walked to through open crossings within
// RETREAT_REACH, the way and the spot never nearer to any awake monster than
// RETREAT_CLEAR (than the player is, if nearer: E3M5, retreating from a
// demon past three imps, clawed and burned from 84 to 15); the nearest,
// what other monsters would see of it counted (Exposure). The fight goes
// on, shooting on the way; a step that
// something flying would hit is left to the fight's own move. Over once
// there (RETREAT_NEAR), once nothing has pressed for RETREAT_CALM, or after
// RETREAT_MOST; not again for RETREAT_AGAIN, nor looked for again for
// RETREAT_LOOK when none was found.
#define RETREAT_PRESS 0.5
#define RETREAT_AFTER (TICRATE / 2)
#define RETREAT_OPEN 128.0
#define RETREAT_GAIN 128.0
#define RETREAT_REACH 768.0
#define RETREAT_CLEAR 128.0
#define RETREAT_NEAR 32.0
#define RETREAT_CALM (TICRATE / 2)
#define RETREAT_MOST (4 * TICRATE)
#define RETREAT_AGAIN (3 * TICRATE)
#define RETREAT_LOOK TICRATE
#define RETREAT_POINTS 32

// In a standoff with a hopeless monster (AVOID_PAST) that sees the player
// for RETREAT_AFTER, backing off in its sight is no answer to bullets or
// rockets (E3M8: the spider mastermind, kept off at 500 in an open arena,
// shot the bot from 100 to dead in 3 s; no cover reachable before its next
// burst, the duck's test, was found): it retreats out of its sight - the
// nearest cell's middle it does not see, within HIDE_REACH through open
// crossings, the way clear of awake monsters as above, however long getting
// there takes. Over once there, once out of its sight for RETREAT_CALM, or
// after RETREAT_MOST.
#define HIDE_REACH 1024.0

// Open floor is floor with no wall within OPEN_REACH.
#define OPEN_REACH 64.0

// The highest step the player climbs.
#define MAX_CLIMB 24.0

// Fired when the facing is within this share of the target's radius of its
// centre, seen from the player, or MIN_TOLERANCE radians.
#define AIM_SHARE 0.8
#define MIN_TOLERANCE 0.005

// With salt (k_salt.h) the aim wanders across a monster, up to AIM_SALT of
// its radius either side of its centre - well within AIM_SHARE - to a new
// point every AIM_SALT_TICS.
#define AIM_SALT 0.4
#define AIM_SALT_TICS 8

// Pistol and chaingun shots are tapped beyond this distance.
#define TAP_DISTANCE 192.0

// A weapon is brought up only if it rates SWITCH_GAIN times the one in
// hand, and not within SWITCH_WAIT tics of the last switch; if the one in
// hand cannot hurt the target at all, after SWITCH_TICS. A switch takes
// about 0.9 s (Doom II MAP02: with a gain of 1.3, 24 switches in a minute).
#define SWITCH_GAIN 2.0
#define SWITCH_WAIT 105
#define SWITCH_TICS 35

// One that rates SWITCH_FAST_GAIN times the one in hand is brought up after
// SWITCH_TICS (MAP02: the pistol, up for a shotgun guy 630 off, stayed 2 s
// more against zombiemen and shotgun guys 40 to 90 off). And none is, while
// the next shot of the one in hand should kill the target: rated no more
// than kills it, a super shotgun looked slower than the pistol against a
// wounded shotgun guy 158 off (MAP02), and was put away for it. In Doom a
// switch takes time; mid-fight it is for when it really pays (the user).
#define SWITCH_FAST_GAIN 3.0

// The pistol is the tool of last resort (the user, Doom II MAP03: the bot
// brought it up for imps, and closed in on a chaingunner with it). It rates
// PISTOL_SHARE of its damage, and is put away after SWITCH_TICS, unless no
// other weapon but the bare fist can hurt the target, or it is SNIPE_REACH
// off or more (SNIPE_KEEP with the pistol in hand already: MAP03, it came
// up for an imp 643 off and went again a second later at 581) with no more
// than SNIPE_HEALTH left, sniped at: a zombieman, shotgun guy, chaingunner,
// imp, SS or lost soul, or anything worn down that far - never a spider
// mastermind or a mancubus, which would take too long (the user). Never
// one closing in - over the last CLOSING_TICS it came CLOSING_MOST nearer
// the player or more, of itself (the player's own moves aside), or it has
// not been watched that long: it comes at the player, and soon it is a
// fight (MAP04: the pistol came up for an imp 646 off as the imps closed
// in, the shotgun again 1.4 s later at 238, and the bot died 2 s after;
// MAP02: shotgun guys sniped at 650 to 730 off walked into a fight within
// a second or two). Every awake monster hunts the player, so that is no
// test (the user: sniping never happened). One standing off - on a far
// ledge, behind a window - is sniped at. Zombiemen and shotgun guys get no
// more than that (MAP02: with the
// pistol theirs at any distance, it stayed up 40 s, the super shotgun
// beside it rating no better against them). And only with the area clear
// for SNIPE_CALM: no fight on, no awake monster in sight within
// SNIPE_CLEAR; mid-fight the player shoots with what it has in hand (the
// user, MAP02: snipes right after a fight ended, the pistol up and away
// again within a second or two). Barrels out of a fight are popped with
// it still (PopWeapon).
#define PISTOL_SHARE 0.25
#define SNIPE_REACH 640.0
#define SNIPE_KEEP 512.0
#define SNIPE_HEALTH 100
#define SNIPE_CLEAR 512.0
#define SNIPE_CALM (3 * TICRATE)
#define CLOSING_TICS TICRATE
#define CLOSING_MOST 32.0

// The bare fist (no berserk) is the last resort of all: it rates FIST_SHARE
// of its damage (MAP04: the shotgun ran dry with an imp 53 off, the fist
// tied with the pistol and came first, and the imps took the bot from 37
// to 5). So is any melee - the berserk fist, the chainsaw - against a
// monster with more than MELEE_HEALTH left: a baron or a cacodemon takes
// many blows, the player standing in reach of its own (the user: the fist
// brought up against a baron of hell, ammo full).
//
// Three weapons, three uses (the user: the fist up and away again a second
// later, the monster out of reach, the switches costing the time to shoot
// and leaving the player open; E2M8: the fist up for the cyberdemon 104 off,
// rockets too close, bullets gone):
// - the chainsaw is for what it keeps in pain, hardly able to attack: those
//   that bite only (a demon, a spectre, a lost soul), and a cacodemon or a
//   pain elemental with no other awake monster within SAW_ALONE of the
//   player. Against them it rates SAW_GAIN times its damage (it saves the
//   ammo), never less for their health, and is brought up MELEE_IN inside
//   its reach and kept up while they stay within MELEE_KEEP - they come
//   back for the next bite;
// - the berserk fist is for the weakest, zombiemen and imps, MELEE_IN inside
//   its reach: aimed badly, a punch that misses lets anything tougher answer
//   hard;
// - the bare fist is useless but when nothing else can fire and no ammo lies
//   in reach to be fetched.
// With nothing else to fire, the chainsaw and the berserk fist go against
// anything. Not for a weapon in hand that cannot fire for now (rockets too
// close). On a tie, a weapon that shoots wins.
#define FIST_SHARE 0.25
#define MELEE_HEALTH 200
#define MELEE_IN 16.0
#define MELEE_KEEP 128.0
#define SAW_ALONE 512.0
#define SAW_GAIN 2.0

// Rockets (the user: the rocket launcher was a suicide on most levels). The
// blast reaches BLAST less the larger of the x and y distances, less the
// victim's radius (P_RadiusAttack), so a rocket is fired only where it
// would burst ROCKET_SAFE off the player measured that way, the blast, the
// player's box and a margin for its own moves: at the target's front, the
// target closing in during the flight (ROCKET_SPEED a tic), or at any other
// monster in its path, its radius counted; with its path clear of walls by
// its radius either side as far as a burst would hurt (a corner the
// centre misses). The weapon is wanted only where that holds whichever way
// the player faces (the diagonal: ROCKET_MIN of flight to the target's
// front). Never at a lost soul, nor with one within SOUL_CLEAR: they charge
// at the player and meet the rocket halfway. All of it unless invulnerable
// for INVULN_LEFT tics more at least. And rockets are dear: against a
// monster with no more than ROCKET_HEALTH left and alone (no other within
// BUNCHED of it, where the blast would get it too) they rate ROCKET_SHARE
// of their damage in hand, and are never brought up for one while another
// gun can fire - no rocket for a zombieman or an imp (the user; E3M3: up
// for imps 650 to 800 off, the shotgun rating little there, and away again
// within a second or two, four times in a minute). Nor without room to stay
// of use (RocketRoom): the target ROCKET_ROOM beyond where rockets stop, and
// no other monster that can come at the player that near, which would soon
// be the target (the user: up for a baron with a demon in the way, the
// shotgun back at once, the demon biting all the while). The
// player's own run along the rocket's way counts (RocketGain): the rocket
// gains only ROCKET_SPEED less that on it, so every burst is nearer by that
// share - at a full run a fifth as far (the user, E2M6: 64 health off its
// own rocket; E1M9: shots on the way at imps and zombiemen 400 to 500 off,
// the player running after each rocket at 13 to 16 a tic and caught in the
// burst, on the imp, a barrel or a ledge - once killed).
#define ROCKET_RADIUS 11.0
#define ROCKET_SPEED 20.0
#define MISSILE_SPAWN 32.0
#define ROCKET_SAFE (BLAST + PLAYER_RADIUS + 48.0)
#define ROCKET_MIN (ROCKET_SAFE * 1.4142)
#define SOUL_CLEAR 384.0
#define INVULN_LEFT 70
#define ROCKET_HEALTH 100

// The BFG (the user, Doom II MAP15: a ball fired at a baron in the tower
// room, then the bot turned for a pain elemental): its 40 tracers leave from
// where the player stands 16 tics after the ball has burst (A_BFGSpray, on
// S_BFGLAND3), spread 45 degrees either way of the way the ball flew, out to
// 1024 - whichever way the player faces by then. So from the ball's leaving
// until its spray, the player keeps where it bursts - the monster it was
// fired at, the burst once it has - in sight, within BFG_CONE of the ball's
// way and BFG_REACH: a fight step that would lose it scores BFG_HOLD less,
// turning to another target or not; a fight is begun or kept on meanwhile
// (the route waits), and no retreat begun. Pressed only from the ready
// state, as rockets: held, A_ReFire fires again at the end of the shot, the
// ball leaving with no aim held for it. Held from the press on: the ball
// leaves 30 tics later, from where the player is then. And fired only with
// the ball's path clear by its radius, BFG_RADIUS, either side (MAP10:
// through a window at the Cyberdemon, the ball burst on its frame 39 to 55
// units off, the spray finding nothing).
#define BFG_CONE (40.0 * K_PI / 180.0)
#define BFG_REACH 960.0
#define BFG_HOLD 3.0
#define BFG_NEAR 32.0
#define BFG_RADIUS 13.0

// Invulnerable for INVULN_BOLD more at least (the user: plan to have it for
// the fight), the target is fought as far as TARGET_REACH, far and
// dodgeable or not, and no hold is begun; rockets fire whatever the burst
// (INVULN_LEFT). Monsters are still kept off as ever. Tried and dropped:
// keeping nothing off, closing in on the target - E3M9, rockets took the
// Cyberdemon (4000, no splash) to half in the 30 s, and as it wore off the
// bot stood 190 from it, and died to its rockets.
#define INVULN_BOLD (4 * TICRATE)

// A hard fight, which an invulnerability sphere is for (the user: grab it
// when preparing for tough fights - a Cyberdemon, masses of monsters): awake
// in sight within HARD_REACH, not fighting another, a monster whose kind
// costs HARD_ONE or more (the Cyberdemon, the spider mastermind, the
// arch-vile) or that is hopeless, or monsters costing HARD_CROWD in all;
// lasting HARD_LINGER after, while the fight comes and goes from sight
// (K_Combat_HardFight).
#define HARD_REACH 1024.0
#define HARD_ONE 50.0
#define HARD_CROWD 80.0
#define HARD_LINGER (5 * TICRATE)

// Left alone for a telefrag (K_Combat_Telefrag): the monsters a player
// landing on the spot would overlap (PIT_StompThing), TELEFRAG_SLACK more for
// one stepping about in its cage.
#define TELEFRAG_SLACK 16.0

// Monsters fighting another, logged as left alone, at most at once.
#define MAX_INFIGHT 16
#define BUNCHED 96.0
#define ROCKET_SHARE 0.25
#define ROCKET_ROOM 64.0

// Tiers, by spawn health: the weak ones - zombiemen, shotgun guys,
// chaingunners, SS, imps, lost souls, demons, spectres - have WEAK_HEALTH or
// less; the tough ones - mancubi, arch-viles, barons, the spider mastermind,
// the Cyberdemon - TOUGH_HEALTH or more; revenants, cacodemons, pain
// elementals, hell knights and arachnotrons are between.
#define WEAK_HEALTH 150
#define TOUGH_HEALTH 600

// Rockets and cells are kept for the tough monsters (the user: with tough
// monsters alive, keep plasma cells in reserve for them; the low tier
// monsters are cleared with low tier weapons): while what the rocket
// launcher, the plasma rifle and the BFG held could do with the rounds there
// are for them (FLIES_HIT of their damage a round) is no more than the health
// of the tough monsters alive in the level - nor than RESERVE_SHARE of what
// they could do with as many as the player can carry - those weapons rate
// nothing against a weak monster while another gun can fire, the pistol
// too, nor against one between while another gun but the pistol can.
// Above that, they are for anything, as before.
#define RESERVE_SHARE 0.5

// Hopeless (plan 7.3 point 3, fight or avoid; the user, E3M9: the shells
// spent on the cyberdemon with the shotgun, then the bullets with the
// pistol, and the red key's room of hitscanners gone into with rockets
// only): a monster that is not weak the guns held cannot kill - the rounds
// there are would not take HOPELESS_SHARE of the health it has left (the
// target of a fight on, HOPELESS_KEEP: it flips at the edge; a wounded
// cacodemon with a little less than its health in hand, kept off and
// passed, bit the bot from 81 to 21 - most of the way is worth fighting, the
// damage rolls and the pain go the player's way often enough), or none of
// them would kill it within HOPELESS_TICS of firing on. A round counts from
// as far as a fight keeps the monster off (kind_t: keep): a projectile's
// FLIES_HIT of its damage (a big, slow target, kept where rockets fire), a
// bullet's or a pellet's as the spread and range allow (HitShare). Melee
// never counts; invulnerable for INVULN_LEFT more, nothing is hopeless.
// A hopeless monster is no target - no round is spent on it - and one
// awake in sight within its kind's distance (AVOID_PAST beyond it once kept
// off), whether it can come at the player or shoots from where it stands,
// is kept off: a standoff, battle mode
// with nothing to shoot, the route waiting while the player backs off it
// toward room, out of its sight (Exposure), until it has been out of sight
// for AVOID_LINGER - hidden round a corner, the route walked straight back
// at it, the cyberdemon having followed to 163 off (Doom II MAP10: two
// rockets, dead) - or is off, then BATTLE_CALM more. Kept off AVOID_MOST in
// all, it is passed for AVOID_SKIP,
// the route going on (never stuck: it may stand where the route goes) - but
// kept off still within CLOSE_REACH, in reach of its blows. While
// one met awake in sight stays awake, the ammo and weapons that would kill
// it in time are wanted (K_Combat_Beats; k_items.h: running out). A boss the
// level needs dead, met, with none of those left to fetch, is fought with
// what there is (K_Combat_Needed): kept off and passed for good it would
// never die.
#define HOPELESS_SHARE 0.5
#define HOPELESS_KEEP 0.25
#define HOPELESS_TICS (60 * TICRATE)
#define FLIES_HIT 0.8
#define AVOID_PAST 128.0
#define AVOID_LINGER (3 * TICRATE)
#define AVOID_MOST (10 * TICRATE)
#define AVOID_SKIP (10 * TICRATE)
#define MAX_HOPELESS 16

// Monsters that cannot come at the player (the user: monsters shut in for
// good behind bars, fought for ammo the bot needed later): judged by the
// straight line from the monster to the player (WayClear), again every
// REACH_TICS - most often right, and a mistake only means the monster is
// killed later. Those within CLOSE_REACH can. One that cannot is shot on
// the way, OUT_SHOTS shots, and left alone for IGNORE_TICS; never fought
// (the route goes on), never shot asleep, nor if it only bites; it scores
// OUT_SHARE as a target. Not gunmen, whose bullets reach the player
// anyway: always cleared (the user). And when comfy - health OK and the
// ammo of the weapon in hand at COMFY_AMMO of what the player can carry or
// more, until it falls under UNCOMFY_AMMO - they are fought as any. Tried
// and dropped: a cacodemon or tougher shooting at the player from where it
// stands fought as any (E3M9: the cyberdemon on a ledge rocketed the bot at
// a switch) - Doom II MAP06's hell knights up on a ledge, left alone before
// and passed, were fought from below for 25 s, the ammo gone, and the bot
// died in all three runs (it had finished two of three).
#define REACH_TICS 17
#define REACH_PIECE 128.0
#define MAX_REACH_MEMO 64
#define OUT_SHOTS 2
#define OUT_SHARE 0.5
#define COMFY_AMMO 0.75
#define UNCOMFY_AMMO 0.5

// Barrels (MT_BARREL, 20 health): dead, one blows up 12 to 15 tics later
// (S_BEXP to A_Explode), hurting whatever can see it for BLAST less the
// distance - the larger of the x and y distances, less the victim's radius
// (P_RadiusAttack) - never a Cyberdemon or Spider Mastermind. Barrels
// under BARREL_CHAIN apart set each other off (20 health: BLAST - 20 + the
// radius). The player keeps BLAST_SAFE from any barrel that could go off:
// the blast, its radius and a margin. No shot can graze a barrel that close
// (BARREL_MARGIN), counting the weapon's spread and the barrels behind the
// target, which a miss flies on to (the user: on Doom II MAP02 the bot came
// close to shooting barrels beside it).
#define BLAST 128.0
#define BARREL_HEALTH 20.0
#define BARREL_RADIUS 10.0
#define BARREL_HEIGHT 42.0
#define BLAST_HEIGHT 10.0
#define BARREL_CHAIN 118.0
#define BLAST_SAFE 160.0
#define BARREL_MARGIN 4.0
#define MAX_BARRELS 256

// A barrel a monster's shot has hit already (less than BARREL_HEALTH left)
// is primed: the next hit sets it off (the user, E1M5: an imp's fireballs
// hit one twice in a doorway, the bot passing beside it, and it went off at
// the second). Kept away from in battle as one going off; and popped
// first, ahead of the target, from where the player is with the weapon in
// hand, when its chain is clear of the player and it lies within
// PRIMED_REACH of the player or by the route ahead. Out of a fight, one
// whose blast could reach the player while an awake monster can see it is
// stepped away from, as one going off, out to where it can be popped; for
// PRIMED_DODGE_TICS at the most, then passed (no shot at it from anywhere,
// the route leading past it).
#define PRIMED_REACH (2.0 * BLAST_SAFE)
#define PRIMED_DODGE_TICS (3 * TICRATE)

// A barrel is shot instead of the target when its blast is worth
// BARREL_GAIN times a shot at the target, and at least BARREL_MIN (a
// zombieman's cost): the monsters it would hurt, each by its kind's cost
// and the share of its health it would take, times the odds a shot pops
// it; one walking about as if it walked away DRIFT_PER_SPEED times its
// speed during the fuse, one attacking or asleep where it stands. Only the
// barrel shot counts, not those it would set off (the user: chains are too
// much). Another barrel is taken only at BARREL_SWITCH times the worth.
// With the weapon in hand, in BARREL_SHOTS shots at the most on average,
// and in a fight at one go (not the pistol or chaingun). It is shot at
// until it goes off: only shots that take none of its health count, and
// after BARREL_MISSES of them it is left alone a while (MAP23: two pistol
// bullets took 15, and it was left for another).
#define BARREL_GAIN 1.5
#define BARREL_SWITCH 1.5
#define BARREL_MIN 5.0
#define DRIFT_PER_SPEED 2.5
#define BARREL_SHOTS 2
#define BARREL_MISSES 3

// The brain's barrel is the live one within BLOCKING_DRIFT of where it
// stood.
#define BLOCKING_DRIFT 32.0

// A trap barrel (the user, MAP02: one the shotgun guys nearly always set
// off as the player passes): within BLAST_SAFE of the route TRAP_AHEAD
// ahead, where a live monster within TRAP_SEEN could see it. Popped first,
// from where the player is, but only when it is cheap: nothing else to
// shoot, ammo not LOW, a hitscan weapon in hand. Looked for every
// TRAP_TICS.
#define TRAP_AHEAD 768.0
#define TRAP_SEEN 1536.0
#define TRAP_TICS 8

// In battle, a spot within BLAST_SAFE of a barrel scores less: a monster's
// shot may set it off (the user: hit hard by a barrel a monster shot).
#define BARREL_AWAY 2.0
#define DYING_AWAY 6.0

// Out of a fight, a shot barrel about to go off whose blast could reach the
// player holds the route while the player steps away from it at DODGE_SPEED
// (MAP02: the bot shot one from a safe distance, then walked on toward it
// and its chain hit it).
#define DODGE_SPEED 16.0

// Projectiles, and the shots monsters wind up for (k_threat.h). Each way
// the player could move is followed from now as nav would steer it
// (K_Nav_Simulate), the player held where it could not step, looked at
// every CHECK_TICS; its hits cost the health they would take. In battle a
// way scores THREAT_WEIGHT less for each unit of that - an imp's fireball
// at full health about as much as losing the target from sight, a rocket
// that could kill far more than anything else (the user: at full health a
// few fireballs do no harm, while dodging them can make things worse;
// rockets always) - tried at STRAFE_SPEED and at DODGE_SPEED, which scores
// SPRINT_COST less. The strafe turns the other way on its own only with
// nothing coming: turning back is what walks into a shot.
#define THREAT_WEIGHT 0.1
#define SPRINT_COST 0.1
#define CHECK_TICS 6

// Out of a fight the way the route takes the player is judged the same way
// (standing, when nav waits at a door, lift or switch, or has no goal);
// when it would cost DODGE_MIN or more, so is each way at DODGE_SPEED, and
// the cheapest is taken: the route by ROUTE_KEEP, the way stepped already
// by DODGE_KEEP; a step within BLAST_SAFE of a barrel costs up to
// BARREL_COST (DYING_COST for one primed or going off). Walking the route
// the player sidesteps on the way, no more than 90 degrees off it, each
// unit of 1 - cos of the angle costing DETOUR_COST; unless a hit could kill:
// then the route waits, as it does standing, while the player steps any way
// (the user). Never riding a lift up, nor running a jump.
// A projectile hit seen DODGEABLE_TICS before or more came in time to step
// out of its way from standing (about 7 tics, and up to 3 before a command
// runs); counted apart: those are the dodging's misses, the rest came too
// close. Of those, one the way taken expected (K_Threat_HitBy) was taken
// knowingly: any other way cost more.
#define DODGEABLE_TICS 12

#define DODGE_MIN 1.0
#define ROUTE_KEEP 1.0
#define DODGE_KEEP 1.0
#define DETOUR_COST 3.0
#define BARREL_COST 10.0
#define DYING_COST 40.0

// Holding (the user: by default stay in safe areas - open a door and hold
// outside, cherry-picking, instead of rushing in to be surrounded). Walking
// the route from an area with no monsters inside (k_area.h; or a door's or
// lift's cells) - seen from the area ahead or not, fighting or not (Doom
// II MAP14: the blue door open, the chaingunner behind it saw the bot's
// area, which was then not safe, and the bot walked in to fight it at 65
// units) - when it is about to enter, within HOLD_START along it, an area
// that is not safe with HOLD_RISK or more of monsters inside (AreaRisk),
// through a narrow way (a door or lift, a narrow opening: over open floor
// there is no way in for them to come through one by one, and room to move
// aside in - the user, E2M9: held in the soulsphere's pit in the middle of
// an open room, short of another piece of it, the bot was surrounded; not
// begun on an island either, OnIsland), the route waits - begun no further
// off, so the route walks the player up
// to it, not a straight line (Doom II MAP04: begun 290 short of the spot,
// the bot walked to it over a floor that hurts, 40 health) - and is kept,
// out to HOLD_AHEAD, as the monsters come and make the player's own
// area unsafe (E1M5, E1M8: holds dropped within half a second, as soon as
// monsters came in sight, and the bot walked into the fight) - while the
// player stands HOLD_BACK short of the way
// in along the route (further back out of a door's or lift's cells, up to
// HOLD_BACK_MOST; straight back past the route's start, planned at the
// door), facing it, and fights what comes or can be seen, the fight
// leashed to the spot (HOLD_LEASH). A shut door on the way there is
// pressed first, and the hold begins as it rises: the player backs off
// while it opens. Shut again while holding (a fight from the spot outlasts
// it), it is opened again and the hold taken up again, its time going on.
// A lift on the way there, not down, starts no hold. The spot is never on
// a floor that hurts, as no fight is (C1), and creeping stops short of
// one, and CREEP_SHORT short of a jump, a crusher or a walk-over line that
// does something (CreepStops). With nothing worth fighting for CREEP_WAIT, the player at the spot
// (within HOLD_AT), the spot creeps toward the way in and
// CREEP_PAST beyond it along the route at CREEP_SPEED (the user: slowly, so
// monsters round the corners there see the player one by one - not stand,
// then rush in), and waits while there is; the player faces the way in,
// the first point of the creeping's way CREEP_LOOK on. Over once
// the area is safe, or below HOLD_RISK; once crept all the way with nothing
// worth fighting, or after HOLD_MOST in all, the area is gone into (never
// stuck) and not held off from again for HOLD_SKIP.
#define HOLD_AHEAD 512.0
#define HOLD_START 192.0
#define HOLD_AT 32.0
#define HOLD_RISK 10.0
#define HOLD_BACK 96.0
#define HOLD_BACK_MOST 384.0
#define HOLD_STEP 16.0
#define HOLD_LEASH 64.0
#define HOLD_MOST (20 * TICRATE)
#define HOLD_SKIP (20 * TICRATE)
#define HOLD_GAIN 0.25
#define HOLD_SPEED 8.0
#define HOLD_NEAR 12.0
#define CREEP_WAIT (TICRATE / 2)
#define CREEP_SPEED 3.0
#define CREEP_PAST 64.0
#define CREEP_LOOK 64.0
#define CREEP_SHORT 24.0
#define CREEP_POINTS 32

// Out of a fight, a player that has come no nearer to the spot for
// HOLD_STALL, HOLD_AT off it or more, cannot get there: the hold is over, the
// route planned again (Doom II MAP03: the floor ahead rose during a hold,
// a step of 184 on the way creeping went; the player stood short of the
// spot until HOLD_MOST was up, 10 s).
#define HOLD_STALL (2 * TICRATE)

// Ducking (plan, 7.8: C4; the user: some threats are answered by getting
// out of sight, not by dodging): an arch-vile winding up its flame (it
// lands as the vile enters A_VileAttack, 66 tics after the attack starts,
// and only if the vile still sees the player), an arachnotron or the
// spider mastermind winding up its stream or firing it (it stops once they
// lose sight: A_SpidRefire), a revenant's homing missile coming (in the
// open it turns back for pass after pass; it meets a wall). From when the
// blow is coming the nearest spot out of its sight (as the monster sees:
// P_CheckSight from its eyes; the missile's, where it will be when the
// player gets there, on its way to the player) is looked for every
// DUCK_REPLAN tics: one the player gets to in time, straight along one of
// MOVE_WAYS rays, every DUCK_STEP on floor it can step on, at a spot every
// DUCK_RING out to DUCK_REACH, from standing, strafing (DUCK_THRUST a tic,
// the friction DUCK_FRICTION: top speed 13.3), DUCK_CORNER tics more for
// each corner, DUCK_LATENCY tics late (the commands already sent; Doom II
// MAP11, MAP14: reckoned at 16 a tic from the start, the bot got to cover
// after the flame). Spots other monsters see (DUCK_SEEN each, by
// what they cost) and ways that something flies along (DUCK_HIT a point
// of health) count as further off. With none along the rays, the cells
// the player can walk to through open crossings, nearest first (portal
// middle to portal middle), out to DUCK_GRAPH_REACH: the nearest whose
// middle is out of sight and in time, walked to by way of the portals
// (Doom II MAP14: a hold in a pocket off a door, ledges on three sides,
// the vile looking in and up the corridor beyond: the cover was back
// through the door and to the side). The player shoots on, and steps there
// once the time left is the time to get there and DUCK_SLACK (Doom II
// MAP14: looked for only 24 tics before the flame, no cover was in reach,
// and the vile killed the bot; a vile's attack is its time to be shot).
// The route waits, the fight goes on from there; over DUCK_LINGER after
// the blow is past, or after DUCK_MOST. None in time: logged once a blow.
#define DUCK_REPLAN 4
#define DUCK_SLACK 6
#define DUCK_STEP 8.0
#define DUCK_RING 32.0
#define DUCK_REACH 512.0
#define DUCK_SPEED 16.0
#define DUCK_THRUST 1.25
#define DUCK_FRICTION 0.90625
#define DUCK_TRAVEL_TICS 105
#define DUCK_CORNER 3
#define DUCK_LATENCY 4
#define DUCK_SEEN 4.0
#define DUCK_HIT 8.0
#define DUCK_LINGER (TICRATE / 4)
#define DUCK_MOST (3 * TICRATE)
#define DUCK_GAIN 0.5
#define DUCK_NEAR 4.0
#define DUCK_MISSILE_REACH 512.0
#define DUCK_STREAM 18
#define DUCK_GRAPH_REACH 768.0
#define DUCK_POINTS 32
#define DUCK_WAYPOINT 24.0

// Exposure (C4): in battle, each way the player could step scores less by
// what the awake monsters but the target that would see it there cost
// (EXPOSE_WEIGHT a point; the MAX_EXPOSERS costliest in sight within
// EXPOSE_REACH, not those that only bite): it fights from where the fewest
// can shoot it - a door frame, a corner, the target alone in sight.
#define EXPOSE_WEIGHT 0.05
#define EXPOSE_REACH 1024.0
#define MAX_EXPOSERS 6

// Out of a fight, hurt in the last BLOW_HOLD tics by a monster within
// BLOW_REACH that the shots cannot find, the player holds the route - even
// waiting at a door or lift - and steps away from it until it is BLOW_SAFE
// off, where the autoaim may find it (the user, Doom II MAP03: an imp at the
// top clawed the bot riding the lift up for 25, vanilla's blows reaching
// any height).
#define BLOW_REACH 96.0
#define BLOW_SAFE 160.0
#define BLOW_HOLD 35

// A spot to step to keeps EDGE_MARGIN from any edge the player could slip
// off; stepping away from a blast or blows, no less than the player is
// already (MAP03: on the very edge of a pit, a demon below biting, no spot
// was far enough from it, and the bot stood and died).
#define EDGE_MARGIN (PLAYER_RADIUS + 8.0)

// A projectile is incoming when it will pass within the player's radius
// and PROJECTILE_MARGIN of it, within PROJECTILE_HORIZON tics.
#define PLAYER_RADIUS 16.0
#define PROJECTILE_MARGIN 8.0
#define PROJECTILE_HORIZON 70.0

// A target that moved further than this in a tic teleported.
#define TARGET_JUMP 64.0

// The monsters guarding a spot: those within DANGER_REACH that can see the
// player standing there, in full within DANGER_FULL, less and less beyond;
// asleep and facing away, a share of ASLEEP_AWAY (it may sleep through the
// visit: P_LookForPlayers looks only ahead).
#define DANGER_REACH 1024.0
#define DANGER_FULL 512.0
#define ASLEEP_AWAY 0.5
#define PLAYER_HEIGHT 56.0

// How far a fight keeps a kind off (keep; KEEP_AWAY for those with none):
// the weak ones are shot from near, as they come; the tougher a kind the
// further, out of reach of its blows, its shots seen coming from further
// off. Monsters fire less the further off they are, at the least past 264
// for those with blows and 392 for those without (P_CheckMissileRange); the
// Cyberdemon's rockets burst on the player from near, and the spider
// mastermind's bullets spread.
typedef struct
{
    mobjtype_t type;
    const char *name;
    double threat;          // how much to fear it as a target, roughly
    double cost;            // the health a fight with it takes, roughly
    boolean hitscan;        // its attack is a bullet
    double keep;            // how far a fight keeps it off, 0 for KEEP_AWAY
} kind_t;

static const kind_t kinds[] =
{
    { MT_POSSESSED, "zombieman",         3.0,   5.0, true,    0.0 },
    { MT_SHOTGUY,   "shotgun guy",       4.0,  10.0, true,    0.0 },
    { MT_CHAINGUY,  "chaingunner",       5.0,  20.0, true,    0.0 },
    { MT_WOLFSS,    "Wolfenstein SS",    4.0,  10.0, true,    0.0 },
    { MT_SPIDER,    "spider mastermind", 6.0, 100.0, true,  512.0 },
    { MT_VILE,      "arch-vile",         6.0,  50.0, false, 384.0 },
    { MT_PAIN,      "pain elemental",    6.0,  20.0, false, 320.0 },
    { MT_UNDEAD,    "revenant",          4.0,  25.0, false, 320.0 },
    { MT_FATSO,     "mancubus",          4.0,  30.0, false, 384.0 },
    { MT_BABY,      "arachnotron",       4.0,  25.0, false, 320.0 },
    { MT_CYBORG,    "cyberdemon",        6.0, 100.0, false, 512.0 },
    { MT_BRUISER,   "baron of hell",     3.0,  30.0, false, 384.0 },
    { MT_KNIGHT,    "hell knight",       3.0,  20.0, false, 320.0 },
    { MT_HEAD,      "cacodemon",         2.5,  15.0, false, 256.0 },
    { MT_TROOP,     "imp",               2.0,   8.0, false,   0.0 },
    { MT_SERGEANT,  "demon",             2.0,   8.0, false, 256.0 },
    { MT_SHADOWS,   "spectre",           2.0,  10.0, false, 256.0 },
    { MT_SKULL,     "lost soul",         2.0,   6.0, false,   0.0 },
    { MT_KEEN,      "Commander Keen",    0.5,   0.0, false,   0.0 },
};

static const kind_t other_kind = { NUMMOBJTYPES, "monster", 1.0, 10.0, false, 0.0 };

typedef struct
{
    weapontype_t weapon;
    weapontype_t slot;      // the number key that brings it up
    const char *name;
    int shot;               // ammo a shot takes
    double damage;          // a shot's average damage, all of it on target
    double tics;            // between shots, firing on
    int lead;               // from the press to the shot leaving: the attack
                            // states before the one that fires
    double spread;          // degrees a shot strays either way
    boolean taps;           // no spread when tapped
    boolean flies;          // a projectile: a moving target can dodge it
    double reach;           // melee: how far it reaches; 0 otherwise
    double range;           // beyond this it rates less and less, down to a
                            // quarter at twice that (shells are not spent
                            // far off); 0 for no limit
} weapon_t;

// The fist's slot brings up the chainsaw when the player has it, and the
// shotgun's the super shotgun.
static const weapon_t weapons[] =
{
    { wp_fist,         wp_fist,     "fist",            0,  11.0, 22.0,  4,  0.0, false, false, 64.0,   0.0 },
    { wp_chainsaw,     wp_fist,     "chainsaw",        0,  11.0,  4.0,  0,  0.0, false, false, 64.0,   0.0 },
    { wp_pistol,       wp_pistol,   "pistol",          1,  10.0, 20.0,  4,  5.6, true,  false,  0.0,   0.0 },
    { wp_shotgun,      wp_shotgun,  "shotgun",         1,  70.0, 37.0,  3,  5.6, false, false,  0.0, 400.0 },
    { wp_supershotgun, wp_shotgun,  "super shotgun",   2, 200.0, 57.0,  3, 11.2, false, false,  0.0, 300.0 },
    { wp_chaingun,     wp_chaingun, "chaingun",        1,  10.0,  4.5,  0,  5.6, true,  false,  0.0,   0.0 },
    { wp_missile,      wp_missile,  "rocket launcher", 1, 120.0, 20.0,  8,  0.0, false, true,   0.0,   0.0 },
    { wp_plasma,       wp_plasma,   "plasma rifle",    1,  22.5,  3.0,  0,  0.0, false, true,   0.0,   0.0 },
    { wp_bfg,          wp_bfg,      "BFG",            40, 500.0, 60.0, 30,  0.0, false, true,   0.0,   0.0 },
};

static k_monster_t *monsters;
static const mobj_t **monster_mobjs;
static int num_monsters;
static int monster_capacity;

static k_projectile_t *projectiles;
static const mobj_t **projectile_mobjs;
static int num_projectiles;
static int projectile_capacity;

// The monsters alive last tic, to tell which died since. Compared, never
// followed: a mobj removed from the level is freed.
static const mobj_t **was_alive;
static int num_was_alive;
static int was_alive_capacity;

// The states of the monsters' attacks: from their missile and melee states
// on, until they go back to chasing.
static byte attack_states[NUMSTATES];
static boolean attack_states_marked;

static k_combatstatus_t status;
static const mobj_t *target_mo;     // the target, last tic
static const mobj_t *lost_mo;       // the target before, and when it was lost
static int lost_tic;
static k_vec2_t target_last;        // where it was then
static k_vec2_t target_velocity;    // smoothed, units per tic
static int wanted = -1;             // the weapon wanted, -1 if none
static boolean can_hurt;            // some weapon in hand can hurt the target
                                    // (one not weak: from where a fight keeps
                                    // it, it being no hopeless one)
static int last_switch;             // leveltime of the last switch pressed
static const mobj_t *shot_mo;       // what the last press read was fired at
static int shot_tic = -1;           // and the tic its shot leaves (lead)
static k_vec2_t shot_at;            // where it was last seen
static int unclear_tic;             // the area was last not clear (UpdateClear)
static int last_health = -1;

// The target's health, and when it last went down (or the target changed);
// how many tics in a row it has been the target.
static int target_health;
static int progress_tic;
static int sighted;

// Where the target was over the last CLOSING_TICS, and whether it is
// closing in (PistolRight).
static k_vec2_t target_track[CLOSING_TICS];
static int tracked;
static boolean closing;

// Shots fired since the target was last hurt, told by the ammo the weapon in
// hand uses going down.
static int misses;
static int last_ammo;
static int last_ammo_type = am_noammo;

// Targets given up on for now, and until when; unreachable: given up on as
// one that could not come at the player, only until it can.
typedef struct
{
    const mobj_t *mo;
    int until;
    boolean unreachable;
} ignored_t;

static ignored_t ignored[MAX_IGNORED];

// Battle mode.
static boolean battle;
static int battle_since;
static int battle_kills;            // the kill count when it began
static k_vec2_t anchor;             // where it began
static int calm;                    // tics without a target worth fighting
static int strafe_dir = 1;          // 1 or -1: to the target's left or right
static int strafe_tics;             // until it turns the other way
static k_vec2_t move;               // the velocity wanted
static boolean pressed;             // a monster presses the player (KEEP_PUSH)
static const mobj_t *pressed_mo;    // the one last logged as pressing
static int presser = -1;            // this tic's, by place in the list, and
static double press;                // by how many KEEP_AWAY inside its distance

// Retreating to open floor (RETREAT_OPEN): the way there (the spot last),
// the point gone for, since when, the tics with nothing pressing; the tics
// pressed hard in a row, and when it may be looked for again.
static boolean retreating;
static boolean retreat_hide;        // out of the sight of a hopeless monster
static int seen_tics;               // in a standoff, seen by it, in a row
static k_vec2_t retreat_points[RETREAT_POINTS];
static int retreat_num_points;
static int retreat_next;
static int retreat_since;
static int retreat_calm;
static int hard_pressed;
static int retreat_again;

// The walk-over lines near the player this tic that do something, which no
// step in a fight crosses (Trips).
static int trips[MAX_TRIPS];
static int num_trips;

// The area with monsters inside the route goes into ahead, as last found
// (K_Combat_MonstersAhead).
static int ahead_area = -1;

// Fights broken off: the brain goes for health, CRITICAL, or dashes for a
// weapon or ammo close by (K_Combat_Escape), and why.
static boolean escape;
static char escape_why[64];

// Rockets and cells kept for the tough monsters (RESERVE_SHARE).
static boolean reserve;

// Hopeless monsters (HOPELESS_SHARE): this tic, by place in the list, and per
// weapon whether it would kill one awake in time (K_Combat_Beats); those
// logged as hopeless; the one kept off this tic (by place, -1 if none), the
// one kept off last and for how long in all, and the one passed, until when.
static boolean *hopeless;
static int hopeless_capacity;
static boolean beats[NUMWEAPONS];
static int needed = -1;             // the kind the level needs dead (K_Combat_Needed)
static boolean fight_needed;        // and fought anyway this tic, hopeless or not
static const mobj_t *hopeless_logged[MAX_HOPELESS];
static int avoid = -1;
static const mobj_t *avoid_mo;
static int avoid_tics;
static int avoid_seen;              // when the one kept off was last in sight
static const mobj_t *passed_mo;
static int passed_until;

// Monsters fighting another, logged as left alone (MAX_INFIGHT).
static const mobj_t *infight_logged[MAX_INFIGHT];

// The player's BFG ball whose spray is to come (BFG_HOLD), this tic: the way
// it flew, where it is, whether it has burst; what the last BFG press was
// fired at; the ball whose spray comes next tic, and the one last logged
// (compared, never followed).
static boolean bfg_pending;
static double bfg_angle;
static k_vec2_t bfg_ball;
static double bfg_z;
static boolean bfg_burst;
static const mobj_t *bfg_mo;
static boolean bfg_spraying;
static const mobj_t *bfg_logged;

// Invulnerable for INVULN_BOLD more, this tic; when a hard fight was last
// on (HARD_LINGER).
static boolean bold;
static int hard_seen;

// The spot the brain goes to land on, to telefrag what stands there
// (K_Combat_Telefrag), and its area.
static boolean telefrag_on;
static k_vec2_t telefrag_at;
static int telefrag_area = -1;

// Barrels in the level, live ones and those about to blow up. Barrels that
// would set each other off share a chain; a chain with a barrel within
// BLAST_SAFE of the player is near: never shot, never grazed.
typedef struct
{
    const mobj_t *mo;
    k_vec2_t at;
    double z;
    boolean live;           // not yet shot to death
    int health;
    int chain;              // the first barrel of its chain
} barrel_t;

typedef enum
{
    BARREL_NONE,
    BARREL_WEAPON,          // its blast is worth more than a shot at the target
    BARREL_TRAP,            // by the route ahead, where monsters could set it off
    BARREL_BLOCKING,        // in the way: the brain asked (K_Combat_ShootBarrel)
    BARREL_PRIMED,          // hit already: the next hit sets it off
} barrelwhy_t;

static barrel_t barrels[MAX_BARRELS];
static int num_barrels;
static boolean chain_near[MAX_BARRELS];     // per chain (its first barrel)

static const mobj_t *barrel_mo;             // the barrel shot at, NULL if none
static int barrel_target = -1;              // its index this tic
static barrelwhy_t barrel_why;
static int barrel_health;                   // its health last tic
static int barrel_misses;                   // shots that took none of it
static char barrel_reason[64];

// The barrel the brain wants gone (BARREL_BLOCKING), if any.
static boolean blocking_wanted;
static k_vec2_t blocking_at;

// Stepping away from a blast about to go off, or from a monster's blows
// (dodging_blow), at DODGE_SPEED.
static boolean dodging;
static boolean dodging_blow;
static boolean dodging_primed;
static const mobj_t *primed_mo;     // the primed barrel stepped away from last
static int primed_tics;             // and for how long in all

// Out of the way of projectiles, out of a fight (DodgeMissiles): the way
// stepped (of MOVE_WAYS), and whether the route goes on meanwhile.
static boolean dodging_missile;
static int missile_way;
static boolean missile_on_way;

// In battle with nothing to shoot for now: the way stepped out of the way
// of projectiles, -1 if none.
static int battle_way = -1;

// Holding short of an area that is not safe (HOLD_AHEAD): which, where the
// route goes in, where the player stands (creeping on to hold_end, along
// the way creeping goes, its point crept toward next), since when, the
// tics with nothing worth fighting, and whether it has crept; the area
// gone into after a hold, until when; the hold given up as its door shut,
// its start, and until when it is taken up again from there.
static boolean holding;
static int hold_area;
static k_vec2_t hold_spot;
static k_vec2_t hold_end;
static k_vec2_t creep_points[CREEP_POINTS];
static int creep_num_points;
static int creep_next;
static int hold_since;
static int hold_quiet;
static boolean hold_crept;
static double hold_nearest;         // the nearest to the spot lately, and the
static int hold_stall;              // tics since (HOLD_STALL)
static int hold_skip_area = -1;
static int hold_skip_until;
static int hold_shut_area = -1;
static int hold_shut_since;
static int hold_shut_until;

// Ducking (DUCK_REPLAN): the way there (the spot last) and the point gone
// for, out of whose sight (a monster or a missile; compared, never
// followed), what, since when, the tics since its blow was last coming,
// and the blow the player found no cover from (logged once). Before it:
// out of whose sight a spot was looked for, when, whether found, and how
// long getting there takes. And the cell search's (DUCK_GRAPH_REACH): per
// cell, how far, where it is come into, and from which cell.
static boolean ducking;
static k_vec2_t duck_points[DUCK_POINTS];
static int duck_num_points;
static int duck_next;
static const mobj_t *duck_from;
static char duck_what[32];
static int duck_since;
static int duck_calm;
static const mobj_t *duck_none_from;
static const mobj_t *plan_from;
static int plan_tic;
static boolean plan_found;
static int plan_tics;
static double *cover_distance;
static k_vec2_t *cover_entry;
static int *cover_from;
static int *cover_hops;
static int *cover_open;
static int cover_capacity;

// What exposes the player (EXPOSE_WEIGHT): this tic's costliest awake
// monsters in sight, but the target.
static int exposers[MAX_EXPOSERS];
static int num_exposers;

// When a monster or its shot last hurt the player.
static int hurt_tic;

// Whether monsters could come at the player (WayClear), judged at tic.
typedef struct
{
    const mobj_t *mo;
    int tic;
    boolean reaches;
} reachmemo_t;

static reachmemo_t reach_memo[MAX_REACH_MEMO];

// Comfy: those that cannot come at the player are fought as any; and the
// shots on the way at the target that cannot.
static boolean comfy;
static int out_shots;

static double ToMap(fixed_t value)
{
    return value / 65536.0;
}

static double Distance(k_vec2_t a, k_vec2_t b)
{
    return sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
}

static double Wrap(double angle)
{
    return atan2(sin(angle), cos(angle));
}

static const kind_t *Kind(int type)
{
    int i;

    for (i = 0; i < arrlen(kinds); ++i)
    {
        if (kinds[i].type == type)
        {
            return &kinds[i];
        }
    }

    return &other_kind;
}

// Projectiles and other things that hurt the player.
static const char *ThingName(int type)
{
    switch (type)
    {
        case MT_TROOPSHOT:      return "imp fireball";
        case MT_HEADSHOT:       return "cacodemon ball";
        case MT_BRUISERSHOT:    return "baron ball";
        case MT_ARACHPLAZ:      return "arachnotron plasma";
        case MT_FATSHOT:        return "mancubus fireball";
        case MT_TRACER:         return "revenant missile";
        case MT_ROCKET:         return "rocket";
        case MT_PLASMA:         return "plasma";
        case MT_BFG:            return "BFG ball";
        case MT_SPAWNSHOT:      return "spawn cube";
        case MT_BARREL:         return "barrel";
        case MT_BOSSBRAIN:      return "boss brain";
        default:                return Kind(type)->name;
    }
}

static const weapon_t *WeaponOf(int weapon)
{
    int i;

    for (i = 0; i < arrlen(weapons); ++i)
    {
        if (weapons[i].weapon == weapon)
        {
            return &weapons[i];
        }
    }

    return NULL;
}

static boolean IsMonster(const mobj_t *mo)
{
    return (mo->flags & MF_COUNTKILL) || mo->type == MT_SKULL;
}

static void MarkAttackStates(void)
{
    int t, k, s, steps;

    if (attack_states_marked)
    {
        return;
    }

    for (t = 0; t < NUMMOBJTYPES; ++t)
    {
        const mobjinfo_t *info = &mobjinfo[t];
        int starts[2] = { info->missilestate, info->meleestate };

        for (k = 0; k < 2; ++k)
        {
            for (s = starts[k], steps = 0;
                 s > S_NULL && s < NUMSTATES && s != info->seestate && s != info->spawnstate
                 && !attack_states[s] && steps < 32; ++steps)
            {
                attack_states[s] = 1;
                s = states[s].nextstate;
            }
        }
    }

    attack_states_marked = true;
}

// The height a player's shots leave from (P_LineAttack, P_AimLineAttack):
// 8 above its middle.
static double GunHeight(const mobj_t *player_mo)
{
    return ToMap(player_mo->z) + ToMap(player_mo->height) * 0.5 + 8.0;
}

static void AddMonster(const mobj_t *mo, const mobj_t *player_mo, k_vec2_t me)
{
    const kind_t *kind = Kind(mo->type);
    k_monster_t *monster;

    if (num_monsters == monster_capacity)
    {
        monster_capacity = SDL_max(64, monster_capacity * 2);
        monsters = realloc(monsters, monster_capacity * sizeof(*monsters));
        monster_mobjs = realloc(monster_mobjs, monster_capacity * sizeof(*monster_mobjs));
    }

    monster = &monsters[num_monsters];
    monster_mobjs[num_monsters++] = mo;
    monster->type = mo->type;
    monster->name = kind->name;
    monster->at.x = ToMap(mo->x);
    monster->at.y = ToMap(mo->y);
    monster->angle = mo->angle * (2.0 * K_PI / 4294967296.0);
    monster->radius = ToMap(mo->radius);
    monster->health = mo->health;
    monster->awake = mo->target != NULL;
    monster->infighting = mo->target != NULL && mo->target->player == NULL
                       && mo->target->health > 0 && (mo->target->flags & MF_SHOOTABLE);
    monster->attacking = mo->state != NULL && attack_states[mo->state - states];
    monster->hitscan = kind->hitscan;
    monster->z = ToMap(mo->z);
    monster->height = ToMap(mo->height);
    monster->distance = Distance(me, monster->at);
    monster->visible = monster->distance <= SIGHT_REACH && K_Sight_Things(player_mo, mo);
    monster->shootable = monster->visible
                      && K_Sight_Aim(me.x, me.y, GunHeight(player_mo), monster->at.x,
                                     monster->at.y, monster->z, monster->z + monster->height);

    ++status.alive;
    status.visible += monster->visible;
    status.awake += monster->visible && monster->awake;
}

static void AddProjectile(const mobj_t *mo, const mobj_t *player_mo, k_vec2_t me)
{
    k_projectile_t *projectile;
    double rx, ry, vv, t;

    if (num_projectiles == projectile_capacity)
    {
        projectile_capacity = SDL_max(64, projectile_capacity * 2);
        projectiles = realloc(projectiles, projectile_capacity * sizeof(*projectiles));
        projectile_mobjs = realloc(projectile_mobjs,
                                   projectile_capacity * sizeof(*projectile_mobjs));
    }

    projectile_mobjs[num_projectiles] = mo;
    projectile = &projectiles[num_projectiles++];
    projectile->name = ThingName(mo->type);
    projectile->at.x = ToMap(mo->x);
    projectile->at.y = ToMap(mo->y);
    projectile->velocity.x = ToMap(mo->momx);
    projectile->velocity.y = ToMap(mo->momy);
    projectile->radius = ToMap(mo->radius);
    projectile->own = mo->target == player_mo;
    projectile->incoming = false;
    projectile->eta = 0.0;

    // Its closest approach, the player standing still.
    rx = projectile->at.x - me.x;
    ry = projectile->at.y - me.y;
    vv = projectile->velocity.x * projectile->velocity.x
       + projectile->velocity.y * projectile->velocity.y;

    if (projectile->own || vv < 1e-6)
    {
        return;
    }

    t = -(rx * projectile->velocity.x + ry * projectile->velocity.y) / vv;

    if (t > 0.0 && t < PROJECTILE_HORIZON)
    {
        double cx = rx + projectile->velocity.x * t;
        double cy = ry + projectile->velocity.y * t;

        if (sqrt(cx * cx + cy * cy) < PLAYER_RADIUS + projectile->radius + PROJECTILE_MARGIN)
        {
            projectile->incoming = true;
            projectile->eta = t;
        }
    }
}

// The larger of the x and y distances: how the engine measures a blast.
static double AxisDistance(k_vec2_t a, k_vec2_t b)
{
    return SDL_max(fabs(b.x - a.x), fabs(b.y - a.y));
}

// A barrel standing, or shot and about to blow up (before A_Explode). Its
// chain is worked out later (UpdateChains).
static void AddBarrel(const mobj_t *mo, boolean *changed)
{
    boolean live = mo->health > 0 && (mo->flags & MF_SHOOTABLE);
    barrel_t *barrel;

    if (num_barrels == MAX_BARRELS
     || (!live && (mo->state == NULL || mo->state - states < S_BEXP
                   || mo->state - states >= S_BEXP4)))
    {
        return;
    }

    barrel = &barrels[num_barrels++];
    *changed |= barrel->mo != mo || barrel->live != live;
    barrel->mo = mo;
    barrel->at.x = ToMap(mo->x);
    barrel->at.y = ToMap(mo->y);
    barrel->z = ToMap(mo->z);
    barrel->live = live;
    barrel->health = mo->health;
}

// Hit already, and still standing: the next hit sets it off.
static boolean Primed(const barrel_t *barrel)
{
    return barrel->live && barrel->health < BARREL_HEALTH;
}

static int ChainOf(int i)
{
    while (barrels[i].chain != i)
    {
        i = barrels[i].chain = barrels[barrels[i].chain].chain;
    }

    return i;
}

// Which live barrels would set each other off, when the list changed
// (live barrels stand still); and which chains are near the player: have a
// barrel within BLAST_SAFE whose blast could see it (a dead barrel is a
// quarter of its height, BLAST_HEIGHT), or that the gun could hit (the user,
// E1M7: through a door going up, the gun could hit a barrel 104 off that the
// blast could not see yet, and the bot shot at it; a door is open by the time
// the barrel goes off).
static void UpdateChains(boolean changed, const mobj_t *player_mo)
{
    k_vec2_t me = { ToMap(player_mo->x), ToMap(player_mo->y) };
    double eye = ToMap(player_mo->z) + ToMap(player_mo->height) * 0.75;
    double gun = GunHeight(player_mo);
    int i, j;

    if (changed)
    {
        for (i = 0; i < num_barrels; ++i)
        {
            barrels[i].chain = i;
        }

        for (i = 0; i < num_barrels; ++i)
        {
            for (j = i + 1; j < num_barrels; ++j)
            {
                if (barrels[i].live && barrels[j].live
                 && AxisDistance(barrels[i].at, barrels[j].at) < BARREL_CHAIN
                 && ChainOf(i) != ChainOf(j) && K_Sight_Things(barrels[j].mo, barrels[i].mo))
                {
                    barrels[ChainOf(j)].chain = ChainOf(i);
                }
            }
        }

        for (i = 0; i < num_barrels; ++i)
        {
            ChainOf(i);
        }
    }

    for (i = 0; i < num_barrels; ++i)
    {
        chain_near[i] = false;
    }

    for (i = 0; i < num_barrels; ++i)
    {
        if (barrels[i].live && !chain_near[barrels[i].chain]
         && AxisDistance(me, barrels[i].at) < BLAST_SAFE
         && (K_Sight_Check(me.x, me.y, eye, barrels[i].at.x, barrels[i].at.y, barrels[i].z,
                           barrels[i].z + BLAST_HEIGHT)
             || K_Sight_Aim(me.x, me.y, gun, barrels[i].at.x, barrels[i].at.y, barrels[i].z,
                            barrels[i].z + BARREL_HEIGHT)))
        {
            chain_near[barrels[i].chain] = true;
        }
    }
}

// Whether a live barrel is near enough that a melee blow might hit it.
static boolean BarrelBeside(const player_t *player)
{
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    int i;

    for (i = 0; i < num_barrels; ++i)
    {
        if (barrels[i].live && AxisDistance(me, barrels[i].at) < BLAST_SAFE)
        {
            return true;
        }
    }

    return false;
}

static boolean WasAlive(const mobj_t *mo)
{
    int i;

    for (i = 0; i < num_was_alive; ++i)
    {
        if (was_alive[i] == mo)
        {
            return true;
        }
    }

    return false;
}

// A BFG ball of the player's: its spray to come, in flight or burst (the
// spray runs on entering S_BFGLAND3), and the tic before it.
static void NoteBfg(const mobj_t *mo)
{
    boolean flying = (mo->flags & MF_MISSILE) != 0;

    if (!flying && mo->state != &states[S_BFGLAND] && mo->state != &states[S_BFGLAND2])
    {
        return;
    }

    bfg_pending = true;
    bfg_angle = mo->angle * (2.0 * K_PI / 4294967296.0);
    bfg_ball.x = ToMap(mo->x);
    bfg_ball.y = ToMap(mo->y);
    bfg_z = ToMap(mo->z);
    bfg_burst = !flying;

    if (mo->state == &states[S_BFGLAND2] && mo->tics <= 1 && mo != bfg_logged)
    {
        bfg_spraying = true;
        bfg_logged = mo;
    }
}

// The monsters and projectiles, and the monsters that died since last tic.
static void Collect(const player_t *player)
{
    const mobj_t *player_mo = player->mo;
    k_vec2_t me = { ToMap(player_mo->x), ToMap(player_mo->y) };
    const thinker_t *thinker;
    int was_barrels = num_barrels;
    boolean changed = false;
    int i;

    num_monsters = 0;
    num_projectiles = 0;
    num_barrels = 0;
    status.alive = status.visible = status.awake = 0;
    bfg_pending = false;
    bfg_spraying = false;
    K_Threat_Begin(player_mo);

    for (thinker = thinkercap.next; thinker != &thinkercap; thinker = thinker->next)
    {
        const mobj_t *mo = (const mobj_t *) thinker;

        if (thinker->function.acp1 != (actionf_p1) P_MobjThinker)
        {
            continue;
        }

        if (mo->type == MT_BFG && mo->target == player_mo)
        {
            NoteBfg(mo);
        }

        if (mo->flags & MF_MISSILE)
        {
            AddProjectile(mo, player_mo, me);
            K_Threat_AddMissile(mo);
        }
        else if (mo->type == MT_BARREL)
        {
            AddBarrel(mo, &changed);
        }
        else if (IsMonster(mo))
        {
            if (mo->health > 0 && !(mo->flags & MF_CORPSE))
            {
                AddMonster(mo, player_mo, me);
                K_Threat_AddMonster(mo, monsters[num_monsters - 1].visible);
            }
            else if (WasAlive(mo))
            {
                K_Log(K_LOG_COMBAT, K_LOG_INFO, "the %s is dead (%d kills of %d)",
                      Kind(mo->type)->name, player->killcount, totalkills);
            }
        }
    }

    if (was_alive_capacity < num_monsters)
    {
        was_alive_capacity = monster_capacity;
        was_alive = realloc(was_alive, was_alive_capacity * sizeof(*was_alive));
    }

    for (i = 0; i < num_monsters; ++i)
    {
        was_alive[i] = monster_mobjs[i];
    }

    num_was_alive = num_monsters;

    // Winding up: the ball will leave facing what it was fired at.
    if (!bfg_pending && bfg_mo != NULL && shot_mo == bfg_mo && gametic <= shot_tic
     && player->readyweapon == wp_bfg)
    {
        for (i = 0; i < num_monsters && monster_mobjs[i] != bfg_mo; ++i)
        {
        }

        if (i < num_monsters)
        {
            bfg_pending = true;
            bfg_burst = false;
            bfg_angle = atan2(monsters[i].at.y - me.y, monsters[i].at.x - me.x);
            bfg_ball = monsters[i].at;
            bfg_z = monsters[i].z;
        }
    }

    UpdateChains(changed || num_barrels != was_barrels, player_mo);
    K_Threat_End();
    status.threats = K_Threat_Count();
}

// "the imp", "its own blast"; what hurt the player.
static void AttackerName(const player_t *player, char *buf, int buf_len)
{
    if (player->attacker == NULL)
    {
        M_StringCopy(buf, "a floor that hurts, or a crusher", buf_len);
    }
    else if (player->attacker == player->mo)
    {
        // A barrel's blast counts as whoever set it off.
        M_StringCopy(buf, "a blast it set off (its rocket, or a barrel)", buf_len);
    }
    else
    {
        M_snprintf(buf, buf_len, "the %s", ThingName(player->attacker->type));
    }
}

// Hits from monsters and blasts; not from floors that hurt, which come
// every 32 tics and are counted by the route (k_path.c). A projectile's
// (k_threat.h) with how long it was seen coming, and what the player was
// doing about it, and counted.
static void LogHits(const player_t *player)
{
    const k_navstatus_t *nav = K_Nav_GetStatus();
    char name[64];
    const char *by;
    int damage, seen, foreseen;

    if (last_health >= 0 && player->health < last_health && player->attacker != NULL)
    {
        damage = last_health - player->health;
        by = player->attacker != player->mo ? K_Threat_HitBy(&seen, &foreseen) : NULL;

        if (by != NULL)
        {
            ++status.missile_hits;
            status.missile_damage += damage;

            if (seen >= DODGEABLE_TICS)
            {
                ++status.dodgeable_hits;
                status.dodgeable_damage += damage;
                status.foreseen_hits += foreseen > 0;
            }

            K_Log(K_LOG_COMBAT, K_LOG_INFO, "hit for %d by the %s, seen %.1f s before%s, %s - "
                  "%d health left", damage, by, (double) seen / TICRATE,
                  seen < DODGEABLE_TICS ? ""
                  : foreseen > 0 ? " (in time to dodge; foreseen, every way cost more)"
                  : foreseen == 0 ? " (in time to dodge; not foreseen)"
                  : " (in time to dodge; no way judged)",
                  ducking ? "ducking" : battle ? "fighting" : dodging_missile ? "dodging"
                  : nav->action == K_ACTION_RIDE ? "riding a lift"
                  : !K_Nav_CanPause() ? "working a door, lift or switch, or jumping"
                  : "not dodging", player->health);
        }
        else
        {
            AttackerName(player, name, sizeof(name));
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "hit for %d by %s - %d health left", damage, name,
                  player->health);
        }

        if (player->attacker != player->mo)
        {
            hurt_tic = leveltime;
        }
    }

    last_health = player->health;
}

static boolean CanFire(const player_t *player, const weapon_t *weapon)
{
    ammotype_t ammo = weaponinfo[weapon->weapon].ammo;

    return player->weaponowned[weapon->weapon]
        && (ammo == am_noammo || player->ammo[ammo] >= weapon->shot);
}

// Whether its number key brings it up: not the fist with the chainsaw in
// hand's reach, nor the shotgun once the player has the super shotgun.
static boolean Selectable(const player_t *player, const weapon_t *weapon)
{
    return !(weapon->weapon == wp_fist && player->weaponowned[wp_chainsaw])
        && !(weapon->weapon == wp_shotgun && player->weaponowned[wp_supershotgun]);
}

// Whether a lost soul is within SOUL_CLEAR of the player: no rockets.
static boolean SoulNear(void)
{
    int i;

    for (i = 0; i < num_monsters; ++i)
    {
        if (monsters[i].type == MT_SKULL && monsters[i].distance < SOUL_CLEAR)
        {
            return true;
        }
    }

    return false;
}

// What share of its flight a rocket fired along the unit way (ux, uy) gains
// on the player: 1 standing or running off, less the faster it runs along.
// In a fight, as it runs now; on the way, as the route runs on after the
// shot, at RUN_SPEED toward the next waypoint (E1M9: fired at a standstill,
// the player then ran on at 16 a tic behind the rocket into its burst).
#define RUN_SPEED 16.0

static double RocketGain(const player_t *player, double ux, double uy)
{
    const k_navstatus_t *nav = K_Nav_GetStatus();
    const k_path_t *path = K_Nav_GetPath();
    double run = ToMap(player->mo->momx) * ux + ToMap(player->mo->momy) * uy;

    if (!battle && !holding && !dodging && !ducking && path != NULL
     && nav->state == K_NAV_DRIVING && nav->target >= 0 && nav->target < path->num_points)
    {
        double dx = path->points[nav->target].x - ToMap(player->mo->x);
        double dy = path->points[nav->target].y - ToMap(player->mo->y);
        double length = sqrt(dx * dx + dy * dy);

        if (length > 1e-6)
        {
            run = SDL_max(run, RUN_SPEED * (dx * ux + dy * uy) / length);
        }
    }

    return (ROCKET_SPEED - SDL_max(0.0, SDL_min(run, ROCKET_SPEED - 1.0))) / ROCKET_SPEED;
}

// RocketGain for rating the rocket launcher: in a fight as if the player
// stood - it moves as the fight wants, and with rockets wanted does not run
// at the target (ChooseMove); they are fired only as it runs (RocketClear).
// E3M9: the launcher up for the cyberdemon 391 off, the pistol 1.5 s later
// at 369 - strafing on a slant toward it, the rockets rated nothing - three
// times, the bullets gone on it and on cacodemons, none left for the
// hitscanners in the red key's room.
static double RateGain(const player_t *player, double ux, double uy)
{
    return battle ? 1.0 : RocketGain(player, ux, uy);
}

// The share of a bullet's or a pellet's damage that lands on a thing of the
// radius at the distance: as the spread allows (none when tapped far off),
// less and less beyond the weapon's range.
static double HitShare(const weapon_t *weapon, double distance, double radius)
{
    double hit = 1.0;

    if (!(weapon->taps && distance > TAP_DISTANCE))
    {
        double half = atan2(radius, distance) * 180.0 / K_PI;

        hit = SDL_min(1.0, half / weapon->spread);
    }

    if (weapon->range > 0.0 && distance > weapon->range)
    {
        double share = weapon->range / distance;

        hit *= SDL_max(0.25, share * share);
    }

    return hit;
}

// The damage a shot of the weapon would do, on average, to a thing of the
// radius at the distance (at `at`, if not NULL): 0 if it cannot fire, the
// thing is out of a melee weapon's reach, a barrel is beside the player (a
// blow could set it off), or a rocket could hurt the player too (ROCKET_MIN,
// the player's run toward the thing counted - RocketGain; SOUL_CLEAR).
static double ShotDamage(const player_t *player, const weapon_t *weapon, double distance,
                         double radius, const k_vec2_t *at)
{
    double damage = weapon->damage, hit = 1.0, gain = 1.0;

    if (!CanFire(player, weapon))
    {
        return 0.0;
    }

    if (weapon->reach > 0.0)
    {
        if (distance - radius > weapon->reach || BarrelBeside(player))
        {
            return 0.0;
        }

        if (weapon->weapon == wp_fist && player->powers[pw_strength])
        {
            damage *= 10.0;
        }
    }
    else if (weapon->flies)
    {
        if (at != NULL && distance > 1e-6)
        {
            gain = RateGain(player, (at->x - ToMap(player->mo->x)) / distance,
                            (at->y - ToMap(player->mo->y)) / distance);
        }

        if (weapon->weapon == wp_missile && player->powers[pw_invulnerability] < INVULN_LEFT
         && (distance - radius - ROCKET_RADIUS < ROCKET_MIN / gain || SoulNear()))
        {
            return 0.0;
        }

        hit = 1.0 / (1.0 + distance / 512.0);
    }
    else
    {
        hit = HitShare(weapon, distance, radius);
    }

    return damage * hit;
}

// The damage a tic the weapon would do to the monster, no more than kills
// it counting (and a half: not all hits land as hoped).
static double Rating(const player_t *player, const weapon_t *weapon, const k_monster_t *monster)
{
    return SDL_min(ShotDamage(player, weapon, monster->distance, monster->radius, &monster->at),
                   monster->health * 1.5) / weapon->tics;
}

static boolean Ignored(const mobj_t *mo)
{
    int i;

    for (i = 0; i < MAX_IGNORED; ++i)
    {
        if (ignored[i].mo == mo && leveltime < ignored[i].until)
        {
            return true;
        }
    }

    return false;
}

static void Ignore(const mobj_t *mo, int tics, boolean unreachable)
{
    int i, slot = 0;

    for (i = 0; i < MAX_IGNORED; ++i)
    {
        if (ignored[i].until < ignored[slot].until)
        {
            slot = i;
        }
    }

    ignored[slot].mo = mo;
    ignored[slot].until = leveltime + tics;
    ignored[slot].unreachable = unreachable;
}

static void DropTarget(void)
{
    status.target = -1;
    target_mo = NULL;
    wanted = -1;
    can_hurt = false;
    status.weapon = NULL;
    sighted = 0;
    misses = 0;
}

// The target in this tic's list, found by its mobj; -1 if it is gone.
static int IndexOf(const mobj_t *mo)
{
    int i;

    for (i = 0; mo != NULL && i < num_monsters; ++i)
    {
        if (monster_mobjs[i] == mo)
        {
            return i;
        }
    }

    return -1;
}

// Where the BFG ball pending bursts, to be kept in sight (BFG_HOLD): the
// monster it was fired at, while it flies and that one lives; else the ball,
// the height of a monster round it.
static void BfgAnchor(k_vec2_t *at, double *bottom, double *top)
{
    int i = bfg_burst ? -1 : IndexOf(bfg_mo);

    if (i >= 0)
    {
        *at = monsters[i].at;
        *bottom = monsters[i].z;
        *top = monsters[i].z + monsters[i].height;
    }
    else
    {
        *at = bfg_ball;
        *bottom = bfg_z - 24.0;
        *top = bfg_z + 32.0;
    }
}

// Whether the spray of the BFG ball pending, from a player at `from` with
// its gun at `gun`, would find what is round its burst: in sight, within
// BFG_CONE of the ball's way and BFG_REACH. True when none is pending.
static boolean BfgSees(k_vec2_t from, double gun)
{
    double bottom, top;
    k_vec2_t at;

    if (!bfg_pending)
    {
        return true;
    }

    BfgAnchor(&at, &bottom, &top);

    return Distance(from, at) <= BFG_REACH
        && (Distance(from, at) < BFG_NEAR
            || fabs(Wrap(atan2(at.y - from.y, at.x - from.x) - bfg_angle)) <= BFG_CONE)
        && K_Sight_Aim(from.x, from.y, gun, at.x, at.y, bottom, top);
}

// The tic before a spray: what it will find, logged.
static void LogSpray(const player_t *player)
{
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    double bottom, top;
    k_vec2_t at;
    int count = 0, i;

    BfgAnchor(&at, &bottom, &top);

    for (i = 0; i < num_monsters; ++i)
    {
        count += monsters[i].shootable && monsters[i].distance <= 1024.0
              && fabs(Wrap(atan2(monsters[i].at.y - me.y, monsters[i].at.x - me.x) - bfg_angle))
                 <= K_PI / 4.0;
    }

    K_Log(K_LOG_COMBAT, K_LOG_INFO, "BFG spray next tic - the burst %.0f units off, %s; %d "
          "monsters in its cone and sight", Distance(me, at),
          BfgSees(me, GunHeight(player->mo)) ? "in sight" : "out of its sight or cone", count);
}

// A tough monster with no bullets across a large room (LEAVE_REACH).
static boolean FarAndDodgeable(const k_monster_t *monster, const mobj_t *mo)
{
    return !monster->hitscan && monster->type != MT_VILE && monster->type != MT_PAIN
        && monster->health > LEAVE_HEALTH
        && monster->distance > (mo == target_mo ? LEAVE_KEEP : LEAVE_REACH);
}

// Whether the target closes in on the player: over the last CLOSING_TICS it
// came CLOSING_MOST nearer or more of itself, the player's own moves aside.
// One not watched that long yet (same: the target last tic too) counts as
// closing in.
static void Track(k_vec2_t me, const k_monster_t *monster, boolean same)
{
    if (!same)
    {
        tracked = 0;
    }

    if (tracked >= CLOSING_TICS)
    {
        k_vec2_t then = target_track[tracked % CLOSING_TICS];

        // Its own move, toward where the player is now.
        closing = ((monster->at.x - then.x) * (me.x - monster->at.x)
                 + (monster->at.y - then.y) * (me.y - monster->at.y))
                / SDL_max(monster->distance, 1.0) >= CLOSING_MOST;
    }
    else
    {
        closing = true;
    }

    target_track[tracked++ % CLOSING_TICS] = monster->at;
}

// Whether the monster could come at the player along the straight line
// between them: no wall, no line that blocks monsters, room for its height
// at every line, and for one that walks, no step up or drop over MAX_CLIMB
// (monsters do not step off ledges, P_TryMove). A door it can open counts
// as open (P_UseSpecialLine: monsters work manual doors, not secret ones);
// one that flies heeds no floors. In pieces of REACH_PIECE, for the lines
// near each.
static boolean WayClear(const k_monster_t *monster, const mobj_t *mo, k_vec2_t me)
{
    const k_map_t *map = K_Map_Get();
    boolean flies = (mo->flags & MF_FLOAT) != 0;
    k_vec2_t a = monster->at;
    double dx = me.x - a.x, dy = me.y - a.y;
    int pieces = (int) (sqrt(dx * dx + dy * dy) / REACH_PIECE) + 1;
    int found[128];
    int p, k, count;

    for (p = 0; p < pieces; ++p)
    {
        k_vec2_t p0 = { a.x + dx * p / pieces, a.y + dy * p / pieces };
        k_vec2_t p1 = { a.x + dx * (p + 1) / pieces, a.y + dy * (p + 1) / pieces };
        k_vec2_t min = { SDL_min(p0.x, p1.x), SDL_min(p0.y, p1.y) };
        k_vec2_t max = { SDL_max(p0.x, p1.x), SDL_max(p0.y, p1.y) };

        count = K_Graph_LinesNear(min, max, found, arrlen(found));

        for (k = 0; k < count; ++k)
        {
            const k_line_t *line = &map->lines[found[k]];
            double ex = line->v2.x - line->v1.x, ey = line->v2.y - line->v1.y;
            double s0 = ex * (p0.y - line->v1.y) - ey * (p0.x - line->v1.x);
            double s1 = ex * (p1.y - line->v1.y) - ey * (p1.x - line->v1.x);
            double t0 = (p1.x - p0.x) * (line->v1.y - p0.y) - (p1.y - p0.y) * (line->v1.x - p0.x);
            double t1 = (p1.x - p0.x) * (line->v2.y - p0.y) - (p1.y - p0.y) * (line->v2.x - p0.x);
            double from_floor, from_ceiling, to_floor, to_ceiling;
            boolean front;

            // Not crossed by this piece.
            if (s0 * s1 >= 0.0 || t0 * t1 >= 0.0)
            {
                continue;
            }

            if (line->back_sector < 0 || (line->flags & (ML_BLOCKING | ML_BLOCKMONSTERS)))
            {
                return false;
            }

            if (line->special == 1 && !(line->flags & ML_SECRET))
            {
                continue;
            }

            // The front side is on the right of v1 to v2.
            front = s0 < 0.0;
            K_Map_SectorHeights(front ? line->front_sector : line->back_sector, &from_floor,
                                &from_ceiling);
            K_Map_SectorHeights(front ? line->back_sector : line->front_sector, &to_floor,
                                &to_ceiling);

            if (SDL_min(from_ceiling, to_ceiling) - SDL_max(from_floor, to_floor) < monster->height
             || (!flies && fabs(to_floor - from_floor) > MAX_CLIMB))
            {
                return false;
            }
        }
    }

    return true;
}

// Whether the monster (by its place in this tic's list) could come at the
// player: within CLOSE_REACH, or the way clear, judged every REACH_TICS.
static boolean Reaches(int index)
{
    const k_monster_t *monster = &monsters[index];
    const mobj_t *mo = monster_mobjs[index];
    const mobj_t *player_mo = players[consoleplayer].mo;
    k_vec2_t me = { ToMap(player_mo->x), ToMap(player_mo->y) };
    int k, slot = 0;

    if (monster->distance < CLOSE_REACH)
    {
        return true;
    }

    for (k = 0; k < MAX_REACH_MEMO; ++k)
    {
        if (reach_memo[k].mo == mo)
        {
            if (reach_memo[k].tic <= leveltime && leveltime - reach_memo[k].tic < REACH_TICS)
            {
                return reach_memo[k].reaches;
            }

            slot = k;
            break;
        }

        if (reach_memo[k].tic < reach_memo[slot].tic)
        {
            slot = k;
        }
    }

    reach_memo[slot].mo = mo;
    reach_memo[slot].tic = leveltime;
    reach_memo[slot].reaches = WayClear(monster, mo, me);
    return reach_memo[slot].reaches;
}

// Monsters that only bite, or charge: no threat from behind bars.
static boolean BitesOnly(int type)
{
    return type == MT_SERGEANT || type == MT_SHADOWS || type == MT_SKULL;
}

// Whether those that cannot come at the player are fought as any: comfy,
// health OK and the weapon in hand's ammo plentiful (COMFY_AMMO, until
// under UNCOMFY_AMMO).
static void UpdateComfy(const player_t *player)
{
    ammotype_t ammo = weaponinfo[player->readyweapon].ammo;
    double share = ammo == am_noammo || player->maxammo[ammo] <= 0
                 ? 0.0 : (double) player->ammo[ammo] / player->maxammo[ammo];

    if (K_Condition_Get()->health != K_NEED_OK || share < UNCOMFY_AMMO)
    {
        comfy = false;
    }
    else if (share >= COMFY_AMMO)
    {
        comfy = true;
    }
}

// Whether the target must be fought as any: a gunman, or one that can come
// at the player, or comfy.
static boolean FoughtAsAny(int index)
{
    return monsters[index].hitscan || comfy || Reaches(index);
}

// Whether a monster (by its place in this tic's list) is left alone as a
// target: given up on for now (Ignore), unless within CLOSE_REACH - in
// reach of its blows, or in the way - or given up on as one that could not
// come at the player, that now can (E1M6: an imp left so up the stairs came
// down and clawed the bot from 83 health to 12, unshot, for the 10 s).
static boolean IgnoredTarget(int index)
{
    int i;

    if (monsters[index].distance < CLOSE_REACH)
    {
        return false;
    }

    for (i = 0; i < MAX_IGNORED; ++i)
    {
        if (ignored[i].mo == monster_mobjs[index] && leveltime < ignored[i].until)
        {
            return !ignored[i].unreachable || !FoughtAsAny(index);
        }
    }

    return false;
}

// The health a kind of monster is spawned with: its tier (WEAK_HEALTH,
// TOUGH_HEALTH).
static int SpawnHealth(int type)
{
    return type >= 0 && type < NUMMOBJTYPES ? mobjinfo[type].spawnhealth : 0;
}

// Whether the weapon fires the rockets or cells kept for the tough monsters
// (RESERVE_SHARE).
static boolean Reserved(const weapon_t *weapon)
{
    ammotype_t ammo = weaponinfo[weapon->weapon].ammo;

    return ammo == am_misl || ammo == am_cell;
}

// The damage a round of the gun does to the monster, from as far as a fight
// keeps it off: a projectile's FLIES_HIT of all of it, a bullet's or a
// pellet's as the spread and range allow (HOPELESS_SHARE).
static double RoundDamage(const weapon_t *weapon, const k_monster_t *monster)
{
    double distance = SDL_max(Kind(monster->type)->keep, KEEP_AWAY);

    return weapon->damage * (weapon->flies ? FLIES_HIT
                                           : HitShare(weapon, distance, monster->radius))
         / weapon->shot;
}

// Whether the gun, firing on, would kill the monster within HOPELESS_TICS,
// the ammo aside.
static boolean InTime(const weapon_t *weapon, const k_monster_t *monster)
{
    return weapon->reach <= 0.0
        && monster->health * weapon->tics
           <= HOPELESS_TICS * RoundDamage(weapon, monster) * weapon->shot;
}

// What the guns held that would kill the monster in time could do to it
// with the rounds there are: of each ammo type, by the best of them.
static double InHand(const player_t *player, const k_monster_t *monster)
{
    double best[NUMAMMO] = { 0.0 }, can = 0.0;
    int i, a;

    for (i = 0; i < arrlen(weapons); ++i)
    {
        const weapon_t *weapon = &weapons[i];
        ammotype_t ammo = weaponinfo[weapon->weapon].ammo;

        if (ammo != am_noammo && player->weaponowned[weapon->weapon]
         && Selectable(player, weapon) && InTime(weapon, monster))
        {
            best[ammo] = SDL_max(best[ammo], RoundDamage(weapon, monster));
        }
    }

    for (a = 0; a < NUMAMMO; ++a)
    {
        can += player->ammo[a] * best[a];
    }

    return can;
}

// Whether the monster (by its place in this tic's list) is hopeless: not
// weak, and what is in hand would not take HOPELESS_SHARE of its health.
static boolean Hopeless(const player_t *player, int index)
{
    const k_monster_t *monster = &monsters[index];
    double share = battle && monster_mobjs[index] == target_mo ? HOPELESS_KEEP : HOPELESS_SHARE;

    return SpawnHealth(monster->type) > WEAK_HEALTH && Kind(monster->type)->cost > 0.0
        && player->powers[pw_invulnerability] < INVULN_LEFT
        && InHand(player, monster) < monster->health * share;
}

// Whether an item that would beat a hopeless monster met lies in reach, worth
// the detour (k_items.h: beats).
static boolean Fetchable(void)
{
    int i;

    for (i = 0; i < K_Items_Count(); ++i)
    {
        const k_item_t *item = K_Items_Get(i);

        if (item->present && item->beats && item->reachable && item->reach > 0.0)
        {
            return true;
        }
    }

    return false;
}

// Whether the monster (by its place in this tic's list) is left alone as
// hopeless: no target, kept off - but for the kind the level needs dead
// while nothing that would beat it is left to fetch (fight_needed).
static boolean LeftAlone(int index)
{
    return hopeless[index] && !(fight_needed && monsters[index].type == needed);
}

// Whether the monster (by its place in this tic's list) stands where the
// brain goes to land, to telefrag it (TELEFRAG_SLACK).
static boolean Spared(int index)
{
    double block = monsters[index].radius + PLAYER_RADIUS + TELEFRAG_SLACK;

    return telefrag_on && fabs(monsters[index].at.x - telefrag_at.x) < block
        && fabs(monsters[index].at.y - telefrag_at.y) < block;
}

// Each tic: the monsters fighting another, logged as each is left alone, in
// sight within TARGET_REACH (MAX_INFIGHT).
static void UpdateInfighting(void)
{
    int i, k;

    for (k = 0; k < MAX_INFIGHT; ++k)
    {
        i = IndexOf(infight_logged[k]);

        if (infight_logged[k] != NULL && (i < 0 || !monsters[i].infighting))
        {
            infight_logged[k] = NULL;
        }
    }

    for (i = 0; i < num_monsters; ++i)
    {
        if (!monsters[i].infighting || !monsters[i].visible
         || monsters[i].distance > TARGET_REACH)
        {
            continue;
        }

        for (k = 0; k < MAX_INFIGHT && infight_logged[k] != monster_mobjs[i]; ++k)
        {
        }

        if (k < MAX_INFIGHT)
        {
            continue;
        }

        for (k = 0; k < MAX_INFIGHT && infight_logged[k] != NULL; ++k)
        {
        }

        if (k < MAX_INFIGHT)
        {
            infight_logged[k] = monster_mobjs[i];
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "leaving the %s alone, %.0f units off - it fights "
                  "the %s", monsters[i].name, monsters[i].distance,
                  Kind(monster_mobjs[i]->target->type)->name);
        }
    }
}

// Each tic: whether a hard fight is on (HARD_ONE, HARD_CROWD), and whether the
// player is invulnerable enough to close in (INVULN_BOLD).
static void UpdateHard(const player_t *player)
{
    double crowd = 0.0;
    boolean one = false;
    int i;

    bold = player->powers[pw_invulnerability] >= INVULN_BOLD;

    for (i = 0; i < num_monsters; ++i)
    {
        const k_monster_t *monster = &monsters[i];

        if (monster->awake && monster->visible && !monster->infighting
         && monster->distance <= HARD_REACH)
        {
            crowd += Kind(monster->type)->cost;
            one |= Kind(monster->type)->cost >= HARD_ONE || hopeless[i];
        }
    }

    if (one || crowd >= HARD_CROWD)
    {
        hard_seen = leveltime;
    }
}

// Each tic: which monsters are hopeless; logged as each becomes so, awake in
// sight, and again once it can be fought. Those logged, met awake, are what
// the player is outgunned by while they stay awake (beats).
static void UpdateHopeless(const player_t *player)
{
    int i, k, w;

    if (hopeless_capacity < num_monsters)
    {
        hopeless_capacity = monster_capacity;
        hopeless = realloc(hopeless, hopeless_capacity * sizeof(*hopeless));
    }

    for (i = 0; i < num_monsters; ++i)
    {
        hopeless[i] = Hopeless(player, i);
    }

    // The kind the level needs dead, met as hopeless (logged by last tic, the
    // items judged since), with nothing to fetch that would beat it.
    for (k = 0, i = -1; k < MAX_HOPELESS && i < 0; ++k)
    {
        i = IndexOf(hopeless_logged[k]);
        i = i >= 0 && hopeless[i] && monsters[i].type == needed ? i : -1;
    }

    if (i >= 0 && !Fetchable())
    {
        if (!fight_needed)
        {
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "the %s must die for the level to go on, and nothing "
                  "that would beat it is left to fetch - fought with what the player holds",
                  monsters[i].name);
        }

        fight_needed = true;
    }
    else
    {
        fight_needed = false;
    }

    for (k = 0; k < MAX_HOPELESS; ++k)
    {
        i = IndexOf(hopeless_logged[k]);

        if (hopeless_logged[k] != NULL && (i < 0 || !hopeless[i]))
        {
            if (i >= 0)
            {
                K_Log(K_LOG_COMBAT, K_LOG_INFO, "the %s can be fought now - %.0f of its %d "
                      "health in hand", monsters[i].name, InHand(player, &monsters[i]),
                      monsters[i].health);
            }

            hopeless_logged[k] = NULL;
        }
    }

    for (i = 0; i < num_monsters; ++i)
    {
        if (!hopeless[i] || !monsters[i].awake || !monsters[i].visible)
        {
            continue;
        }

        for (k = 0; k < MAX_HOPELESS && hopeless_logged[k] != monster_mobjs[i]; ++k)
        {
        }

        if (k < MAX_HOPELESS)
        {
            continue;
        }

        for (k = 0; k < MAX_HOPELESS && hopeless_logged[k] != NULL; ++k)
        {
        }

        if (k < MAX_HOPELESS)
        {
            hopeless_logged[k] = monster_mobjs[i];
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "the %s, %.0f units off, is beyond what the player "
                  "holds - %.0f of its %d health in hand; not fought, kept off",
                  monsters[i].name, monsters[i].distance, InHand(player, &monsters[i]),
                  monsters[i].health);
        }
    }

    memset(beats, 0, sizeof(beats));

    for (k = 0; k < MAX_HOPELESS; ++k)
    {
        i = IndexOf(hopeless_logged[k]);

        for (w = 0; i >= 0 && monsters[i].awake && w < arrlen(weapons); ++w)
        {
            beats[weapons[w].weapon] |= InTime(&weapons[w], &monsters[i]);
        }
    }
}

// Each tic: whether rockets and cells are kept for the tough monsters left
// (RESERVE_SHARE); logged as it changes.
static void UpdateReserve(const player_t *player)
{
    double best[NUMAMMO] = { 0.0 }, need = 0.0, can = 0.0, most = 0.0;
    boolean held;
    int i, a;

    for (i = 0; i < num_monsters; ++i)
    {
        if (SpawnHealth(monsters[i].type) >= TOUGH_HEALTH)
        {
            need += monsters[i].health;
        }
    }

    for (i = 0; i < arrlen(weapons); ++i)
    {
        if (Reserved(&weapons[i]) && player->weaponowned[weapons[i].weapon])
        {
            ammotype_t ammo = weaponinfo[weapons[i].weapon].ammo;

            best[ammo] = SDL_max(best[ammo],
                                 weapons[i].damage * FLIES_HIT / SDL_max(weapons[i].shot, 1));
        }
    }

    for (a = 0; a < NUMAMMO; ++a)
    {
        can += player->ammo[a] * best[a];
        most += player->maxammo[a] * best[a];
    }

    held = need > 0.0 && can > 0.0 && can <= SDL_min(need, most * RESERVE_SHARE);

    if (held && !reserve)
    {
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "rockets and cells kept for the tough monsters left - "
              "%.0f of their damage in hand, %.0f of their health", can, need);
    }
    else if (!held && reserve && can > 0.0)
    {
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "rockets and cells for anything again - %.0f of their "
              "damage in hand, %.0f of the tough monsters' health", can, need);
    }

    reserve = held;
}

// The hopeless monster to keep off (AVOID_PAST): awake, in sight - or the
// one kept off, seen within AVOID_LINGER - nearer than its kind's distance;
// the nearest; not the one passed, unless within CLOSE_REACH, nor one about
// to be telefragged (Spared). One that
// cannot come at the player too: from where it stands it shoots (E3M8: the
// spider mastermind, 212 off, shot the bot dead as it picked up a rocket;
// E3M9: a switch worked in the cyberdemon's sight). -1 if none.
static int Avoided(void)
{
    int best = -1, i;

    for (i = 0; i < num_monsters; ++i)
    {
        const k_monster_t *monster = &monsters[i];
        boolean kept = battle && monster_mobjs[i] == avoid_mo;
        double keep = SDL_max(Kind(monster->type)->keep, KEEP_AWAY) + (kept ? AVOID_PAST : 0.0);

        if (LeftAlone(i) && !Spared(i) && monster->awake && monster->distance < keep
         && (monster->visible || (kept && leveltime - avoid_seen < AVOID_LINGER))
         && !(monster_mobjs[i] == passed_mo && leveltime < passed_until
              && monster->distance >= CLOSE_REACH)
         && (best < 0 || monster->distance < monsters[best].distance))
        {
            best = i;
        }
    }

    return best;
}

// Each tic: the hopeless monster kept off (Avoided), and for how long in
// all while fighting; after AVOID_MOST, passed.
static void UpdateAvoid(void)
{
    avoid = Avoided();

    if (avoid < 0 || !battle)
    {
        return;
    }

    if (monster_mobjs[avoid] != avoid_mo)
    {
        avoid_mo = monster_mobjs[avoid];
        avoid_tics = 0;
        avoid_seen = leveltime;
    }

    if (monsters[avoid].visible)
    {
        avoid_seen = leveltime;
    }

    if (++avoid_tics >= AVOID_MOST)
    {
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "passing the %s, %.0f units off - kept off %d s in all; "
              "the route goes on", monsters[avoid].name, monsters[avoid].distance,
              AVOID_MOST / TICRATE);
        passed_mo = avoid_mo;
        passed_until = leveltime + AVOID_SKIP;
        avoid_mo = NULL;
        avoid_tics = 0;
        avoid = Avoided();
    }
}

static void ChooseTarget(const player_t *player)
{
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    double facing = player->mo->angle * (2.0 * K_PI / 4294967296.0);
    double best_score = 0.0;
    boolean same, pain_near = false;
    int best = -1;
    int i;

    for (i = 0; i < num_monsters && !pain_near; ++i)
    {
        pain_near = monsters[i].type == MT_PAIN && monsters[i].shootable
                 && monsters[i].distance <= TARGET_REACH && !IgnoredTarget(i)
                 && !monsters[i].infighting;
    }

    // Not one fighting another, nor one about to be telefragged.
    for (i = 0; i < num_monsters; ++i)
    {
        const k_monster_t *monster = &monsters[i];
        double score, turn;

        if (!monster->shootable || monster->distance > TARGET_REACH
         || (!monster->awake && monster->distance > ASLEEP_REACH)
         || (!bold && FarAndDodgeable(monster, monster_mobjs[i])) || IgnoredTarget(i)
         || LeftAlone(i) || monster->infighting || Spared(i))
        {
            continue;
        }

        // One that cannot come at the player: never asleep, nor one that
        // only bites, and less than one that can.
        if (!FoughtAsAny(i) && (!monster->awake || BitesOnly(monster->type)))
        {
            continue;
        }

        score = Kind(monster->type)->threat * (monster->awake ? 2.0 : 1.0)
              * (monster->attacking ? 1.5 : 1.0) * (1.0 + 384.0 / (monster->distance + 64.0));

        if (!FoughtAsAny(i))
        {
            score *= OUT_SHARE;
        }

        // Turning far round to it takes time.
        turn = Wrap(atan2(monster->at.y - me.y, monster->at.x - me.x) - facing);
        score *= 1.25 + 0.25 * cos(turn);

        // Finish what was started.
        if (monster_mobjs[i] == target_mo)
        {
            score *= TARGET_STICK;
        }

        score *= 1.0 + WOUNDED * (1.0 - (double) monster->health
                                        / SDL_max(monster_mobjs[i]->info->spawnhealth, 1));
        score *= 1.0 + SDL_min(K_Items_DropWorth(monster->type), DROP_MOST) / DROP_WORTH;

        if (monster->distance < CLOSE_REACH)
        {
            score *= CLOSE_FIRST;
        }

        if (monster->type == MT_SKULL && pain_near)
        {
            score *= SOUL_SHARE;
        }

        if (score > best_score)
        {
            best_score = score;
            best = i;
        }
    }

    same = best >= 0 && monster_mobjs[best] == target_mo;

    if (same)
    {
        k_vec2_t moved = { monsters[best].at.x - target_last.x,
                           monsters[best].at.y - target_last.y };

        if (fabs(moved.x) < TARGET_JUMP && fabs(moved.y) < TARGET_JUMP)
        {
            target_velocity.x = (target_velocity.x + moved.x) * 0.5;
            target_velocity.y = (target_velocity.y + moved.y) * 0.5;
        }
        else
        {
            target_velocity.x = target_velocity.y = 0.0;
        }

        if (monsters[best].health < target_health)
        {
            progress_tic = leveltime;
            misses = 0;
        }

        ++sighted;
    }
    else if (best >= 0)
    {
        if (monster_mobjs[best] != lost_mo || leveltime - lost_tic > REGAIN_TICS)
        {
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "target: the %s at (%.0f,%.0f), %.0f units%s%s",
                  monsters[best].name, monsters[best].at.x, monsters[best].at.y,
                  monsters[best].distance, monsters[best].awake ? "" : ", asleep",
                  FoughtAsAny(best) ? "" : ", cannot come at the player");
            progress_tic = leveltime;
            misses = 0;
            out_shots = 0;
        }

        target_velocity.x = target_velocity.y = 0.0;
        sighted = 1;
    }
    else
    {
        sighted = 0;
    }

    if (best >= 0)
    {
        target_health = monsters[best].health;
    }

    if (target_mo != NULL && (best < 0 || monster_mobjs[best] != target_mo))
    {
        lost_mo = target_mo;
        lost_tic = leveltime;
    }

    status.target = best;
    target_mo = best >= 0 ? monster_mobjs[best] : NULL;

    if (best >= 0)
    {
        target_last = monsters[best].at;
        Track(me, &monsters[best], same);
    }
}

static void DropBarrel(void);

// A shot fired at the target (the ammo went down) that did not hurt it;
// after MISS_SHOTS of them the target is left alone for a moment.
static void CountMisses(const player_t *player)
{
    ammotype_t ammo = weaponinfo[player->readyweapon].ammo;
    int now = ammo == am_noammo ? 0 : player->ammo[ammo];
    boolean fired = ammo == last_ammo_type && now < last_ammo;

    last_ammo_type = ammo;
    last_ammo = now;

    // Shots at a barrel are no misses at the target. One that took none of
    // its health is (a hitscan hit lands the tic it is fired); BARREL_MISSES
    // of them: left alone a while.
    if (barrel_mo != NULL)
    {
        int health = 0, i;

        for (i = 0; i < num_barrels; ++i)
        {
            if (barrels[i].mo == barrel_mo && barrels[i].live)
            {
                health = barrel_mo->health;
            }
        }

        if (fired && health > 0 && health >= barrel_health && ++barrel_misses >= BARREL_MISSES)
        {
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "the barrel at (%.0f,%.0f) seems out of reach of "
                  "the shots - %d without a hit; left alone for %d s", status.barrel_at.x,
                  status.barrel_at.y, barrel_misses, IGNORE_TICS / TICRATE);
            Ignore(barrel_mo, IGNORE_TICS, false);
            DropBarrel();
            return;
        }

        barrel_health = health;
        return;
    }

    if (status.target >= 0 && fired)
    {
        ++misses;

        // On the way past one that cannot come at the player: a shot or
        // two, then it is left alone, until it can.
        if (!FoughtAsAny(status.target) && ++out_shots >= OUT_SHOTS)
        {
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "leaving the %s alone for %d s - it cannot come "
                  "at the player; %d shots on the way", monsters[status.target].name,
                  IGNORE_TICS / TICRATE, out_shots);
            Ignore(target_mo, IGNORE_TICS, true);
            DropTarget();
            return;
        }
    }

    if (status.target >= 0 && misses >= MISS_SHOTS)
    {
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "the %s seems out of reach of the shots - %d without "
              "a hit; left alone for %d s", monsters[status.target].name, misses,
              MISS_IGNORE_TICS / TICRATE);
        Ignore(target_mo, MISS_IGNORE_TICS, false);
        DropTarget();
    }
}

// Each tic: when the area was last not clear - a fight on, or an awake
// monster in sight within SNIPE_CLEAR.
static void UpdateClear(void)
{
    int i;

    for (i = 0; i < num_monsters && !battle; ++i)
    {
        if (monsters[i].visible && monsters[i].awake && monsters[i].distance < SNIPE_CLEAR)
        {
            break;
        }
    }

    if (battle || i < num_monsters)
    {
        unclear_tic = leveltime;
    }
}

// Whether a gun that fires neither rockets nor cells can fire - but the
// pistol, without `pistol`.
static boolean LowGun(const player_t *player, boolean pistol)
{
    int i;

    for (i = 0; i < arrlen(weapons); ++i)
    {
        if (weapons[i].reach <= 0.0 && !Reserved(&weapons[i])
         && (pistol || weapons[i].weapon != wp_pistol)
         && Selectable(player, &weapons[i]) && CanFire(player, &weapons[i]))
        {
            return true;
        }
    }

    return false;
}

// Whether the rockets and cells kept for the tough monsters are kept from
// this one (RESERVE_SHARE): not tough, and another gun can fire - the pistol
// too against a weak one.
static boolean KeptFrom(const player_t *player, const k_monster_t *monster)
{
    int spawn = SpawnHealth(monster->type);

    return reserve && spawn < TOUGH_HEALTH && LowGun(player, spawn <= WEAK_HEALTH);
}

// Whether the pistol is right for the target: far off with little health
// left, not closing in, the area clear a while; or nothing better can hurt
// it - rockets and cells kept from it aside.
static boolean PistolRight(const player_t *player, const k_monster_t *monster)
{
    weapontype_t in_hand = player->pendingweapon != wp_nochange ? player->pendingweapon
                                                                : player->readyweapon;
    int i;

    if (monster->distance >= (in_hand == wp_pistol ? SNIPE_KEEP : SNIPE_REACH)
     && monster->health <= SNIPE_HEALTH && !closing
     && leveltime - unclear_tic >= SNIPE_CALM)
    {
        return true;
    }

    for (i = 0; i < arrlen(weapons); ++i)
    {
        const weapon_t *weapon = &weapons[i];

        if (weapon->weapon != wp_pistol
         && !(weapon->weapon == wp_fist && !player->powers[pw_strength])
         && Selectable(player, weapon) && !(Reserved(weapon) && KeptFrom(player, monster))
         && ShotDamage(player, weapon, monster->distance, monster->radius, &monster->at) > 0.0)
        {
            return false;
        }
    }

    return true;
}

// Whether another monster stands within BUNCHED of this one: a rocket's
// blast would get it too.
static boolean Bunched(const k_monster_t *monster)
{
    int i;

    for (i = 0; i < num_monsters; ++i)
    {
        if (&monsters[i] != monster && Kind(monsters[i].type)->cost > 0.0
         && AxisDistance(monsters[i].at, monster->at) < BUNCHED)
        {
            return true;
        }
    }

    return false;
}

// Whether the monster is what the chainsaw is for (SAW_ALONE).
static boolean SawFor(const k_monster_t *monster)
{
    int i;

    if (BitesOnly(monster->type))
    {
        return true;
    }

    if (monster->type != MT_HEAD && monster->type != MT_PAIN)
    {
        return false;
    }

    for (i = 0; i < num_monsters; ++i)
    {
        if (&monsters[i] != monster && monsters[i].awake && monsters[i].distance < SAW_ALONE
         && Kind(monsters[i].type)->cost > 0.0)
        {
            return false;
        }
    }

    return true;
}

// Whether ammo for a weapon held lies in reach, to be fetched (k_items.h).
static boolean AmmoLying(void)
{
    int i;

    for (i = 0; i < K_Items_Count(); ++i)
    {
        const k_item_t *item = K_Items_Get(i);

        if (item->present && item->reach > 0.0 && item->worth > 0.0
         && (item->kind == K_ITEM_AMMO || item->kind == K_ITEM_WEAPON))
        {
            return true;
        }
    }

    return false;
}

// Whether melee is worth having up against the monster: the chainsaw
// against what it is for, the berserk fist against the weakest, in reach -
// well in (MELEE_IN) to be brought up, the chainsaw within MELEE_KEEP to be
// kept in hand - or else only when no other weapon can fire, and, for the
// bare fist, no ammo lies in reach.
static boolean MeleeWorth(const player_t *player, const weapon_t *weapon,
                          const k_monster_t *monster, boolean in_hand)
{
    double edge = monster->distance - monster->radius;
    boolean punches = weapon->weapon == wp_fist && player->powers[pw_strength];
    int i;

    if (weapon->weapon == wp_chainsaw && SawFor(monster)
     && edge <= (in_hand ? MELEE_KEEP : weapon->reach - MELEE_IN))
    {
        return true;
    }

    if (punches && (monster->type == MT_POSSESSED || monster->type == MT_TROOP)
     && edge <= weapon->reach - (in_hand ? 0.0 : MELEE_IN))
    {
        return true;
    }

    for (i = 0; i < arrlen(weapons); ++i)
    {
        if (weapons[i].reach <= 0.0 && Selectable(player, &weapons[i])
         && CanFire(player, &weapons[i]))
        {
            return false;
        }
    }

    return weapon->weapon == wp_chainsaw || punches || !AmmoLying();
}

// Whether a gun but the rocket launcher can fire - but the pistol too,
// without `pistol`.
static boolean OtherGun(const player_t *player, boolean pistol)
{
    int i;

    for (i = 0; i < arrlen(weapons); ++i)
    {
        if (weapons[i].reach <= 0.0 && weapons[i].weapon != wp_missile
         && (pistol || weapons[i].weapon != wp_pistol)
         && Selectable(player, &weapons[i]) && CanFire(player, &weapons[i]))
        {
            return true;
        }
    }

    return false;
}

// Whether rockets are for the monster: a tough one, or one bunched with
// others - not a lost soul.
static boolean RocketsFor(const k_monster_t *monster)
{
    return monster->type != MT_SKULL && (monster->health > ROCKET_HEALTH || Bunched(monster));
}

// Whether the rocket launcher has room to stay of use against the target if
// brought up (ROCKET_ROOM): the target that far beyond where rockets stop
// (ShotDamage), and no other awake monster that can come at the player
// within that of where they stop.
static boolean RocketRoom(const player_t *player, const k_monster_t *target)
{
    double gain = 1.0;
    int i;

    if (target->distance > 1e-6)
    {
        gain = RateGain(player, (target->at.x - ToMap(player->mo->x)) / target->distance,
                        (target->at.y - ToMap(player->mo->y)) / target->distance);
    }

    if (target->distance - target->radius - ROCKET_RADIUS < ROCKET_MIN / gain + ROCKET_ROOM)
    {
        return false;
    }

    for (i = 0; i < num_monsters; ++i)
    {
        const k_monster_t *monster = &monsters[i];

        if (monster != target && monster->awake && Kind(monster->type)->cost > 0.0
         && monster->distance - monster->radius - ROCKET_RADIUS < ROCKET_MIN + ROCKET_ROOM
         && Reaches(i))
        {
            return false;
        }
    }

    return true;
}

// Rating, less for the last resorts: the pistol where it is not right, the
// bare fist always, any melee against a tough monster, rockets at one weak
// monster alone (none to bring them up while another gun can fire); none
// for rockets at a lost soul, nor to bring them up without room while
// another gun can fire (RocketRoom), nor for melee where it is not worth it
// (MeleeWorth), nor for rockets and cells kept for the tough monsters
// (KeptFrom), nor for a gun that would not kill one that is not weak in
// time (InTime: E2M8, the pistol brought up whenever the cyberdemon came too
// near for rockets, and put away as the player backed off - a switch each
// way, the fight going on without either); more for the chainsaw against
// what it is for.
static double Preference(const player_t *player, const weapon_t *weapon,
                         const k_monster_t *monster, boolean pistol_right, boolean in_hand)
{
    double rating = Rating(player, weapon, monster);

    if (rating > 0.0
     && ((Reserved(weapon) && KeptFrom(player, monster))
         || (weapon->reach <= 0.0 && SpawnHealth(monster->type) > WEAK_HEALTH
             && !InTime(weapon, monster))))
    {
        return 0.0;
    }

    if (weapon->weapon == wp_missile && !in_hand && rating > 0.0
     && !RocketRoom(player, monster) && OtherGun(player, true))
    {
        return 0.0;
    }

    if (weapon->reach > 0.0 && !MeleeWorth(player, weapon, monster, in_hand))
    {
        rating = 0.0;
    }
    else if (weapon->weapon == wp_chainsaw && SawFor(monster))
    {
        rating *= SAW_GAIN;
    }
    else if (weapon->weapon == wp_pistol && !pistol_right)
    {
        rating *= PISTOL_SHARE;
    }
    else if (weapon->reach > 0.0
          && ((weapon->weapon == wp_fist && !player->powers[pw_strength])
              || monster->health > MELEE_HEALTH))
    {
        rating *= FIST_SHARE;
    }
    else if (weapon->weapon == wp_missile && monster->type == MT_SKULL)
    {
        rating = 0.0;
    }
    else if (weapon->weapon == wp_missile && monster->health <= ROCKET_HEALTH
          && !Bunched(monster))
    {
        rating *= in_hand || !OtherGun(player, true) ? ROCKET_SHARE : 0.0;
    }

    return rating;
}

static void ChooseWeapon(const player_t *player)
{
    const k_monster_t *monster;
    const weapon_t *best = NULL, *current;
    double best_rating = 0.0, current_rating;
    boolean pistol_right;
    int i;

    wanted = -1;
    can_hurt = false;
    status.weapon = NULL;

    if (status.target < 0)
    {
        return;
    }

    monster = &monsters[status.target];
    pistol_right = PistolRight(player, monster);
    current = WeaponOf(player->pendingweapon != wp_nochange ? player->pendingweapon
                                                           : player->readyweapon);

    // On a tie, a weapon that shoots over one that must be in reach (the
    // bare fist and the pistol, both last resorts, rate alike).
    for (i = 0; i < arrlen(weapons); ++i)
    {
        double rating = Selectable(player, &weapons[i])
                      ? Preference(player, &weapons[i], monster, pistol_right,
                                   &weapons[i] == current)
                      : 0.0;

        if (rating > best_rating
         || (rating > 0.0 && rating >= best_rating && best != NULL && best->reach > 0.0
             && weapons[i].reach <= 0.0))
        {
            best_rating = rating;
            best = &weapons[i];
        }
    }

    current_rating = current != NULL ? Preference(player, current, monster, pistol_right, true)
                                     : 0.0;
    can_hurt = best_rating > 0.0 || current_rating > 0.0
            || SpawnHealth(monster->type) > WEAK_HEALTH;

    // The chainsaw stays up while what it is for stays near, out of reach a
    // moment (MELEE_KEEP).
    if (current != NULL && current->weapon == wp_chainsaw && SawFor(monster)
     && monster->distance - monster->radius <= MELEE_KEEP)
    {
        best = current;
    }

    // And rockets in a fight against what they are for, too near for a
    // moment: the player backs off to fire them (ChooseMove), not put them
    // away - unless it comes within KEEP_AWAY, or they are kept from it.
    if (battle && current != NULL && current->weapon == wp_missile && CanFire(player, current)
     && RocketsFor(monster) && monster->distance >= KEEP_AWAY && !KeptFrom(player, monster))
    {
        best = current;
    }

    if (best != NULL && best != current
     && !(current != NULL && current_rating > 0.0
          && ShotDamage(player, current, monster->distance, monster->radius, &monster->at) >= monster->health)
     && ((current_rating <= 0.0 && leveltime - last_switch >= SWITCH_TICS)
         || (best_rating > current_rating * SWITCH_GAIN
             && leveltime - last_switch
                >= ((current != NULL && current->weapon == wp_pistol && !pistol_right)
                    || best_rating >= current_rating * SWITCH_FAST_GAIN
                    ? SWITCH_TICS : SWITCH_WAIT))))
    {
        current = best;
    }

    if (current != NULL)
    {
        wanted = current->weapon;
        status.weapon = current->name;
    }
}

// The damage a shot of the weapon would do to a barrel: pistol and chaingun
// are tapped at it, so their first bullet goes straight.
static double BarrelShot(const player_t *player, const weapon_t *weapon, double distance)
{
    if (weapon == NULL || weapon->reach > 0.0 || weapon->weapon == wp_bfg
     || !CanFire(player, weapon))
    {
        return 0.0;
    }

    return weapon->taps ? weapon->damage : ShotDamage(player, weapon, distance, BARREL_RADIUS, NULL);
}

// Whether the weapon pops a barrel at the distance in BARREL_SHOTS shots.
static boolean CanPop(const player_t *player, const weapon_t *weapon, double distance)
{
    return BarrelShot(player, weapon, distance) * BARREL_SHOTS >= BARREL_HEALTH;
}

// A weapon the player has that pops the barrel, the cheapest ammo first;
// NULL if none.
static const weapon_t *PopWeapon(const player_t *player, double distance)
{
    static const weapontype_t order[] =
    {
        wp_pistol, wp_chaingun, wp_shotgun, wp_supershotgun, wp_plasma, wp_missile
    };
    int i;

    for (i = 0; i < arrlen(order); ++i)
    {
        const weapon_t *weapon = WeaponOf(order[i]);

        if (Selectable(player, weapon) && CanPop(player, weapon, distance))
        {
            return weapon;
        }
    }

    return NULL;
}

// Whether a monster stands in the line of fire to the barrel, before it:
// the shot (and the autoaim) would find the monster first (MAP02: three
// shots at a barrel behind a shotgun guy all hit the guy).
static boolean MonsterInTheWay(k_vec2_t me, const barrel_t *barrel)
{
    double distance = Distance(me, barrel->at);
    double ux, uy;
    int i;

    if (distance < 1.0)
    {
        return false;
    }

    ux = (barrel->at.x - me.x) / distance;
    uy = (barrel->at.y - me.y) / distance;

    for (i = 0; i < num_monsters; ++i)
    {
        const k_monster_t *monster = &monsters[i];
        double along = (monster->at.x - me.x) * ux + (monster->at.y - me.y) * uy;
        double across = fabs((monster->at.y - me.y) * ux - (monster->at.x - me.x) * uy);

        if (along > 0.0 && along < distance - BARREL_RADIUS
         && across < monster->radius + BARREL_MARGIN)
        {
            return true;
        }
    }

    return false;
}

// Whether the barrel can be shot at from where the player is: live, its
// chain clear of the player, the shot reaching it with no monster in the
// way, not left alone.
static boolean BarrelInReach(const player_t *player, const barrel_t *barrel, k_vec2_t me)
{
    return barrel->live && !chain_near[barrel->chain] && !Ignored(barrel->mo)
        && Distance(me, barrel->at) <= TARGET_REACH
        && K_Sight_Aim(me.x, me.y, GunHeight(player->mo), barrel->at.x, barrel->at.y,
                       barrel->z, barrel->z + BARREL_HEIGHT)
        && !MonsterInTheWay(me, barrel);
}

static void AddName(char *list, int len, int *count, const char *name)
{
    if (*count < 2)
    {
        M_snprintf(list + strlen(list), len - strlen(list), "%sthe %s",
                   *count > 0 ? " and " : "", name);
    }
    else if (*count == 2)
    {
        M_StringConcat(list, " and more", len);
    }

    ++*count;
}

// What the barrel's blast is worth (BARREL_GAIN), and what it would do,
// for the log ("kill the zombieman, hurt the imp").
static double BlastWorth(const barrel_t *barrel, char *what, int what_len)
{
    char killed[64] = "", hurt[64] = "";
    int num_killed = 0, num_hurt = 0;
    double worth = 0.0;
    int i;

    for (i = 0; i < num_monsters; ++i)
    {
        const k_monster_t *monster = &monsters[i];
        const mobj_t *mo = monster_mobjs[i];
        double cost = Kind(monster->type)->cost;
        double reach, damage, share;

        if (cost <= 0.0 || monster->type == MT_CYBORG || monster->type == MT_SPIDER)
        {
            continue;
        }

        reach = SDL_max(0.0, AxisDistance(barrel->at, monster->at) - monster->radius);

        if (monster->awake && !monster->attacking)
        {
            reach += mo->info->speed * DRIFT_PER_SPEED;
        }

        damage = BLAST - reach;

        if (damage <= 0.0 || !K_Sight_Things(mo, barrel->mo))
        {
            continue;
        }

        share = SDL_min(1.0, damage / SDL_max(monster->health, 1));

        if (!monster->awake
         && fabs(Wrap(atan2(barrel->at.y - monster->at.y, barrel->at.x - monster->at.x)
                      - monster->angle)) > K_PI / 2.0)
        {
            share *= ASLEEP_AWAY;
        }

        worth += cost * share;

        if (damage >= monster->health)
        {
            AddName(killed, sizeof(killed), &num_killed, monster->name);
        }
        else
        {
            AddName(hurt, sizeof(hurt), &num_hurt, monster->name);
        }
    }

    M_snprintf(what, what_len, "%s%s%s%s%s", num_killed > 0 ? "kill " : "", killed,
               num_killed > 0 && num_hurt > 0 ? ", " : "", num_hurt > 0 ? "hurt " : "", hurt);
    return worth;
}

// What a shot at the target with the weapon in hand is worth, as a blast
// is (BlastWorth): its kind's cost times the share of its health it takes.
static double ShotWorth(const player_t *player)
{
    const weapon_t *weapon = WeaponOf(player->readyweapon);
    const k_monster_t *monster;

    if (status.target < 0 || weapon == NULL)
    {
        return 0.0;
    }

    monster = &monsters[status.target];
    return Kind(monster->type)->cost
         * SDL_min(1.0, ShotDamage(player, weapon, monster->distance, monster->radius, &monster->at)
                        / SDL_max(monster->health, 1));
}

// Whether the route ahead passes within BLAST_SAFE of the barrel, and some
// live monster could see it (and so set it off as the player passes).
static boolean TrapBarrel(const barrel_t *barrel, k_vec2_t me)
{
    const k_path_t *path = K_Nav_GetPath();
    const k_navstatus_t *nav = K_Nav_GetStatus();
    double left = TRAP_AHEAD;
    boolean by_route = false;
    k_vec2_t a = me;
    int k, s, i;

    if (path == NULL || nav->state != K_NAV_DRIVING)
    {
        return false;
    }

    for (k = SDL_max(nav->target, 1); k < path->num_points && left > 0.0 && !by_route; ++k)
    {
        k_vec2_t b = path->points[k];
        double length = Distance(a, b);
        int steps = (int) (length / 16.0) + 1;

        for (s = 0; s <= steps && !by_route; ++s)
        {
            k_vec2_t point = { a.x + (b.x - a.x) * s / steps, a.y + (b.y - a.y) * s / steps };

            by_route = AxisDistance(point, barrel->at) < BLAST_SAFE;
        }

        left -= length;
        a = b;
    }

    for (i = 0; by_route && i < num_monsters; ++i)
    {
        if (Kind(monsters[i].type)->cost > 0.0
         && Distance(monsters[i].at, barrel->at) < TRAP_SEEN
         && K_Sight_Things(monster_mobjs[i], barrel->mo))
        {
            return true;
        }
    }

    return false;
}

static void DropBarrel(void)
{
    barrel_mo = NULL;
    barrel_target = -1;
    barrel_why = BARREL_NONE;
    barrel_misses = 0;
}

static void SetBarrel(int index, barrelwhy_t why, const char *reason)
{
    if (barrels[index].mo != barrel_mo || why != barrel_why)
    {
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "shooting the barrel at (%.0f,%.0f) - %s",
              barrels[index].at.x, barrels[index].at.y, reason);
        barrel_misses = 0;
        barrel_health = barrels[index].mo->health;
    }

    barrel_mo = barrels[index].mo;
    barrel_target = index;
    barrel_why = why;
    status.barrel_at = barrels[index].at;

    if (reason != barrel_reason)
    {
        M_StringCopy(barrel_reason, reason, sizeof(barrel_reason));
    }
}

// Whether to shoot a barrel rather than the target (plan, 7.3; the user, on
// Doom II MAP02's barrels): one the brain wants out of the way, when no
// monster is to be shot; one whose blast is worth more than a shot at the
// target; or a trap barrel by the route ahead, when nothing else is to be
// shot and it is cheap.
static void ChooseBarrel(const player_t *player)
{
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    const weapon_t *weapon = WeaponOf(player->readyweapon);
    double shot_worth = ShotWorth(player), best_worth = 0.0, current_worth = 0.0;
    char what[96], best_what[96] = "", current_what[96] = "", reason[96];
    boolean fighting = status.target >= 0 || battle;
    int current = -1, best = -1, i;

    for (i = 0; barrel_mo != NULL && i < num_barrels; ++i)
    {
        if (barrels[i].mo == barrel_mo)
        {
            current = i;
        }
    }

    if (barrel_mo != NULL && (current < 0 || !barrels[current].live))
    {
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "the barrel at (%.0f,%.0f) is going off",
              status.barrel_at.x, status.barrel_at.y);
        DropBarrel();
        current = -1;
    }

    barrel_target = -1;

    // In the way (K_Combat_ShootBarrel): from where the brain sent the
    // player, with whatever weapon pops it. Held on to once found: each hit
    // pushes it a little (MAP23: two bullets pushed one off where the brain
    // saw it, and it was not shot again).
    if (blocking_wanted && status.target < 0)
    {
        if (current >= 0 && barrel_why == BARREL_BLOCKING
         && BarrelInReach(player, &barrels[current], me))
        {
            SetBarrel(current, BARREL_BLOCKING, barrel_reason);
            return;
        }

        for (i = 0; i < num_barrels; ++i)
        {
            if (barrels[i].live && AxisDistance(barrels[i].at, blocking_at) < BLOCKING_DRIFT
             && (best < 0 || Distance(barrels[i].at, blocking_at)
                             < Distance(barrels[best].at, blocking_at)))
            {
                best = i;
            }
        }

        if (best >= 0 && BarrelInReach(player, &barrels[best], me))
        {
            SetBarrel(best, BARREL_BLOCKING, "it is in the way");
            return;
        }

        best = -1;
    }

    if (player->pendingweapon != wp_nochange || weapon == NULL)
    {
        DropBarrel();
        return;
    }

    // Primed, near or by the route ahead: popped first, the nearest, if the
    // weapon in hand does it at one go.
    for (i = 0; i < num_barrels; ++i)
    {
        double distance = Distance(me, barrels[i].at);

        if (Primed(&barrels[i]) && BarrelShot(player, weapon, distance) >= barrels[i].health
         && (best < 0 || distance < Distance(me, barrels[best].at))
         && BarrelInReach(player, &barrels[i], me)
         && (distance < PRIMED_REACH || TrapBarrel(&barrels[i], me)))
        {
            best = i;
        }
    }

    if (best >= 0)
    {
        SetBarrel(best, BARREL_PRIMED, "it was hit already, and the next hit sets it off");
        return;
    }

    for (i = 0; i < num_barrels; ++i)
    {
        double chance = SDL_min(1.0, BarrelShot(player, weapon, Distance(me, barrels[i].at))
                                     / BARREL_HEALTH);
        double worth;

        // In a fight, only with a weapon that pops it at one go (the user:
        // with the pistol it takes two or three shots, the autoaim sends
        // some to the monsters, and things change before it goes off).
        if (chance * BARREL_SHOTS < 1.0 || (fighting && chance < 1.0)
         || !BarrelInReach(player, &barrels[i], me))
        {
            continue;
        }

        // Worth the odds that a shot pops it.
        worth = BlastWorth(&barrels[i], what, sizeof(what)) * chance;

        if (i == current && barrel_why == BARREL_WEAPON)
        {
            current_worth = worth;
            M_StringCopy(current_what, what, sizeof(current_what));
        }

        if (worth > best_worth)
        {
            best = i;
            best_worth = worth;
            M_StringCopy(best_what, what, sizeof(best_what));
        }
    }

    // Another barrel only when clearly better (MAP02: three barrels in a
    // tenth of a second, none of them shot).
    if (current_worth > 0.0 && best != current && best_worth < current_worth * BARREL_SWITCH)
    {
        best = current;
        best_worth = current_worth;
        M_StringCopy(best_what, current_what, sizeof(best_what));
    }

    // Kept while still worth the shot at the target.
    if (best >= 0 && best_worth >= BARREL_MIN
     && (best_worth > shot_worth * BARREL_GAIN
         || (best == current && barrel_why == BARREL_WEAPON && best_worth >= shot_worth)))
    {
        M_snprintf(reason, sizeof(reason), "its blast would %s", best_what);
        SetBarrel(best, BARREL_WEAPON, reason);
        return;
    }

    // A trap ahead: only when it is cheap.
    if (status.target >= 0 || battle || K_Condition_Get()->ammo != K_NEED_OK
     || weapon->reach > 0.0 || weapon->flies)
    {
        DropBarrel();
        return;
    }

    if (current >= 0 && barrel_why == BARREL_TRAP
     && BarrelInReach(player, &barrels[current], me)
     && CanPop(player, weapon, Distance(me, barrels[current].at))
     && (leveltime % TRAP_TICS != 0 || TrapBarrel(&barrels[current], me)))
    {
        SetBarrel(current, BARREL_TRAP, barrel_reason);
        return;
    }

    DropBarrel();

    if (leveltime % TRAP_TICS != 0)
    {
        return;
    }

    best = -1;

    for (i = 0; i < num_barrels; ++i)
    {
        double distance = Distance(me, barrels[i].at);

        if ((best < 0 || distance < Distance(me, barrels[best].at))
         && BarrelInReach(player, &barrels[i], me) && CanPop(player, weapon, distance)
         && TrapBarrel(&barrels[i], me))
        {
            best = i;
        }
    }

    if (best >= 0)
    {
        SetBarrel(best, BARREL_TRAP, "the way passes it, where monsters could set it off");
    }
}

// A cheap hash for the strafing's timing: Karp may not draw the game's
// random numbers (plan, 2.4), and the same game must give the same moves,
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

// Whether the player can step to a point: floor it can walk onto from the
// floor it stands on, clear of walls, solid things and monsters, not within
// `edge` of an edge it could slip off, and not a floor that hurts unless it
// stands on one already.
static boolean Steppable(k_vec2_t at, double floor, boolean hurts, double edge)
{
    const k_map_t *map = K_Map_Get();
    int cell = K_Map_CellAt(at.x, at.y);
    double cell_floor;
    int i;

    if (cell < 0 || K_Map_CellBlock(cell) >= 0)
    {
        return false;
    }

    cell_floor = K_Map_CellFloor(cell);

    if (cell_floor > floor + MAX_CLIMB || cell_floor < floor - MAX_CLIMB
     || K_Graph_Clearance(at, PLAYER_RADIUS + 1.0) < PLAYER_RADIUS + 1.0
     || K_Graph_DropDistance(at, cell_floor, edge) < edge
     || (!hurts && K_Map_FloorDamage(map->subsectors[map->cells[cell].subsector].sector) > 0.0))
    {
        return false;
    }

    for (i = 0; i < num_monsters; ++i)
    {
        if (Distance(at, monsters[i].at) < monsters[i].radius + PLAYER_RADIUS + 4.0)
        {
            return false;
        }
    }

    return true;
}

// While fighting: well away from edges.
static boolean Walkable(k_vec2_t at, double floor, boolean hurts)
{
    return Steppable(at, floor, hurts, EDGE_MARGIN);
}

// Where moving at (vx, vy) takes the player over the next K_THREAT_TICS:
// from now, as nav would steer it (K_Nav_Simulate), facing face if not
// NULL; held where it could not step on (Steppable, every CHECK_TICS).
// Returns how many points it gave.
static int MovePath(k_vec2_t me, double vx, double vy, const double *face, double floor,
                    boolean hurts, double edge, k_vec2_t *path)
{
    k_vec2_t good = me;
    int tics = K_Nav_Simulate(vx, vy, face, K_THREAT_TICS, path);
    int t, k;

    for (t = CHECK_TICS - 1; t < tics && (vx != 0.0 || vy != 0.0); t += CHECK_TICS)
    {
        if (!Steppable(path[t], floor, hurts, edge))
        {
            for (k = t - CHECK_TICS + 1; k < tics; ++k)
            {
                path[k] = good;
            }

            break;
        }

        good = path[t];
    }

    return tics;
}

// What moving at (vx, vy) would cost in health from what flies at the
// player or is about to (k_threat.h), along MovePath. hit, if not NULL,
// gets the costliest.
static double MoveCost(k_vec2_t me, double vx, double vy, const double *face, double floor,
                       boolean hurts, double edge, k_threathit_t *hit)
{
    k_vec2_t path[K_THREAT_TICS];
    int tics = MovePath(me, vx, vy, face, floor, hurts, edge, path);

    return K_Threat_PathCost(path, tics, floor, hit);
}

// The way taken: what it expects to meet, for the log (K_Threat_Expect).
static void ExpectMove(k_vec2_t me, double vx, double vy, const double *face, double floor,
                       boolean hurts, double edge)
{
    k_vec2_t path[K_THREAT_TICS];
    int tics = MovePath(me, vx, vy, face, floor, hurts, edge, path);

    K_Threat_Expect(path, tics, floor);
}

// This tic's exposers (EXPOSE_WEIGHT): the MAX_EXPOSERS costliest awake
// monsters in sight within EXPOSE_REACH, but the target and those that only
// bite.
static void UpdateExposers(void)
{
    int i, k;

    num_exposers = 0;

    for (i = 0; i < num_monsters; ++i)
    {
        const k_monster_t *monster = &monsters[i];
        double cost = Kind(monster->type)->cost;

        if (i == status.target || !monster->awake || !monster->visible || cost <= 0.0
         || monster->distance > EXPOSE_REACH || BitesOnly(monster->type)
         || (num_exposers == MAX_EXPOSERS
             && cost <= Kind(monsters[exposers[num_exposers - 1]].type)->cost))
        {
            continue;
        }

        for (k = num_exposers < MAX_EXPOSERS ? num_exposers++ : num_exposers - 1;
             k > 0 && Kind(monsters[exposers[k - 1]].type)->cost < cost; --k)
        {
            exposers[k] = exposers[k - 1];
        }

        exposers[k] = i;
    }
}

// What the exposers that would see the player at a spot, on the floor at
// `floor`, cost.
static double Exposure(k_vec2_t at, double floor)
{
    double cost = 0.0;
    int i;

    for (i = 0; i < num_exposers; ++i)
    {
        const k_monster_t *monster = &monsters[exposers[i]];

        if (K_Sight_Check(monster->at.x, monster->at.y, monster->z + monster->height * 0.75,
                          at.x, at.y, floor, floor + PLAYER_HEIGHT))
        {
            cost += Kind(monster->type)->cost;
        }
    }

    return cost;
}

// Whether the weapon wanted is a melee one for the monster (MeleeWorth): the
// chainsaw against what it is for, the berserk fist against the weakest.
static boolean MeleeFor(const player_t *player, const k_monster_t *monster)
{
    return (wanted == wp_chainsaw && SawFor(monster))
        || (wanted == wp_fist && player->powers[pw_strength]
            && (monster->type == MT_POSSESSED || monster->type == MT_TROOP));
}

// This tic's trips: the walk-over lines within reach of me that do
// something.
static void FindTrips(k_vec2_t me, double reach)
{
    k_vec2_t min = { me.x - reach, me.y - reach };
    k_vec2_t max = { me.x + reach, me.y + reach };
    int found[256];
    int count = K_Graph_LinesNear(min, max, found, arrlen(found));
    int k;

    num_trips = 0;

    for (k = 0; k < count && num_trips < MAX_TRIPS; ++k)
    {
        if (K_Specials_WalkOver(K_Specials_LineSpecial(found[k])))
        {
            trips[num_trips++] = found[k];
        }
    }
}

// Whether a straight step from a to b crosses a trip: the player's centre
// goes over its line with the player's box on it (P_CrossSpecialLine).
static boolean Trips(k_vec2_t a, k_vec2_t b)
{
    const k_map_t *map = K_Map_Get();
    int k;

    for (k = 0; k < num_trips; ++k)
    {
        const k_line_t *line = &map->lines[trips[k]];
        double ex = line->v2.x - line->v1.x, ey = line->v2.y - line->v1.y;
        double length = sqrt(ex * ex + ey * ey);
        double sa = ex * (a.y - line->v1.y) - ey * (a.x - line->v1.x);
        double sb = ex * (b.y - line->v1.y) - ey * (b.x - line->v1.x);
        double t, along;

        if (length < 1e-6 || sa * sb > 0.0 || sa == sb)
        {
            continue;
        }

        // Where the step meets the line, along it from v1.
        t = sa / (sa - sb);
        along = ((a.x + (b.x - a.x) * t - line->v1.x) * ex
               + (a.y + (b.y - a.y) * t - line->v1.y) * ey) / length;

        if (along > -PLAYER_RADIUS && along < length + PLAYER_RADIUS)
        {
            return true;
        }
    }

    return false;
}

// The room to back off along the unit way (ux, uy) from MOVE_LOOK on, out to
// ROOM_REACH, as a share of that: floor to walk on, over no trip.
static double Room(k_vec2_t me, double ux, double uy, double floor, boolean hurts)
{
    k_vec2_t last = { me.x + ux * MOVE_LOOK, me.y + uy * MOVE_LOOK };
    double free = MOVE_LOOK, r;

    for (r = MOVE_LOOK + ROOM_STEP; r <= ROOM_REACH; r += ROOM_STEP)
    {
        k_vec2_t at = { me.x + ux * r, me.y + uy * r };

        if (!Walkable(at, floor, hurts) || Trips(last, at))
        {
            break;
        }

        last = at;
        free = r;
    }

    return (free - MOVE_LOOK) / (ROOM_REACH - MOVE_LOOK);
}

// Where to move while fighting: each of MOVE_WAYS directions that can be
// walked, over no trip, scored by the side strafed to (and across the line
// of fire rather than along it), the distance from where the fight began
// (holding, from the spot held at, on a shorter leash: back off a door) -
// none while pressed - and the awake monsters nearer than they are kept off
// (KEEP_PUSH), which it backs away from, pressed toward the room to back
// off further; keeping the target where the shots find it (the user:
// strafing into a corridor and out, the bot looked to be peeking round the
// corner), and open floor before a passage; out of the sight of the other
// monsters that shoot (Exposure); and out of the way of projectiles
// (THREAT_WEIGHT). Round the monster by its place in the list: the target,
// or in a standoff the hopeless one kept off (AVOID_PAST), which is not
// shot at - out of its sight (an exposer) rather than in it.
static void ChooseMove(const player_t *player, int round, boolean standoff)
{
    static const double speeds[] = { STRAFE_SPEED, DODGE_SPEED };
    const k_monster_t *target = &monsters[round];
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    double floor = ToMap(player->mo->floorz);
    double gun = GunHeight(player->mo);
    boolean hurts = K_Map_FloorDamage((int) (player->mo->subsector->sector - sectors)) > 0.0;
    boolean threats = K_Threat_Count() > 0;
    boolean rockets = !standoff
                   && (wanted == wp_missile
                       || (RocketsFor(target) && CanFire(player, WeaponOf(wp_missile))
                           && !OtherGun(player, false) && !KeptFrom(player, target)));
    double tx = target->at.x - me.x, ty = target->at.y - me.y;
    double length = sqrt(tx * tx + ty * ty);
    double face = atan2(ty, tx);
    double sx, sy, best_score = -1e9;
    double pusher_keep[MAX_PUSHERS];
    int pushers[MAX_PUSHERS];
    int num_pushers = 0;
    boolean found = false;
    int k, i, s;

    move.x = move.y = 0.0;
    press = 0.0;
    presser = -1;

    if (length < 1e-6)
    {
        return;
    }

    tx /= length;
    ty /= length;

    // Those it backs away from: awake, nearer than KEEP_AWAY, or than their
    // kind's own distance if they can come at the player - the target with
    // rockets wanted, than ROCKET_ROOM beyond where they stop - but not the
    // target of a melee weapon. The one deepest inside its own distance
    // presses the player.
    for (i = 0; i < num_monsters && num_pushers < MAX_PUSHERS; ++i)
    {
        const k_monster_t *monster = &monsters[i];
        double keep = Kind(monster->type)->keep;

        if (!monster->awake || monster->distance < 1e-6
         || (i == status.target && MeleeFor(player, monster)))
        {
            continue;
        }

        if (keep <= KEEP_AWAY || monster->distance >= keep || !Reaches(i))
        {
            keep = KEEP_AWAY;
        }

        if (i == round && rockets)
        {
            keep = SDL_max(keep, ROCKET_MIN + monster->radius + ROCKET_RADIUS + ROCKET_ROOM);
        }

        if (monster->distance >= keep)
        {
            continue;
        }

        pushers[num_pushers] = i;
        pusher_keep[num_pushers++] = keep;

        if (keep > KEEP_AWAY && (keep - monster->distance) / KEEP_AWAY > press)
        {
            press = (keep - monster->distance) / KEEP_AWAY;
            presser = i;
        }
    }

    // Pressed: where the fight began moves along as the player backs off.
    if (presser >= 0)
    {
        pressed = true;
        anchor = me;

        if (monster_mobjs[presser] != pressed_mo)
        {
            pressed_mo = monster_mobjs[presser];
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "backing off from the %s, %.0f units off - kept "
                  "%.0f off", monsters[presser].name, monsters[presser].distance,
                  monsters[presser].distance + press * KEEP_AWAY);
        }
    }

    FindTrips(me, ROOM_REACH + PLAYER_RADIUS);

    if (--strafe_tics <= 0 && !threats)
    {
        strafe_dir = -strafe_dir;
        strafe_tics = STRAFE_MIN + Hash(K_Salt_Mix(leveltime)) % STRAFE_SPREAD;
    }

    // To the target's left, or its right.
    sx = -ty * strafe_dir;
    sy = tx * strafe_dir;

    for (k = 0; k < MOVE_WAYS; ++k)
    {
        double angle = k * 2.0 * K_PI / MOVE_WAYS;
        double ux = cos(angle), uy = sin(angle);
        k_vec2_t near = { me.x + ux * MOVE_LOOK * 0.5, me.y + uy * MOVE_LOOK * 0.5 };
        k_vec2_t far = { me.x + ux * MOVE_LOOK, me.y + uy * MOVE_LOOK };
        double score, from_anchor, speed = STRAFE_SPEED, dodge = 0.0;

        if (!Walkable(near, floor, hurts) || !Walkable(far, floor, hurts) || Trips(me, far))
        {
            continue;
        }

        for (s = 0; threats && s < arrlen(speeds); ++s)
        {
            double here = -THREAT_WEIGHT * MoveCost(me, ux * speeds[s], uy * speeds[s], &face,
                                                    floor, hurts, EDGE_MARGIN, NULL)
                        - (s > 0 ? SPRINT_COST : 0.0);

            if (s == 0 || here > dodge)
            {
                dodge = here;
                speed = speeds[s];
            }
        }

        score = ux * sx + uy * sy + 0.3 * (1.0 - fabs(ux * tx + uy * ty))
              + 0.3 * K_Graph_Clearance(far, OPEN_REACH) / OPEN_REACH;

        if (!standoff && !K_Sight_Aim(far.x, far.y, gun, target->at.x, target->at.y, target->z,
                                      target->z + target->height))
        {
            score -= 1.5;
        }

        // The BFG's spray to come, whatever the target now.
        if (!BfgSees(far, gun))
        {
            score -= BFG_HOLD;
        }

        score -= EXPOSE_WEIGHT * Exposure(far, floor);

        if (pressed)
        {
            score += ROOM_WEIGHT * press * Room(me, ux, uy, floor, hurts);
        }
        else
        {
            from_anchor = Distance(far, holding ? hold_spot : anchor);

            if (from_anchor > (holding ? HOLD_LEASH : LEASH))
            {
                score -= (from_anchor - (holding ? HOLD_LEASH : LEASH)) / 32.0;
            }
        }

        for (i = 0; i < num_pushers; ++i)
        {
            const k_monster_t *monster = &monsters[pushers[i]];

            score -= KEEP_PUSH * (ux * (monster->at.x - me.x) + uy * (monster->at.y - me.y))
                   / monster->distance * (pusher_keep[i] - monster->distance) / KEEP_AWAY;
        }

        if (rockets)
        {
            score -= ROCKET_TOWARD * SDL_max(0.0, ux * tx + uy * ty);
        }

        // Out of the blast of barrels the monsters' shots may set off, and
        // most of all of those already going off, or primed.
        for (i = 0; i < num_barrels; ++i)
        {
            double axis = AxisDistance(far, barrels[i].at);

            if (axis < BLAST_SAFE)
            {
                score -= (barrels[i].live && !Primed(&barrels[i]) ? BARREL_AWAY : DYING_AWAY)
                       * (BLAST_SAFE - axis) / BLAST_SAFE;
            }
        }

        score += dodge;

        if (score > best_score)
        {
            best_score = score;
            move.x = ux * speed;
            move.y = uy * speed;
            found = true;
        }
    }

    // Hemmed in, the player stands.
    if (threats)
    {
        ExpectMove(me, move.x, move.y, &face, floor, hurts, EDGE_MARGIN);
    }

    // Going the other way after all (a wall, a monster): strafe that way.
    if (found && move.x * sx + move.y * sy < 0.0)
    {
        strafe_dir = -strafe_dir;
        strafe_tics = STRAFE_MIN + Hash(K_Salt_Mix(leveltime)) % STRAFE_SPREAD;
    }
}

// The cheapest way out of what flies at the player or is about to
// (k_threat.h): going on at `velocity` (the route's way; zero, standing)
// against each of MOVE_WAYS ways at DODGE_SPEED (DODGE_MIN). With sidestep,
// only ways no more than 90 degrees off velocity, unless a hit going on
// could kill. keep is the way stepped already, -1 if none. Returns the way,
// or -1 to go on as it is; hit gets the costliest hit going on, *on_way
// whether the way is a sidestep. The way returned is the one taken
// (Expect).
static int DodgeWay(const player_t *player, k_vec2_t velocity, boolean sidestep, int keep,
                    k_threathit_t *hit, boolean *on_way)
{
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    double floor = ToMap(player->mo->floorz);
    boolean hurts = K_Map_FloorDamage((int) (player->mo->subsector->sector - sectors)) > 0.0;
    double edge = SDL_min(EDGE_MARGIN, K_Graph_DropDistance(me, floor, EDGE_MARGIN));
    double speed = sqrt(velocity.x * velocity.x + velocity.y * velocity.y);
    k_vec2_t ahead = { 0.0, 0.0 };
    const double *face = NULL;
    double face_angle, best_cost;
    int k, i, best = -1;

    *on_way = false;

    if (status.target >= 0)
    {
        face_angle = atan2(monsters[status.target].at.y - me.y,
                           monsters[status.target].at.x - me.x);
        face = &face_angle;
    }

    if (speed > 1e-6)
    {
        ahead.x = velocity.x / speed;
        ahead.y = velocity.y / speed;
    }

    best_cost = MoveCost(me, velocity.x, velocity.y, face, floor, hurts, edge, hit);

    if (best_cost < DODGE_MIN)
    {
        ExpectMove(me, velocity.x, velocity.y, face, floor, hurts, edge);
        return -1;
    }

    *on_way = sidestep && !hit->lethal;
    best_cost -= keep >= 0 ? 0.0 : ROUTE_KEEP;

    for (k = 0; k < MOVE_WAYS; ++k)
    {
        double angle = k * 2.0 * K_PI / MOVE_WAYS;
        double ux = cos(angle), uy = sin(angle);
        double along = ux * ahead.x + uy * ahead.y;
        k_vec2_t near = { me.x + ux * MOVE_LOOK * 0.5, me.y + uy * MOVE_LOOK * 0.5 };
        k_vec2_t far = { me.x + ux * MOVE_LOOK, me.y + uy * MOVE_LOOK };
        double cost;

        if ((*on_way && along < 0.0) || !Steppable(near, floor, hurts, edge)
         || !Steppable(far, floor, hurts, edge))
        {
            continue;
        }

        cost = MoveCost(me, ux * DODGE_SPEED, uy * DODGE_SPEED, face, floor, hurts, edge, NULL);

        if (*on_way)
        {
            cost += DETOUR_COST * (1.0 - along);
        }

        if (k == keep)
        {
            cost -= DODGE_KEEP;
        }

        for (i = 0; i < num_barrels; ++i)
        {
            double axis = AxisDistance(far, barrels[i].at);

            if (axis < BLAST_SAFE)
            {
                cost += (barrels[i].live && !Primed(&barrels[i]) ? BARREL_COST : DYING_COST)
                      * (BLAST_SAFE - axis) / BLAST_SAFE;
            }
        }

        if (cost < best_cost)
        {
            best_cost = cost;
            best = k;
        }
    }

    if (best >= 0)
    {
        ExpectMove(me, cos(best * 2.0 * K_PI / MOVE_WAYS) * DODGE_SPEED,
                   sin(best * 2.0 * K_PI / MOVE_WAYS) * DODGE_SPEED, face, floor, hurts, edge);
    }
    else
    {
        ExpectMove(me, velocity.x, velocity.y, face, floor, hurts, edge);
    }

    return best;
}

// A target to stop for: one some weapon in hand can hurt, in sight for a
// while, awake and near enough to matter (asleep ones are targets only when
// close anyway), and one that can come at the player, a gunman, or comfy.
static boolean WorthFighting(void)
{
    const k_monster_t *target;

    if (status.target < 0 || !can_hurt || sighted < SIGHTED_TICS)
    {
        return false;
    }

    target = &monsters[status.target];
    return (!target->awake || target->distance <= (bold ? TARGET_REACH : BATTLE_REACH))
        && FoughtAsAny(status.target);
}

static void StartBattle(const player_t *player)
{
    battle = true;
    status.battle = true;
    battle_since = leveltime;
    battle_kills = player->killcount;
    anchor.x = ToMap(player->mo->x);
    anchor.y = ToMap(player->mo->y);
    pressed_mo = NULL;
    progress_tic = leveltime;
    strafe_tics = STRAFE_MIN + Hash(K_Salt_Mix(leveltime)) % STRAFE_SPREAD;

    if (status.target >= 0)
    {
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "fighting the %s, %.0f units off - the route waits",
              monsters[status.target].name, monsters[status.target].distance);
    }
    else if (avoid >= 0)
    {
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "keeping off the %s, %.0f units off - beyond what the "
              "player holds; the route waits", monsters[avoid].name, monsters[avoid].distance);
    }
    else
    {
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "keeping the BFG ball's burst in sight for its spray - "
              "the route waits");
    }

    K_Nav_Pause(1);
}

// Over, or broken off for why (NULL if not).
static void EndBattle(const player_t *player, const char *why)
{
    battle = false;
    status.battle = false;
    move.x = move.y = 0.0;

    if (why != NULL)
    {
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "breaking off the fight after %.1f s, %d killed - %s",
              (double) (leveltime - battle_since) / TICRATE, player->killcount - battle_kills,
              why);
    }
    else
    {
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "fight over after %.1f s, %d killed - back on the way",
              (double) (leveltime - battle_since) / TICRATE, player->killcount - battle_kills);
    }

    // Holding or ducking, the route waits on.
    if (!holding && !ducking)
    {
        K_Nav_Pause(0);
    }
}

// An island: the dry floor the player can walk to from its cell without
// stepping onto a floor that hurts fits within ISLAND_SPAN either way, in
// ISLAND_CELLS cells at the most, and such a floor lies round it - not a
// small room shut all round. Looked at again when the player's cell
// changes.
#define ISLAND_SPAN 192.0
#define ISLAND_CELLS 32

static boolean CellHurts(int cell)
{
    const k_map_t *map = K_Map_Get();

    return K_Map_FloorDamage(map->subsectors[map->cells[cell].subsector].sector) > 0.0;
}

static boolean OnIsland(int cell)
{
    static int last_cell = -1;
    static unsigned int last_generation;
    static boolean last_island;
    const k_map_t *map = K_Map_Get();
    const k_graph_t *graph = K_Graph_Get();
    int queue[ISLAND_CELLS];
    int head = 0, tail = 0, k, i;
    boolean slime = false, island = true;
    k_vec2_t min, max;

    if (cell == last_cell && map->generation == last_generation)
    {
        return last_island;
    }

    last_cell = cell;
    last_generation = map->generation;

    if (cell < 0 || cell >= map->num_cells || CellHurts(cell))
    {
        last_island = false;
        return false;
    }

    min = map->cells[cell].min;
    max = map->cells[cell].max;
    queue[tail++] = cell;

    while (head < tail && island)
    {
        int at = queue[head++];

        for (k = 0; k < graph->link_count[at] && island; ++k)
        {
            const k_link_t *link = &graph->links[graph->first_link[at] + k];
            const k_cell_t *next = &map->cells[link->neighbor];

            if (K_Graph_CanCross(link->portal, at) != K_CROSS_OPEN)
            {
                continue;
            }

            if (CellHurts(link->neighbor))
            {
                slime = true;
                continue;
            }

            for (i = 0; i < tail && queue[i] != link->neighbor; ++i)
            {
            }

            if (i < tail)
            {
                continue;
            }

            min.x = SDL_min(min.x, next->min.x);
            min.y = SDL_min(min.y, next->min.y);
            max.x = SDL_max(max.x, next->max.x);
            max.y = SDL_max(max.y, next->max.y);
            island = tail < ISLAND_CELLS && max.x - min.x <= ISLAND_SPAN
                  && max.y - min.y <= ISLAND_SPAN;

            if (island)
            {
                queue[tail++] = link->neighbor;
            }
        }
    }

    last_island = island && slime;
    return last_island;
}

// Whether an awake monster is within reach: no run for an item (escape)
// turns away from it, it would only strike the player from behind.
static boolean BlowsNear(double reach)
{
    int i;

    for (i = 0; i < num_monsters; ++i)
    {
        if (monsters[i].awake && monsters[i].distance < reach
         && Kind(monsters[i].type)->cost > 0.0)
        {
            return true;
        }
    }

    return false;
}

// No fight is fought on a floor that hurts (the user, Doom II MAP15: fights
// in the middle of large lava pits): none is begun there, and one going on
// is broken off at once; the route goes on, taking the player off it,
// shooting on the way. A fight begun on dry floor keeps off such floors
// (Steppable). Nor on an island (OnIsland) - the fight could not step off
// it (the user, E2M9: the soulsphere's pit, 128 across in a ring of slime
// in the middle of an open room; the fight begun there stayed there, the
// monsters coming from all sides, and "a skilled player would move aside
// and shoot from a distance"): the route takes the player off it too. A
// hopeless monster that presses (Avoided) is kept off as a fight is fought,
// with nothing to shoot (a standoff); and a BFG ball's spray to come keeps
// one on (BFG_HOLD).
static void UpdateBattle(const player_t *player)
{
    boolean burning = K_Map_FloorDamage((int) (player->mo->subsector->sector - sectors)) > 0.0;
    boolean island = !burning && OnIsland(K_Map_CellAt(ToMap(player->mo->x),
                                                       ToMap(player->mo->y)));
    boolean fight, flee;

    // Out of reach of the shots (behind bars, too high for the aim):
    // left alone for a while.
    if (battle && status.target >= 0 && leveltime - progress_tic > STALEMATE_TICS)
    {
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "leaving the %s alone for now - not hurt in %d s",
              monsters[status.target].name, STALEMATE_TICS / TICRATE);
        Ignore(target_mo, IGNORE_TICS, false);
        DropTarget();
    }

    // Going for health, CRITICAL, or dashing for a weapon or ammo: no fight
    // is started, and one going on is broken off at once; the player shoots
    // on the way. Not with an awake monster within CLOSE_REACH, in reach of
    // its blows - KEEP_AWAY once fighting, or it flips at the edge (E3M4: at
    // 39 health the bot broke off a fight with a demon 50 off for health
    // bonuses beside it, stuck on the demon, bitten down to 21).
    flee = escape && !BlowsNear(battle ? KEEP_AWAY : CLOSE_REACH);
    fight = (WorthFighting() || avoid >= 0 || bfg_pending) && !flee && !burning && !island;
    calm = fight ? 0 : calm + 1;

    if (!battle && fight && K_Nav_CanPause())
    {
        StartBattle(player);
    }
    else if (battle && (flee || burning || island))
    {
        EndBattle(player, flee ? escape_why
                        : burning ? "the floor hurts, going on to dry floor"
                        : "a floor that hurts all round, going on off it");
    }
    else if (battle && calm >= BATTLE_CALM)
    {
        EndBattle(player, NULL);
    }

    pressed = false;
    presser = -1;
    press = 0.0;

    // Nothing to shoot for now (a monster ducked out of sight): standing,
    // or out of the way of what it fired (DodgeWay).
    if (battle && status.target >= 0)
    {
        ChooseMove(player, status.target, false);
        battle_way = -1;
    }
    else if (battle && avoid >= 0)
    {
        ChooseMove(player, avoid, true);
        battle_way = -1;
    }
    else if (battle && K_Threat_Count() > 0)
    {
        k_vec2_t still = { 0.0, 0.0 };
        k_threathit_t hit;
        boolean on_way;

        battle_way = DodgeWay(player, still, false, battle_way, &hit, &on_way);
        move.x = battle_way >= 0 ? cos(battle_way * 2.0 * K_PI / MOVE_WAYS) * DODGE_SPEED : 0.0;
        move.y = battle_way >= 0 ? sin(battle_way * 2.0 * K_PI / MOVE_WAYS) * DODGE_SPEED : 0.0;
    }
    else
    {
        move.x = move.y = 0.0;
        battle_way = -1;
    }
}

// Room for the searches through the cells (CoverByCells, FindRetreat).
static void GrowCover(int cells)
{
    if (cover_capacity < cells)
    {
        cover_capacity = cells;
        cover_distance = realloc(cover_distance, cover_capacity * sizeof(*cover_distance));
        cover_entry = realloc(cover_entry, cover_capacity * sizeof(*cover_entry));
        cover_from = realloc(cover_from, cover_capacity * sizeof(*cover_from));
        cover_hops = realloc(cover_hops, cover_capacity * sizeof(*cover_hops));
        cover_open = realloc(cover_open, cover_capacity * sizeof(*cover_open));
    }
}

// Whether a point is clear of the awake monsters near (RETREAT_CLEAR): no
// nearer to any than RETREAT_CLEAR, or than the player is.
static boolean RetreatClear(k_vec2_t at, const int *near, int num_near)
{
    int i;

    for (i = 0; i < num_near; ++i)
    {
        const k_monster_t *monster = &monsters[near[i]];

        if (Distance(at, monster->at) < SDL_min(monster->distance, RETREAT_CLEAR))
        {
            return false;
        }
    }

    return true;
}

// Where to retreat to from a monster (by its place in the list): open floor
// (RETREAT_OPEN), or with `hide` a spot out of its sight (HIDE_REACH) - the
// nearest cell's middle, walked to through open crossings (portal middle to
// portal middle), none on a floor that hurts unless the player stands on
// one; the way there, the spot last.
static boolean FindRetreat(const player_t *player, int away, boolean hide, k_vec2_t *points,
                           int *num_points)
{
    const k_map_t *map = K_Map_Get();
    const k_graph_t *graph = K_Graph_Get();
    const k_monster_t *from = &monsters[away];
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    boolean hurts = K_Map_FloorDamage((int) (player->mo->subsector->sector - sectors)) > 0.0;
    double reach = hide ? HIDE_REACH : RETREAT_REACH;
    double eye = from->z + from->height * 0.75;
    int start = K_Map_CellAt(me.x, me.y);
    int near[MAX_PUSHERS];
    int num_open = 0, num_near = 0, best = -1, cell, k, i;
    double best_score = 1e30;

    for (i = 0; i < num_monsters && num_near < MAX_PUSHERS; ++i)
    {
        if (monsters[i].awake && Kind(monsters[i].type)->cost > 0.0
         && monsters[i].distance < reach + RETREAT_CLEAR)
        {
            near[num_near++] = i;
        }
    }

    if (start < 0 || graph->generation != map->generation)
    {
        return false;
    }

    GrowCover(map->num_cells);

    for (i = 0; i < map->num_cells; ++i)
    {
        cover_distance[i] = 1e30;
    }

    cover_distance[start] = 0.0;
    cover_entry[start] = me;
    cover_from[start] = -1;
    cover_open[num_open++] = start;

    while (num_open > 0)
    {
        int nearest = 0;
        const k_cell_t *c;
        double length;

        for (i = 1; i < num_open; ++i)
        {
            if (cover_distance[cover_open[i]] < cover_distance[cover_open[nearest]])
            {
                nearest = i;
            }
        }

        cell = cover_open[nearest];
        cover_open[nearest] = cover_open[--num_open];

        if (cover_distance[cell] > reach)
        {
            break;
        }

        c = &map->cells[cell];
        length = cover_distance[cell] + Distance(cover_entry[cell], c->centroid);

        if (cell != start && length <= reach
         && (hide || Distance(c->centroid, from->at) >= from->distance + RETREAT_GAIN))
        {
            double floor = K_Map_CellFloor(cell);

            if (Steppable(c->centroid, floor, hurts, EDGE_MARGIN)
             && (hide ? !K_Sight_Check(from->at.x, from->at.y, eye, c->centroid.x, c->centroid.y,
                                       floor, floor + PLAYER_HEIGHT)
                      : K_Graph_Clearance(c->centroid, RETREAT_OPEN) >= RETREAT_OPEN)
             && RetreatClear(c->centroid, near, num_near))
            {
                double score = length + DUCK_SEEN * Exposure(c->centroid, floor);

                if (score < best_score)
                {
                    best_score = score;
                    best = cell;
                }
            }
        }

        for (k = 0; k < graph->link_count[cell]; ++k)
        {
            const k_link_t *link = &graph->links[graph->first_link[cell] + k];
            const k_portal_t *portal = &graph->portals[link->portal];
            int next = link->neighbor;
            k_vec2_t middle = { (portal->a.x + portal->b.x) * 0.5, (portal->a.y + portal->b.y) * 0.5 };
            double distance = cover_distance[cell] + Distance(cover_entry[cell], middle);

            if (distance >= cover_distance[next] || portal->jump >= 0
             || !RetreatClear(middle, near, num_near)
             || K_Graph_CanCross(link->portal, cell) != K_CROSS_OPEN
             || (!hurts && K_Map_FloorDamage(map->subsectors[map->cells[next].subsector].sector)
                           > 0.0))
            {
                continue;
            }

            if (cover_distance[next] >= 1e30)
            {
                cover_open[num_open++] = next;
            }

            cover_distance[next] = distance;
            cover_entry[next] = middle;
            cover_from[next] = cell;
        }
    }

    if (best < 0)
    {
        return false;
    }

    // The way back from the cell to the player's, then turned round.
    *num_points = 0;
    points[(*num_points)++] = map->cells[best].centroid;

    for (cell = best; cover_from[cell] >= 0 && *num_points < RETREAT_POINTS;
         cell = cover_from[cell])
    {
        points[(*num_points)++] = cover_entry[cell];
    }

    for (i = 0; i < *num_points / 2; ++i)
    {
        k_vec2_t swap = points[i];

        points[i] = points[*num_points - 1 - i];
        points[*num_points - 1 - i] = swap;
    }

    return true;
}

static void StopRetreat(const char *why)
{
    retreating = false;
    retreat_again = leveltime + RETREAT_AGAIN;
    hard_pressed = 0;
    K_Log(K_LOG_COMBAT, K_LOG_INFO, "retreat over after %.1f s - %s",
          (double) (leveltime - retreat_since) / TICRATE, why);
}

// Retreating to open floor when pressed hard (RETREAT_AFTER), or out of the
// sight of a hopeless monster kept off that sees the player (HIDE_REACH):
// begun, over, and the step along the way there, instead of the fight's own
// move - unless something flying would hit it.
static void UpdateRetreat(const player_t *player)
{
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    boolean standoff = battle && status.target < 0 && avoid >= 0;
    k_vec2_t to;
    double dx, dy, distance;

    hard_pressed = battle && presser >= 0 && press >= RETREAT_PRESS
                && !BitesOnly(monsters[presser].type)
                && K_Graph_Clearance(me, RETREAT_OPEN) < RETREAT_OPEN ? hard_pressed + 1 : 0;
    seen_tics = standoff && monsters[avoid].visible ? seen_tics + 1 : 0;

    if (retreating)
    {
        retreat_calm = (retreat_hide ? seen_tics > 0 : presser >= 0) ? 0 : retreat_calm + 1;

        if (!battle)
        {
            StopRetreat("the fight is over");
        }
        else if (Distance(me, retreat_points[retreat_num_points - 1]) < RETREAT_NEAR)
        {
            StopRetreat(retreat_hide ? "out of its sight" : "on open floor");
        }
        else if (retreat_calm >= RETREAT_CALM)
        {
            StopRetreat(retreat_hide ? "out of its sight" : "nothing presses now");
        }
        else if (leveltime - retreat_since >= RETREAT_MOST)
        {
            StopRetreat("retreated long enough");
        }
    }
    else if (leveltime >= retreat_again && !bfg_pending
          && (seen_tics >= RETREAT_AFTER || hard_pressed >= RETREAT_AFTER))
    {
        boolean hide = seen_tics >= RETREAT_AFTER;
        int away = hide ? avoid : presser;

        if (FindRetreat(player, away, hide, retreat_points, &retreat_num_points))
        {
            k_vec2_t spot = retreat_points[retreat_num_points - 1];

            retreating = true;
            retreat_hide = hide;
            retreat_next = 0;
            retreat_since = leveltime;
            retreat_calm = 0;
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "%s the %s, %.0f units off - to %s at (%.0f,%.0f), "
                  "%.0f units%s", hide ? "hiding from" : "retreating from", monsters[away].name,
                  monsters[away].distance, hide ? "a spot out of its sight" : "open floor",
                  spot.x, spot.y, Distance(me, spot),
                  retreat_num_points > 1 ? ", round a corner" : "");
        }
        else
        {
            retreat_again = leveltime + RETREAT_LOOK;
        }
    }

    if (!retreating)
    {
        return;
    }

    while (retreat_next < retreat_num_points - 1
        && Distance(me, retreat_points[retreat_next]) < DUCK_WAYPOINT)
    {
        ++retreat_next;
    }

    to = retreat_points[retreat_next];
    dx = to.x - me.x;
    dy = to.y - me.y;
    distance = sqrt(dx * dx + dy * dy);

    if (distance > 1e-6)
    {
        double floor = ToMap(player->mo->floorz);
        boolean hurts = K_Map_FloorDamage((int) (player->mo->subsector->sector - sectors)) > 0.0;
        double vx = dx / distance * DODGE_SPEED, vy = dy / distance * DODGE_SPEED;
        double face = 0.0;

        if (status.target >= 0)
        {
            face = atan2(monsters[status.target].at.y - me.y, monsters[status.target].at.x - me.x);
        }

        if (K_Threat_Count() == 0
         || MoveCost(me, vx, vy, status.target >= 0 ? &face : NULL, floor, hurts, EDGE_MARGIN,
                     NULL) < DODGE_MIN)
        {
            move.x = vx;
            move.y = vy;
        }
    }
}

// The nearest shot barrel about to go off whose blast could reach the
// player (a dead barrel is a quarter of its height), or -1.
static int PendingBlast(const player_t *player)
{
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    double eye = ToMap(player->mo->z) + ToMap(player->mo->height) * 0.75;
    int best = -1, i;

    for (i = 0; i < num_barrels; ++i)
    {
        if (!barrels[i].live && AxisDistance(me, barrels[i].at) < BLAST_SAFE
         && (best < 0 || AxisDistance(me, barrels[i].at) < AxisDistance(me, barrels[best].at))
         && K_Sight_Check(me.x, me.y, eye, barrels[i].at.x, barrels[i].at.y, barrels[i].z,
                          barrels[i].z + BLAST_HEIGHT))
        {
            best = i;
        }
    }

    return best;
}

// The nearest primed barrel whose blast could reach the player, that an
// awake monster can see, not stepped away from for PRIMED_DODGE_TICS
// already; or -1.
static int PrimedNear(const player_t *player)
{
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    double eye = ToMap(player->mo->z) + ToMap(player->mo->height) * 0.75;
    int best = -1, i, k;

    for (i = 0; i < num_barrels; ++i)
    {
        if (!Primed(&barrels[i]) || AxisDistance(me, barrels[i].at) >= BLAST_SAFE
         || (barrels[i].mo == primed_mo && primed_tics >= PRIMED_DODGE_TICS)
         || (best >= 0 && AxisDistance(me, barrels[i].at) >= AxisDistance(me, barrels[best].at))
         || !K_Sight_Check(me.x, me.y, eye, barrels[i].at.x, barrels[i].at.y, barrels[i].z,
                           barrels[i].z + BLAST_HEIGHT))
        {
            continue;
        }

        for (k = 0; k < num_monsters; ++k)
        {
            if (monsters[k].awake && Kind(monsters[k].type)->cost > 0.0
             && Distance(monsters[k].at, barrels[i].at) < TRAP_SEEN
             && K_Sight_Things(monster_mobjs[k], barrels[i].mo))
            {
                best = i;
                break;
            }
        }
    }

    return best;
}

// The monster that hurt the player in the last BLOW_HOLD tics from within
// BLOW_REACH (BLOW_SAFE once stepping away from it) where the shots cannot
// find it, or -1.
static int BlowFrom(const player_t *player)
{
    int i;

    if (leveltime - hurt_tic > BLOW_HOLD)
    {
        return -1;
    }

    i = IndexOf(player->attacker);

    return i >= 0 && !monsters[i].shootable
        && monsters[i].distance < (dodging_blow ? BLOW_SAFE : BLOW_REACH) ? i : -1;
}

static void StopDodge(void)
{
    if (dodging)
    {
        dodging = false;
        dodging_blow = false;
        dodging_primed = false;
        dodging_missile = false;
        move.x = move.y = 0.0;

        if (!holding && !ducking)
        {
            K_Nav_Pause(0);
        }
    }
}

static void StopMissileDodge(void)
{
    if (dodging_missile)
    {
        StopDodge();
    }
}

// Out of a fight, out of the way of what flies at the player or is about
// to (k_threat.h): the way the route takes it, or standing, against the
// others (DodgeWay).
static void DodgeMissiles(const player_t *player)
{
    const k_navstatus_t *nav = K_Nav_GetStatus();
    const k_path_t *path = K_Nav_GetPath();
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    boolean walking = nav->state == K_NAV_DRIVING && !K_Nav_Waiting() && K_Nav_CanPause()
                   && path != NULL && nav->target < path->num_points && !holding && !ducking;
    boolean standing = !walking && (K_Nav_CanPause()
                                    || (K_Nav_Waiting() && nav->action != K_ACTION_RIDE));
    k_vec2_t velocity = { 0.0, 0.0 };
    k_threathit_t hit;
    boolean on_way;
    int best;

    if (K_Threat_Count() == 0 || (!walking && !standing))
    {
        StopMissileDodge();
        return;
    }

    if (walking && Distance(me, path->points[nav->target]) > 1e-6)
    {
        double length = Distance(me, path->points[nav->target]);

        velocity.x = (path->points[nav->target].x - me.x) / length * K_Nav_Tuning()->max_speed;
        velocity.y = (path->points[nav->target].y - me.y) / length * K_Nav_Tuning()->max_speed;
    }

    best = DodgeWay(player, velocity, walking, dodging_missile ? missile_way : -1, &hit, &on_way);

    if (best < 0)
    {
        StopMissileDodge();
        return;
    }

    if (!dodging_missile || missile_on_way != on_way)
    {
        if (on_way)
        {
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "sidestepping the %s - on the way still",
                  hit.name != NULL ? hit.name : "projectile");
        }
        else
        {
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "stepping out of the way of the %s%s - the route "
                  "waits", hit.name != NULL ? hit.name : "projectile",
                  hit.lethal ? ", a hit could kill" : "");
        }

        status.dodges += !dodging_missile;
    }

    dodging = true;
    dodging_missile = true;
    missile_way = best;
    missile_on_way = on_way;
    K_Nav_Pause(!on_way);
    move.x = cos(best * 2.0 * K_PI / MOVE_WAYS) * DODGE_SPEED;
    move.y = sin(best * 2.0 * K_PI / MOVE_WAYS) * DODGE_SPEED;
}

// Out of a fight, away from a blast about to go off (or a primed barrel a
// monster can see), or from blows the shots cannot answer: the route waits
// while the player steps the walkable
// way that gets furthest from it. A blow is stepped away from even at a door
// or lift (the action is begun again after), but only if some way is
// walkable: a lift going down rides on. In battle the strafing keeps away
// from barrels (ChooseMove). With none of these, out of the way of
// projectiles (DodgeMissiles).
static void UpdateDodge(const player_t *player)
{
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    double floor = ToMap(player->mo->floorz);
    boolean hurts = K_Map_FloorDamage((int) (player->mo->subsector->sector - sectors)) > 0.0;
    double edge = SDL_min(EDGE_MARGIN, K_Graph_DropDistance(me, floor, EDGE_MARGIN));
    int blast, primed, blow;
    k_vec2_t from, way = { 0.0, 0.0 };
    double best = -1.0;
    int k;

    if (battle)
    {
        dodging = false;
        dodging_blow = false;
        dodging_primed = false;
        dodging_missile = false;
        return;
    }

    blast = PendingBlast(player);
    primed = blast < 0 ? PrimedNear(player) : -1;
    blow = blast < 0 && primed < 0 ? BlowFrom(player) : -1;

    if (blast < 0 && primed < 0 && blow < 0)
    {
        if (!dodging_missile)
        {
            StopDodge();
        }

        DodgeMissiles(player);
        return;
    }

    if (!dodging && !K_Nav_CanPause() && !(blow >= 0 && K_Nav_Waiting()))
    {
        return;
    }

    // A primed barrel: stepped away from as one going off, for a while.
    if (primed >= 0)
    {
        if (barrels[primed].mo != primed_mo)
        {
            primed_mo = barrels[primed].mo;
            primed_tics = 0;
        }

        ++primed_tics;
        blast = primed;
    }

    from = blast >= 0 ? barrels[blast].at : monsters[blow].at;

    for (k = 0; k < MOVE_WAYS; ++k)
    {
        double angle = k * 2.0 * K_PI / MOVE_WAYS;
        double ux = cos(angle), uy = sin(angle);
        k_vec2_t near = { me.x + ux * MOVE_LOOK * 0.5, me.y + uy * MOVE_LOOK * 0.5 };
        k_vec2_t far = { me.x + ux * MOVE_LOOK, me.y + uy * MOVE_LOOK };
        double away = blast >= 0 ? AxisDistance(far, from) : Distance(far, from);

        if (away > best && Steppable(near, floor, hurts, edge)
         && Steppable(far, floor, hurts, edge))
        {
            best = away;
            way.x = ux;
            way.y = uy;
        }
    }

    if ((blast < 0 || primed >= 0) && best < 0.0)
    {
        StopDodge();
        return;
    }

    if (!dodging || dodging_missile || dodging_blow != (blast < 0)
     || dodging_primed != (primed >= 0))
    {
        if (primed >= 0)
        {
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "stepping away from the barrel at (%.0f,%.0f) - "
                  "hit already, a monster in sight of it; the route waits", from.x, from.y);
        }
        else if (blast >= 0)
        {
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "stepping away from the barrel at (%.0f,%.0f) "
                  "about to go off - the route waits", from.x, from.y);
        }
        else
        {
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "stepping away from the %s at (%.0f,%.0f), "
                  "%.0f units off - it hurts the player where the shots cannot find it; "
                  "the route waits", monsters[blow].name, from.x, from.y,
                  monsters[blow].distance);
        }

        dodging = true;
        dodging_blow = blast < 0;
        dodging_primed = primed >= 0;
        dodging_missile = false;
        K_Nav_Pause(1);
    }

    move.x = way.x * DODGE_SPEED;
    move.y = way.y * DODGE_SPEED;
}

// What is at stake in an area for holding: the monsters inside it, those
// that could come out. Not those elsewhere that could see into it: waiting
// does not bring them (E1M8: a hold short of an empty area, 66 of danger
// from monsters away from it).
static double AreaRisk(int area)
{
    return K_Area_Get()->areas[area].inside;
}

// Where the route ahead goes into an area that is not safe, with HOLD_RISK
// at stake, within `reach` along it from me, no lift before it that is not
// down, through a narrow way - a door or lift, a narrow opening, not just on
// over open floor (K_Area_OpenFloor) - passing over area `except`, and with
// `only` not -1 over all others: the area, the point it goes in at, and that
// point's index; -1 if none. *shut tells whether a door before it is shut,
// or closing; one rising, or open at the top, is as good as open.
static int UnsafeAhead(k_vec2_t me, double reach, int except, int only, k_vec2_t *at,
                       int *point, boolean *shut)
{
    const k_map_t *map = K_Map_Get();
    const k_navstatus_t *nav = K_Nav_GetStatus();
    const k_path_t *path = K_Nav_GetPath();
    const k_areamap_t *areas = K_Area_Get();
    k_vec2_t from = me;
    double along = 0.0;
    int i, k, was = -1, crossed;
    boolean narrow = false;

    *shut = false;

    if (path == NULL || nav->state != K_NAV_DRIVING)
    {
        return -1;
    }

    crossed = SDL_max(nav->route_cell, 0);

    // Leg i runs from point i in route cell leg_cells[i].
    for (i = SDL_max(nav->target, 1); i < path->num_points - 1; ++i)
    {
        int r = path->leg_cells[i];
        int area = K_Area_Of(path->cells[r]);

        along += Distance(from, path->points[i]);
        from = path->points[i];

        if (along > reach)
        {
            return -1;
        }

        // The crossings on to it from the player's route cell: portal k
        // leads from route cell k to k + 1.
        for (k = crossed; k < r && k < path->num_cells - 1; ++k)
        {
            narrow |= !K_Area_OpenFloor(path->portals[k]);
        }

        crossed = SDL_max(crossed, r);

        if (area < 0 || r == was)
        {
            continue;
        }

        was = r;

        if (areas->areas[area].passage)
        {
            k_crossing_t crossing = r > 0
                                  ? K_Graph_CanCross(path->portals[r - 1], path->cells[r - 1])
                                  : K_CROSS_OPEN;

            if (crossing == K_CROSS_DOOR)
            {
                k_mover_t mover = K_Graph_SectorMover(
                    map->subsectors[map->cells[path->cells[r]].subsector].sector);

                *shut |= mover != K_MOVER_UP && mover != K_MOVER_WAITING;
            }
            else if (crossing != K_CROSS_OPEN)
            {
                return -1;
            }

            continue;
        }

        if (narrow && area != except && (only < 0 || area == only)
         && !K_Area_Safe(area) && AreaRisk(area) >= HOLD_RISK
         && !(area == hold_skip_area && leveltime < hold_skip_until))
        {
            *at = path->points[i];
            *point = i;
            return area;
        }
    }

    return -1;
}

// Whether the floor at a point hurts.
static boolean FloorHurts(k_vec2_t at)
{
    const k_map_t *map = K_Map_Get();
    int cell = K_Map_CellAt(at.x, at.y);

    return cell >= 0
        && K_Map_FloorDamage(map->subsectors[map->cells[cell].subsector].sector) > 0.0;
}

// Where to stand while holding: HOLD_BACK short of route point `point`
// along the route, further back out of a door's or lift's cells (up to
// HOLD_BACK_MOST); past the route's start, straight on back away from the
// way in, as far as the player could stand, in HOLD_STEP steps (Doom II
// MAP14: a route planned at the blue door had the spot 16 units off it).
// *ahead gets the first route point on from the spot. Returns false if the
// player could not stand there - or with `hurts` false, not without a floor
// that hurts.
static boolean HoldSpot(int point, double floor, boolean hurts, k_vec2_t *spot, int *ahead)
{
    const k_path_t *path = K_Nav_GetPath();
    const k_areamap_t *areas = K_Area_Get();
    k_vec2_t at = path->points[point];
    double left = HOLD_BACK, back = 0.0, dx, dy, length;
    int i = point;

    while (i > 0)
    {
        k_vec2_t before = path->points[i - 1];
        double length = Distance(at, before);
        int area;

        if (length < left)
        {
            left -= length;
            back += length;
            at = before;
            --i;
            continue;
        }

        at.x += (before.x - at.x) * left / SDL_max(length, 1e-6);
        at.y += (before.y - at.y) * left / SDL_max(length, 1e-6);
        back += left;
        area = K_Area_Of(K_Map_CellAt(at.x, at.y));

        if (area < 0 || !areas->areas[area].passage || back >= HOLD_BACK_MOST)
        {
            break;
        }

        left = 32.0;
    }

    dx = at.x - path->points[point].x;
    dy = at.y - path->points[point].y;
    length = sqrt(dx * dx + dy * dy);

    while (i == 0 && length > 1e-6 && back < HOLD_BACK_MOST)
    {
        int area = K_Area_Of(K_Map_CellAt(at.x, at.y));
        k_vec2_t next = { at.x + dx / length * HOLD_STEP, at.y + dy / length * HOLD_STEP };

        if ((left <= 0.0 && area >= 0 && !areas->areas[area].passage)
         || !Steppable(next, floor, hurts, EDGE_MARGIN))
        {
            break;
        }

        at = next;
        left -= HOLD_STEP;
        back += HOLD_STEP;
    }

    *spot = at;
    *ahead = i;
    return Steppable(at, floor, hurts, EDGE_MARGIN);
}

// Whether creeping stops short of route point k: the leg to it is no walk
// (a jump, a teleport), or crosses what no slow step should: a crossing
// that is not open but for a door (a jump, a crusher), or a walk-over line
// that does something (the user: creeping into a jump, and failing it;
// E2M4: crept over line 1113, which set the yellow key's crusher going,
// and caught under it).
static boolean CreepStops(const k_path_t *path, int k)
{
    int r;

    if (path->legs[k - 1] != K_LEG_WALK)
    {
        return true;
    }

    for (r = path->leg_cells[k - 1]; r < path->leg_cells[k] && r < path->num_cells - 1; ++r)
    {
        const k_portal_t *portal = &K_Graph_Get()->portals[path->portals[r]];
        k_crossing_t crossing = K_Graph_CanCross(path->portals[r], path->cells[r]);

        if ((crossing != K_CROSS_OPEN && crossing != K_CROSS_DOOR)
         || (portal->line >= 0 && K_Specials_WalkOver(K_Specials_LineSpecial(portal->line))))
        {
            return true;
        }
    }

    return false;
}

// The way creeping goes (creep_points): from the spot along the route from
// route point `ahead`, the first on from it, past route point `point` to
// CREEP_PAST beyond it, where it ends (returned) - round the corners the
// route turns there (E1M3: straight from the spot to the end, the spot
// crept through a corner's walls, the player stood against them, too far
// from it to creep on, until HOLD_MOST was up) - and CREEP_SHORT short of
// where it stops (CreepStops).
static k_vec2_t CreepWay(k_vec2_t spot, int ahead, int point)
{
    const k_path_t *path = K_Nav_GetPath();
    k_vec2_t at = spot;
    double past = -1.0;     // left past `point`, once there
    int k;

    creep_num_points = 0;
    creep_next = 0;

    for (k = ahead; k < path->num_points && creep_num_points < CREEP_POINTS; ++k)
    {
        k_vec2_t next = path->points[k];
        double length = Distance(at, next);

        if (k > 0 && CreepStops(path, k))
        {
            double back = SDL_min(CREEP_SHORT, length);

            next.x += (at.x - next.x) * back / SDL_max(length, 1e-6);
            next.y += (at.y - next.y) * back / SDL_max(length, 1e-6);
            creep_points[creep_num_points++] = next;
            break;
        }

        if (past >= 0.0 && length >= past)
        {
            at.x += (next.x - at.x) * past / SDL_max(length, 1e-6);
            at.y += (next.y - at.y) * past / SDL_max(length, 1e-6);
            creep_points[creep_num_points++] = at;
            break;
        }

        if (past >= 0.0)
        {
            past -= length;
        }

        at = next;
        creep_points[creep_num_points++] = at;

        if (k == point)
        {
            past = CREEP_PAST;
        }
    }

    return creep_num_points > 0 ? creep_points[creep_num_points - 1] : spot;
}

// What is left of the creeping: from the spot along its way to the end.
static double CreepLeft(void)
{
    k_vec2_t at = hold_spot;
    double left = 0.0;
    int i;

    for (i = creep_next; i < creep_num_points; ++i)
    {
        left += Distance(at, creep_points[i]);
        at = creep_points[i];
    }

    return left;
}

// Where the player looks while holding with nothing to shoot: the way in,
// the first point of the creeping's way CREEP_LOOK on from the spot - round
// a corner, the corner - else its end.
static k_vec2_t CreepLook(void)
{
    int i;

    for (i = creep_next; i < creep_num_points; ++i)
    {
        if (Distance(hold_spot, creep_points[i]) >= CREEP_LOOK)
        {
            return creep_points[i];
        }
    }

    return hold_end;
}

static void StopHold(const char *why)
{
    if (!holding)
    {
        return;
    }

    holding = false;
    status.holding = false;

    if (why != NULL)
    {
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "on into area %d after holding %.1f s - %s", hold_area,
              (double) (leveltime - hold_since) / TICRATE, why);
    }

    if (!battle && !dodging && !ducking)
    {
        move.x = move.y = 0.0;
        K_Nav_Pause(0);
    }
}

// Holding short of an area that is not safe (HOLD_AHEAD): begun, kept or
// over; and, out of a fight and dodging, back to the spot.
static void UpdateHold(const player_t *player)
{
    const k_areamap_t *areas = K_Area_Get();
    double floor = ToMap(player->mo->floorz);
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) }, at = me, spot;
    int here = K_Area_Of(K_Map_CellAt(me.x, me.y));
    int area = -1, point = -1, ahead = -1;
    boolean shut = false;

    // Begun from an area with no monsters inside, whatever those ahead see
    // of it, in a fight or not (Doom II MAP14: begun from a safe area only,
    // none began at a door with monsters behind it - shut, it was opened
    // first; opening, the nav was busy; open, they saw the player's area),
    // a door on the way pressed and rising, the player backing off as it
    // opens; kept whatever the monsters it let come make of it, wherever
    // the fight takes the player meanwhile - from the spot, which stands
    // further back than where the hold began, or creeps on (MAP02: a hold
    // begun 500 off the way in was over the next tic as more than
    // HOLD_AHEAD from the spot, and begun again, tic after tic) - its own
    // area judged only, not another before it on the way (Doom II MAP14: a
    // shotgun guy stepping to and fro over the border of the area before it
    // swapped holds between the two, 190 units apart, every few tics).
    // Never short of where a telefrag lands, nor begun invulnerable.
    if (holding && !escape)
    {
        area = UnsafeAhead(hold_spot, HOLD_AHEAD + HOLD_BACK_MOST + CREEP_PAST,
                           telefrag_on ? telefrag_area : -1, hold_area, &at, &point, &shut);
    }
    else if (!holding && !escape && !bold && here >= 0
          && (areas->areas[here].monsters == 0 || areas->areas[here].passage)
          && !dodging && K_Nav_CanPause() && !OnIsland(K_Map_CellAt(me.x, me.y))
          && (!K_Nav_Waiting() || K_Nav_GetStatus()->action == K_ACTION_DOOR))
    {
        area = UnsafeAhead(me, HOLD_START, telefrag_on ? telefrag_area : -1, -1, &at, &point,
                           &shut);
    }

    if (holding)
    {
        double left = CreepLeft();

        // A far target, or one that cannot come, shot at on the way, holds
        // no creeping up - once judged (SIGHTED_TICS); the player at the
        // spot (MAP04: the spot crept off before the player got to it).
        hold_quiet = !battle && Distance(me, hold_spot) < HOLD_AT
                  && (status.target < 0 || (sighted >= SIGHTED_TICS && !WorthFighting()))
                   ? hold_quiet + 1 : 0;

        // Coming no nearer to the spot, out of a fight (HOLD_STALL).
        if (battle || dodging || ducking || Distance(me, hold_spot) < HOLD_AT)
        {
            hold_nearest = 1.0e9;
            hold_stall = 0;
        }
        else if (Distance(me, hold_spot) < hold_nearest - 1.0)
        {
            hold_nearest = Distance(me, hold_spot);
            hold_stall = 0;
        }
        else
        {
            ++hold_stall;
        }

        if (area != hold_area)
        {
            StopHold(K_Area_Safe(hold_area) || AreaRisk(hold_area) < HOLD_RISK
                     ? "it is safe now" : escape ? escape_why : "the way changed");
        }
        else if (shut)
        {
            hold_shut_area = hold_area;
            hold_shut_since = hold_since;
            hold_shut_until = leveltime + HOLD_SKIP;
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "holding short of area %d, the door shut after %.1f s "
                  "- opening it again, to hold again", hold_area,
                  (double) (leveltime - hold_since) / TICRATE);
            StopHold(NULL);
        }
        else if ((left < 1.0 && hold_quiet >= CREEP_WAIT) || leveltime - hold_since >= HOLD_MOST
              || hold_stall >= HOLD_STALL)
        {
            hold_skip_area = hold_area;
            hold_skip_until = leveltime + HOLD_SKIP;
            StopHold(hold_stall >= HOLD_STALL ? "the spot is out of reach"
                     : left < 1.0 ? "crept in, nothing came, nothing worth fighting"
                     : "held long enough");
        }
        else if (hold_quiet >= CREEP_WAIT)
        {
            // Creeping on along its way, slowly - never onto a floor that
            // hurts: the creeping ends short of it, and the route crosses
            // it.
            double step = CREEP_SPEED;
            k_vec2_t next = hold_spot;
            int k = creep_next;

            while (k < creep_num_points && step > 0.0)
            {
                double length = Distance(next, creep_points[k]);

                if (length <= step)
                {
                    step -= length;
                    next = creep_points[k++];
                }
                else
                {
                    next.x += (creep_points[k].x - next.x) * step / length;
                    next.y += (creep_points[k].y - next.y) * step / length;
                    step = 0.0;
                }
            }

            if (!hold_crept)
            {
                hold_crept = true;
                K_Log(K_LOG_COMBAT, K_LOG_INFO, "nothing worth fighting for %.1f s - creeping on "
                      "into area %d", (double) CREEP_WAIT / TICRATE, hold_area);
            }

            if (FloorHurts(next))
            {
                hold_end = hold_spot;
                creep_next = creep_num_points;
            }
            else
            {
                hold_spot = next;
                creep_next = k;
            }
        }
    }
    else if (area >= 0 && !shut && HoldSpot(point, floor, false, &spot, &ahead))
    {
        holding = true;
        status.holding = true;
        hold_area = area;
        hold_spot = spot;
        hold_end = CreepWay(spot, ahead, point);
        hold_since = area == hold_shut_area && leveltime < hold_shut_until
                   ? hold_shut_since : leveltime;
        hold_quiet = 0;
        hold_crept = false;
        hold_nearest = 1.0e9;
        hold_stall = 0;
        hold_shut_area = -1;
        K_Nav_Pause(1);
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "holding at (%.0f,%.0f), short of area %d - %d monsters "
              "inside (%d awake), danger %.0f: letting them come", spot.x, spot.y, area,
              areas->areas[area].monsters, areas->areas[area].awake, areas->areas[area].danger);
    }

    status.hold_area = holding ? hold_area : -1;
    status.hold_spot = hold_spot;
    status.hold_end = hold_end;
    status.creeping = holding && hold_quiet >= CREEP_WAIT && CreepLeft() >= 1.0;

    if (holding && !battle && !dodging)
    {
        double dx = hold_spot.x - me.x, dy = hold_spot.y - me.y;
        double distance = sqrt(dx * dx + dy * dy);
        double speed = SDL_min(HOLD_SPEED, distance * HOLD_GAIN);

        move.x = distance > HOLD_NEAR ? dx / distance * speed : 0.0;
        move.y = distance > HOLD_NEAR ? dy / distance * speed : 0.0;
    }
}

// What a duck is out of the sight of (DUCK_REPLAN): a monster winding up
// its blow, or a homing missile.
typedef struct
{
    const mobj_t *mo;
    const char *name;       // "arch-vile", "revenant missile"
    const char *blow;       // "flame"
    k_vec2_t at;
    double eye;
    int tics;               // until its blow lands (or might)
    boolean missile;
    k_vec2_t toward;        // a missile: it turns after the player, here
    double speed;           // at this speed
} duckfrom_t;

// Tics until the monster's states next run `action`, from where it is now
// (the current state's action has run already); -1 if not within its next
// 32 states, or past one that lasts for ever.
static int TicsUntil(const mobj_t *mo, actionf_v action)
{
    int tics = mo->tics, steps;
    int s = mo->state->nextstate;

    for (steps = 0; steps < 32 && s > S_NULL && tics >= 0; ++steps)
    {
        if (states[s].action.acv == action)
        {
            return tics;
        }

        if (states[s].tics < 0)
        {
            return -1;
        }

        tics += states[s].tics;
        s = states[s].nextstate;
    }

    return -1;
}

// The most pressing blow to duck from, if any: a monster in sight winding
// up a blow at the player, a stream going on (it may land once more before
// the monster loses sight: DUCK_STREAM to get away), or a homing missile
// coming within DUCK_MISSILE_REACH.
static boolean DuckThreat(const player_t *player, duckfrom_t *from)
{
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    boolean found = false;
    int i;

    for (i = 0; i < num_monsters; ++i)
    {
        const mobj_t *mo = monster_mobjs[i];
        boolean stream = mo->type != MT_VILE;
        int tics;

        // As the monster sees (A_VileAttack, A_SpidRefire), not as the
        // player does (Doom II MAP14: riding a lift, the player did not see
        // the vile that saw it, and took the flame unducked).
        if (mo->target != player->mo || mo->state == NULL || !attack_states[mo->state - states]
         || (mo->type != MT_VILE && mo->type != MT_BABY && mo->type != MT_SPIDER)
         || monsters[i].distance > SIGHT_REACH || !K_Sight_Things(mo, player->mo))
        {
            continue;
        }

        tics = TicsUntil(mo, mo->type == MT_VILE ? (actionf_v) A_VileAttack
                           : mo->type == MT_BABY ? (actionf_v) A_BspiAttack
                           : (actionf_v) A_SPosAttack);

        if (tics < 0)
        {
            continue;
        }

        if (stream && mo->state != &states[mo->info->missilestate])
        {
            tics = SDL_max(tics, DUCK_STREAM);
        }

        if (!found || tics < from->tics)
        {
            found = true;
            from->mo = mo;
            from->name = monsters[i].name;
            from->blow = mo->type == MT_VILE ? "flame" : mo->type == MT_BABY ? "plasma"
                       : "bullets";
            from->at = monsters[i].at;
            from->eye = monsters[i].z + monsters[i].height * 0.75;
            from->tics = tics;
            from->missile = false;
        }
    }

    for (i = 0; i < num_projectiles; ++i)
    {
        const k_projectile_t *projectile = &projectiles[i];
        double dx = me.x - projectile->at.x, dy = me.y - projectile->at.y;
        double distance = sqrt(dx * dx + dy * dy);
        double speed = sqrt(projectile->velocity.x * projectile->velocity.x
                            + projectile->velocity.y * projectile->velocity.y);
        int tics;

        if (!K_Threat_Homes(projectile_mobjs[i], player->mo) || distance > DUCK_MISSILE_REACH
         || speed < 1e-6 || dx * projectile->velocity.x + dy * projectile->velocity.y <= 0.0)
        {
            continue;
        }

        tics = (int) (distance / speed);

        if (!found || tics < from->tics)
        {
            found = true;
            from->mo = projectile_mobjs[i];
            from->name = projectile->name;
            from->blow = "hit";
            from->at = projectile->at;
            from->eye = ToMap(projectile_mobjs[i]->z) + ToMap(projectile_mobjs[i]->height) * 0.5;
            from->tics = tics;
            from->missile = true;
            from->toward = me;
            from->speed = speed;
        }
    }

    return found;
}

// How far the player gets in `tics` from standing, strafing (DUCK_THRUST,
// DUCK_FRICTION), up to DUCK_TRAVEL_TICS.
static double TravelDistance(int tics)
{
    static double covered[DUCK_TRAVEL_TICS + 1];
    static boolean made;

    if (!made)
    {
        double speed = 0.0;
        int t;

        for (t = 1; t <= DUCK_TRAVEL_TICS; ++t)
        {
            speed = speed * DUCK_FRICTION + DUCK_THRUST;
            covered[t] = covered[t - 1] + speed;
        }

        made = true;
    }

    return covered[SDL_max(0, SDL_min(tics, DUCK_TRAVEL_TICS))];
}

// The tics until the player stands `distance` off by way of `corners`
// corners: the commands already sent, then getting there from standing.
static int TravelTics(double distance, int corners)
{
    int t = 0;

    while (t < DUCK_TRAVEL_TICS && TravelDistance(t) < distance)
    {
        ++t;
    }

    return t + DUCK_LATENCY + DUCK_CORNER * corners;
}

// Whether a spot on the floor at `floor` is out of its sight `reach` tics
// on - a missile's from where it will be then, on its way to where the
// player is now, short of it (Doom II MAP06: judged from where it would
// fly on straight, no cover was ever found from a missile turning after
// the player in the open).
static boolean DuckHidden(const duckfrom_t *from, k_vec2_t at, double floor, int reach)
{
    k_vec2_t eye = from->at;

    if (from->missile)
    {
        double dx = from->toward.x - from->at.x, dy = from->toward.y - from->at.y;
        double distance = sqrt(dx * dx + dy * dy);
        double flown = SDL_min(from->speed * reach, SDL_max(distance - 2.0 * PLAYER_RADIUS, 0.0));

        eye.x += dx / SDL_max(distance, 1e-6) * flown;
        eye.y += dy / SDL_max(distance, 1e-6) * flown;
    }

    return !K_Sight_Check(eye.x, eye.y, from->eye, at.x, at.y, floor, floor + PLAYER_HEIGHT);
}

// With no spot along the rays (DUCK_GRAPH_REACH): the nearest cell whose
// middle is out of its sight, walked to in time through open crossings,
// none on a floor that hurts unless the player stands on one; the way
// there (the portals' middles, the cell's middle last), and when.
static boolean CoverByCells(const player_t *player, const duckfrom_t *from, k_vec2_t *points,
                            int *num_points, int *spot_tics)
{
    const k_map_t *map = K_Map_Get();
    const k_graph_t *graph = K_Graph_Get();
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    boolean hurts = K_Map_FloorDamage((int) (player->mo->subsector->sector - sectors)) > 0.0;
    double reach = SDL_min(DUCK_GRAPH_REACH, TravelDistance(from->tics - DUCK_LATENCY));
    int start = K_Map_CellAt(me.x, me.y);
    int num_open = 0, best = -1, best_tics = 0, cell, k, i;
    double best_score = 1e30;

    if (start < 0 || reach <= 0.0 || graph->generation != map->generation)
    {
        return false;
    }

    GrowCover(map->num_cells);

    for (i = 0; i < map->num_cells; ++i)
    {
        cover_distance[i] = 1e30;
    }

    cover_distance[start] = 0.0;
    cover_entry[start] = me;
    cover_from[start] = -1;
    cover_hops[start] = 0;
    cover_open[num_open++] = start;

    while (num_open > 0)
    {
        int nearest = 0;
        const k_cell_t *c;
        double length;

        for (i = 1; i < num_open; ++i)
        {
            if (cover_distance[cover_open[i]] < cover_distance[cover_open[nearest]])
            {
                nearest = i;
            }
        }

        cell = cover_open[nearest];
        cover_open[nearest] = cover_open[--num_open];

        if (cover_distance[cell] > reach)
        {
            break;
        }

        c = &map->cells[cell];
        length = cover_distance[cell] + Distance(cover_entry[cell], c->centroid);

        if (cell != start && length <= reach)
        {
            double floor = K_Map_CellFloor(cell);
            int tics = TravelTics(length, cover_hops[cell]);

            if (tics <= from->tics && Steppable(c->centroid, floor, hurts, EDGE_MARGIN)
             && DuckHidden(from, c->centroid, floor, tics))
            {
                double score = length + DUCK_SEEN * Exposure(c->centroid, floor);

                if (score < best_score)
                {
                    best_score = score;
                    best = cell;
                    best_tics = tics;
                }
            }
        }

        for (k = 0; k < graph->link_count[cell]; ++k)
        {
            const k_link_t *link = &graph->links[graph->first_link[cell] + k];
            const k_portal_t *portal = &graph->portals[link->portal];
            int next = link->neighbor;
            k_vec2_t middle = { (portal->a.x + portal->b.x) * 0.5, (portal->a.y + portal->b.y) * 0.5 };
            double distance = cover_distance[cell] + Distance(cover_entry[cell], middle);

            if (distance >= cover_distance[next] || portal->jump >= 0
             || K_Graph_CanCross(link->portal, cell) != K_CROSS_OPEN
             || (!hurts && K_Map_FloorDamage(map->subsectors[map->cells[next].subsector].sector)
                           > 0.0))
            {
                continue;
            }

            if (cover_distance[next] >= 1e30)
            {
                cover_open[num_open++] = next;
            }

            cover_distance[next] = distance;
            cover_entry[next] = middle;
            cover_from[next] = cell;
            cover_hops[next] = cover_hops[cell] + 1;
        }
    }

    if (best < 0)
    {
        return false;
    }

    // The way back from the cell to the player's, then turned round.
    *num_points = 0;
    points[(*num_points)++] = map->cells[best].centroid;

    for (cell = best; cover_from[cell] >= 0 && *num_points < DUCK_POINTS; cell = cover_from[cell])
    {
        points[(*num_points)++] = cover_entry[cell];
    }

    for (i = 0; i < *num_points / 2; ++i)
    {
        k_vec2_t swap = points[i];

        points[i] = points[*num_points - 1 - i];
        points[*num_points - 1 - i] = swap;
    }

    *spot_tics = best_tics;
    return true;
}

// The nearest spot out of its sight the player gets to in time
// (DUCK_REPLAN), the way there (the spot last), and when. Along each ray
// the first spot out of sight, a step or two deeper if still out of sight
// and in time; with none, by way of the cells (CoverByCells).
static boolean FindDuckSpot(const player_t *player, const duckfrom_t *from, k_vec2_t *points,
                            int *num_points, int *spot_tics)
{
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    double floor = ToMap(player->mo->floorz);
    boolean hurts = K_Map_FloorDamage((int) (player->mo->subsector->sector - sectors)) > 0.0;
    double face = atan2(from->at.y - me.y, from->at.x - me.x);
    double best = 1e30;
    int k, step;

    for (k = 0; k < MOVE_WAYS; ++k)
    {
        double angle = k * 2.0 * K_PI / MOVE_WAYS;
        double ux = cos(angle), uy = sin(angle);
        double step_floor = floor;
        int steps = (int) (DUCK_REACH / DUCK_STEP), ring = (int) (DUCK_RING / DUCK_STEP);
        int found = -1, found_tics = 0;
        double found_floor = floor;

        for (step = 1; step <= steps; ++step)
        {
            k_vec2_t at = { me.x + ux * DUCK_STEP * step, me.y + uy * DUCK_STEP * step };
            int cell = K_Map_CellAt(at.x, at.y);
            int tics = TravelTics(DUCK_STEP * step, 0);

            if (cell < 0 || tics > from->tics || !Steppable(at, step_floor, hurts, EDGE_MARGIN))
            {
                break;
            }

            step_floor = K_Map_CellFloor(cell);

            // Deeper in cover, a step or two on.
            if (found >= 0)
            {
                if (step > found + 2 || !DuckHidden(from, at, step_floor, tics))
                {
                    break;
                }

                found = step;
                found_tics = tics;
                found_floor = step_floor;
                continue;
            }

            if (step % ring == 0 && DuckHidden(from, at, step_floor, tics))
            {
                found = step;
                found_tics = tics;
                found_floor = step_floor;
            }
        }

        if (found >= 0)
        {
            k_vec2_t at = { me.x + ux * DUCK_STEP * found, me.y + uy * DUCK_STEP * found };
            double score = DUCK_STEP * found
                         + DUCK_HIT * MoveCost(me, ux * DUCK_SPEED, uy * DUCK_SPEED, &face, floor,
                                               hurts, EDGE_MARGIN, NULL)
                         + DUCK_SEEN * Exposure(at, found_floor);

            if (score < best)
            {
                best = score;
                points[0] = at;
                *num_points = 1;
                *spot_tics = found_tics;
            }
        }
    }

    return best < 1e30 || CoverByCells(player, from, points, num_points, spot_tics);
}

static void StopDuck(const char *why)
{
    if (!ducking)
    {
        return;
    }

    ducking = false;
    status.ducking = false;
    K_Log(K_LOG_COMBAT, K_LOG_INFO, "out of the %s's sight for %.1f s - %s", duck_what,
          (double) (leveltime - duck_since) / TICRATE, why);

    if (!battle && !dodging && !holding)
    {
        move.x = move.y = 0.0;
        K_Nav_Pause(0);
    }
}

// Ducking out of sight of a blow coming (DUCK_REPLAN): the spot looked for,
// begun, kept, moved (a new blow, or the spot seen now), or over; and the
// step along the way there.
static void UpdateDuck(const player_t *player)
{
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    k_vec2_t points[DUCK_POINTS];
    duckfrom_t from;
    boolean coming = DuckThreat(player, &from);
    int num_points, tics;

    if (!coming)
    {
        duck_none_from = NULL;
        plan_from = NULL;
    }

    if (ducking)
    {
        k_vec2_t spot = duck_points[duck_num_points - 1];
        int cell = K_Map_CellAt(spot.x, spot.y);
        double spot_floor = cell >= 0 ? K_Map_CellFloor(cell) : ToMap(player->mo->floorz);

        duck_calm = coming ? 0 : duck_calm + 1;

        if (duck_calm >= DUCK_LINGER)
        {
            StopDuck("the blow is past");
        }
        else if (leveltime - duck_since >= DUCK_MOST)
        {
            StopDuck("ducked long enough");
        }
        else if (coming && (from.mo != duck_from || !DuckHidden(&from, spot, spot_floor, 0))
              && FindDuckSpot(player, &from, points, &num_points, &tics))
        {
            memcpy(duck_points, points, num_points * sizeof(*points));
            duck_num_points = num_points;
            duck_next = 0;
            duck_from = from.mo;
            M_StringCopy(duck_what, from.name, sizeof(duck_what));
        }
    }
    else if (coming && (K_Nav_CanPause() || battle || holding))
    {
        if (from.mo != plan_from || leveltime - plan_tic >= DUCK_REPLAN)
        {
            plan_from = from.mo;
            plan_tic = leveltime;
            plan_found = FindDuckSpot(player, &from, points, &num_points, &plan_tics);
        }

        // Shooting on till the time left is the time to get there; then
        // from here (the fight has moved the player since).
        if (plan_found && from.tics <= plan_tics + DUCK_SLACK
         && FindDuckSpot(player, &from, points, &num_points, &tics))
        {
            k_vec2_t spot = points[num_points - 1];

            ducking = true;
            memcpy(duck_points, points, num_points * sizeof(*points));
            duck_num_points = num_points;
            duck_next = 0;
            duck_from = from.mo;
            duck_since = leveltime;
            duck_calm = 0;
            plan_from = NULL;
            M_StringCopy(duck_what, from.name, sizeof(duck_what));
            K_Nav_Pause(1);
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "ducking out of the %s's sight (%s in %d tics), %.0f "
                  "units to (%.0f,%.0f)%s", from.name, from.blow, from.tics, Distance(me, spot),
                  spot.x, spot.y, num_points > 1 ? ", round a corner" : "");
        }
        else if (!plan_found && from.tics <= DUCK_LATENCY + DUCK_SLACK
              && from.mo != duck_none_from)
        {
            duck_none_from = from.mo;
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "no cover from the %s in reach (%s in %d tics)",
                  from.name, from.blow, from.tics);
        }
    }

    status.ducking = ducking;

    if (ducking)
    {
        status.duck_spot = duck_points[duck_num_points - 1];
    }

    if (ducking)
    {
        k_vec2_t to;
        double dx, dy, distance, speed;

        while (duck_next < duck_num_points - 1
            && Distance(me, duck_points[duck_next]) < DUCK_WAYPOINT)
        {
            ++duck_next;
        }

        to = duck_points[duck_next];
        dx = to.x - me.x;
        dy = to.y - me.y;
        distance = sqrt(dx * dx + dy * dy);
        speed = duck_next < duck_num_points - 1 ? DUCK_SPEED
              : SDL_min(DUCK_SPEED, distance * DUCK_GAIN);
        move.x = distance > DUCK_NEAR ? dx / distance * speed : 0.0;
        move.y = distance > DUCK_NEAR ? dy / distance * speed : 0.0;
    }
}

void K_Combat_Reset(void)
{
    int i;

    MarkAttackStates();
    num_monsters = 0;
    num_projectiles = 0;
    num_was_alive = 0;
    memset(&status, 0, sizeof(status));
    memset(ignored, 0, sizeof(ignored));
    status.target = -1;
    target_mo = NULL;
    lost_mo = NULL;
    lost_tic = 0;
    unclear_tic = 0;
    wanted = -1;
    can_hurt = false;
    last_switch = -SWITCH_TICS;
    last_health = -1;
    target_health = 0;
    progress_tic = 0;
    sighted = 0;
    misses = 0;
    last_ammo_type = am_noammo;
    battle = false;
    calm = 0;
    strafe_dir = 1;
    strafe_tics = 0;
    move.x = move.y = 0.0;
    pressed = false;
    pressed_mo = NULL;
    presser = -1;
    press = 0.0;
    retreating = false;
    retreat_hide = false;
    seen_tics = 0;
    hard_pressed = 0;
    retreat_again = 0;
    num_trips = 0;
    ahead_area = -1;
    escape = false;
    reserve = false;
    memset(beats, 0, sizeof(beats));
    needed = -1;
    fight_needed = false;

    for (i = 0; i < MAX_HOPELESS; ++i)
    {
        hopeless_logged[i] = NULL;
    }

    for (i = 0; i < MAX_INFIGHT; ++i)
    {
        infight_logged[i] = NULL;
    }

    bold = false;
    hard_seen = -HARD_LINGER - 1;
    bfg_pending = false;
    bfg_spraying = false;
    bfg_mo = NULL;
    bfg_logged = NULL;
    telefrag_on = false;
    telefrag_area = -1;
    avoid = -1;
    avoid_mo = NULL;
    avoid_tics = 0;
    avoid_seen = 0;
    passed_mo = NULL;
    passed_until = 0;
    num_barrels = 0;
    DropBarrel();
    blocking_wanted = false;
    dodging = false;
    dodging_blow = false;
    hurt_tic = -BLOW_HOLD - 1;
    memset(reach_memo, 0, sizeof(reach_memo));
    comfy = false;
    out_shots = 0;
    dodging_primed = false;
    primed_mo = NULL;
    primed_tics = 0;
    shot_mo = NULL;
    shot_tic = -1;
    dodging_missile = false;
    missile_way = 0;
    missile_on_way = false;
    battle_way = -1;
    holding = false;
    hold_skip_area = -1;
    hold_skip_until = 0;
    hold_shut_area = -1;
    status.hold_area = -1;
    ducking = false;
    duck_from = NULL;
    duck_none_from = NULL;
    plan_from = NULL;
    num_exposers = 0;
    K_Threat_Reset();
}

void K_Combat_Tick(int driving)
{
    const player_t *player = &players[consoleplayer];
    char name[64];

    if (player->mo == NULL)
    {
        return;
    }

    Collect(player);
    LogHits(player);
    status.kills = player->killcount;

    // The list was made anew: the target's place in it, if it is still in.
    status.target = IndexOf(target_mo);

    if (status.target < 0)
    {
        target_mo = NULL;
    }

    UpdateHopeless(player);
    UpdateInfighting();
    UpdateHard(player);

    if (bfg_spraying)
    {
        LogSpray(player);
    }

    if (player->playerstate == PST_DEAD || player->health <= 0)
    {
        battle = false;
        status.battle = false;
        holding = false;
        status.holding = false;
        status.hold_area = -1;
        status.creeping = false;
        ducking = false;
        status.ducking = false;
        retreating = false;

        // Not when the death ended the level (E1M8's exit floor).
        if (!status.died && gameaction != ga_completed)
        {
            AttackerName(player, name, sizeof(name));
            K_Log(K_LOG_COMBAT, K_LOG_WARN, "died at (%.0f,%.0f), %.1f s into the level - "
                  "killed by %s; %d kills of %d", ToMap(player->mo->x), ToMap(player->mo->y),
                  (double) leveltime / TICRATE, name, player->killcount, totalkills);
            K_Combat_LogSummary();
        }

        status.died = true;
        status.target = -1;
        target_mo = NULL;
        wanted = -1;
        status.weapon = NULL;
        DropBarrel();
        status.barrel = false;
        M_StringCopy(status.text, "dead", sizeof(status.text));
        return;
    }

    // Not driving (a menu, the pause): everything stays as it is.
    if (driving)
    {
        UpdateClear();
        UpdateComfy(player);
        UpdateReserve(player);
        ChooseTarget(player);
        CountMisses(player);
        ChooseWeapon(player);
        ChooseBarrel(player);
        UpdateExposers();
        UpdateAvoid();
        UpdateBattle(player);
        UpdateRetreat(player);
        UpdateDodge(player);
        UpdateHold(player);
        UpdateDuck(player);
    }
    else
    {
        barrel_target = -1;
        avoid = -1;
    }

    status.barrel = barrel_target >= 0;

    if (ducking)
    {
        M_snprintf(status.text, sizeof(status.text), "ducking out of the %s's sight%s%s",
                   duck_what, status.target >= 0 ? ", shooting at the " : "",
                   status.target >= 0 ? monsters[status.target].name : "");
    }
    else if (barrel_target >= 0)
    {
        M_snprintf(status.text, sizeof(status.text), "shooting the barrel at (%.0f,%.0f) - %s",
                   status.barrel_at.x, status.barrel_at.y, barrel_reason);
    }
    else if (status.target >= 0)
    {
        M_snprintf(status.text, sizeof(status.text), "%s%s the %s, %.0f units - %s%s",
                   battle ? "fighting" : escape ? escape_why : "shooting on the way at",
                   escape && !battle ? ", shooting at" : "",
                   monsters[status.target].name, monsters[status.target].distance,
                   status.weapon != NULL ? status.weapon : "no weapon for it",
                   battle && retreating ? ", retreating"
                   : battle && pressed ? ", backing off" : "");
    }
    else if (battle && avoid >= 0)
    {
        M_snprintf(status.text, sizeof(status.text), "keeping off the %s, %.0f units - beyond "
                   "what the player holds%s", monsters[avoid].name, monsters[avoid].distance,
                   retreating ? ", hiding" : "");
    }
    else if (battle)
    {
        M_StringCopy(status.text, "fighting - nothing in sight for now", sizeof(status.text));
    }
    else if (holding)
    {
        M_snprintf(status.text, sizeof(status.text), "holding short of area %d, %d monsters "
                   "inside - %s", hold_area, K_Area_Get()->areas[hold_area].monsters,
                   status.creeping ? "creeping in" : "letting them come");
    }
    else if (status.visible > 0)
    {
        M_snprintf(status.text, sizeof(status.text), "%d in sight, none to shoot",
                   status.visible);
    }
    else
    {
        M_StringCopy(status.text, "nothing in sight", sizeof(status.text));
    }
}

// Where the target will be when game tic `tic` runs.
static k_vec2_t TargetAt(int tic)
{
    const k_monster_t *monster = &monsters[status.target];
    int lead = SDL_max(tic - gametic, 0);
    k_vec2_t at = { monster->at.x + target_velocity.x * lead,
                    monster->at.y + target_velocity.y * lead };

    return at;
}

// A press read whose shot has not left yet, at what is no longer aimed at:
// the aim holds on it until the shot leaves (the user, E1M7: the bot
// pressed fire at a barrel and dropped it the next tic, and the shotgun's
// pellets left three tics later along the route) - on where it was last
// seen once it is dead or gone (E1M9: the target killed by the rocket
// before, the aim swung 33 degrees in a rocket's wind-up, and the rocket
// left into a wall 93 off).
static boolean HeldAim(int tic, k_vec2_t *at)
{
    int i;

    if (shot_mo == NULL || tic > shot_tic
     || (barrel_target >= 0 ? barrels[barrel_target].mo == shot_mo : shot_mo == target_mo))
    {
        return false;
    }

    i = IndexOf(shot_mo);

    if (i >= 0)
    {
        shot_at = monsters[i].at;
    }

    for (i = 0; i < num_barrels; ++i)
    {
        if (barrels[i].mo == shot_mo && barrels[i].live)
        {
            shot_at = barrels[i].at;
        }
    }

    *at = shot_at;
    return true;
}

int K_Combat_Aim(int tic, double *angle)
{
    k_pose_t pose;
    k_vec2_t at;
    boolean held = HeldAim(tic, &at);

    // Holding with nothing to shoot: watching the way in, and past it. In a
    // standoff: facing the monster kept off.
    if (!held && status.target < 0 && barrel_target < 0 && (holding || (battle && avoid >= 0))
     && K_Nav_Predict(tic, &pose))
    {
        at = battle && avoid >= 0 ? monsters[avoid].at : CreepLook();
        *angle = atan2(at.y - pose.y, at.x - pose.x);
        return 1;
    }

    if ((!held && status.target < 0 && barrel_target < 0) || !K_Nav_Predict(tic, &pose))
    {
        return 0;
    }

    if (!held)
    {
        at = barrel_target >= 0 ? barrels[barrel_target].at : TargetAt(tic);
    }

    if (!held && barrel_target < 0 && K_Salt_Enabled())
    {
        double dx = at.x - pose.x, dy = at.y - pose.y;
        double length = sqrt(dx * dx + dy * dy);
        double off = K_Salt_Unit((unsigned int) (tic / AIM_SALT_TICS)) * AIM_SALT
                   * monsters[status.target].radius;

        if (length > 1.0)
        {
            at.x -= dy / length * off;
            at.y += dx / length * off;
        }
    }

    *angle = atan2(at.y - pose.y, at.x - pose.x);
    return 1;
}

// Up, and in its ready state, where pressing fire fires (A_WeaponReady).
static boolean WeaponReady(const player_t *player)
{
    return player->pendingweapon == wp_nochange
        && player->psprites[ps_weapon].state
           == &states[weaponinfo[player->readyweapon].readystate];
}

// Whether a shot along the facing would get to the target's front without
// meeting a wall: it goes where the player faces, not to the target's
// centre, and can graze a corner the centre is clear of.
static boolean LineOfFire(const player_t *player, k_vec2_t from, double facing,
                          const k_monster_t *monster)
{
    double distance = Distance(from, monster->at);
    double reach = SDL_max(distance - monster->radius, distance * 0.5);

    return K_Sight_Aim(from.x, from.y, GunHeight(player->mo), from.x + cos(facing) * reach,
                       from.y + sin(facing) * reach, monster->z, monster->z + monster->height);
}

// Whether a BFG ball fired along the facing would get to the target's front:
// no wall within its radius either side of its path, at the height it flies
// climbing to the target's middle (as RocketClear; BFG_RADIUS).
static boolean BallClear(const player_t *player, k_vec2_t from, double facing,
                         const k_monster_t *monster)
{
    double ux = cos(facing), uy = sin(facing);
    double distance = Distance(from, monster->at);
    double reach = SDL_max(distance - monster->radius - BFG_RADIUS, distance * 0.5);
    double gun = GunHeight(player->mo);
    double z = gun + (monster->z + monster->height * 0.5 - gun) * reach / SDL_max(distance, 1.0);
    int side;

    for (side = -1; side <= 1; side += 2)
    {
        double ox = -uy * BFG_RADIUS * side, oy = ux * BFG_RADIUS * side;

        if (!K_Sight_Aim(from.x + ox, from.y + oy, gun, from.x + ox + ux * reach,
                         from.y + oy + uy * reach, z - 4.0, z + 4.0))
        {
            return false;
        }
    }

    return true;
}

// Whether a rocket fired along the facing at what lies `distance` off (of
// the radius, its middle at height `middle`; the target, or NULL) would
// burst ROCKET_SAFE off the player as the blast measures it (the larger of
// the x and y distances): `burst` along, or at another monster it would meet
// first; with no wall within its radius either side of its path, as far as
// a burst there would still be too near - each nearer by the player's own
// run after it (RocketGain). The path is looked at climbing to the middle,
// as the engine aims it, and flat from where it leaves the player: with
// nothing found along the facing to aim at, the engine fires it level
// (P_SpawnPlayerMissile; E1M9: at an imp up on a ledge, level into the
// ledge's face 120 off, the player running after it).
static boolean RocketClear(const player_t *player, k_vec2_t from, double facing,
                           double distance, double radius, double burst, double middle,
                           const k_monster_t *target)
{
    double ux = cos(facing), uy = sin(facing);
    double axis = SDL_max(fabs(ux), fabs(uy));
    double gun = GunHeight(player->mo);
    double spawn = ToMap(player->mo->z) + MISSILE_SPAWN;
    double gain = RocketGain(player, ux, uy);
    double reach = SDL_min(SDL_max(distance - radius, distance * 0.5),
                           ROCKET_SAFE / (axis * gain) + ROCKET_RADIUS);
    int i, side;

    burst *= gain;

    for (i = 0; i < num_monsters; ++i)
    {
        const k_monster_t *monster = &monsters[i];
        double along = (monster->at.x - from.x) * ux + (monster->at.y - from.y) * uy;
        double across = fabs((monster->at.y - from.y) * ux - (monster->at.x - from.x) * uy);

        if (monster != target && along > 0.0 && along < distance
         && across < monster->radius + ROCKET_RADIUS)
        {
            burst = SDL_min(burst, (along - monster->radius - ROCKET_RADIUS) * gain);
        }
    }

    if (burst * axis < ROCKET_SAFE)
    {
        return false;
    }

    // Its edges, at the height it flies there.
    for (side = -1; side <= 1; side += 2)
    {
        double ox = -uy * ROCKET_RADIUS * side, oy = ux * ROCKET_RADIUS * side;
        double z = gun + (middle - gun) * reach / SDL_max(distance, 1.0);

        if (!K_Sight_Aim(from.x + ox, from.y + oy, gun, from.x + ox + ux * reach,
                         from.y + oy + uy * reach, z - 4.0, z + 4.0)
         || !K_Sight_Aim(from.x + ox, from.y + oy, spawn + 4.0, from.x + ox + ux * reach,
                         from.y + oy + uy * reach, spawn, spawn + 8.0))
        {
            return false;
        }
    }

    return true;
}

// RocketClear at the target: bursting at its front, nearer by what it closes
// in during the flight. And no lost soul within SOUL_CLEAR.
static boolean RocketSafe(const player_t *player, k_vec2_t from, double facing,
                          const k_monster_t *target, double distance)
{
    double closing = -(target_velocity.x * (target->at.x - from.x)
                     + target_velocity.y * (target->at.y - from.y)) / SDL_max(distance, 1.0);

    return !SoulNear()
        && RocketClear(player, from, facing, distance, target->radius,
                       distance - target->radius - ROCKET_RADIUS
                       - SDL_max(0.0, closing) * distance / ROCKET_SPEED,
                       target->z + target->height * 0.5, target);
}

// Whether a shot along the facing could set off a barrel whose chain would
// hurt the player: one the shot could graze - within the weapon's spread
// (none when accurate), and a margin, of the facing, in reach of the shot -
// before the target or behind it, where a miss flies on to. A rocket also
// sets off barrels near where it lands (impact), and grazes them with its
// radius.
static boolean BarrelAtRisk(const player_t *player, k_vec2_t from, double facing,
                            const weapon_t *weapon, k_vec2_t impact, boolean accurate)
{
    double gun = GunHeight(player->mo);
    int i;

    for (i = 0; i < num_barrels; ++i)
    {
        const barrel_t *barrel = &barrels[i];
        double distance, spread;

        if (!barrel->live || !chain_near[barrel->chain])
        {
            continue;
        }

        if (weapon->weapon == wp_missile && AxisDistance(barrel->at, impact) < BARREL_CHAIN)
        {
            return true;
        }

        distance = SDL_max(Distance(from, barrel->at), 1.0);
        spread = (accurate ? 0.0 : weapon->spread * K_PI / 180.0)
               + atan2(BARREL_RADIUS + BARREL_MARGIN
                       + (weapon->weapon == wp_missile ? ROCKET_RADIUS : 0.0), distance);

        if (fabs(Wrap(atan2(barrel->at.y - from.y, barrel->at.x - from.x) - facing)) < spread
         && K_Sight_Aim(from.x, from.y, gun, barrel->at.x, barrel->at.y, barrel->z,
                        barrel->z + BARREL_HEIGHT))
        {
            return true;
        }
    }

    return false;
}

// Where a shot pressed for now leaves from, and which way: the weapon's lead
// tics later (A_FireShotgun three tics after the press), the player running
// on as fast, the aim on what it is fired at, there by then (the user, E1M7:
// running past a barrel 40 units off, the bot pressed fire at a zombieman
// with the barrel clear of the shotgun's spread; three tics and 20 units on
// the pellets left with the barrel in it, and its blast took 76).
static void ShotLeaves(const k_pose_t *pose, const weapon_t *weapon, k_vec2_t at,
                       k_vec2_t *from, double *facing)
{
    from->x = pose->x + pose->momx * weapon->lead;
    from->y = pose->y + pose->momy * weapon->lead;
    *facing = atan2(at.y - from->y, at.x - from->x);
}

// Whether a barrel of the chain stands within BLAST_SAFE of the spot.
static boolean ChainNearAt(int chain, k_vec2_t at)
{
    int i;

    for (i = 0; i < num_barrels; ++i)
    {
        if (barrels[i].live && barrels[i].chain == chain
         && AxisDistance(at, barrels[i].at) < BLAST_SAFE)
        {
            return true;
        }
    }

    return false;
}

// The press is read (the weapon is ready for it): the aim holds on what it
// is fired at until the shot leaves (HeldAim).
static void Pressed(const player_t *player, const weapon_t *weapon, const mobj_t *mo,
                    k_vec2_t at, int tic)
{
    if (WeaponReady(player))
    {
        ++status.shots;
        shot_mo = mo;
        shot_tic = tic + weapon->lead;
        shot_at = at;

        if (weapon->weapon == wp_bfg)
        {
            bfg_mo = mo;
        }
    }
}

// Firing at the barrel shot at: with the weapon in hand, or, at one in the
// way, one that pops it. Pistol and chaingun only from the ready state, so
// the bullet goes straight. Not where the shot leaves from (ShotLeaves)
// near its chain.
static void FireAtBarrel(const player_t *player, const k_pose_t *pose, int tic,
                         k_command_t *command)
{
    const barrel_t *barrel = &barrels[barrel_target];
    const weapon_t *weapon = WeaponOf(player->readyweapon);
    k_vec2_t from = { pose->x, pose->y };
    double distance = SDL_max(Distance(from, barrel->at), 1.0);
    k_vec2_t leaves;
    double facing, tolerance, leaves_facing;

    if (barrel_why == BARREL_BLOCKING && !CanPop(player, weapon, distance))
    {
        const weapon_t *pop = PopWeapon(player, distance);

        if (pop != NULL && player->pendingweapon == wp_nochange
         && leveltime - last_switch >= SWITCH_TICS)
        {
            command->change = 1;
            command->weapon = pop->slot;
            last_switch = leveltime;
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "bringing up the %s for the barrel at (%.0f,%.0f)",
                  pop->name, barrel->at.x, barrel->at.y);
        }

        return;
    }

    if (weapon == NULL || !CanFire(player, weapon))
    {
        return;
    }

    facing = pose->angle + command->turn / ANGLETURN_PER_RADIAN;
    tolerance = SDL_max(atan2(BARREL_RADIUS * AIM_SHARE, distance), MIN_TOLERANCE);
    ShotLeaves(pose, weapon, barrel->at, &leaves, &leaves_facing);

    if (fabs(Wrap(atan2(barrel->at.y - from.y, barrel->at.x - from.x) - facing)) > tolerance
     || ((weapon->taps || weapon->weapon == wp_missile) && !WeaponReady(player))
     || (weapon->weapon == wp_missile && player->attackdown)
     || (weapon->weapon == wp_missile && player->powers[pw_invulnerability] < INVULN_LEFT
         && (!RocketClear(player, from, facing, distance, BARREL_RADIUS,
                          distance - BARREL_RADIUS - ROCKET_RADIUS,
                          barrel->z + BARREL_HEIGHT * 0.5, NULL)
             || !RocketClear(player, leaves, leaves_facing, Distance(leaves, barrel->at),
                             BARREL_RADIUS, Distance(leaves, barrel->at) - BARREL_RADIUS
                             - ROCKET_RADIUS, barrel->z + BARREL_HEIGHT * 0.5, NULL)))
     || ChainNearAt(barrel->chain, leaves)
     || BarrelAtRisk(player, from, facing, weapon, barrel->at, weapon->taps)
     || BarrelAtRisk(player, leaves, leaves_facing, weapon, barrel->at, weapon->taps)
     || !K_Sight_Aim(from.x, from.y, GunHeight(player->mo),
                     from.x + cos(facing) * (distance - BARREL_RADIUS),
                     from.y + sin(facing) * (distance - BARREL_RADIUS), barrel->z,
                     barrel->z + BARREL_HEIGHT))
    {
        return;
    }

    command->attack = 1;
    Pressed(player, weapon, barrel->mo, barrel->at, tic);
}

void K_Combat_Fire(int tic, k_command_t *command)
{
    const player_t *player = &players[consoleplayer];
    const k_monster_t *monster;
    const weapon_t *weapon;
    k_pose_t pose;
    k_vec2_t from, at, leaves, leaves_at;
    double distance, facing, tolerance, leaves_facing;
    boolean accurate;

    // At a gun switch nav has the weapon it needs up; not while fighting,
    // when nav waits.
    if ((status.target < 0 && barrel_target < 0) || status.died || command->use
     || command->attack || command->change
     || (!battle && K_Nav_GetStatus()->action == K_ACTION_SHOOT) || !K_Nav_Predict(tic, &pose))
    {
        return;
    }

    if (barrel_target >= 0)
    {
        FireAtBarrel(player, &pose, tic, command);
        return;
    }

    monster = &monsters[status.target];

    // The weapon first; nothing is fired while it changes.
    if (wanted >= 0 && wanted != player->readyweapon)
    {
        if (player->pendingweapon == wp_nochange && leveltime - last_switch >= SWITCH_TICS)
        {
            command->change = 1;
            command->weapon = WeaponOf(wanted)->slot;
            last_switch = leveltime;
            K_Log(K_LOG_COMBAT, K_LOG_INFO, "bringing up the %s for the %s, %.0f units off",
                  WeaponOf(wanted)->name, monster->name, monster->distance);
        }

        return;
    }

    weapon = WeaponOf(player->readyweapon);

    if (weapon == NULL || !CanFire(player, weapon))
    {
        return;
    }

    // The rocket launcher and the BFG fire only on a fresh press
    // (A_WeaponReady: not while attackdown): fire held on from before is
    // let go for a tic first (Doom II MAP10: fire held on from the shotgun
    // through the BFG picked up, it never fired, a demon biting the bot for
    // 5 s). And they are pressed only when ready, never held on: held,
    // A_ReFire fires again at the end of the shot, a press not looked at
    // where that rocket or ball leaves, and with no aim held for it (E1M9:
    // one left 33 degrees off, the target dead, into a wall 93 off -
    // killed; BFG_HOLD).
    if ((weapon->weapon == wp_missile || weapon->weapon == wp_bfg) && player->attackdown
     && WeaponReady(player))
    {
        return;
    }

    if ((weapon->weapon == wp_missile || weapon->weapon == wp_bfg) && !WeaponReady(player))
    {
        return;
    }

    from.x = pose.x;
    from.y = pose.y;
    at = TargetAt(tic);
    distance = Distance(from, at);
    facing = pose.angle + command->turn / ANGLETURN_PER_RADIAN;
    tolerance = SDL_max(atan2(monster->radius * AIM_SHARE, distance), MIN_TOLERANCE);
    leaves_at = TargetAt(tic + weapon->lead);
    ShotLeaves(&pose, weapon, leaves_at, &leaves, &leaves_facing);
    accurate = weapon->taps && player->refire == 0;

    // Checked where the press is read and where the shot leaves.
    if (fabs(Wrap(atan2(at.y - from.y, at.x - from.x) - facing)) > tolerance
     || (weapon->reach > 0.0 && distance - monster->radius > weapon->reach)
     || (weapon->weapon == wp_missile && player->powers[pw_invulnerability] < INVULN_LEFT
         && (!RocketSafe(player, from, facing, monster, distance)
             || !RocketSafe(player, leaves, leaves_facing, monster,
                            Distance(leaves, leaves_at))))
     || BarrelAtRisk(player, from, facing, weapon, at, accurate)
     || BarrelAtRisk(player, leaves, leaves_facing, weapon, leaves_at, accurate)
     || (weapon->taps && distance > TAP_DISTANCE && !WeaponReady(player))
     || !LineOfFire(player, from, facing, monster)
     || (weapon->weapon == wp_bfg && !BallClear(player, from, facing, monster)))
    {
        return;
    }

    command->attack = 1;
    Pressed(player, weapon, target_mo, at, tic);
}

double K_Combat_Danger(double x, double y, double floor)
{
    k_vec2_t at = { x, y };
    double danger = 0.0;
    int i;

    for (i = 0; i < num_monsters; ++i)
    {
        const k_monster_t *monster = &monsters[i];
        double cost = Kind(monster->type)->cost;
        double distance = Distance(monster->at, at);
        double share;

        if (cost <= 0.0 || distance > DANGER_REACH
         || !K_Sight_Check(monster->at.x, monster->at.y, monster->z + monster->height * 0.75,
                           x, y, floor, floor + PLAYER_HEIGHT))
        {
            continue;
        }

        share = distance <= DANGER_FULL
              ? 1.0 : (DANGER_REACH - distance) / (DANGER_REACH - DANGER_FULL);

        if (!monster->awake && distance > 1e-6
         && fabs(Wrap(atan2(y - monster->at.y, x - monster->at.x) - monster->angle)) > K_PI / 2.0)
        {
            share *= ASLEEP_AWAY;
        }

        danger += cost * share;
    }

    return danger;
}

int K_Combat_MonstersWithin(double reach)
{
    int count = 0;
    int i;

    for (i = 0; i < num_monsters; ++i)
    {
        if (monsters[i].visible && monsters[i].distance < reach)
        {
            ++count;
        }
    }

    return count;
}

int K_Combat_MonstersAhead(void)
{
    const player_t *player = &players[consoleplayer];
    k_vec2_t me, at;
    int point;
    boolean shut;

    if (holding)
    {
        return hold_area;
    }

    if (player->mo == NULL)
    {
        return -1;
    }

    // Found, it stays while a fight goes on: the monsters and the player move
    // about in it, and it may go from ahead (E3M3: running for shells at
    // firepower CRITICAL, a fight with a baron begun for the room ahead was
    // broken off for the run at once and begun again, eight times in 4 s,
    // the run taking the bot into the baron's claws).
    if (battle && ahead_area >= 0)
    {
        return ahead_area;
    }

    me.x = ToMap(player->mo->x);
    me.y = ToMap(player->mo->y);
    ahead_area = UnsafeAhead(me, HOLD_AHEAD, K_Area_Of(K_Map_CellAt(me.x, me.y)), -1, &at,
                             &point, &shut);
    return ahead_area;
}

int K_Combat_Beats(int weapon)
{
    return weapon >= 0 && weapon < NUMWEAPONS && beats[weapon];
}

void K_Combat_Needed(int type)
{
    needed = type;
}

int K_Combat_HardFight(void)
{
    return leveltime - hard_seen <= HARD_LINGER;
}

int K_Combat_OnSpot(double x, double y)
{
    int best = -1, i;

    for (i = 0; i < num_monsters; ++i)
    {
        double block = monsters[i].radius + PLAYER_RADIUS;

        if (SpawnHealth(monsters[i].type) >= TOUGH_HEALTH
         && fabs(monsters[i].at.x - x) < block && fabs(monsters[i].at.y - y) < block
         && (best < 0 || SpawnHealth(monsters[i].type) > SpawnHealth(best)))
        {
            best = monsters[i].type;
        }
    }

    return best;
}

void K_Combat_Telefrag(int on, double x, double y)
{
    telefrag_on = on != 0;
    telefrag_at.x = x;
    telefrag_at.y = y;
    telefrag_area = on ? K_Area_Of(K_Map_CellAt(x, y)) : -1;
}

void K_Combat_Escape(const char *why)
{
    escape = why != NULL;

    if (escape)
    {
        M_StringCopy(escape_why, why, sizeof(escape_why));
    }
}

void K_Combat_ShootBarrel(int on, double x, double y)
{
    blocking_wanted = on != 0;
    blocking_at.x = x;
    blocking_at.y = y;
}

int K_Combat_BlastsPending(double x, double y, double reach)
{
    k_vec2_t at = { x, y };
    int count = 0, i;

    for (i = 0; i < num_barrels; ++i)
    {
        count += !barrels[i].live && AxisDistance(barrels[i].at, at) < reach;
    }

    return count;
}

void K_Combat_LogSummary(void)
{
    K_Log(K_LOG_COMBAT, K_LOG_INFO, "projectiles this level: %d hits for %d health, %d of them "
          "seen in time to dodge (%d health, %d foreseen and taken); stepped out of their way "
          "%d times out of fights", status.missile_hits, status.missile_damage,
          status.dodgeable_hits, status.dodgeable_damage, status.foreseen_hits, status.dodges);
}

int K_Combat_Move(double *vx, double *vy)
{
    if (!battle && !dodging && !holding && !ducking)
    {
        return 0;
    }

    *vx = move.x;
    *vy = move.y;
    return 1;
}

const char *K_Combat_KindName(int type)
{
    return Kind(type)->name;
}

double K_Combat_KindCost(int type)
{
    return Kind(type)->cost;
}

int K_Combat_MonsterCount(void)
{
    return num_monsters;
}

const k_monster_t *K_Combat_Monster(int index)
{
    return &monsters[index];
}

int K_Combat_ProjectileCount(void)
{
    return num_projectiles;
}

const k_projectile_t *K_Combat_Projectile(int index)
{
    return &projectiles[index];
}

const k_combatstatus_t *K_Combat_GetStatus(void)
{
    return &status;
}
