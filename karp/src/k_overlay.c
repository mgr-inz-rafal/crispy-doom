//
// DESCRIPTION:
//	Karp overlay (-karpoverlay; karp/docs/plan.md, 3.12): for showing the
//	bot to an audience, without a broadcast program - the Karp logo at the
//	top of the game's picture and under it a line saying what the bot is
//	about, drawn over each frame in the window's own pixels (H13), so
//	neither Doom's palette, its low resolution nor its screen flashes touch
//	them, and they stay over the intermissions and menus too.
//
//	The logo is karp_logo.png next to the executable (the build copies
//	karp/assets/karp_logo.png there), read once, trimmed to what shows and
//	shrunk to the size it is drawn at. The line is in Doom's own HUD font,
//	read from the IWAD (STCFN), in the logo lettering's lime, outlined in
//	black, at a whole multiple of its pixels.
//
//	The line is coarse and steady (the user: it cannot flicker, and needs
//	far less than the console says): what the bot is after (k_brain.h:
//	k_activity_t) - the key, the shotgun, the exit - not the switch or the
//	doors on the way there; a switch only while it is worked ("opening a
//	door with a switch"), barrels while shot at; fights while there is
//	something to fight, runs out of them and falls back over all that. A
//	new line shows once it has held for a moment (ENTER_MS and the like:
//	passing pickups never show), and a line shown stays at least SHOW_MS (a
//	switch's SHOW_FIX_MS); but a run out of a fight, a fall back, the
//	level's end or the player's death at once. Short, so the line is a
//	second behind the game at most - a little shaky rather than stale (the
//	user). Lines cross-fade. Each line shown is logged, to count the
//	changes.
//

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "doomstat.h"
#include "i_swap.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_misc.h"
#include "v_patch.h"
#include "w_wad.h"
#include "z_zone.h"

#include "karp.h"
#include "k_brain.h"
#include "k_combat.h"
#include "k_demo.h"
#include "k_items.h"
#include "k_log.h"
#include "k_nav.h"
#include "k_overlay.h"
#include "k_specials.h"

// Our own logo only, never an image from elsewhere (third_party/stb/VERSION).
// Not STBI_NO_LINEAR: with it and PNG only, v2.31 leaves a function it uses
// undefined (stbi__mad4sizes_valid).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

// The logo: its file, next to the executable; its width, a share of the
// game's picture; its gap from the top, a share of the picture's height;
// the alpha from which one of its pixels shows (the empty margins round it
// are trimmed).
#define LOGO_FILE "karp_logo.png"
#define LOGO_SHARE 0.40
#define LOGO_GAP 0.015
#define LOGO_SEEN 8

// The line: its gap under the logo, a share of the picture's height; the
// picture's height for each whole multiple of the font's pixels (5 at
// 1080); the most of the picture's width it may take; the lime of the
// logo's lettering.
#define LINE_GAP 0.008
#define LINE_STEP 216
#define LINE_FIT 0.96
#define LINE_R 140
#define LINE_G 255
#define LINE_B 0

// Doom's HUD font (hu_stuff.c: HU_FONTSTART to HU_FONTEND, a space 4 wide).
#define FONT_FIRST '!'
#define FONT_LAST '_'
#define FONT_COUNT (FONT_LAST - FONT_FIRST + 1)
#define FONT_SPACE 4

// How long a new line holds before it shows: most, an item fetched (passing
// pickups never show), a switch or barrels worked, a fight, a run out of
// one or a fall back, the level over or the player dead. Short: the line a
// second behind the game at most (the user: rather a little shaky than
// "fighting monsters" for seconds after the last imp died).
#define ENTER_MS 700
#define ENTER_ITEM_MS 1200
#define ENTER_FIX_MS 500
#define ENTER_FIGHT_MS 500
#define ENTER_URGENT_MS 500
#define ENTER_NOW_MS 200

