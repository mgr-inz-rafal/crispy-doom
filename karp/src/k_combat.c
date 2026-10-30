//
// DESCRIPTION:
//	Karp combat, the first of it (karp/docs/plan.md, 7.2 and 7.3):
//	shooting on the way.
//
//	Each tic the monsters and projectiles are listed from the thinkers,
//	and Karp's own sight checks tell which monsters the player can see.
//	The target is the one scoring best: the most dangerous kinds first
//	(hitscanners), awake ones, attacking ones, near ones; far ones are
//	left alone (the user: chasing a far hitscanner wastes time and ammo).
//	The weapon is the one that would hurt it most a tic, counting no more
//	than kills it, brought up only when clearly better than the one in
//	hand (a switch takes about 0.9 s). Nav faces the target wherever it
//	does not need the facing itself, and the weapon fires once the facing
//	after a command's turn is on the target.
//
//	Pistol and chaingun shots stray only when the trigger is held through
//	a shot (P_GunShot is accurate while refire is 0): far off they are
//	tapped, fired only from the ready state. The plasma rifle is held: let
//	go, it pauses 20 tics after each ball. Rockets are never fired at
//	anything close (the blast reaches 128 from where it hits), unless the
//	player is invulnerable; no shot goes through a barrel near the player.
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
//	player shoots from where it is, as on the way.
//
//	Escaping (plan, 7.4): while the brain goes for health with health
//	CRITICAL, no fight is started and one going on is broken off; the
//	player runs for it, shooting on the way at what it sees.
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
#include "k_graph.h"
#include "k_log.h"
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

// Pistol and chaingun shots are tapped beyond this distance.
#define TAP_DISTANCE 192.0

// A weapon is brought up only if it rates SWITCH_GAIN times the one in
// hand, and not within SWITCH_WAIT tics of the last switch; if the one in
// hand cannot hurt the target at all, after SWITCH_TICS. A switch takes
// about 0.9 s (Doom II MAP02: with a gain of 1.3, 24 switches in a minute).
#define SWITCH_GAIN 2.0
#define SWITCH_WAIT 105
#define SWITCH_TICS 35

// Rockets: none at anything nearer than ROCKET_MIN (the blast reaches 128
// from where it hits, the player's box 16 more), unless invulnerable for
// INVULN_LEFT tics more at least.
#define ROCKET_MIN 160.0
#define INVULN_LEFT 70

// No shot through a standing barrel within BARREL_HOLD of the player.
#define BARREL_HOLD 200.0
#define BARREL_MARGIN 4.0

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
    { wp_fist,         wp_fist,     "fist",            0,  11.0, 22.0,  0.0, false, false, 64.0,   0.0 },
    { wp_chainsaw,     wp_fist,     "chainsaw",        0,  11.0,  4.0,  0.0, false, false, 64.0,   0.0 },
    { wp_pistol,       wp_pistol,   "pistol",          1,  10.0, 20.0,  5.6, true,  false,  0.0,   0.0 },
    { wp_shotgun,      wp_shotgun,  "shotgun",         1,  70.0, 37.0,  5.6, false, false,  0.0, 400.0 },
    { wp_supershotgun, wp_shotgun,  "super shotgun",   2, 200.0, 57.0, 11.2, false, false,  0.0, 300.0 },
    { wp_chaingun,     wp_chaingun, "chaingun",        1,  10.0,  4.5,  5.6, true,  false,  0.0,   0.0 },
    { wp_missile,      wp_missile,  "rocket launcher", 1, 120.0, 20.0,  0.0, false, true,   0.0,   0.0 },
    { wp_plasma,       wp_plasma,   "plasma rifle",    1,  22.5,  3.0,  0.0, false, true,   0.0,   0.0 },
    { wp_bfg,          wp_bfg,      "BFG",            40, 500.0, 60.0,  0.0, false, true,   0.0,   0.0 },
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
static k_vec2_t target_last;        // where it was then
static k_vec2_t target_velocity;    // smoothed, units per tic
static int wanted = -1;             // the weapon wanted, -1 if none
static boolean can_hurt;            // some weapon in hand can hurt the target
static int last_switch;             // leveltime of the last switch pressed
static int last_health = -1;

// The target's health, and when it last went down (or the target changed);
// how many tics in a row it has been the target.
static int target_health;
static int progress_tic;
static int sighted;

// Shots fired since the target was last hurt, told by the ammo the weapon in
// hand uses going down.
static int misses;
static int last_ammo;
static int last_ammo_type = am_noammo;

