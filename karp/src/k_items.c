//
// DESCRIPTION:
//	Karp items: every item lying in the level, told by its sprite as
//	P_TouchSpecialThing tells it, and what picking it up would be worth
//	now - what P_TouchSpecialThing would give: the health below the most
//	it may reach, the armour that is better than what is worn (a third of
//	the damage taken off for green, a half for blue), the ammo below the
//	most the player may carry (bullets cheap, rockets dear; a quarter for
//	ammo no weapon it holds fires), a weapon it does not have - twice as
//	much while it has nothing better than the pistol. Ammo below its
//	comfortable level is worth more a round, the less there is, and any
//	ammo there is room for is worth a short detour, however much the player
//	has. Nothing it would not pick up counts. With no monster left alive,
//	weapons, ammo and the power-ups for fighting are worth nothing.
//
//	The worth, weighted by need, becomes how far out of its way the player
//	may go for the item (plan, 7.5), and the more an item is worth, the
//	more each point pays for: a step aside for a clip, a long walk for blue
//	armour, a soulsphere or a first chaingun (the user, Doom II MAP03).
//	Health weighs more when LOW - the lower, the more - and pays for any
//	detour at all when CRITICAL (the user: a minute or two back to the
//	level's start beats dying), and between fights, to keep near 100, a
//	long detour, any when LOW. Green or blue armour, while armour is LOW, a
//	long detour too. Ammo weighs three times when LOW, and the
//	ammo running out pays for any detour at all, unguarded. Items
//	lying together are picked up on one trip, so each counts with those
//	beside it (Doom II MAP02: three stimpacks side by side, 30 health on
//	one trip, were left at 25 health as worth 10 each).
//
//	Some items are worth breaking a fight off for, close by (urgent):
//	health when LOW or CRITICAL (not a lone health bonus), a weapon the
//	player lacks, and the ammo running out - for a strong weapon while
//	firepower is LOW (k_condition.h), any while ammo is - from ammo,
//	weapons and backpacks; the health, and the ammo when ammo or firepower
//	is LOW, however far (escape). A
//	weapon better than the pistol, at a pistol start, goes before other
//	items (first).
//
//	What the monsters guarding an item would cost in health is taken off,
//	weighed as health is (the user, MAP02: going for four stimpacks started
//	a fight a lonely medikit would not have). Nothing is worth more health
//	than the player has left, unless health is CRITICAL. Only the guards
//	count, not monsters on the way there (the user: those are met and fought
//	anyway, and move about), and of the guards only those beyond the
//	monsters that see the player where it stands, fought anyway too; with
//	the pistol alone, a weapon's guards count a quarter.
//
//	An item no spot the player can stand at touches - under a tree, a big
//	column - is out of reach; under a barrel, until the barrel goes.
//
//	Items are found by place and sprite each tic, so each keeps its index
//	for the level, and an item no longer found has been picked up.
//

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "deh_misc.h"
#include "doomdef.h"
#include "doomstat.h"
#include "p_local.h"

#include "k_combat.h"
#include "k_condition.h"
#include "k_graph.h"
#include "k_items.h"

// Map units of detour a point of worth pays for, and a whole
// UNITS_PER_POINT more for each BIG_WORTH the item is worth (the user,
// Doom II MAP03: blue armour at 0% was passed by 1424 units off, any
// detour then capped at 400). Never more than REACH_MOST, about five
// seconds' run, but any detour at all for health when CRITICAL.
#define UNITS_PER_POINT 8.0
#define BIG_WORTH 25.0
#define REACH_MOST 3000.0
#define REACH_ANY 1.0e6