// How long a line shown stays at least, but for an urgent one (a run out
// of a fight, a fall back, the level over, the player dead): most, and a
// switch's or barrels' (gone soon after the work, not over the run on - the
// user: "opening..." stayed while the bot ran on through a corridor).
#define SHOW_MS 1500
#define SHOW_FIX_MS 1000

// The cross-fade; how recent a fight makes a run for health one out of it.
#define FADE_MS 250
#define FOUGHT_MS 3000

#define LINE_LEN 64

// A glyph in the font's strip: where, its size, and its patch offsets.
typedef struct
{
    int x, w, h;
    int left, top;
} glyph_t;

static unsigned char *logo;         // RGBA, trimmed
static int logo_w, logo_h;

static glyph_t glyphs[FONT_COUNT];
static unsigned char *font;         // RGBA strip of every glyph
static int font_w, font_h;

// Made for the renderer they belong to: gone with it if it is made again.
static SDL_Renderer *made_for;
static SDL_Texture *logo_texture;
static int logo_texture_w, logo_texture_h;
static SDL_Texture *font_texture;

static char shown[LINE_LEN];        // the line on screen, since when, for
static Uint32 shown_since;          // how long at least
static Uint32 shown_stay;
static char fading[LINE_LEN];       // the one before, fading out
static char pending[LINE_LEN];      // the one that would follow, since when
static Uint32 pending_since;
static Uint32 fought_at;            // when last in a fight; 0 never

void K_Overlay_Enable(void)
{
    K_Log(K_LOG_SYS, K_LOG_INFO, "overlay enabled");
}

// Reads the logo, trimmed to the box of its pixels that show.
static void LoadLogo(void)
{
    char *path = M_StringJoin(exedir, LOGO_FILE, NULL);
    int w, h, channels, x, y, x0, y0, x1 = -1, y1 = -1;
    unsigned char *pixels = stbi_load(path, &w, &h, &channels, 4);

    if (pixels == NULL)
    {
        K_Log(K_LOG_SYS, K_LOG_WARN, "overlay: no logo - %s: %s", path, stbi_failure_reason());
        free(path);
        return;
    }

    x0 = w;
    y0 = h;

    for (y = 0; y < h; ++y)
    {
        for (x = 0; x < w; ++x)
        {
            if (pixels[(y * w + x) * 4 + 3] >= LOGO_SEEN)
            {
                x0 = SDL_min(x0, x);
                x1 = SDL_max(x1, x);
                y0 = SDL_min(y0, y);
                y1 = SDL_max(y1, y);
            }
        }
    }

    if (x1 < 0)
    {
        K_Log(K_LOG_SYS, K_LOG_WARN, "overlay: the logo %s shows nothing", path);
        stbi_image_free(pixels);
        free(path);
        return;
    }

    logo_w = x1 - x0 + 1;
    logo_h = y1 - y0 + 1;
    logo = malloc(logo_w * logo_h * 4);

    for (y = 0; y < logo_h; ++y)
    {
        memcpy(logo + y * logo_w * 4, pixels + ((y0 + y) * w + x0) * 4, logo_w * 4);
    }

    K_Log(K_LOG_SYS, K_LOG_INFO, "overlay: logo %s, %dx%d, of it %dx%d shown", path, w, h,
          logo_w, logo_h);
    stbi_image_free(pixels);
    free(path);
}