// Targets given up on for now, and until when.
typedef struct
{
    const mobj_t *mo;
    int until;
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

// Fights broken off: the brain goes for health, CRITICAL (K_Combat_Escape).
static boolean escape;

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
    int i;

    num_monsters = 0;
    num_projectiles = 0;
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
        M_StringCopy(buf, "its own blast", buf_len);
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

// The damage a tic the weapon would do to the monster, no more than kills
// it counting (and a half: not all hits land as hoped).
static double Rating(const player_t *player, const weapon_t *weapon, const k_monster_t *monster)
{
    double damage = weapon->damage, hit = 1.0;

    if (!CanFire(player, weapon))
    {
        return 0.0;
    }

    if (weapon->reach > 0.0)
    {
        if (monster->distance - monster->radius > weapon->reach)
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
        if (weapon->weapon == wp_missile && monster->distance < ROCKET_MIN
         && player->powers[pw_invulnerability] < INVULN_LEFT)
        {
            return 0.0;
        }

        hit = 1.0 / (1.0 + monster->distance / 512.0);
    }
    else if (!(weapon->taps && monster->distance > TAP_DISTANCE))
    {
        double half = atan2(monster->radius, monster->distance) * 180.0 / K_PI;

        hit = SDL_min(1.0, half / weapon->spread);
    }

    if (weapon->range > 0.0 && monster->distance > weapon->range)
    {
        double share = weapon->range / monster->distance;

        hit *= SDL_max(0.25, share * share);
    }

    return SDL_min(damage * hit, monster->health * 1.5) / weapon->tics;
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

static void Ignore(const mobj_t *mo, int tics)
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

static void ChooseTarget(const player_t *player)
{
    k_vec2_t me = { ToMap(player->mo->x), ToMap(player->mo->y) };
    double facing = player->mo->angle * (2.0 * K_PI / 4294967296.0);
    double best_score = 0.0;
    int best = -1;
    int i;

    for (i = 0; i < num_monsters; ++i)
    {
        const k_monster_t *monster = &monsters[i];
        double score, turn;

        if (!monster->shootable || monster->distance > TARGET_REACH
         || (!monster->awake && monster->distance > ASLEEP_REACH)
         || Ignored(monster_mobjs[i]))
        {
            continue;
        }

        score = Kind(monster->type)->threat * (monster->awake ? 2.0 : 1.0)
              * (monster->attacking ? 1.5 : 1.0) * (1.0 + 384.0 / (monster->distance + 64.0));

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

        if (score > best_score)
        {
            best_score = score;
            best = i;
        }
    }

    if (best >= 0 && monster_mobjs[best] == target_mo)
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
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "target: the %s at (%.0f,%.0f), %.0f units%s",
              monsters[best].name, monsters[best].at.x, monsters[best].at.y,
              monsters[best].distance, monsters[best].awake ? "" : ", asleep");
        target_velocity.x = target_velocity.y = 0.0;
        progress_tic = leveltime;
        sighted = 1;
        misses = 0;
    }
    else
    {
        sighted = 0;
    }

    if (best >= 0)
    {
        target_health = monsters[best].health;
    }

    status.target = best;
    target_mo = best >= 0 ? monster_mobjs[best] : NULL;

    if (best >= 0)
    {
        target_last = monsters[best].at;
    }
}

// A shot fired at the target (the ammo went down) that did not hurt it;
// after MISS_SHOTS of them the target is left alone for a moment.
static void CountMisses(const player_t *player)
{
    ammotype_t ammo = weaponinfo[player->readyweapon].ammo;
    int now = ammo == am_noammo ? 0 : player->ammo[ammo];

    if (status.target >= 0 && ammo == last_ammo_type && now < last_ammo)
    {
        ++misses;
    }

    last_ammo_type = ammo;
    last_ammo = now;

    if (status.target >= 0 && misses >= MISS_SHOTS)
    {
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "the %s seems out of reach of the shots - %d without "
              "a hit; left alone for %d s", monsters[status.target].name, misses,
              MISS_IGNORE_TICS / TICRATE);
        Ignore(target_mo, MISS_IGNORE_TICS);
        DropTarget();
    }
}

static void ChooseWeapon(const player_t *player)
{
    const k_monster_t *monster;
    const weapon_t *best = NULL, *current;
    double best_rating = 0.0, current_rating;
    int i;

    wanted = -1;
    can_hurt = false;
    status.weapon = NULL;

    if (status.target < 0)
    {
        return;
    }

    monster = &monsters[status.target];

    for (i = 0; i < arrlen(weapons); ++i)
    {
        double rating = Selectable(player, &weapons[i])
                      ? Rating(player, &weapons[i], monster) : 0.0;

        if (rating > best_rating)
        {
            best_rating = rating;
            best = &weapons[i];
        }
    }

    current = WeaponOf(player->pendingweapon != wp_nochange ? player->pendingweapon
                                                           : player->readyweapon);
    current_rating = current != NULL ? Rating(player, current, monster) : 0.0;
    can_hurt = best_rating > 0.0 || current_rating > 0.0;

    if (best != NULL && best != current
     && ((current_rating <= 0.0 && leveltime - last_switch >= SWITCH_TICS)
         || (best_rating > current_rating * SWITCH_GAIN
             && leveltime - last_switch >= SWITCH_WAIT)))
    {
        current = best;
    }

    if (current != NULL)
    {
        wanted = current->weapon;
        status.weapon = current->name;
    }
}

