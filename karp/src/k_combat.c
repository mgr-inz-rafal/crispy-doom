//
// DESCRIPTION:
//	Karp combat, the first of it (karp/docs/plan.md, 7.2 and 7.3):
//	shooting on the way.
//
//	Each tic the monsters and projectiles are listed from the thinkers,
//	and Karp's own sight checks tell which monsters the player can see.
//	The target is the one scoring best: one within reach of its blows or in
//	the way first, then the most dangerous kinds (hitscanners), awake ones,
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
//	bare fist comes after it, and melee is for weak monsters only. Nav
//	faces the target wherever it
//	does not need the facing itself, and the weapon fires once the facing
//	after a command's turn is on the target.
//
//	Pistol and chaingun shots stray only when the trigger is held through
//	a shot (P_GunShot is accurate while refire is 0): far off they are
//	tapped, fired only from the ready state. The plasma rifle is held: let
//	go, it pauses 20 tics after each ball. Rockets are fired only where
//	they would burst well clear of the player, as the blast measures it,
//	their path clear of walls and other monsters, no lost soul near; and
//	only at tough monsters, or bunched ones, unless the player is
//	invulnerable.
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
//	off from monsters that come close, never over a drop or onto a floor
//	that hurts. Once nothing has been worth shooting for a second, nav
//	plans its route again from where the player is. A target that has not
//	lost health for a while (out of reach of the shots) is left alone for
//	a time. Nav busy at a door, lift or switch, or running a jump, the
//	player shoots from where it is, as on the way. Hurt there by a monster
//	in reach of its blows that the shots cannot find (an imp clawing down
//	at a lift: vanilla's blows reach any height), the player steps away
//	from it, the route waiting.
//
//	Escaping (plan, 7.4): while the brain goes for health with health
//	CRITICAL, or dashes for a weapon or ammo close by, no fight is started
//	and one going on is broken off; the player runs for it, shooting on the
//	way at what it sees.
//
//	Danger (plan, 7.5): what going to a spot would cost in health - the
//	monsters that could see the player there, each by what a fight with its
//	kind takes. A guarded medikit loses to a lonely one.
//

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "doomdef.h"
#include "doomstat.h"
#include "m_misc.h"
#include "p_local.h"

#include "k_combat.h"
#include "k_condition.h"
#include "k_graph.h"
#include "k_items.h"
#include "k_log.h"
#include "k_salt.h"
#include "k_sight.h"

#define K_PI 3.14159265358979323846
#define ANGLETURN_PER_RADIAN (65536.0 / (2.0 * K_PI))

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
// away from awake monsters within KEEP_AWAY. Each way is tried at
// MOVE_LOOK ahead, in MOVE_WAYS directions.
#define STRAFE_SPEED 10.0
#define STRAFE_MIN 20
#define STRAFE_SPREAD 30
#define LEASH 128.0
#define KEEP_AWAY 192.0
#define MOVE_LOOK 32.0
#define MOVE_WAYS 16

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
#define FIST_SHARE 0.25
#define MELEE_HEALTH 200

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
// of their damage - no rocket for a zombieman or an imp (the user).
#define ROCKET_RADIUS 11.0
#define ROCKET_SPEED 20.0
#define ROCKET_SAFE (BLAST + PLAYER_RADIUS + 48.0)
#define ROCKET_MIN (ROCKET_SAFE * 1.4142)
#define SOUL_CLEAR 384.0
#define INVULN_LEFT 70
#define ROCKET_HEALTH 100
#define BUNCHED 96.0
#define ROCKET_SHARE 0.25

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
// more, until it falls under UNCOMFY_AMMO - they are fought as any.
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

typedef struct
{
    mobjtype_t type;
    const char *name;
    double threat;          // how much to fear it as a target, roughly
    double cost;            // the health a fight with it takes, roughly
    boolean hitscan;        // its attack is a bullet
} kind_t;