// Ammo there is room for is worth having, however much the player has (the
// user: the clips and shotguns of the zombiemen and shotgun guys just killed
// were left lying; Doom II MAP31, the SS's clips were picked up only when run
// over by chance): ammo for a weapon held, a weapon, a backpack pays for
// ROOM_REACH of detour at least, DROP_REACH when a monster dropped it (the
// user: killing monsters is an investment their ammo pays back; MAP31, where
// the SS are most of the ammo, their clips lay 500 to 800 off to the sides of
// the route, a detour of about 1000, and were left while ammo ran LOW). Less
// the more it is guarded, none from ROOM_DANGER health at risk (MAP31: clips
// with 1.5 at risk were left as guarded, then at 10 - one SS that could see
// them - the clips of a room just cleared). The ammo running out (Wanted;
// while ammo is LOW, any for a weapon held) pays for any detour at all, as
// health does when CRITICAL, unguarded as above (the user, MAP31: ammo LOW for
// most of the level, clips left lying in rooms cleared before; near dry, go
// and get them all).
#define ROOM_REACH 512.0
#define DROP_REACH 1024.0
#define ROOM_DANGER 20.0

// The same for health, to keep near 100 between fights (the user, Doom II
// MAP10: at 40 health the bot went on into fights with hitscanners, stimpacks
// lying unguarded in rooms cleared before): while monsters are left, health
// below 100, an item giving URGENT_HEALTH at least with those beside it pays
// for HEALTH_REACH of detour at least, less the more it is guarded; when LOW
// or CRITICAL, any detour at all, unguarded.
#define HEALTH_REACH 1024.0

// The same for armour, a little less eager (the user, E1M5: green armour in
// plain sight by the start, at 0%, was left; worth 33, it paid for some 470
// units of detour with a sleeping shotgun guy near): while monsters are left
// and armour is LOW, green or blue armour pays for ARMOUR_REACH of detour at
// least, less the more it is guarded. Never any detour at all, and not
// armour bonuses.
#define ARMOUR_REACH 1024.0

// Health is worth breaking a fight off for when CRITICAL only if it gives
// URGENT_HEALTH at least, with what lies beside it: running from a fight for
// a health bonus gains nothing.
#define URGENT_HEALTH 10.0

// Ammo below its comfortable level is worth more a round, the less there
// is: up to SCARCE times as much at none, the first rounds the shots that
// keep the player fighting (the user, Doom II MAP03: with six shells left
// - not LOW - the bot left the shotguns the shotgun guys dropped, and met
// the imps with the pistol).
#define SCARCE 10.0

static const int ammo_comfort[NUMAMMO] = { 80, 25, 100, 10 };   // clip, shell, cell, misl

// Health weighs LOW_WEIGHT at the LOW mark (75), rising to LOW_WEIGHT_MOST
// at the CRITICAL one (25); CRITICAL_WEIGHT below.
#define LOW_WEIGHT 3.0
#define LOW_WEIGHT_MOST 6.0
#define CRITICAL_WEIGHT 12.0

// Items this close to one another are picked up on one trip.
#define TOGETHER 64.0

// The danger where the player stands, looked at every HERE_TICS: the
// monsters that can see it there are fought anyway, so an item's guards
// count only for the danger beyond that (the user, Doom II MAP04: the
// shotgun lying in the opening fight was given up as guarded by the
// shotgun guys being fought). A weapon not held, while nothing better than
// the pistol is, counts its guards a PISTOL_START_GUARDS share: with the
// pistol alone, it is what makes those fights cheaper (the user: run for
// it).
#define HERE_TICS 8
#define PISTOL_START_GUARDS 0.25

// An item's guards and pickup spot are looked at every LOOK_TICS, the items
// spread over the tics.
#define LOOK_TICS TICRATE

#define BUCKETS 4096

typedef struct
{
    k_item_t item;
    fixed_t x, y;
    spritenum_t sprite;
    double z;               // the floor it lies on
    double guards;          // what its guards would cost, all of them
    int seen;               // the tic it was last found, plus one
    int looked_tic;         // when its guards and spot were last looked at
    int next;               // in its bucket, -1 at the end
    double weighted;        // its worth, weighted by need
    double most;            // the longest detour its need allows
    boolean running_out;    // it gives the ammo running out (RunningOut)
} entry_t;

static entry_t *entries;
static int num_entries;
static int capacity;
static int buckets[BUCKETS];
static double danger_here;