// A cheap hash for the strafing's timing: Karp may not draw the game's
// random numbers (plan, 2.4), and the same game must give the same moves.
static unsigned int Hash(unsigned int x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// Whether the player can step to a point while fighting: floor it can walk
// onto from the floor it stands on, clear of walls, solid things and
// monsters, not near an edge it could slip off, and not a floor that hurts
// unless it stands on one already.
static boolean Walkable(k_vec2_t at, double floor, boolean hurts)
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
     || K_Graph_DropDistance(at, cell_floor, PLAYER_RADIUS + 8.0) < PLAYER_RADIUS + 8.0
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
        strafe_tics = STRAFE_MIN + Hash(leveltime) % STRAFE_SPREAD;
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
        strafe_tics = STRAFE_MIN + Hash(leveltime) % STRAFE_SPREAD;
    }
}

// A target to stop for: one some weapon in hand can hurt, in sight for a
// while, awake and near enough to matter (asleep ones are targets only when
// close anyway).
static boolean WorthFighting(void)
{
    const k_monster_t *target;

    if (status.target < 0 || !can_hurt || sighted < SIGHTED_TICS)
    {
        return false;
    }

    target = &monsters[status.target];
    return !target->awake || target->distance <= BATTLE_REACH;
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
    strafe_tics = STRAFE_MIN + Hash(leveltime) % STRAFE_SPREAD;
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
        K_Log(K_LOG_COMBAT, K_LOG_INFO, "breaking off the fight after %.1f s, %d killed - "
              "health CRITICAL, going for health", (double) (leveltime - battle_since) / TICRATE,
              player->killcount - battle_kills);
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
        Ignore(target_mo, IGNORE_TICS);
        DropTarget();
    }

    // Going for health, CRITICAL: no fight is started, and one going on is
    // broken off at once; the player shoots on the way.
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
        M_StringCopy(status.text, "dead", sizeof(status.text));
        return;
    }

    // Not driving (a menu, the pause): everything stays as it is.
    if (driving)
    {
        ChooseTarget(player);
        CountMisses(player);
        ChooseWeapon(player);
        UpdateBattle(player);
    }

    if (status.target >= 0)
    {
        M_snprintf(status.text, sizeof(status.text), "%s the %s, %.0f units - %s",
                   battle ? "fighting" : escape ? "escaping, shooting at"
                                                : "shooting on the way at",
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

int K_Combat_Aim(int tic, double *angle)
{
    k_pose_t pose;
    k_vec2_t at;

    if (status.target < 0 || !K_Nav_Predict(tic, &pose))
    {
        return 0;
    }

    at = TargetAt(tic);
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

// Whether a standing barrel near the player is on the line of fire: the
// shot would set it off.
static boolean BarrelInTheWay(k_vec2_t from, k_vec2_t to)
{
    const k_map_t *map = K_Map_Get();
    double distance = Distance(from, to);
    double ux, uy;
    int b;

    if (distance < 1.0)
    {
        return false;
    }

    ux = (to.x - from.x) / distance;
    uy = (to.y - from.y) / distance;

    for (b = 0; b < map->num_blocks; ++b)
    {
        const k_block_t *block = &map->blocks[b];
        double along, across;

        if (!block->barrel || !K_Map_BlockStands(b))
        {
            continue;
        }

        along = (block->at.x - from.x) * ux + (block->at.y - from.y) * uy;
        across = fabs((block->at.y - from.y) * ux - (block->at.x - from.x) * uy);

        if (along > 0.0 && along < SDL_min(distance, BARREL_HOLD)
         && across < block->radius + BARREL_MARGIN)
        {
            return true;
        }
    }

    return false;
}

void K_Combat_Fire(int tic, k_command_t *command)
{
    const player_t *player = &players[consoleplayer];
    const k_monster_t *monster;
    const weapon_t *weapon;
    k_pose_t pose;
    k_vec2_t from, at;
    double distance, facing, tolerance;

    // At a gun switch nav has the weapon it needs up; not while fighting,
    // when nav waits.
    if (status.target < 0 || status.died || command->use || command->attack || command->change
     || (!battle && K_Nav_GetStatus()->action == K_ACTION_SHOOT) || !K_Nav_Predict(tic, &pose))
    {
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

    if (fabs(Wrap(atan2(at.y - from.y, at.x - from.x) - facing)) > tolerance
     || (weapon->reach > 0.0 && distance - monster->radius > weapon->reach)
     || (weapon->weapon == wp_missile && distance < ROCKET_MIN
         && player->powers[pw_invulnerability] < INVULN_LEFT)
     || BarrelInTheWay(from, at)
     || (weapon->taps && distance > TAP_DISTANCE && !WeaponReady(player))
     || !LineOfFire(player, from, facing, monster))
    {
        return;
    }

    command->attack = 1;

    if (WeaponReady(player))
    {
        ++status.shots;
    }
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

void K_Combat_Escape(int on)
{
    escape = on != 0;
}

int K_Combat_Move(double *vx, double *vy)
{
    if (!battle)
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
