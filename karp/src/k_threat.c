//
// DESCRIPTION:
//	Karp threats (karp/docs/plan.md, 7.4).
//
//	Projectiles fly straight at a steady speed, so where each will be is
//	known; each flies until the first wall on its way, found with sight
//	checks (missiles fly over blocking lines, as sight does). Monsters
//	never lead a shot (P_SpawnMissile aims at where the player is that
//	tic): a shot wound up for, seen coming in the monster's states, is
//	aimed at where the way puts the player when it is fired (a mancubus's
//	volleys, each shot off that line by its angle; the lost soul a pain
//	elemental spits out, charging at once). A revenant's
//	missile turns toward the player every 4 tics, TRACE_TURN at the most,
//	when the tic it was fired on lets it (A_Tracer: about half of them); it
//	is followed tic by tic along each way, walls and all. A lost soul
//	charging flies as a projectile does. A rocket bursts where it stops,
//	and the blast reaches the player near that spot.
//
//	A hit costs the health it takes on average, armour and skill counted,
//	the more the less health is left, and KILL_COST times the chance that
//	it kills.
//

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "d_loop.h"
#include "doomdef.h"
#include "doomstat.h"
#include "p_local.h"

#include "k_sight.h"
#include "k_threat.h"

#define K_PI 3.14159265358979323846

// The player's box (its radius) and height. A projectile hits when its box
// comes within HIT_MARGIN of the player's: leeway for the way not going
// quite as planned.
#define PLAYER_RADIUS 16.0
#define PLAYER_HEIGHT 56.0
#define HIT_MARGIN 4.0

// The same up and down: one falling steeply at a player on a step may pass
// a few units under its feet in the reckoning a tic before it hits (Doom II
// MAP11: a cacodemon's ball falling 6 a tic).
#define HIT_RISE 8.0

// The player runs no faster than this (units per tic): threats further off
// than it can run before they get there never reach it.
#define RUN_SPEED 17.0

// Hits within URGENT_TICS count in full, later ones less and less, down to
// LATE_SHARE at K_THREAT_TICS: the way can still change.
#define URGENT_TICS 12
#define LATE_SHARE 0.5

// A hit that could kill costs KILL_COST more, times the chance it does.
#define KILL_COST 200.0

// A revenant's missile turns this much at the most (TRACEANGLE).
#define TRACE_TURN (16.875 * K_PI / 180.0)

// A rocket's burst (A_Explode) hurts within BLAST, less the distance.
#define BLAST 128.0

// A lost soul charges at SKULL_SPEED (A_SkullAttack).
#define SKULL_SPEED 20.0

// A shot wound up for flies WOUND_PAST tics beyond where it was aimed: the
// walls further on are not known.
#define WOUND_PAST 8

// A hit counts as foreseen when the way taken expected it within
// FORESEEN_TICS of when it came. UNJUDGED: no way was judged that tic.
#define FORESEEN_TICS 4
#define UNJUDGED (-2)

// Within this a monster that claws or bites does that instead of shooting
// (P_CheckMeleeRange: 44 plus the player's radius, and a margin).
#define MELEE_RANGE 64.0

#define MAX_THREATS 256

extern void A_TroopAttack();
extern void A_HeadAttack();
extern void A_BruisAttack();
extern void A_SkelMissile();
extern void A_FatAttack1();
extern void A_FatAttack2();
extern void A_FatAttack3();
extern void A_BspiAttack();
extern void A_CyberAttack();
extern void A_SkullAttack();
extern void A_PainAttack();

extern int demostarttic;            // g_game.c

#define MAX_SHOTS 2

// The states' actions that fire a shot.
typedef struct
{
    actionf_v action;
    mobjtype_t missile;
    double launch;          // how far ahead of the monster it is at the end
                            // of the tic it is fired, in steps of its speed
                            // (P_CheckMissileSpawn: half a step, and its own
                            // first step that tic; a pain elemental's lost
                            // soul, A_PainShootSkull: 74.5 ahead, and its
                            // first step)
    boolean melee;          // in reach, the monster claws or bites instead
    int count;              // how many it fires at once
    double angles[MAX_SHOTS];   // each one's way off the straight line to
                                // the player, degrees (a mancubus's volleys,
                                // A_FatAttack1 to 3)
} shot_t;