// Doom's HUD font from the IWAD, as hu_stuff.c loads it, into one strip:
// each pixel grey by its colour's brightness (the brightest white), so the
// lime it is drawn in keeps the font's shading.
static void LoadFont(void)
{
    const byte *palette;
    byte *index, *mask;
    int lumps[FONT_COUNT];
    int c, x = 0, brightest = 1, i, palette_lump;
    char name[9];

    for (c = 0; c < FONT_COUNT; ++c)
    {
        const patch_t *patch;

        M_snprintf(name, sizeof(name), "STCFN%.3d", c + FONT_FIRST);
        lumps[c] = W_CheckNumForName(name);
        memset(&glyphs[c], 0, sizeof(glyphs[c]));

        if (lumps[c] < 0)
        {
            continue;
        }

        patch = W_CacheLumpNum(lumps[c], PU_STATIC);
        glyphs[c].x = x;
        glyphs[c].w = SHORT(patch->width);
        glyphs[c].h = SHORT(patch->height);
        glyphs[c].left = SHORT(patch->leftoffset);
        glyphs[c].top = SHORT(patch->topoffset);
        x += glyphs[c].w + 1;
        font_h = SDL_max(font_h, glyphs[c].h);
    }

    font_w = x;

    if (font_w == 0 || font_h == 0)
    {
        K_Log(K_LOG_SYS, K_LOG_WARN, "overlay: no HUD font in the IWAD - no line");
        return;
    }

    index = calloc(font_w * font_h, 1);
    mask = calloc(font_w * font_h, 1);

    // The posts of each column (r_draw: a delta not past the last one is
    // relative, as crispy reads tall patches).
    for (c = 0; c < FONT_COUNT; ++c)
    {
        const patch_t *patch;
        int col;

        if (lumps[c] < 0)
        {
            continue;
        }

        patch = W_CacheLumpNum(lumps[c], PU_STATIC);

        for (col = 0; col < glyphs[c].w; ++col)
        {
            const column_t *column = (const column_t *) ((const byte *) patch
                                                          + LONG(patch->columnofs[col]));
            int top = -1;

            while (column->topdelta != 0xff)
            {
                const byte *source = (const byte *) column + 3;

                top = column->topdelta <= top ? top + column->topdelta : column->topdelta;

                for (i = 0; i < column->length; ++i)
                {
                    int y = top + i;

                    if (y >= 0 && y < font_h)
                    {
                        index[y * font_w + glyphs[c].x + col] = source[i];
                        mask[y * font_w + glyphs[c].x + col] = 1;
                    }
                }

                column = (const column_t *) ((const byte *) column + column->length + 4);
            }
        }

        W_ReleaseLumpNum(lumps[c]);
        W_ReleaseLumpNum(lumps[c]);
    }

    palette_lump = W_GetNumForName("PLAYPAL");
    palette = W_CacheLumpNum(palette_lump, PU_STATIC);

    for (i = 0; i < font_w * font_h; ++i)
    {
        const byte *rgb = palette + index[i] * 3;

        if (mask[i])
        {
            brightest = SDL_max(brightest, SDL_max(rgb[0], SDL_max(rgb[1], rgb[2])));
        }
    }

    font = calloc(font_w * font_h * 4, 1);

    for (i = 0; i < font_w * font_h; ++i)
    {
        const byte *rgb = palette + index[i] * 3;
        int grey = SDL_max(rgb[0], SDL_max(rgb[1], rgb[2])) * 255 / brightest;

        if (mask[i])
        {
            font[i * 4] = font[i * 4 + 1] = font[i * 4 + 2] = (unsigned char) grey;
            font[i * 4 + 3] = 255;
        }
    }

    W_ReleaseLumpNum(palette_lump);
    free(index);
    free(mask);
    K_Log(K_LOG_SYS, K_LOG_INFO, "overlay: HUD font, %d glyphs in a %dx%d strip", FONT_COUNT,
          font_w, font_h);
}

void K_Overlay_Open(void)
{
    LoadLogo();
    LoadFont();
}

void K_Overlay_Close(void)
{
    // The renderer is still there (I_ShutdownGraphics comes after).
    if (made_for != NULL)
    {
        if (logo_texture != NULL)
        {
            SDL_DestroyTexture(logo_texture);
        }

        if (font_texture != NULL)
        {
            SDL_DestroyTexture(font_texture);
        }
    }

    made_for = NULL;
    logo_texture = NULL;
    font_texture = NULL;
    free(logo);
    free(font);
    logo = NULL;
    font = NULL;
}

//
// The line
//

