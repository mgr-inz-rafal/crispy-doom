//
// DESCRIPTION:
//	Karp items: every item lying in the level, told by its sprite as
//	P_TouchSpecialThing tells it, and what picking it up would be worth
//	now - what P_TouchSpecialThing would give: the health below the most
//	it may reach, the armour that is better than what is worn (a third of
//	the damage taken off for green, a half for blue), the ammo below the
//	most the player may carry (bullets cheap, rockets dear; a quarter for
//	ammo no weapon it holds fires), a weapon it does not have. Nothing it
//	would not pick up counts. With no monster left alive, weapons, ammo
//	and the power-ups for fighting are worth nothing.
//
//	The worth, weighted by need, becomes how far out of its way the player
//	may go for the item (plan, 7.5): a small detour for anything useful, a
//	long one for health when health is LOW - the lower, the longer - and
//	any detour at all when CRITICAL (the user: a minute or two back to the
//	level's start beats dying). Ammo weighs three times when LOW. Items
//	lying together are picked up on one trip, so each counts with those
//	beside it (Doom II MAP02: three stimpacks side by side, 30 health on
//	one trip, were left at 25 health as worth 10 each).
//
//	What the monsters guarding an item would cost in health is taken off,
//	weighed as health is (the user, MAP02: going for four stimpacks started
//	a fight a lonely medikit would not have). Nothing is worth more health
//	than the player has left. Only the guards count, not monsters on the
//	way there (the user: those are met and fought anyway, and move about).
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

// Map units of detour a point of worth pays for, and the most for each
// need: a small detour whatever the need, more for what is needed, and no
// limit for health when CRITICAL.
#define UNITS_PER_POINT 8.0
#define REACH_SMALL 400.0
#define REACH_LOW 1500.0
#define REACH_ANY 1.0e6

// Health weighs LOW_WEIGHT at the LOW mark (75), rising to LOW_WEIGHT_MOST
// at the CRITICAL one (25); CRITICAL_WEIGHT below.
#define LOW_WEIGHT 3.0
#define LOW_WEIGHT_MOST 6.0
#define CRITICAL_WEIGHT 12.0

// Items this close to one another are picked up on one trip.
#define TOGETHER 64.0

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
    int seen;               // the tic it was last found, plus one
    int looked_tic;         // when its guards and spot were last looked at
    int next;               // in its bucket, -1 at the end
    double weighted;        // its worth, weighted by need
    double most;            // the longest detour its need allows
} entry_t;

static entry_t *entries;
static int num_entries;
static int capacity;
static int buckets[BUCKETS];

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

// P_GiveAmmo: clips of ammo (0 for half of one), doubled on the easiest and
// hardest skills, halved when dropped, up to the most the player carries.
static double AmmoWorth(const player_t *player, ammotype_t ammo, int clips, boolean dropped)
{
    int num = clips > 0 ? clips * clip_ammo[ammo] : clip_ammo[ammo] / 2;

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
    return num * ammo_worth[ammo] * (Usable(player, ammo) ? 1.0 : 0.25);
}

static double WeaponWorth(const player_t *player, weapontype_t weapon, boolean dropped)
{
    return (player->weaponowned[weapon] ? 0.0 : weapon_worth[weapon])
         + AmmoWorth(player, weaponinfo[weapon].ammo, 2, dropped);
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
    int a;

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

    switch (entry->sprite)
    {
        case SPR_CLIP: return AmmoWorth(player, am_clip, 1, dropped);
        case SPR_AMMO: return AmmoWorth(player, am_clip, 5, dropped);
        case SPR_SHEL: return AmmoWorth(player, am_shell, 1, dropped);
        case SPR_SBOX: return AmmoWorth(player, am_shell, 5, dropped);
        case SPR_ROCK: return AmmoWorth(player, am_misl, 1, dropped);
        case SPR_BROK: return AmmoWorth(player, am_misl, 5, dropped);
        case SPR_CELL: return AmmoWorth(player, am_cell, 1, dropped);
        case SPR_CELP: return AmmoWorth(player, am_cell, 5, dropped);
        case SPR_SHOT: return WeaponWorth(player, wp_shotgun, dropped);
        case SPR_SGN2: return WeaponWorth(player, wp_supershotgun, dropped);
        case SPR_MGUN: return WeaponWorth(player, wp_chaingun, dropped);
        case SPR_LAUN: return WeaponWorth(player, wp_missile, dropped);
        case SPR_PLAS: return WeaponWorth(player, wp_plasma, dropped);
        case SPR_BFUG: return WeaponWorth(player, wp_bfg, dropped);
        case SPR_CSAW: return WeaponWorth(player, wp_chainsaw, dropped);
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

void K_Items_Tick(void)
{
    const player_t *player = &players[consoleplayer];
    const k_conditionstatus_t *condition = K_Condition_Get();
    boolean fights = K_Combat_GetStatus()->alive > 0;
    double health_weight, ammo_weight, armour_weight;
    double health_reach, ammo_reach;
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
    health_reach = condition->health == K_NEED_CRITICAL ? REACH_ANY
                 : condition->health == K_NEED_LOW ? REACH_LOW : REACH_SMALL;
    armour_weight = condition->health != K_NEED_OK ? 2.0 : 1.0;
    ammo_weight = condition->ammo == K_NEED_LOW ? 3.0 : 1.0;
    ammo_reach = condition->ammo == K_NEED_LOW ? REACH_LOW : REACH_SMALL;

    for (i = 0; i < num_entries; ++i)
    {
        entry_t *entry = &entries[i];
        k_item_t *item = &entry->item;
        double weight = 1.0;

        item->present = entry->seen == stamp;
        item->worth = item->present ? Worth(player, entry, fights) : 0.0;
        entry->most = REACH_SMALL;

        // Its guards and spot, about once a second; at once when it has been
        // worth nothing for a while.
        if (item->worth > 0.0
         && ((leveltime + i) % LOOK_TICS == 0 || leveltime - entry->looked_tic > LOOK_TICS))
        {
            item->danger = K_Combat_Danger(item->at.x, item->at.y, entry->z);
            item->reachable = K_Graph_PickupSpot(item->at, &item->spot);
            entry->looked_tic = leveltime;
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
                entry->most = ammo_reach;
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
        double weighted = entry->weighted, most = entry->most;
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

        // Less what its guards would cost, weighed as health.
        weighted -= item->danger * health_weight;
        item->reach = item->danger >= player->health
                    ? 0.0 : SDL_max(0.0, SDL_min(weighted * UNITS_PER_POINT, most));
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