// P_GiveAmmo's clip sizes, and what a round of each is worth: clip,
// shell, cell, rocket.
static const int clip_ammo[NUMAMMO] = { 10, 4, 20, 1 };
static const double ammo_worth[NUMAMMO] = { 0.15, 0.8, 0.2, 2.0 };

// What having a weapon is worth, besides its ammo.
static const double weapon_worth[NUMWEAPONS] =
{
    0.0,    // fist
    0.0,    // pistol
    40.0,   // shotgun
    40.0,   // chaingun
    40.0,   // rocket launcher
    60.0,   // plasma rifle
    80.0,   // BFG
    15.0,   // chainsaw
    60.0,   // super shotgun
};

static double ToMap(fixed_t value)
{
    return value / 65536.0;
}

void K_Items_Reset(void)
{
    int i;

    num_entries = 0;
    danger_here = 0.0;

    for (i = 0; i < BUCKETS; ++i)
    {
        buckets[i] = -1;
    }
}

// The items that count, by sprite: their name and kind. Keys and the
// beta items are not among them.
static boolean Describe(spritenum_t sprite, const char **name, k_itemkind_t *kind)
{
    switch (sprite)
    {
        case SPR_STIM: *name = "stimpack";        *kind = K_ITEM_HEALTH; return true;
        case SPR_MEDI: *name = "medikit";         *kind = K_ITEM_HEALTH; return true;
        case SPR_BON1: *name = "health bonus";    *kind = K_ITEM_HEALTH; return true;
        case SPR_SOUL: *name = "soulsphere";      *kind = K_ITEM_HEALTH; return true;
        case SPR_MEGA: *name = "megasphere";      *kind = K_ITEM_HEALTH; return true;
        case SPR_PSTR: *name = "berserk";         *kind = K_ITEM_HEALTH; return true;
        case SPR_ARM1: *name = "green armour";    *kind = K_ITEM_ARMOUR; return true;
        case SPR_ARM2: *name = "blue armour";     *kind = K_ITEM_ARMOUR; return true;
        case SPR_BON2: *name = "armour bonus";    *kind = K_ITEM_ARMOUR; return true;
        case SPR_CLIP: *name = "clip";            *kind = K_ITEM_AMMO;   return true;
        case SPR_AMMO: *name = "box of bullets";  *kind = K_ITEM_AMMO;   return true;
        case SPR_SHEL: *name = "shells";          *kind = K_ITEM_AMMO;   return true;
        case SPR_SBOX: *name = "box of shells";   *kind = K_ITEM_AMMO;   return true;
        case SPR_ROCK: *name = "rocket";          *kind = K_ITEM_AMMO;   return true;
        case SPR_BROK: *name = "box of rockets";  *kind = K_ITEM_AMMO;   return true;
        case SPR_CELL: *name = "cell charge";     *kind = K_ITEM_AMMO;   return true;
        case SPR_CELP: *name = "cell pack";       *kind = K_ITEM_AMMO;   return true;
        case SPR_BPAK: *name = "backpack";        *kind = K_ITEM_WEAPON; return true;
        case SPR_SHOT: *name = "shotgun";         *kind = K_ITEM_WEAPON; return true;
        case SPR_SGN2: *name = "super shotgun";   *kind = K_ITEM_WEAPON; return true;
        case SPR_MGUN: *name = "chaingun";        *kind = K_ITEM_WEAPON; return true;
        case SPR_LAUN: *name = "rocket launcher"; *kind = K_ITEM_WEAPON; return true;
        case SPR_PLAS: *name = "plasma rifle";    *kind = K_ITEM_WEAPON; return true;
        case SPR_BFUG: *name = "BFG";             *kind = K_ITEM_WEAPON; return true;
        case SPR_CSAW: *name = "chainsaw";        *kind = K_ITEM_WEAPON; return true;
        case SPR_PINV: *name = "invulnerability"; *kind = K_ITEM_POWER;  return true;
        case SPR_PINS: *name = "invisibility";    *kind = K_ITEM_POWER;  return true;
        case SPR_SUIT: *name = "radiation suit";  *kind = K_ITEM_POWER;  return true;
        case SPR_PMAP: *name = "computer map";    *kind = K_ITEM_POWER;  return true;
        case SPR_PVIS: *name = "light amplifier"; *kind = K_ITEM_POWER;  return true;
        default:       return false;
    }
}