static const shot_t shots[] =
{
    { A_TroopAttack, MT_TROOPSHOT,   1.5, true,  1, { 0.0 } },
    { A_HeadAttack,  MT_HEADSHOT,    1.5, true,  1, { 0.0 } },
    { A_BruisAttack, MT_BRUISERSHOT, 1.5, true,  1, { 0.0 } },
    { A_SkelMissile, MT_TRACER,      2.5, false, 1, { 0.0 } },
    { A_FatAttack1,  MT_FATSHOT,     1.5, false, 2, { 0.0, 11.25 } },
    { A_FatAttack2,  MT_FATSHOT,     1.5, false, 2, { 0.0, -22.5 } },
    { A_FatAttack3,  MT_FATSHOT,     1.5, false, 2, { -5.625, 5.625 } },
    { A_BspiAttack,  MT_ARACHPLAZ,   1.5, false, 1, { 0.0 } },
    { A_CyberAttack, MT_ROCKET,      1.5, false, 1, { 0.0 } },
    { A_SkullAttack, MT_SKULL,       0.0, false, 1, { 0.0 } },
    { A_PainAttack,  MT_SKULL,       4.7, false, 1, { 0.0 } },
};

typedef enum
{
    THREAT_FLYING,          // straight on, until the first wall
    THREAT_HOMING,          // a revenant's missile that turns
    THREAT_WOUND,           // a shot not fired yet
} threatkind_t;

typedef struct
{
    const mobj_t *mo;       // the projectile or lost soul; the monster
                            // winding up
    threatkind_t kind;
    const char *name;
    k_vec2_t at;
    double z, height;
    k_vec2_t velocity;      // map units per tic
    double vz;
    double speed;
    double radius;
    int life;               // FLYING: the tics it flies before it stops at
                            // a wall, K_THREAT_TICS if none is that near;
                            // WOUND: the tics it flies once fired
    int turn;               // HOMING: the first tic it turns
    int fires;              // WOUND: the tic it is fired
    const shot_t *shot;     // WOUND: what it fires
    double blast;           // its burst's reach (a rocket), 0 if none
    double cost;            // of a hit
    boolean lethal;         // a hit could kill
    boolean near;           // it could reach the player
    int seen;               // the leveltime it was first seen
    int expected;           // the leveltime the way taken expects it to hit,
                            // -1 if never; UNJUDGED (K_Threat_Expect)
} threat_t;

static threat_t threats[MAX_THREATS];
static int num_threats;
static int num_near;

// Last tic's: what hit the player is gone from this tic's.
static threat_t was[MAX_THREATS];
static int num_was;

static const mobj_t *player_mo;
static const player_t *player;
static k_vec2_t me;

static const char *hit_name;
static int hit_seen;
static int hit_foreseen;
static int hit_tic = -1;

static double ToMap(fixed_t value)
{
    return value / 65536.0;
}

static double Distance(k_vec2_t a, k_vec2_t b)
{
    return sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
}

// The larger of the x and y distances: how the engine measures a blast.
static double AxisDistance(k_vec2_t a, k_vec2_t b)
{
    return SDL_max(fabs(b.x - a.x), fabs(b.y - a.y));
}

static double Wrap(double angle)
{
    return atan2(sin(angle), cos(angle));
}

static const char *MissileName(int type)
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
        case MT_SKULL:          return "lost soul";
        default:                return "projectile";
    }
}

static double Health(void)
{
    return SDL_max(player->health, 1);
}

// Nothing hurts it for the whole time ahead.
static boolean Unhurt(void)
{
    return player->powers[pw_invulnerability] > K_THREAT_TICS || (player->cheats & CF_GODMODE);
}

// The health a hit for `damage` takes (P_DamageMobj): half on the easiest
// skill, and what the armour saves.
static double HealthLoss(double damage)
{
    if (gameskill == sk_baby)
    {
        damage = floor(damage / 2.0);
    }

    if (player->armortype)
    {
        damage -= SDL_min(floor(damage / (player->armortype == 1 ? 3.0 : 2.0)),
                          (double) player->armorpoints);
    }

    return damage;
}

static double LossCost(double loss)
{
    return loss * (1.0 + loss / Health());
}

// A direct hit: 1 to 8 times its damage (P_DamageMobj from PIT_CheckThing),
// and `extra` (a rocket's own burst, right beside the player).
static double HitCost(double damage, double extra, boolean *lethal)
{
    double cost = 0.0;
    int k, kills = 0;

    for (k = 1; k <= 8; ++k)
    {
        double loss = HealthLoss(k * damage + extra);

        cost += LossCost(loss);
        kills += loss >= Health();
    }

    *lethal = kills > 0;
    return (cost + KILL_COST * kills) / 8.0;
}