// A kind of monster's name, for more than one of it: "barons of hell",
// "mancubi", "cyberdemons".
static void Plural(const char *name, char *buf, int len)
{
    const char *of = strstr(name, " of ");
    size_t n = strlen(name);

    if (of != NULL)
    {
        M_snprintf(buf, len, "%.*ss%s", (int) (of - name), name, of);
    }
    else if (n > 2 && !strcmp(name + n - 2, "us"))
    {
        M_snprintf(buf, len, "%.*si", (int) (n - 2), name);
    }
    else
    {
        M_snprintf(buf, len, "%ss", name);
    }
}

// The kind a map needs dead, by how many are left: "the baron of hell",
// "the barons of hell".
static void Bosses(int kind, const char *name, char *buf, int len)
{
    k_vec2_t at[16];
    char plural[48];

    if (K_Specials_FindMonsters(kind, at, arrlen(at)) > 1)
    {
        Plural(name, plural, sizeof(plural));
        M_snprintf(buf, len, "THE %s", plural);
    }
    else
    {
        M_snprintf(buf, len, "THE %s", name);
    }
}

// An item fetched, by what it is; in the sweep before the exit, the small
// things - ammo, health but for the spheres and berserk, armour bonuses -
// as loot (the user: "collecting loot" when the collecting starts, not for
// the first shotgun at a level's start, E1M1's exit counting as near).
static void ItemLine(const k_activity_t *activity, char *buf, int len, int *enter)
{
    const char *name = activity->name != NULL ? activity->name : "";
    boolean big = !strcmp(name, "soulsphere") || !strcmp(name, "megasphere")
               || !strcmp(name, "berserk");

    *enter = ENTER_ITEM_MS;

    if (activity->flag
     && (activity->detail == K_ITEM_AMMO || (activity->detail == K_ITEM_HEALTH && !big)
         || !strcmp(name, "armour bonus")))
    {
        M_StringCopy(buf, "COLLECTING LOOT", len);
        return;
    }

    switch (activity->detail)
    {
        case K_ITEM_HEALTH:
            if (big)
            {
                M_snprintf(buf, len, "GRABBING THE %s", name);
            }
            else
            {
                M_StringCopy(buf, "GETTING HEALTH", len);
            }
            break;

        // Spelt as Doom spells it, on screen (the user); "armour" in the code.
        case K_ITEM_ARMOUR:
            M_StringCopy(buf, "GETTING ARMOR", len);
            break;

        case K_ITEM_AMMO:
            M_StringCopy(buf, "REPLENISHING AMMO", len);
            break;

        case K_ITEM_WEAPON:
            M_snprintf(buf, len, !strcmp(name, "backpack") ? "GETTING A %s" : "GETTING THE %s",
                       name);
            break;

        default:
            M_snprintf(buf, len, "GRABBING THE %s", name);
            break;
    }
}

// A switch or walk-over line worked for the way (k_opens_t, k_worked_t):
// "opening a door with a switch"; a walk-over line, crossed in passing, by
// the wait for what it moves.
static void TriggerLine(int opens, int worked, char *buf, int len)
{
    static const char *doing[] = { "OPENING THE WAY", "OPENING A DOOR", "LOWERING A LIFT",
                                   "RAISING A FLOOR", "LOWERING A FLOOR",
                                   "OPENING THE WAY BACK" };
    static const char *waiting[] = { "WAITING FOR THE WAY TO OPEN", "WAITING FOR A DOOR TO OPEN",
                                     "WAITING FOR A LIFT", "WAITING FOR A FLOOR TO RISE",
                                     "WAITING FOR A FLOOR TO LOWER", "WAITING FOR THE WAY BACK" };

    if (opens < 0 || opens >= (int) arrlen(doing))
    {
        opens = K_OPENS_WAY;
    }

    if (worked == K_WORKED_PRESS)
    {
        M_snprintf(buf, len, "%s WITH A SWITCH", doing[opens]);
    }
    else if (worked == K_WORKED_SHOOT)
    {
        M_snprintf(buf, len, "%s BY SHOOTING A SWITCH", doing[opens]);
    }
    else
    {
        M_StringCopy(buf, waiting[opens], len);
    }
}