static const kind_t kinds[] =
{
    { MT_POSSESSED, "zombieman",         3.0,   5.0, true },
    { MT_SHOTGUY,   "shotgun guy",       4.0,  10.0, true },
    { MT_CHAINGUY,  "chaingunner",       5.0,  20.0, true },
    { MT_WOLFSS,    "Wolfenstein SS",    4.0,  10.0, true },
    { MT_SPIDER,    "spider mastermind", 6.0, 100.0, true },
    { MT_VILE,      "arch-vile",         6.0,  50.0, false },
    { MT_PAIN,      "pain elemental",    4.0,  20.0, false },
    { MT_UNDEAD,    "revenant",          4.0,  25.0, false },
    { MT_FATSO,     "mancubus",          4.0,  30.0, false },
    { MT_BABY,      "arachnotron",       4.0,  25.0, false },
    { MT_CYBORG,    "cyberdemon",        6.0, 100.0, false },
    { MT_BRUISER,   "baron of hell",     3.0,  30.0, false },
    { MT_KNIGHT,    "hell knight",       3.0,  20.0, false },
    { MT_HEAD,      "cacodemon",         2.5,  15.0, false },
    { MT_TROOP,     "imp",               2.0,   8.0, false },
    { MT_SERGEANT,  "demon",             2.0,   8.0, false },
    { MT_SHADOWS,   "spectre",           2.0,  10.0, false },
    { MT_SKULL,     "lost soul",         2.0,   6.0, false },
    { MT_KEEN,      "Commander Keen",    0.5,   0.0, false },
};

static const kind_t other_kind = { NUMMOBJTYPES, "monster", 1.0, 10.0, false };

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
static int last_switch;             // leveltime of the last switch pressed
static const mobj_t *shot_mo;       // what the last press read was fired at
static int shot_tic = -1;           // and the tic its shot leaves (lead)
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