// Where a straight one is k tics from now.
static k_vec2_t Along(const threat_t *threat, double k)
{
    k_vec2_t at = { threat->at.x + threat->velocity.x * k,
                    threat->at.y + threat->velocity.y * k };

    return at;
}

static boolean Clear(k_vec2_t from, double from_z, k_vec2_t to, double to_z, double height)
{
    return K_Sight_Check(from.x, from.y, from_z + height * 0.5, to.x, to.y, to_z, to_z + height);
}

// The tics a straight one flies before it stops at a wall: the last tic
// its way is clear to, found by halving.
static int Life(const threat_t *threat)
{
    int low = 0, high = K_THREAT_TICS;

    if (Clear(threat->at, threat->z, Along(threat, high), threat->z + threat->vz * high,
              threat->height))
    {
        return high;
    }

    while (high - low > 1)
    {
        int middle = (low + high) / 2;

        if (Clear(threat->at, threat->z, Along(threat, middle),
                  threat->z + threat->vz * middle, threat->height))
        {
            low = middle;
        }
        else
        {
            high = middle;
        }
    }

    return low;
}

// Whether it could reach the player, wherever the player runs.
static boolean Near(const threat_t *threat)
{
    double reach = threat->radius + PLAYER_RADIUS + HIT_MARGIN + threat->blast;
    int k;

    if (threat->kind != THREAT_FLYING)
    {
        return Distance(threat->at, me) < (threat->speed + RUN_SPEED) * K_THREAT_TICS + reach;
    }

    for (k = 0; k <= threat->life; k += 2)
    {
        if (Distance(Along(threat, k), me) < RUN_SPEED * k + reach)
        {
            return true;
        }
    }

    return Distance(Along(threat, threat->life), me) < RUN_SPEED * threat->life + reach;
}

// One as it was last tic, by its mobj.
static const threat_t *WasThreat(const mobj_t *mo, boolean wound)
{
    int i;

    for (i = 0; i < num_was; ++i)
    {
        if (was[i].mo == mo && (was[i].kind == THREAT_WOUND) == wound)
        {
            return &was[i];
        }
    }

    return NULL;
}

static threat_t *NewThreat(const mobj_t *mo, threatkind_t kind)
{
    const threat_t *before = WasThreat(mo, kind == THREAT_WOUND);
    threat_t *threat;

    if (num_threats == MAX_THREATS)
    {
        return NULL;
    }

    threat = &threats[num_threats++];
    memset(threat, 0, sizeof(*threat));
    threat->mo = mo;
    threat->kind = kind;
    threat->seen = before != NULL ? before->seen : leveltime;
    threat->expected = UNJUDGED;
    return threat;
}

// Flying: a projectile, or a lost soul charging.
static void AddFlying(const mobj_t *mo, const char *name, double damage)
{
    threat_t *threat = NewThreat(mo, THREAT_FLYING);

    if (threat == NULL)
    {
        return;
    }

    threat->name = name;
    threat->at.x = ToMap(mo->x);
    threat->at.y = ToMap(mo->y);
    threat->z = ToMap(mo->z);
    threat->height = ToMap(mo->height);
    threat->velocity.x = ToMap(mo->momx);
    threat->velocity.y = ToMap(mo->momy);
    threat->vz = ToMap(mo->momz);
    threat->speed = sqrt(threat->velocity.x * threat->velocity.x
                       + threat->velocity.y * threat->velocity.y);
    threat->radius = ToMap(mo->radius);
    threat->blast = mo->type == MT_ROCKET ? BLAST : 0.0;
    threat->cost = HitCost(damage, threat->blast > 0.0 ? threat->blast - threat->radius : 0.0,
                           &threat->lethal);
    threat->life = K_THREAT_TICS;
}