// The goal worked on, read for its purpose; false for none.
static boolean GoalLine(const k_activity_t *activity, char *buf, int len, int *enter)
{
    static const char *keys[] = { "BLUE", "YELLOW", "RED" };
    char bosses[64];

    switch (activity->kind)
    {
        case K_ACT_NONE:
        case K_ACT_FALLBACK:
            return false;

        case K_ACT_EXIT:
            M_StringCopy(buf, activity->detail == 1 ? "GOING TO THE SECRET EXIT"
                                                    : "GOING TO THE EXIT", len);
            break;

        case K_ACT_LOOT:
            M_StringCopy(buf, "COLLECTING LOOT", len);
            break;

        case K_ACT_EXPLORE:
            M_StringCopy(buf, "EXPLORING THE LEVEL", len);
            break;

        case K_ACT_SECRET:
            M_StringCopy(buf, "LOOKING FOR A SECRET", len);
            break;

        case K_ACT_KEY:
            M_snprintf(buf, len, "GETTING THE %s KEY",
                       activity->detail >= 0 && activity->detail < (int) arrlen(keys)
                       ? keys[activity->detail] : "");
            break;

        case K_ACT_TRIGGER:
            TriggerLine(activity->detail, activity->flag, buf, len);
            break;

        case K_ACT_ITEM:
        case K_ACT_ESCAPE:
            ItemLine(activity, buf, len, enter);
            break;

        case K_ACT_BARREL:
            M_StringCopy(buf, "CLEARING BARRELS FROM THE WAY", len);
            break;

        case K_ACT_BOSS:
            if (activity->flag)
            {
                M_snprintf(buf, len, "CHASING THE %s", activity->name);
            }
            else
            {
                Bosses(activity->detail, activity->name, bosses, sizeof(bosses));
                M_snprintf(buf, len, "HUNTING %s", bosses);
            }
            break;

        case K_ACT_TELEFRAG:
            M_snprintf(buf, len, "TELEFRAGGING THE %s", activity->name);
            break;
    }

    return true;
}

// A fight going on: a boss on the player, or the kind the map needs dead
// fought, by name; outmatched, from cover; else monsters.
static void FightLine(const k_activity_t *activity, char *buf, int len)
{
    const k_combatstatus_t *combat = K_Combat_GetStatus();
    const k_monster_t *target = combat->target >= 0 && combat->target < K_Combat_MonsterCount()
                              ? K_Combat_Monster(combat->target) : NULL;
    int boss = K_Combat_Boss();
    char bosses[64];

    if (boss >= 0)
    {
        M_snprintf(buf, len, "FIGHTING THE %s", K_Combat_KindName(boss));
    }
    else if (activity->kind == K_ACT_BOSS && target != NULL && target->type == activity->detail)
    {
        Bosses(activity->detail, activity->name, bosses, sizeof(bosses));
        M_snprintf(buf, len, "FIGHTING %s", bosses);
    }
    else if (K_Combat_Outmatched())
    {
        M_StringCopy(buf, "OUTNUMBERED - FIGHTING FROM COVER", len);
    }
    else
    {
        M_StringCopy(buf, "FIGHTING MONSTERS", len);
    }
}