static unsigned int Hash(fixed_t x, fixed_t y)
{
    return (((unsigned int) (x >> FRACBITS) * 73856093u)
          ^ ((unsigned int) (y >> FRACBITS) * 19349663u)) & (BUCKETS - 1);
}

// The entry for an item found this tic: the one at its place with its
// sprite not found yet this tic (items can lie stacked), else a new one.
static entry_t *Find(const mobj_t *mo, int stamp)
{
    unsigned int bucket = Hash(mo->x, mo->y);
    entry_t *entry;
    int i;

    for (i = buckets[bucket]; i >= 0; i = entries[i].next)
    {
        if (entries[i].x == mo->x && entries[i].y == mo->y && entries[i].sprite == mo->sprite
         && entries[i].seen != stamp)
        {
            return &entries[i];
        }
    }

    if (num_entries == capacity)
    {
        capacity = SDL_max(256, capacity * 2);
        entries = realloc(entries, capacity * sizeof(*entries));
    }

    entry = &entries[num_entries];
    memset(entry, 0, sizeof(*entry));
    entry->x = mo->x;
    entry->y = mo->y;
    entry->sprite = mo->sprite;
    entry->item.at.x = ToMap(mo->x);
    entry->item.at.y = ToMap(mo->y);
    entry->item.dropped = (mo->flags & MF_DROPPED) != 0;
    entry->looked_tic = -2 * LOOK_TICS;
    Describe(mo->sprite, &entry->item.name, &entry->item.kind);
    entry->next = buckets[bucket];
    buckets[bucket] = num_entries++;
    return entry;
}

// Whether the player has a weapon that fires the ammo.
static boolean Usable(const player_t *player, ammotype_t ammo)
{
    int w;

    for (w = 0; w < NUMWEAPONS; ++w)
    {
        if (player->weaponowned[w] && weaponinfo[w].ammo == ammo)
        {
            return true;
        }
    }

    return false;
}

// An ammo item's ammo and clips of it (P_TouchSpecialThing); false if it
// is no ammo.
static boolean AmmoItem(spritenum_t sprite, ammotype_t *ammo, int *clips)
{
    switch (sprite)
    {
        case SPR_CLIP: *ammo = am_clip;  *clips = 1; return true;
        case SPR_AMMO: *ammo = am_clip;  *clips = 5; return true;
        case SPR_SHEL: *ammo = am_shell; *clips = 1; return true;
        case SPR_SBOX: *ammo = am_shell; *clips = 5; return true;
        case SPR_ROCK: *ammo = am_misl;  *clips = 1; return true;
        case SPR_BROK: *ammo = am_misl;  *clips = 5; return true;
        case SPR_CELL: *ammo = am_cell;  *clips = 1; return true;
        case SPR_CELP: *ammo = am_cell;  *clips = 5; return true;
        default:       return false;
    }
}

// A weapon item's weapon; false if it is no weapon.
static boolean WeaponItem(spritenum_t sprite, weapontype_t *weapon)
{
    switch (sprite)
    {
        case SPR_SHOT: *weapon = wp_shotgun;      return true;
        case SPR_SGN2: *weapon = wp_supershotgun; return true;
        case SPR_MGUN: *weapon = wp_chaingun;     return true;
        case SPR_LAUN: *weapon = wp_missile;      return true;
        case SPR_PLAS: *weapon = wp_plasma;       return true;
        case SPR_BFUG: *weapon = wp_bfg;          return true;
        case SPR_CSAW: *weapon = wp_chainsaw;     return true;
        default:       return false;
    }
}