// The shot a monster's states lead to before it goes back to chasing, and
// the tic it is fired; -1 if none comes within K_THREAT_TICS.
static int ShotIn(const mobj_t *mo, const shot_t **shot)
{
    int t = mo->tics, steps, s, i;

    if (mo->state == NULL || t < 0)
    {
        return -1;
    }

    for (s = mo->state->nextstate, steps = 0;
         s > S_NULL && s < NUMSTATES && steps < 16 && t <= K_THREAT_TICS; ++steps)
    {
        if (s == mo->info->seestate || s == mo->info->spawnstate)
        {
            return -1;
        }

        for (i = 0; i < arrlen(shots); ++i)
        {
            if (states[s].action.acv == shots[i].action)
            {
                *shot = &shots[i];
                return t;
            }
        }

        if (states[s].tics < 0)
        {
            return -1;
        }

        t += states[s].tics;
        s = states[s].nextstate;
    }

    return -1;
}

void K_Threat_Reset(void)
{
    num_threats = 0;
    num_near = 0;
    num_was = 0;
    hit_name = NULL;
    hit_tic = -1;
}

void K_Threat_Begin(const mobj_t *mo)
{
    memcpy(was, threats, num_threats * sizeof(*threats));
    num_was = num_threats;
    num_threats = 0;
    num_near = 0;
    player_mo = mo;
    player = mo->player;
    me.x = ToMap(mo->x);
    me.y = ToMap(mo->y);
}

void K_Threat_AddMissile(const mobj_t *mo)
{
    threat_t *threat;
    unsigned int when;

    // Not the player's own, nor the Icon of Sin's cubes: they fly through
    // walls to their spots and hurt no one.
    if (mo->target == player_mo || mo->type == MT_SPAWNSHOT || (mo->momx == 0 && mo->momy == 0))
    {
        return;
    }

    AddFlying(mo, MissileName(mo->type), mo->info->damage);

    if (!K_Threat_Homes(mo, player_mo) || num_threats == 0 || threats[num_threats - 1].mo != mo)
    {
        return;
    }

    threat = &threats[num_threats - 1];
    when = (unsigned int) (gametic + mo->tics - demostarttic);
    threat->kind = THREAT_HOMING;
    threat->turn = (when & 3) == 0 ? mo->tics : mo->tics + 2;
}

int K_Threat_Homes(const mobj_t *mo, const mobj_t *target_mo)
{
    // A_Tracer runs as each state starts, every 2 tics from mo->tics on,
    // and turns the missile only on tics that are a multiple of 4.
    return mo->type == MT_TRACER && mo->tracer == target_mo && mo->state != NULL
        && (mo->state - states == S_TRACER || mo->state - states == S_TRACER2)
        && !((unsigned int) (gametic + mo->tics - demostarttic) & 1);
}

void K_Threat_AddMonster(const mobj_t *mo, int visible)
{
    const shot_t *shot;
    threat_t *threat;
    k_vec2_t at = { ToMap(mo->x), ToMap(mo->y) };
    double distance = Distance(me, at);
    int fires;

    if (mo->flags & MF_SKULLFLY)
    {
        if (mo->momx != 0 || mo->momy != 0)
        {
            AddFlying(mo, MissileName(MT_SKULL), mo->info->damage);
        }

        return;
    }

    if (!visible || mo->target != player_mo || (fires = ShotIn(mo, &shot)) < 0
     || (shot->melee && distance < MELEE_RANGE)
     || (threat = NewThreat(mo, THREAT_WOUND)) == NULL)
    {
        return;
    }

    threat->name = MissileName(shot->missile);
    threat->at = at;
    threat->z = ToMap(mo->z) + 32.0;
    threat->height = ToMap(mobjinfo[shot->missile].height);
    threat->speed = shot->missile == MT_SKULL ? SKULL_SPEED
                                              : ToMap(mobjinfo[shot->missile].speed);
    threat->radius = ToMap(mobjinfo[shot->missile].radius);
    threat->fires = SDL_max(fires, 1);
    threat->shot = shot;
    threat->life = (int) (distance / SDL_max(threat->speed, 1.0)) + WOUND_PAST;
    threat->blast = shot->missile == MT_ROCKET ? BLAST : 0.0;
    threat->cost = HitCost(mobjinfo[shot->missile].damage,
                           threat->blast > 0.0 ? threat->blast - threat->radius : 0.0,
                           &threat->lethal);
}

// Whether it is still flying this tic.
static boolean StillFlying(const mobj_t *mo)
{
    int i;

    for (i = 0; i < num_threats; ++i)
    {
        if (threats[i].mo == mo && threats[i].kind != THREAT_WOUND)
        {
            return true;
        }
    }

    return false;
}