// The line wanted now: how long it must hold before it shows (enter), how
// long it stays once shown (stay), and whether it may cut a line shown short
// (urgent). False for no say - in a menu, or nothing on the bot's list: the
// line shown stays. "" hides it.
static boolean Line(char *buf, int len, int *enter, int *stay, boolean *urgent, Uint32 now)
{
    const player_t *player = &players[consoleplayer];
    const k_combatstatus_t *combat;
    k_activity_t activity;
    boolean fighting;

    buf[0] = '\0';
    *enter = ENTER_NOW_MS;
    *stay = SHOW_MS;
    *urgent = true;

    if (gamestate == GS_INTERMISSION)
    {
        M_StringCopy(buf, "LEVEL COMPLETE", len);
        return true;
    }

    if (gamestate == GS_FINALE)
    {
        M_StringCopy(buf, "EPISODE COMPLETE", len);
        return true;
    }

    if (gamestate != GS_LEVEL || !K_AutoplayEnabled())
    {
        return true;
    }

    if (player->playerstate == PST_DEAD)
    {
        if (K_Demo_Enabled())
        {
            M_StringCopy(buf, "TRYING AGAIN", len);
        }

        return true;
    }

    if (menuactive || paused)
    {
        return false;
    }

    // A fight while there is something to fight: combat's battle goes on
    // a second past the last kill, to be sure (E1M1: the imp dead at 19.8 s,
    // the fight over at 20.8 s).
    combat = K_Combat_GetStatus();
    fighting = combat->battle && (combat->target >= 0 || combat->awake > 0);

    if (fighting)
    {
        fought_at = SDL_max(now, 1);
    }

    K_Brain_Activity(&activity);
    *enter = ENTER_URGENT_MS;

    if (activity.kind == K_ACT_ESCAPE && fought_at != 0 && now - fought_at < FOUGHT_MS)
    {
        if (activity.name != NULL && !strcmp(activity.name, "invulnerability"))
        {
            M_StringCopy(buf, "RUNNING FOR THE INVULNERABILITY", len);
        }
        else
        {
            M_StringCopy(buf, activity.detail == K_ITEM_HEALTH
                              ? "ESCAPING THE FIGHT TO GET HEALTH"
                              : "ESCAPING THE FIGHT TO GET AMMO", len);
        }
    }
    else if (activity.kind == K_ACT_FALLBACK)
    {
        M_StringCopy(buf, "FALLING BACK TO RECOVER", len);
    }
    else
    {
        *urgent = false;

        if (fighting)
        {
            FightLine(&activity, buf, len);
            *enter = ENTER_FIGHT_MS;
        }
        else if (combat->holding)
        {
            M_StringCopy(buf, "LURING THE MONSTERS OUT", len);
            *enter = ENTER_MS;
        }
        else if (activity.fix == K_ACT_TRIGGER)
        {
            TriggerLine(activity.fix_detail, activity.fix_flag, buf, len);
            *enter = ENTER_FIX_MS;
            *stay = SHOW_FIX_MS;
        }
        else if (activity.fix == K_ACT_BARREL)
        {
            M_StringCopy(buf, "CLEARING BARRELS FROM THE WAY", len);
            *enter = ENTER_FIX_MS;
            *stay = SHOW_FIX_MS;
        }
        else if (K_Nav_Unsticking())
        {
            M_StringCopy(buf, "GETTING UNSTUCK", len);
            *enter = ENTER_MS;
        }
        else
        {
            *enter = ENTER_MS;

            if (!GoalLine(&activity, buf, len, enter))
            {
                return false;
            }
        }
    }

    M_ForceUppercase(buf);
    return true;
}

// The line shown: a new one once it has held long enough, and the one shown
// has stayed long enough or is cut short.
static void UpdateLine(Uint32 now)
{
    char line[LINE_LEN];
    boolean urgent;
    int enter, stay;

    if (!Line(line, sizeof(line), &enter, &stay, &urgent, now) || !strcmp(line, shown))
    {
        pending[0] = '\0';
        return;
    }

    if (strcmp(line, pending))
    {
        M_StringCopy(pending, line, sizeof(pending));
        pending_since = now;
        return;
    }

    if (now - pending_since < (Uint32) enter
     || (shown[0] != '\0' && !urgent && now - shown_since < shown_stay))
    {
        return;
    }

    M_StringCopy(fading, shown, sizeof(fading));
    M_StringCopy(shown, line, sizeof(shown));
    shown_since = now;
    shown_stay = stay;
    pending[0] = '\0';
    K_Log(K_LOG_SYS, K_LOG_INFO, "overlay: %s", shown[0] != '\0' ? shown : "(no line)");
}