// Ammo running out that matters: firepower is LOW (k_condition.h: no strong
// weapon held can fire), and the ammo fires a strong weapon held (the user,
// Doom II MAP03: out of shells, with bullets left for the pistol - ammo not
// LOW - the bot walked past the shotguns the shotgun guys dropped, and up a
// lift to the imps with the pistol). Not the best weapon's ammo while
// another strong weapon fires (MAP10: cells below a BFG shot, the shotgun
// loaded, were gone for 6467 units out of the way, any detour paying).
static boolean Wanted(const player_t *player, ammotype_t ammo)
{
    int w;

    if (ammo == am_noammo || K_Condition_Get()->firepower != K_NEED_LOW)
    {
        return false;
    }

    for (w = 0; w < NUMWEAPONS; ++w)
    {
        if (player->weaponowned[w] && K_Condition_Strong(w) && weaponinfo[w].ammo == ammo)
        {
            return true;
        }
    }

    return false;
}

// P_GiveAmmo: clips of ammo (0 for half of one), doubled on the easiest and
// hardest skills, halved when dropped, up to the most the player carries.
// Each round below the comfortable level is worth up to SCARCE times as
// much, the more the fewer there were before it: 1 + (SCARCE - 1) * (1 -
// before / comfortable), added up over the rounds.
static double AmmoWorth(const player_t *player, ammotype_t ammo, int clips, boolean dropped)
{
    int num = clips > 0 ? clips * clip_ammo[ammo] : clip_ammo[ammo] / 2;
    double comfort = ammo_comfort[ammo], scarce = 0.0;
    int have = player->ammo[ammo], below;

    if (ammo == am_noammo || player->ammo[ammo] >= player->maxammo[ammo])
    {
        return 0.0;
    }

    if (gameskill == sk_baby || gameskill == sk_nightmare)
    {
        num <<= 1;
    }

    if (dropped)
    {
        num = SDL_max(num >> 1, 1);
    }

    num = SDL_min(num, player->maxammo[ammo] - player->ammo[ammo]);

    if (!Usable(player, ammo))
    {
        return num * ammo_worth[ammo] * 0.25;
    }

    // The rounds from `have` up to `have + below`, below the comfortable
    // level: sum of (1 - k / comfort) over them.
    below = SDL_max(0, SDL_min(num, ammo_comfort[ammo] - have));

    if (below > 0)
    {
        scarce = below - (2.0 * have + below - 1.0) * below / (2.0 * comfort);
    }

    return (num + (SCARCE - 1.0) * scarce) * ammo_worth[ammo];
}

// Nothing better than the pistol: a pistol start, so far (the chainsaw is
// no better at a distance).
static boolean PistolOnly(const player_t *player)
{
    return !player->weaponowned[wp_shotgun] && !player->weaponowned[wp_chaingun]
        && !player->weaponowned[wp_missile] && !player->weaponowned[wp_plasma]
        && !player->weaponowned[wp_bfg] && !player->weaponowned[wp_supershotgun];
}

// A weapon not held is worth twice as much while the player has nothing
// better than the pistol (the user, Doom II MAP03: a chaingun lay by, and
// the bot fought on with the pistol).
static double WeaponWorth(const player_t *player, weapontype_t weapon, boolean dropped)
{
    double worth = player->weaponowned[weapon] ? 0.0 : weapon_worth[weapon];

    if (PistolOnly(player))
    {
        worth *= 2.0;
    }

    return worth + AmmoWorth(player, weaponinfo[weapon].ammo, 2, dropped);
}

// Whether the item gives ammo the player can fire: ammo for a weapon held, a
// weapon, a backpack.
static boolean GivesAmmo(const player_t *player, const entry_t *entry)
{
    ammotype_t ammo;
    weapontype_t weapon;
    int clips;

    if (AmmoItem(entry->sprite, &ammo, &clips))
    {
        return Usable(player, ammo);
    }

    return (WeaponItem(entry->sprite, &weapon) && weapon != wp_chainsaw)
        || entry->sprite == SPR_BPAK;
}

// P_GiveArmor: only armour better than what is worn is taken, and it takes
// a third (green) or a half (blue) of the damage.
static double ArmourWorth(const player_t *player, int armour_class)
{
    int points = armour_class * 100;

    if (player->armorpoints >= points)
    {
        return 0.0;
    }

    return (points - player->armorpoints) * (armour_class == 1 ? 1.0 / 3.0 : 0.5);
}