void K_Threat_End(void)
{
    int i;

    for (i = 0; i < num_threats; ++i)
    {
        threat_t *threat = &threats[i];

        if (threat->kind == THREAT_FLYING
         && Distance(threat->at, me) < (threat->speed + RUN_SPEED) * K_THREAT_TICS
                                     + threat->radius + PLAYER_RADIUS + threat->blast)
        {
            threat->life = Life(threat);
        }

        threat->near = Near(threat);
        num_near += threat->near;
    }

    // What hit the player: one that was about to reach it, gone now.
    for (i = 0; i < num_was; ++i)
    {
        const threat_t *threat = &was[i];
        k_vec2_t next = Along(threat, 1.0);
        double reach = threat->radius + PLAYER_RADIUS + 8.0;

        if (threat->kind != THREAT_WOUND && fabs(next.x - me.x) < reach
         && fabs(next.y - me.y) < reach && !StillFlying(threat->mo))
        {
            hit_name = threat->name;
            hit_seen = leveltime - threat->seen;
            hit_foreseen = threat->expected == UNJUDGED ? -1
                         : threat->expected >= 0
                           && abs(threat->expected - leveltime) <= FORESEEN_TICS;
            hit_tic = leveltime;
        }
    }
}

int K_Threat_Count(void)
{
    return num_near;
}

static boolean Overlaps(k_vec2_t at, double radius, k_vec2_t player_at)
{
    double reach = radius + PLAYER_RADIUS + HIT_MARGIN;

    return fabs(at.x - player_at.x) < reach && fabs(at.y - player_at.y) < reach;
}

// A rocket bursting at `at`, the player at player_at then: the blast less the
// distance, if it can see the player (P_RadiusAttack).
static int Burst(const threat_t *threat, k_vec2_t at, double z, k_vec2_t player_at,
                 double floor, int tic, double *cost, boolean *lethal)
{
    double damage = threat->blast - SDL_max(0.0, AxisDistance(at, player_at) - PLAYER_RADIUS);
    double loss;

    if (damage <= 0.0
     || !K_Sight_Check(player_at.x, player_at.y, floor + PLAYER_HEIGHT * 0.75, at.x, at.y, z,
                       z + threat->height))
    {
        return 0;
    }

    loss = HealthLoss(damage);
    *lethal = loss >= Health();
    *cost = LossCost(loss) + (*lethal ? KILL_COST : 0.0);
    return tic;
}

// The tic a straight one hits the player along the way (half steps checked
// too: the fast ones), or 0.
static int FlyingHit(const threat_t *threat, const k_vec2_t *path, int tics, double floor,
                     double *cost, boolean *lethal)
{
    int end = SDL_min(threat->life, tics);
    int k;

    for (k = 1; k <= end; ++k)
    {
        double z = threat->z + threat->vz * k;

        if ((Overlaps(Along(threat, k), threat->radius, path[k - 1])
             || Overlaps(Along(threat, k - 0.5), threat->radius, path[k - 1]))
         && z < floor + PLAYER_HEIGHT + HIT_RISE && z + threat->height > floor - HIT_RISE)
        {
            *cost = threat->cost;
            *lethal = threat->lethal;
            return k;
        }
    }

    if (threat->blast > 0.0 && threat->life < tics)
    {
        k = threat->life;
        return Burst(threat, Along(threat, k), threat->z + threat->vz * k, path[k], floor,
                     k + 1, cost, lethal);
    }

    return 0;
}

// A revenant's missile, turning toward the player on its tics: gone at a
// wall, looked for every 4 tics and before a hit.
static int HomingHit(const threat_t *threat, const k_vec2_t *path, int tics, double *cost,
                     boolean *lethal)
{
    k_vec2_t at = threat->at, checked = threat->at, velocity = threat->velocity;
    double angle = atan2(velocity.y, velocity.x);
    int k;

    for (k = 1; k <= tics; ++k)
    {
        boolean hits;

        at.x += velocity.x;
        at.y += velocity.y;
        hits = Overlaps(at, threat->radius, path[k - 1]);

        if (k % 4 == 0 || hits)
        {
            if (!Clear(checked, threat->z, at, threat->z, threat->height))
            {
                return 0;
            }

            checked = at;
        }

        if (hits)
        {
            *cost = threat->cost;
            *lethal = threat->lethal;
            return k;
        }

        if (k >= threat->turn && (k - threat->turn) % 4 == 0)
        {
            double exact = atan2(path[k - 1].y - at.y, path[k - 1].x - at.x);
            double turn = Wrap(exact - angle);

            angle = fabs(turn) <= TRACE_TURN ? exact : angle + (turn > 0.0 ? TRACE_TURN
                                                                           : -TRACE_TURN);
            velocity.x = cos(angle) * threat->speed;
            velocity.y = sin(angle) * threat->speed;
        }
    }

    return 0;
}

