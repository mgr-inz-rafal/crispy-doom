//
// DESCRIPTION:
//	Karp condition: the player's ammo, health and armour, each judged on
//	its own, with hysteresis - a need goes LOW below one mark and back to
//	OK only above a higher one, so a stimpack does not flip it back and
//	forth. The condition is the worst need, in the order COMFY, LOW AMMO,
//	LOW HEALTH, CRITICAL; armour alone does not change it. For now it is
//	only shown and logged; nothing acts on it yet (plan, 7.7: C4).
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

static k_conditionstatus_t status;
static boolean judged;

static const char *condition_names[] = { "COMFY", "LOW AMMO", "LOW HEALTH", "CRITICAL" };
static const char *need_names[] = { "OK", "LOW", "CRITICAL" };

void K_Condition_Reset(void)
{
    memset(&status, 0, sizeof(status));
    judged = false;
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
    status.health = JudgeHealth(player->health, judged ? was.health : K_NEED_OK);
    status.armour = JudgeArmour(player->armorpoints, judged ? was.armour : K_NEED_OK);

    status.condition = status.health == K_NEED_CRITICAL ? K_CONDITION_CRITICAL
                     : status.health == K_NEED_LOW ? K_CONDITION_LOW_HEALTH
                     : status.ammo == K_NEED_LOW ? K_CONDITION_LOW_AMMO
                     : K_CONDITION_COMFY;

    if (!judged || status.ammo != was.ammo || status.health != was.health
     || status.armour != was.armour)
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "condition %s - ammo %s, health %s (%d), armour %s (%d)",
              condition_names[status.condition], need_names[status.ammo],
              need_names[status.health], status.health_points, need_names[status.armour],
              status.armour_points);
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