static double Worth(const player_t *player, const entry_t *entry, boolean fights)
{
    int health = player->health;
    boolean dropped = entry->item.dropped;
    double ammo = 0.0;
    ammotype_t ammo_type;
    weapontype_t weapon;
    int a, clips;

    switch (entry->sprite)
    {
        // P_GiveBody: up to 100.
        case SPR_STIM:
            return SDL_max(0, SDL_min(10, 100 - health));

        case SPR_MEDI:
            return SDL_max(0, SDL_min(25, 100 - health));

        case SPR_BON1:
            return health < deh_max_health ? 1.0 : 0.0;

        case SPR_SOUL:
            return SDL_max(0, SDL_min(deh_soulsphere_health, deh_max_soulsphere - health));

        case SPR_MEGA:
            return SDL_max(0, deh_megasphere_health - health) + ArmourWorth(player, 2);

        case SPR_PSTR:
            return SDL_max(0, 100 - health) + (player->powers[pw_strength] ? 0.0 : 10.0);

        case SPR_ARM1:
            return ArmourWorth(player, deh_green_armor_class);

        case SPR_ARM2:
            return ArmourWorth(player, deh_blue_armor_class);

        case SPR_BON2:
            return player->armorpoints < deh_max_armor ? 1.0 / 3.0 : 0.0;

        case SPR_SUIT:
            return 10.0;

        case SPR_PMAP:
        case SPR_PVIS:
            return 0.0;

        default:
            break;
    }

    // The rest is for fighting.
    if (!fights)
    {
        return 0.0;
    }

    if (AmmoItem(entry->sprite, &ammo_type, &clips))
    {
        return AmmoWorth(player, ammo_type, clips, dropped);
    }

    if (WeaponItem(entry->sprite, &weapon))
    {
        return WeaponWorth(player, weapon, dropped);
    }

    switch (entry->sprite)
    {
        case SPR_PINV: return 30.0;
        case SPR_PINS: return 10.0;

        case SPR_BPAK:
            for (a = 0; a < NUMAMMO; ++a)
            {
                ammo += AmmoWorth(player, a, 1, false);
            }

            return (player->backpack ? 0.0 : 25.0) + ammo;

        default:
            return 0.0;
    }
}

// Whether the item gives the ammo that is running out (Wanted; while ammo is
// LOW, any for a weapon held): ammo, a weapon, a backpack. A strong weapon,
// while firepower is LOW, held or not: it brings its ammo - but for a pistol
// start, where weapons go first anyway (k_items.h: first).
static boolean RunningOut(const player_t *player, const entry_t *entry, boolean low)
{
    ammotype_t ammo;
    weapontype_t weapon;
    int clips;

    if (AmmoItem(entry->sprite, &ammo, &clips))
    {
        return (low && Usable(player, ammo)) || Wanted(player, ammo);
    }

    if (WeaponItem(entry->sprite, &weapon))
    {
        return weapon != wp_chainsaw
            && (low || (K_Condition_Strong(weapon) && !PistolOnly(player)
                        && K_Condition_Get()->firepower == K_NEED_LOW));
    }

    if (entry->sprite != SPR_BPAK)
    {
        return false;
    }

    if (low)
    {
        return true;
    }

    for (ammo = 0; ammo < NUMAMMO; ++ammo)
    {
        if (Wanted(player, ammo))
        {
            return true;
        }
    }

    return false;
}

// Worth breaking a fight off for, close by (the user, Doom II MAP03: the
// chaingun a chaingunner dropped lay near, and the bot fought on with the
// pistol): health when LOW or CRITICAL, a weapon the player lacks, and the
// ammo that is running out, from ammo, a weapon or a backpack. Health LOW or
// CRITICAL, and the ammo when ammo is LOW, are worth leaving a fight for
// however far (escape): the bot cannot tell a fight it will win from one it
// will not - imps at 10 health, a dozen hitscanners at 50 - so it plays safe,
// and tops up (the user, Doom II MAP10: at 43 health it fought on in a room
// of hitscanners, more coming, stimpacks lying in rooms cleared before, and
// died).
static boolean Urgent(const player_t *player, const entry_t *entry,
                      const k_conditionstatus_t *condition)
{
    weapontype_t weapon;

    if (entry->item.worth <= 0.0)
    {
        return false;
    }

    switch (entry->item.kind)
    {
        case K_ITEM_HEALTH:
            return condition->health != K_NEED_OK;

        case K_ITEM_AMMO:
        case K_ITEM_WEAPON:
            return (WeaponItem(entry->sprite, &weapon) && !player->weaponowned[weapon])
                || entry->running_out;

        default:
            return false;
    }
}