// A shot not fired yet: aimed at where the way puts the player then, each
// of a volley off that line by its angle; the hits add up.
static int WoundHit(const threat_t *threat, const k_vec2_t *path, int tics, double *cost,
                    boolean *lethal)
{
    k_vec2_t aim;
    double straight;
    int first = 0, s, k, end;

    if (threat->fires > tics)
    {
        return 0;
    }

    aim = path[threat->fires - 1];

    if (Distance(threat->at, aim) < 1.0)
    {
        return 0;
    }

    straight = atan2(aim.y - threat->at.y, aim.x - threat->at.x);
    end = SDL_min(tics, threat->fires + threat->life);

    for (s = 0; s < threat->shot->count; ++s)
    {
        double angle = straight + threat->shot->angles[s] * K_PI / 180.0;
        k_vec2_t velocity = { cos(angle) * threat->speed, sin(angle) * threat->speed };
        k_vec2_t at = { threat->at.x + velocity.x * threat->shot->launch,
                        threat->at.y + velocity.y * threat->shot->launch };

        for (k = threat->fires; k <= end; ++k)
        {
            if (k > threat->fires)
            {
                at.x += velocity.x;
                at.y += velocity.y;
            }

            if (Overlaps(at, threat->radius, path[k - 1]))
            {
                *cost += threat->cost;
                *lethal |= threat->lethal;
                first = first == 0 ? k : SDL_min(first, k);
                break;
            }
        }
    }

    return first;
}

// The tic it hits the player along the way, or 0; with what the hit costs.
static int ThreatHit(const threat_t *threat, const k_vec2_t *path, int tics, double floor,
                     double *cost, boolean *lethal)
{
    *cost = 0.0;
    *lethal = false;

    switch (threat->kind)
    {
        case THREAT_FLYING:
            return FlyingHit(threat, path, tics, floor, cost, lethal);

        case THREAT_HOMING:
            return HomingHit(threat, path, tics, cost, lethal);

        default:
            return WoundHit(threat, path, tics, cost, lethal);
    }
}

double K_Threat_PathCost(const k_vec2_t *path, int tics, double floor, k_threathit_t *worst)
{
    double total = 0.0;
    int i;

    if (worst != NULL)
    {
        memset(worst, 0, sizeof(*worst));
    }

    if (player == NULL || Unhurt())
    {
        return 0.0;
    }

    tics = SDL_min(tics, K_THREAT_TICS);

    for (i = 0; i < num_threats; ++i)
    {
        const threat_t *threat = &threats[i];
        double cost;
        boolean lethal;
        int tic;

        if (!threat->near
         || (tic = ThreatHit(threat, path, tics, floor, &cost, &lethal)) <= 0)
        {
            continue;
        }

        if (tic > URGENT_TICS)
        {
            cost *= 1.0 - (1.0 - LATE_SHARE) * (tic - URGENT_TICS)
                                             / (double) (K_THREAT_TICS - URGENT_TICS);
        }

        total += cost;

        if (worst != NULL && cost > worst->cost)
        {
            worst->name = threat->name;
            worst->tic = tic;
            worst->cost = cost;
            worst->lethal = lethal;
        }
    }

    return total;
}

void K_Threat_Expect(const k_vec2_t *path, int tics, double floor)
{
    int i;

    tics = SDL_min(tics, K_THREAT_TICS);

    for (i = 0; i < num_threats; ++i)
    {
        threat_t *threat = &threats[i];
        double cost;
        boolean lethal;
        int tic = threat->near ? ThreatHit(threat, path, tics, floor, &cost, &lethal) : 0;

        threat->expected = tic > 0 ? leveltime + tic : -1;
    }
}

const char *K_Threat_HitBy(int *seen, int *foreseen)
{
    if (hit_tic != leveltime || hit_name == NULL)
    {
        return NULL;
    }

    *seen = hit_seen;
    *foreseen = hit_foreseen;
    return hit_name;
}