//
// Drawing
//

// The textures, made for the renderer (again if it was made again: theirs
// went with the old one).
static void MakeTextures(SDL_Renderer *renderer)
{
    if (renderer != made_for)
    {
        made_for = renderer;
        logo_texture = NULL;
        font_texture = NULL;
        logo_texture_w = logo_texture_h = 0;
    }

    if (font != NULL && font_texture == NULL)
    {
        font_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, font_w, font_h);

        if (font_texture != NULL)
        {
            SDL_UpdateTexture(font_texture, NULL, font, font_w * 4);
            SDL_SetTextureBlendMode(font_texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(font_texture, SDL_ScaleModeNearest);
        }
    }
}

// The logo shrunk to w by h: each pixel the average of those it covers,
// weighed by how much they show (no dark fringe at the edges).
static unsigned char *Shrink(int w, int h)
{
    unsigned char *out = malloc(w * h * 4);
    int x, y, i, j;

    for (y = 0; y < h; ++y)
    {
        int y0 = y * logo_h / h, y1 = SDL_max(y0 + 1, (y + 1) * logo_h / h);

        for (x = 0; x < w; ++x)
        {
            int x0 = x * logo_w / w, x1 = SDL_max(x0 + 1, (x + 1) * logo_w / w);
            double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
            unsigned char *to = out + (y * w + x) * 4;

            for (j = y0; j < y1; ++j)
            {
                for (i = x0; i < x1; ++i)
                {
                    const unsigned char *from = logo + (j * logo_w + i) * 4;

                    r += from[0] * from[3];
                    g += from[1] * from[3];
                    b += from[2] * from[3];
                    a += from[3];
                }
            }

            to[0] = a > 0.0 ? (unsigned char) (r / a + 0.5) : 0;
            to[1] = a > 0.0 ? (unsigned char) (g / a + 0.5) : 0;
            to[2] = a > 0.0 ? (unsigned char) (b / a + 0.5) : 0;
            to[3] = (unsigned char) (a / ((x1 - x0) * (y1 - y0)) + 0.5);
        }
    }

    return out;
}

// The logo's texture at w by h: shrunk to it once (again when the window
// changes size); larger than the file, the file, scaled smoothly.
static void SizeLogo(SDL_Renderer *renderer, int w, int h)
{
    unsigned char *pixels;
    int tw = SDL_min(w, logo_w), th = w < logo_w ? h : logo_h;

    if (logo_texture != NULL && logo_texture_w == tw && logo_texture_h == th)
    {
        return;
    }

    if (logo_texture != NULL)
    {
        SDL_DestroyTexture(logo_texture);
    }

    pixels = tw < logo_w ? Shrink(tw, th) : logo;
    logo_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                     tw, th);

    if (logo_texture != NULL)
    {
        SDL_UpdateTexture(logo_texture, NULL, pixels, tw * 4);
        SDL_SetTextureBlendMode(logo_texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(logo_texture, SDL_ScaleModeLinear);
    }

    if (pixels != logo)
    {
        free(pixels);
    }

    logo_texture_w = tw;
    logo_texture_h = th;
}

// A line's width in the font's pixels.
static int TextWidth(const char *text)
{
    int width = 0;

    for (; *text != '\0'; ++text)
    {
        int c = *text - FONT_FIRST;

        width += c >= 0 && c < FONT_COUNT && glyphs[c].w > 0 ? glyphs[c].w : FONT_SPACE;
    }

    return width;
}