// Fights broken off: the brain goes for health, CRITICAL, or dashes for a
// weapon or ammo close by (K_Combat_Escape), and why.
static boolean escape;
static char escape_why[64];

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
    }

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

    for (thinker = thinkercap.next; thinker != &thinkercap; thinker = thinker->next)
    {
        const mobj_t *mo = (const mobj_t *) thinker;

        if (thinker->function.acp1 != (actionf_p1) P_MobjThinker)
        {
            continue;
        }

        if (mo->flags & MF_MISSILE)
        {
            AddProjectile(mo, player_mo, me);
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
    UpdateChains(changed || num_barrels != was_barrels, player_mo);
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
// every 32 tics and are counted by the route (k_path.c).
static void LogHits(const player_t *player)
{
    char name[64];

    if (last_health >= 0 && player->health < last_health && player->attacker != NULL)
    {
        AttackerName(player, name, sizeof(name));
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "hit for %d by %s - %d health left",
              last_health - player->health, name, player->health);

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

// The damage a shot of the weapon would do, on average, to a thing of the
// radius at the distance: 0 if it cannot fire, the thing is out of a melee
// weapon's reach, a barrel is beside the player (a blow could set it off),
// or a rocket could hurt the player too (ROCKET_MIN, SOUL_CLEAR).
static double ShotDamage(const player_t *player, const weapon_t *weapon, double distance,
                         double radius)
{
    double damage = weapon->damage, hit = 1.0;

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
        if (weapon->weapon == wp_missile && player->powers[pw_invulnerability] < INVULN_LEFT
         && (distance - radius - ROCKET_RADIUS < ROCKET_MIN || SoulNear()))
        {
            return 0.0;
        }

        hit = 1.0 / (1.0 + distance / 512.0);
    }
    else if (!(weapon->taps && distance > TAP_DISTANCE))
    {
        double half = atan2(radius, distance) * 180.0 / K_PI;

        hit = SDL_min(1.0, half / weapon->spread);
    }

    if (weapon->range > 0.0 && distance > weapon->range)
    {
        double share = weapon->range / distance;

        hit *= SDL_max(0.25, share * share);
    }

    return damage * hit;
}

// The damage a tic the weapon would do to the monster, no more than kills
// it counting (and a half: not all hits land as hoped).
static double Rating(const player_t *player, const weapon_t *weapon, const k_monster_t *monster)
{
    return SDL_min(ShotDamage(player, weapon, monster->distance, monster->radius),
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

static void ChooseTarget(const player_t *player)
{
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    double facing = player->mo->angle * (2.0 * K_PI / 4294967296.0);
    double best_score = 0.0;
    boolean same;
    int best = -1;
    int i;

    for (i = 0; i < num_monsters; ++i)
    {
        const k_monster_t *monster = &monsters[i];
        double score, turn;

        if (!monster->shootable || monster->distance > TARGET_REACH
         || (!monster->awake && monster->distance > ASLEEP_REACH)
         || FarAndDodgeable(monster, monster_mobjs[i]) || IgnoredTarget(i))
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

// Whether the pistol is right for the target: far off with little health
// left, not closing in, the area clear a while; or nothing better can hurt
// it.
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
         && Selectable(player, weapon)
         && ShotDamage(player, weapon, monster->distance, monster->radius) > 0.0)
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

// Rating, less for the last resorts: the pistol where it is not right, the
// bare fist always, any melee against a tough monster, rockets at one weak
// monster alone; none for rockets at a lost soul.
static double Preference(const player_t *player, const weapon_t *weapon,
                         const k_monster_t *monster, boolean pistol_right)
{
    double rating = Rating(player, weapon, monster);

    if (weapon->weapon == wp_pistol && !pistol_right)
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
        rating *= ROCKET_SHARE;
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

    for (i = 0; i < arrlen(weapons); ++i)
    {
        double rating = Selectable(player, &weapons[i])
                      ? Preference(player, &weapons[i], monster, pistol_right) : 0.0;

        if (rating > best_rating)
        {
            best_rating = rating;
            best = &weapons[i];
        }
    }

    current = WeaponOf(player->pendingweapon != wp_nochange ? player->pendingweapon
                                                           : player->readyweapon);
    current_rating = current != NULL ? Preference(player, current, monster, pistol_right)
                                     : 0.0;
    can_hurt = best_rating > 0.0 || current_rating > 0.0;

    if (best != NULL && best != current
     && !(current != NULL && current_rating > 0.0
          && ShotDamage(player, current, monster->distance, monster->radius) >= monster->health)
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

    return weapon->taps ? weapon->damage : ShotDamage(player, weapon, distance, BARREL_RADIUS);
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
         * SDL_min(1.0, ShotDamage(player, weapon, monster->distance, monster->radius)
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

// Where to move while fighting: each of MOVE_WAYS directions that can be
// walked, scored by the side strafed to (and across the line of fire rather
// than along it), the distance from where the fight began, and the awake
// monsters close by, which it backs away from; keeping the target where
// the shots find it (the user: strafing into a corridor and out, the bot
// looked to be peeking round the corner), and open floor before a passage.
static void ChooseMove(const player_t *player)
{
    const k_monster_t *target = &monsters[status.target];
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    double floor = ToMap(player->mo->floorz);
    double gun = GunHeight(player->mo);
    boolean hurts = K_Map_FloorDamage((int) (player->mo->subsector->sector - sectors)) > 0.0;
    double tx = target->at.x - me.x, ty = target->at.y - me.y;
    double length = sqrt(tx * tx + ty * ty);
    double sx, sy, best_score = -1e9;
    boolean found = false;
    int k, i;

    move.x = move.y = 0.0;

    if (length < 1e-6)
    {
        return;
    }

    tx /= length;
    ty /= length;

    if (--strafe_tics <= 0)
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
        double score, from_anchor;

        if (!Walkable(near, floor, hurts) || !Walkable(far, floor, hurts))
        {
            continue;
        }

        score = ux * sx + uy * sy + 0.3 * (1.0 - fabs(ux * tx + uy * ty))
              + 0.3 * K_Graph_Clearance(far, OPEN_REACH) / OPEN_REACH;

        if (!K_Sight_Aim(far.x, far.y, gun, target->at.x, target->at.y, target->z,
                         target->z + target->height))
        {
            score -= 1.5;
        }

        from_anchor = Distance(far, anchor);

        if (from_anchor > LEASH)
        {
            score -= (from_anchor - LEASH) / 32.0;
        }

        for (i = 0; i < num_monsters; ++i)
        {
            const k_monster_t *monster = &monsters[i];
            double mx = monster->at.x - me.x, my = monster->at.y - me.y;
            double distance = sqrt(mx * mx + my * my);

            if (monster->awake && distance < KEEP_AWAY && distance > 1e-6)
            {
                score -= 2.0 * (ux * mx + uy * my) / distance
                       * (KEEP_AWAY - distance) / KEEP_AWAY;
            }
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

        if (score > best_score)
        {
            best_score = score;
            move.x = ux * STRAFE_SPEED;
            move.y = uy * STRAFE_SPEED;
            found = true;
        }
    }

    // Going the other way after all (a wall, a monster): strafe that way.
    if (found && move.x * sx + move.y * sy < 0.0)
    {
        strafe_dir = -strafe_dir;
        strafe_tics = STRAFE_MIN + Hash(K_Salt_Mix(leveltime)) % STRAFE_SPREAD;
    }
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
    return (!target->awake || target->distance <= BATTLE_REACH) && FoughtAsAny(status.target);
}

static void StartBattle(const player_t *player)
{
    battle = true;
    status.battle = true;
    battle_since = leveltime;
    battle_kills = player->killcount;
    anchor.x = ToMap(player->mo->x);
    anchor.y = ToMap(player->mo->y);
    progress_tic = leveltime;
    strafe_tics = STRAFE_MIN + Hash(K_Salt_Mix(leveltime)) % STRAFE_SPREAD;
    K_Log(K_LOG_COMBAT, K_LOG_INFO, "fighting the %s, %.0f units off - the route waits",
          monsters[status.target].name, monsters[status.target].distance);
    K_Nav_Pause(1);
}

static void EndBattle(const player_t *player)
{
    battle = false;
    status.battle = false;
    move.x = move.y = 0.0;

    if (escape)
    {
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "breaking off the fight after %.1f s, %d killed - %s",
              (double) (leveltime - battle_since) / TICRATE, player->killcount - battle_kills,
              escape_why);
    }
    else
    {
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "fight over after %.1f s, %d killed - back on the way",
              (double) (leveltime - battle_since) / TICRATE, player->killcount - battle_kills);
    }

    K_Nav_Pause(0);
}

static void UpdateBattle(const player_t *player)
{
    boolean fight;

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
    // on the way.
    fight = WorthFighting() && !escape;
    calm = fight ? 0 : calm + 1;

    if (!battle && fight && K_Nav_CanPause())
    {
        StartBattle(player);
    }
    else if (battle && (calm >= BATTLE_CALM || escape))
    {
        EndBattle(player);
    }

    if (battle && status.target >= 0)
    {
        ChooseMove(player);
    }
    else
    {
        move.x = move.y = 0.0;
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
        move.x = move.y = 0.0;
        K_Nav_Pause(0);
    }
}

// Out of a fight, away from a blast about to go off (or a primed barrel a
// monster can see), or from blows the shots cannot answer: the route waits
// while the player steps the walkable
// way that gets furthest from it. A blow is stepped away from even at a door
// or lift (the action is begun again after), but only if some way is
// walkable: a lift going down rides on. In battle the strafing keeps away
// from barrels (ChooseMove).
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
        return;
    }

    blast = PendingBlast(player);
    primed = blast < 0 ? PrimedNear(player) : -1;
    blow = blast < 0 && primed < 0 ? BlowFrom(player) : -1;

    if ((blast < 0 && primed < 0 && blow < 0)
     || (!dodging && !K_Nav_CanPause() && !(blow >= 0 && K_Nav_Waiting())))
    {
        StopDodge();
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

    if (!dodging || dodging_blow != (blast < 0) || dodging_primed != (primed >= 0))
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
        K_Nav_Pause(1);
    }

    move.x = way.x * DODGE_SPEED;
    move.y = way.y * DODGE_SPEED;
}

void K_Combat_Reset(void)
{
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
    escape = false;
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

    if (player->playerstate == PST_DEAD || player->health <= 0)
    {
        battle = false;
        status.battle = false;

        if (!status.died)
        {
            AttackerName(player, name, sizeof(name));
            K_Log(K_LOG_COMBAT, K_LOG_WARN, "died at (%.0f,%.0f), %.1f s into the level - "
                  "killed by %s; %d kills of %d", ToMap(player->mo->x), ToMap(player->mo->y),
                  (double) leveltime / TICRATE, name, player->killcount, totalkills);
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
        ChooseTarget(player);
        CountMisses(player);
        ChooseWeapon(player);
        ChooseBarrel(player);
        UpdateBattle(player);
        UpdateDodge(player);
    }
    else
    {
        barrel_target = -1;
    }

    status.barrel = barrel_target >= 0;

    if (barrel_target >= 0)
    {
        M_snprintf(status.text, sizeof(status.text), "shooting the barrel at (%.0f,%.0f) - %s",
                   status.barrel_at.x, status.barrel_at.y, barrel_reason);
    }
    else if (status.target >= 0)
    {
        M_snprintf(status.text, sizeof(status.text), "%s%s the %s, %.0f units - %s",
                   battle ? "fighting" : escape ? escape_why : "shooting on the way at",
                   escape && !battle ? ", shooting at" : "",
                   monsters[status.target].name, monsters[status.target].distance,
                   status.weapon != NULL ? status.weapon : "no weapon for it");
    }
    else if (battle)
    {
        M_StringCopy(status.text, "fighting - nothing in sight for now", sizeof(status.text));
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
// pellets left three tics later along the route).
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
        *at = monsters[i].at;
        return true;
    }

    for (i = 0; i < num_barrels; ++i)
    {
        if (barrels[i].mo == shot_mo && barrels[i].live)
        {
            *at = barrels[i].at;
            return true;
        }
    }

    return false;
}

int K_Combat_Aim(int tic, double *angle)
{
    k_pose_t pose;
    k_vec2_t at;
    boolean held = HeldAim(tic, &at);

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

// Whether a rocket fired along the facing at the target would burst
// ROCKET_SAFE off the player as the blast measures it (the larger of the x
// and y distances): at the target's front, nearer by what it closes in
// during the flight; or at another monster it would meet first; with no
// wall within its radius either side of its path, as far as a burst there
// would still be too near. And no lost soul within SOUL_CLEAR.
static boolean RocketSafe(const player_t *player, k_vec2_t from, double facing,
                          const k_monster_t *target, double distance)
{
    double ux = cos(facing), uy = sin(facing);
    double axis = SDL_max(fabs(ux), fabs(uy));
    double gun = GunHeight(player->mo);
    double closing = -(target_velocity.x * (target->at.x - from.x)
                     + target_velocity.y * (target->at.y - from.y)) / SDL_max(distance, 1.0);
    double burst = distance - target->radius - ROCKET_RADIUS
                 - SDL_max(0.0, closing) * distance / ROCKET_SPEED;
    double reach = SDL_min(SDL_max(distance - target->radius, distance * 0.5),
                           ROCKET_SAFE / axis + ROCKET_RADIUS);
    double middle = target->z + target->height * 0.5;
    int i, side;

    if (SoulNear())
    {
        return false;
    }

    for (i = 0; i < num_monsters; ++i)
    {
        const k_monster_t *monster = &monsters[i];
        double along = (monster->at.x - from.x) * ux + (monster->at.y - from.y) * uy;
        double across = fabs((monster->at.y - from.y) * ux - (monster->at.x - from.x) * uy);

        if (monster != target && along > 0.0 && along < distance
         && across < monster->radius + ROCKET_RADIUS)
        {
            burst = SDL_min(burst, along - monster->radius - ROCKET_RADIUS);
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
                         from.y + oy + uy * reach, z - 4.0, z + 4.0))
        {
            return false;
        }
    }

    return true;
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
static void Pressed(const player_t *player, const weapon_t *weapon, const mobj_t *mo, int tic)
{
    if (WeaponReady(player))
    {
        ++status.shots;
        shot_mo = mo;
        shot_tic = tic + weapon->lead;
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
     || (weapon->taps && !WeaponReady(player))
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
    Pressed(player, weapon, barrel->mo, tic);
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
     || !LineOfFire(player, from, facing, monster))
    {
        return;
    }

    command->attack = 1;
    Pressed(player, weapon, target_mo, tic);
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

int K_Combat_Move(double *vx, double *vy)
{
    if (!battle && !dodging)
    {
        return 0;
    }

    *vx = move.x;
    *vy = move.y;
    return 1;
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
