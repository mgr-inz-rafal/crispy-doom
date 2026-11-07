//
// DESCRIPTION:
//	Karp condition: the player's ammo, firepower, health and armour, each
//	judged on its own, with hysteresis - a need goes LOW below one mark and
//	back to OK only above a higher one, so a stimpack does not flip it back
//	and forth. The condition is the worst need, in the order COMFY, LOW
//	AMMO (ammo or firepower), LOW HEALTH, CRITICAL; armour alone does not
//	change it.
//

#include <string.h>

#include "doomdef.h"
#include "doomstat.h"
#include "m_misc.h"

#include "k_condition.h"
#include "k_log.h"

// Health: CRITICAL below 25 until above 35; LOW below 75 until above 85
// (the user: at 50 the bot looked anything but comfy; below 75 a medikit's
// 25 is none of it wasted).
#define HEALTH_CRITICAL 25
#define HEALTH_CRITICAL_OK 35
#define HEALTH_LOW 75
#define HEALTH_LOW_OK 85

// Armour: LOW below 25 until above 40.
#define ARMOUR_LOW 25
#define ARMOUR_LOW_OK 40

// Ammo is LOW when no ammo type the player has a weapon for is at its low
// mark or above; back to OK once one is above its OK mark.
static const int ammo_low[NUMAMMO] = { 20, 5, 40, 3 };     // clip, shell, cell, misl
static const int ammo_ok[NUMAMMO] = { 30, 8, 60, 5 };

// Firepower is CRITICAL when no strong weapon held (K_Condition_Strong) has
// its ammo at the CRITICAL mark: the pistol, the fist or the chainsaw is
// what the player fights with, however many bullets it has (the user, Doom
// II MAP10: out of shells, cells below a BFG shot, bullets plenty - ammo not
// LOW - the bot fought lost souls, demons and a pain elemental with the
// pistol, the shotguns the shotgun guys dropped lying by). Back above it
// once one has its ammo at the CRITICAL_OK mark (MAP06: a shotgun picked
// up, 8 shells, was not enough, and the bot broke off a fight for more).
// LOW the same way at higher marks: running short, a few fights left (the
// user, E2M5: with 7 shells, firepower OK, the bot ran past 4 shells, and
// later fought on with the pistol - "even more aggressive collecting ammo").
static const int firepower_critical[NUMAMMO] = { 20, 5, 40, 3 };
static const int firepower_critical_ok[NUMAMMO] = { 30, 8, 60, 5 };
static const int firepower_low[NUMAMMO] = { 40, 10, 60, 5 };
static const int firepower_ok[NUMAMMO] = { 60, 16, 100, 8 };

static k_conditionstatus_t status;
static boolean judged;

static const char *condition_names[] = { "COMFY", "LOW AMMO", "LOW HEALTH", "CRITICAL" };
static const char *need_names[] = { "OK", "LOW", "CRITICAL" };

void K_Condition_Reset(void)
{
    memset(&status, 0, sizeof(status));
    judged = false;
}

int K_Condition_Strong(int weapon)
{
    return weapon == wp_shotgun || weapon == wp_supershotgun || weapon == wp_chaingun
        || weapon == wp_missile || weapon == wp_plasma || weapon == wp_bfg;
}

// Whether the player has a weapon that fires the ammo type.
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

static k_need_t JudgeAmmo(const player_t *player, k_need_t was)
{
    boolean at_low = false, above_ok = false;
    int a;

    for (a = 0; a < NUMAMMO; ++a)
    {
        if (Usable(player, a))
        {
            at_low |= player->ammo[a] >= ammo_low[a];
            above_ok |= player->ammo[a] > ammo_ok[a];
        }
    }

    if (was == K_NEED_OK)
    {
        return at_low ? K_NEED_OK : K_NEED_LOW;
    }

    return above_ok ? K_NEED_OK : K_NEED_LOW;
}

// Whether some strong weapon held has its ammo at the mark.
static boolean StrongAt(const player_t *player, const int *mark)
{
    int w;

    for (w = 0; w < NUMWEAPONS; ++w)
    {
        if (player->weaponowned[w] && K_Condition_Strong(w)
         && player->ammo[weaponinfo[w].ammo] >= mark[weaponinfo[w].ammo])
        {
            return true;
        }
    }

    return false;
}

static k_need_t JudgeFirepower(const player_t *player, k_need_t was)
{
    if (!StrongAt(player, was == K_NEED_CRITICAL ? firepower_critical_ok : firepower_critical))
    {
        return K_NEED_CRITICAL;
    }

    if (!StrongAt(player, was == K_NEED_OK ? firepower_low : firepower_ok))
    {
        return K_NEED_LOW;
    }

    return K_NEED_OK;
}

static k_need_t JudgeHealth(int health, k_need_t was)
{
    if (health < HEALTH_CRITICAL || (was == K_NEED_CRITICAL && health <= HEALTH_CRITICAL_OK))
    {
        return K_NEED_CRITICAL;
    }

    if (health < HEALTH_LOW || (was != K_NEED_OK && health <= HEALTH_LOW_OK))
    {
        return K_NEED_LOW;
    }

    return K_NEED_OK;
}

static k_need_t JudgeArmour(int armour, k_need_t was)
{
    if (armour < ARMOUR_LOW || (was != K_NEED_OK && armour <= ARMOUR_LOW_OK))
    {
        return K_NEED_LOW;
    }

    return K_NEED_OK;
}

void K_Condition_Tick(void)
{
    const player_t *player = &players[consoleplayer];
    k_conditionstatus_t was = status;

    if (player->mo == NULL)
    {
        return;
    }

    status.health_points = player->health;
    status.armour_points = player->armorpoints;
    status.bullets = player->ammo[am_clip];
    status.shells = player->ammo[am_shell];
    status.rockets = player->ammo[am_misl];
    status.cells = player->ammo[am_cell];

    status.ammo = JudgeAmmo(player, judged ? was.ammo : K_NEED_OK);
    status.firepower = JudgeFirepower(player, judged ? was.firepower : K_NEED_OK);
    status.health = JudgeHealth(player->health, judged ? was.health : K_NEED_OK);
    status.armour = JudgeArmour(player->armorpoints, judged ? was.armour : K_NEED_OK);

    status.condition = status.health == K_NEED_CRITICAL ? K_CONDITION_CRITICAL
                     : status.health == K_NEED_LOW ? K_CONDITION_LOW_HEALTH
                     : status.ammo == K_NEED_LOW || status.firepower != K_NEED_OK
                     ? K_CONDITION_LOW_AMMO
                     : K_CONDITION_COMFY;

    if (!judged || status.ammo != was.ammo || status.firepower != was.firepower
     || status.health != was.health || status.armour != was.armour)
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "condition %s - ammo %s, firepower %s, health %s (%d), "
              "armour %s (%d)", condition_names[status.condition], need_names[status.ammo],
              need_names[status.firepower], need_names[status.health], status.health_points,
              need_names[status.armour], status.armour_points);
    }

    judged = true;
}

const k_conditionstatus_t *K_Condition_Get(void)
{
    return &status;
}

const char *K_Condition_Name(k_condition_t condition)
{
    return condition_names[condition];
}

const char *K_Condition_NeedName(k_need_t need)
{
    return need_names[need];
}

int K_Condition_AmmoLow(int ammo)
{
    return ammo >= 0 && ammo < NUMAMMO ? ammo_low[ammo] : 0;
}