// A line centred on the picture, its top at top: outlined in black (eight
// copies round it), then in lime on top.
static void DrawText(SDL_Renderer *renderer, const char *text, const SDL_Rect *picture,
                     int top, Uint8 alpha)
{
    int scale = SDL_max(1, picture->h / LINE_STEP);
    int width, outline, pass, x0, x;

    while (scale > 1 && TextWidth(text) * scale > picture->w * LINE_FIT)
    {
        --scale;
    }

    width = TextWidth(text) * scale;
    outline = SDL_max(1, scale / 2);
    x0 = picture->x + (picture->w - width) / 2;
    top += outline;
    SDL_SetTextureAlphaMod(font_texture, alpha);

    // Passes 0 to 8 the ring round a 3 by 3 grid but its middle, 9 the lime.
    for (pass = 0; pass < 10; ++pass)
    {
        int dx = pass == 9 ? 0 : (pass % 3 - 1) * outline;
        int dy = pass == 9 ? 0 : (pass / 3 - 1) * outline;
        const char *at;

        if (pass == 4)
        {
            continue;
        }

        if (pass == 9)
        {
            SDL_SetTextureColorMod(font_texture, LINE_R, LINE_G, LINE_B);
        }
        else
        {
            SDL_SetTextureColorMod(font_texture, 0, 0, 0);
        }

        for (at = text, x = x0; *at != '\0'; ++at)
        {
            int c = *at - FONT_FIRST;
            const glyph_t *glyph;
            SDL_Rect from, to;

            if (c < 0 || c >= FONT_COUNT || glyphs[c].w == 0)
            {
                x += FONT_SPACE * scale;
                continue;
            }

            glyph = &glyphs[c];
            from.x = glyph->x;
            from.y = 0;
            from.w = glyph->w;
            from.h = glyph->h;
            to.x = x - glyph->left * scale + dx;
            to.y = top - glyph->top * scale + dy;
            to.w = glyph->w * scale;
            to.h = glyph->h * scale;
            SDL_RenderCopy(renderer, font_texture, &from, &to);
            x += glyph->w * scale;
        }
    }
}

void K_Overlay_Draw(SDL_Renderer *renderer)
{
    Uint32 now = SDL_GetTicks();
    SDL_Rect view, picture;
    float scale_x, scale_y;
    int logical_w, logical_h, top;

    UpdateLine(now);
    MakeTextures(renderer);

    // The game's picture in the window's pixels (SDL letterboxes the
    // logical size into the window), then drawn in those pixels.
    SDL_RenderGetLogicalSize(renderer, &logical_w, &logical_h);
    SDL_RenderGetViewport(renderer, &view);
    SDL_RenderGetScale(renderer, &scale_x, &scale_y);
    picture.x = (int) lround(view.x * scale_x);
    picture.y = (int) lround(view.y * scale_y);
    picture.w = (int) lround(view.w * scale_x);
    picture.h = (int) lround(view.h * scale_y);

    if (picture.w <= 0 || picture.h <= 0)
    {
        return;
    }

    SDL_RenderSetLogicalSize(renderer, 0, 0);
    top = picture.y + (int) lround(picture.h * LOGO_GAP);

    if (logo != NULL)
    {
        int w = (int) lround(picture.w * LOGO_SHARE);
        int h = (int) lround((double) w * logo_h / logo_w);
        SDL_Rect to;

        SizeLogo(renderer, w, h);
        to.x = picture.x + (picture.w - w) / 2;
        to.y = top;
        to.w = w;
        to.h = h;

        if (logo_texture != NULL)
        {
            SDL_RenderCopy(renderer, logo_texture, NULL, &to);
        }

        top += h + (int) lround(picture.h * LINE_GAP);
    }

    if (font_texture != NULL)
    {
        Uint32 age = now - shown_since;

        if (fading[0] != '\0' && age < FADE_MS)
        {
            DrawText(renderer, fading, &picture, top, (Uint8) (255 * (FADE_MS - age) / FADE_MS));
        }

        if (shown[0] != '\0')
        {
            DrawText(renderer, shown, &picture, top,
                     (Uint8) (age < FADE_MS ? 255 * age / FADE_MS : 255));
        }
    }

    if (logical_w > 0 && logical_h > 0)
    {
        SDL_RenderSetLogicalSize(renderer, logical_w, logical_h);
    }
}