void K_Items_Tick(void)
{
    const player_t *player = &players[consoleplayer];
    const k_conditionstatus_t *condition = K_Condition_Get();
    boolean fights = K_Combat_GetStatus()->alive > 0;
    double health_weight, ammo_weight, armour_weight;
    double health_reach;
    const thinker_t *thinker;
    int stamp = leveltime + 1;
    int i;

    if (player->mo == NULL)
    {
        return;
    }

    for (thinker = thinkercap.next; thinker != &thinkercap; thinker = thinker->next)
    {
        const mobj_t *mo = (const mobj_t *) thinker;
        const char *name;
        k_itemkind_t kind;

        if (thinker->function.acp1 == (actionf_p1) P_MobjThinker && (mo->flags & MF_SPECIAL)
         && Describe(mo->sprite, &name, &kind))
        {
            entry_t *entry = Find(mo, stamp);

            entry->seen = stamp;
            entry->z = ToMap(mo->z);
        }
    }

    health_weight = condition->health == K_NEED_CRITICAL ? CRITICAL_WEIGHT
                  : condition->health == K_NEED_LOW
                  ? LOW_WEIGHT + (LOW_WEIGHT_MOST - LOW_WEIGHT)
                                 * SDL_max(0.0, SDL_min(1.0, (75.0 - player->health) / 50.0))
                  : 1.0;
    if (leveltime % HERE_TICS == 0)
    {
        danger_here = K_Combat_Danger(ToMap(player->mo->x), ToMap(player->mo->y),
                                      ToMap(player->mo->floorz));
    }

    health_reach = condition->health == K_NEED_CRITICAL ? REACH_ANY : REACH_MOST;
    armour_weight = condition->health != K_NEED_OK ? 2.0 : 1.0;
    ammo_weight = condition->ammo == K_NEED_LOW ? 3.0 : 1.0;

    for (i = 0; i < num_entries; ++i)
    {
        entry_t *entry = &entries[i];
        k_item_t *item = &entry->item;
        double weight = 1.0;
        weapontype_t weapon;

        item->present = entry->seen == stamp;
        item->worth = item->present ? Worth(player, entry, fights) : 0.0;
        entry->running_out = item->worth > 0.0
                          && RunningOut(player, entry, condition->ammo == K_NEED_LOW);
        item->urgent = Urgent(player, entry, condition);
        item->escape = false;
        item->first = item->worth > 0.0 && WeaponItem(entry->sprite, &weapon)
                   && weapon != wp_chainsaw && !player->weaponowned[weapon] && PistolOnly(player);
        item->suit = entry->sprite == SPR_SUIT;
        entry->most = REACH_MOST;

        // Its guards and spot, about once a second; at once when it has been
        // worth nothing for a while.
        if (item->worth > 0.0
         && ((leveltime + i) % LOOK_TICS == 0 || leveltime - entry->looked_tic > LOOK_TICS))
        {
            entry->guards = K_Combat_Danger(item->at.x, item->at.y, entry->z);
            item->reachable = K_Graph_PickupSpot(item->at, &item->spot);
            entry->looked_tic = leveltime;
        }

        // What going there adds.
        item->danger = SDL_max(0.0, entry->guards - danger_here);

        if (WeaponItem(entry->sprite, &weapon) && !player->weaponowned[weapon]
         && PistolOnly(player))
        {
            item->danger *= PISTOL_START_GUARDS;
        }

        switch (item->kind)
        {
            case K_ITEM_HEALTH:
                weight = health_weight;
                entry->most = health_reach;
                break;

            case K_ITEM_ARMOUR:
                weight = armour_weight;
                break;

            case K_ITEM_AMMO:
            case K_ITEM_WEAPON:
                weight = ammo_weight;
                break;

            default:
                break;
        }

        entry->weighted = item->worth * weight;
    }

    // Each with those beside it, which come on the same trip.
    for (i = 0; i < num_entries; ++i)
    {
        entry_t *entry = &entries[i];
        k_item_t *item = &entry->item;
        double weighted = entry->weighted, most = entry->most, worth;
        int j;

        item->together = item->worth;

        if (item->worth <= 0.0 || !item->reachable)
        {
            item->reach = 0.0;
            continue;
        }

        for (j = 0; j < num_entries; ++j)
        {
            const entry_t *other = &entries[j];

            if (j != i && other->item.worth > 0.0
             && fabs(other->item.at.x - item->at.x) <= TOGETHER
             && fabs(other->item.at.y - item->at.y) <= TOGETHER)
            {
                weighted += other->weighted;
                most = SDL_max(most, other->most);
                item->together += other->item.worth;
            }
        }

        // Less what its guards would cost, weighed as health. Nothing is
        // worth more health than is left, unless health is CRITICAL: then
        // staying where the fight is costs as much (MAP02: at 9 health every
        // medikit near was guarded by a shotgun guy, and the bot stayed).
        // The more it is worth, the more each point pays for.
        weighted -= item->danger * health_weight;
        worth = SDL_max(0.0, item->together - item->danger);
        item->reach = item->danger >= player->health && condition->health != K_NEED_CRITICAL
                    ? 0.0
                    : SDL_max(0.0, SDL_min(weighted * UNITS_PER_POINT * (1.0 + worth / BIG_WORTH),
                                           most));

        if (item->danger < ROOM_DANGER && entry->running_out)
        {
            item->reach = REACH_ANY;
        }
        else if (item->danger < ROOM_DANGER && GivesAmmo(player, entry))
        {
            item->reach = SDL_max(item->reach, (item->dropped ? DROP_REACH : ROOM_REACH)
                                               * (1.0 - item->danger / ROOM_DANGER));
        }
        else if (item->danger < ROOM_DANGER && item->kind == K_ITEM_HEALTH && fights
              && player->health < 100 && item->together >= URGENT_HEALTH)
        {
            item->reach = condition->health != K_NEED_OK
                        ? REACH_ANY
                        : SDL_max(item->reach,
                                  HEALTH_REACH * (1.0 - item->danger / ROOM_DANGER));
        }
        else if (item->danger < ROOM_DANGER && fights && condition->armour != K_NEED_OK
              && (entry->sprite == SPR_ARM1 || entry->sprite == SPR_ARM2))
        {
            item->reach = SDL_max(item->reach, ARMOUR_REACH * (1.0 - item->danger / ROOM_DANGER));
        }

        if (item->kind == K_ITEM_HEALTH && item->together < URGENT_HEALTH)
        {
            item->urgent = false;
        }

        item->escape = item->urgent
                    && (item->kind == K_ITEM_HEALTH
                        || (entry->running_out && (condition->ammo == K_NEED_LOW
                                                   || condition->firepower == K_NEED_LOW)));
    }
}

int K_Items_Count(void)
{
    return num_entries;
}

const k_item_t *K_Items_Get(int index)
{
    return &entries[index].item;
}

// P_KillMobj's drops.
double K_Items_DropWorth(int type)
{
    const player_t *player = &players[consoleplayer];
    entry_t drop;

    if (player->mo == NULL)
    {
        return 0.0;
    }

    memset(&drop, 0, sizeof(drop));
    drop.item.dropped = true;

    switch (type)
    {
        case MT_POSSESSED:
        case MT_WOLFSS:
            drop.sprite = SPR_CLIP;
            break;

        case MT_SHOTGUY:
            drop.sprite = SPR_SHOT;
            break;

        case MT_CHAINGUY:
            drop.sprite = SPR_MGUN;
            break;

        default:
            return 0.0;
    }

    return Worth(player, &drop, true);
}
