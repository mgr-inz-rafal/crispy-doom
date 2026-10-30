//
// DESCRIPTION:
//	Karp sight checks: P_CheckSight's REJECT lookup and BSP trace, copied.
//	The engine's own marks each line it has looked at with validcount,
//	which Karp may not touch (karp/docs/plan.md, 2.4), so these keep
//	marks of their own. The maths is the engine's, fixed point and all,
//	so the bot sees exactly what the monsters' sight would (Doom 1.2's
//	sight, kept by crispy for that version, is not copied: Karp plays
//	Doom II).
//

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "doomdef.h"
#include "doomstat.h"
#include "m_fixed.h"
#include "p_local.h"
#include "p_mapformat.h"
#include "r_main.h"
#include "r_state.h"

#include "k_sight.h"

typedef struct
{
    fixed_t eye;                // the looker's eye height
    fixed_t topslope;           // to the top and bottom of the target,
    fixed_t bottomslope;        // narrowed by each opening on the way
    divline_t trace;            // from the looker to the target
    fixed_t x2, y2;             // the target
} sight_t;

// Per line, the check that last looked at it.
static int *line_marks;
static int marks_size;
static int mark;

void K_Sight_Reset(void)
{
    free(line_marks);
    marks_size = numlines;
    line_marks = calloc(SDL_max(marks_size, 1), sizeof(*line_marks));
    mark = 0;
}

// P_DivlineSide: 0 in front, 1 behind, 2 on the line; its slip in the
// horizontal case (x compared with node->y) kept.
static int DivlineSide(fixed_t x, fixed_t y, const divline_t *node)
{
    fixed_t dx, dy, left, right;

    if (!node->dx)
    {
        if (x == node->x)
        {
            return 2;
        }

        if (x <= node->x)
        {
            return node->dy > 0;
        }

        return node->dy < 0;
    }

    if (!node->dy)
    {
        if (x == node->y)
        {
            return 2;
        }

        if (y <= node->y)
        {
            return node->dx < 0;
        }

        return node->dx > 0;
    }

    dx = x - node->x;
    dy = y - node->y;
    left = (node->dy >> FRACBITS) * (dx >> FRACBITS);
    right = (dy >> FRACBITS) * (node->dx >> FRACBITS);

    if (right < left)
    {
        return 0;
    }

    if (left == right)
    {
        return 2;
    }

    return 1;
}

// P_InterceptVector2: how far along v2 it meets v1.
static fixed_t InterceptVector(const divline_t *v2, const divline_t *v1)
{
    fixed_t den = FixedMul(v1->dy >> 8, v2->dx) - FixedMul(v1->dx >> 8, v2->dy);
    fixed_t num;

    if (den == 0)
    {
        return 0;
    }

    num = FixedMul((v1->x - v2->x) >> 8, v1->dy) + FixedMul((v2->y - v1->y) >> 8, v1->dx);
    return FixedDiv(num, den);
}

// P_CrossSubsector: whether the trace gets through the subsector.
static boolean CrossSubsector(sight_t *sight, int num)
{
    const subsector_t *sub = &subsectors[num];
    const seg_t *seg = &segs[sub->firstline];
    int count;

    for (count = sub->numlines; count > 0; ++seg, --count)
    {
        const line_t *line = seg->linedef;
        const sector_t *front, *back;
        fixed_t opentop, openbottom, frac, slope;
        divline_t divl;
        int index, s1, s2;

        // Minisegs.
        if (line == NULL)
        {
            continue;
        }

        // Already looked at from its other side.
        index = (int) (line - lines);

        if (line_marks[index] == mark)
        {
            continue;
        }

        line_marks[index] = mark;

        s1 = DivlineSide(line->v1->x, line->v1->y, &sight->trace);
        s2 = DivlineSide(line->v2->x, line->v2->y, &sight->trace);

        if (s1 == s2)
        {
            continue;
        }

        divl.x = line->v1->x;
        divl.y = line->v1->y;
        divl.dx = line->v2->x - line->v1->x;
        divl.dy = line->v2->y - line->v1->y;
        s1 = DivlineSide(sight->trace.x, sight->trace.y, &divl);
        s2 = DivlineSide(sight->x2, sight->y2, &divl);

        if (s1 == s2)
        {
            continue;
        }

        // An "impassable glass" line has no back.
        if (line->backsector == NULL || !(line->flags & ML_TWOSIDED))
        {
            return false;
        }

        front = seg->frontsector;
        back = seg->backsector;

        if (front->floorheight == back->floorheight
         && front->ceilingheight == back->ceilingheight)
        {
            continue;
        }

        opentop = SDL_min(front->ceilingheight, back->ceilingheight);
        openbottom = SDL_max(front->floorheight, back->floorheight);

        // A closed door.
        if (openbottom >= opentop)
        {
            return false;
        }

        frac = InterceptVector(&sight->trace, &divl);

        if (front->floorheight != back->floorheight)
        {
            slope = FixedDiv(openbottom - sight->eye, frac);

            if (slope > sight->bottomslope)
            {
                sight->bottomslope = slope;
            }
        }

        if (front->ceilingheight != back->ceilingheight)
        {
            slope = FixedDiv(opentop - sight->eye, frac);

            if (slope < sight->topslope)
            {
                sight->topslope = slope;
            }
        }

        if (sight->topslope <= sight->bottomslope)
        {
            return false;
        }
    }

    return true;
}

// P_CrossBSPNode.
static boolean CrossBSPNode(sight_t *sight, int bspnum)
{
    const node_t *bsp;
    int side;

    if (bspnum & NF_SUBSECTOR)
    {
        return CrossSubsector(sight, bspnum == -1 ? 0 : bspnum & ~NF_SUBSECTOR);
    }

    bsp = &nodes[bspnum];
    side = DivlineSide(sight->trace.x, sight->trace.y, (const divline_t *) bsp);

    // On the partition: crosses both sides.
    if (side == 2)
    {
        side = 0;
    }

    if (!CrossBSPNode(sight, bsp->children[side]))
    {
        return false;
    }

    if (side == DivlineSide(sight->x2, sight->y2, (const divline_t *) bsp))
    {
        return true;
    }

    return CrossBSPNode(sight, bsp->children[side ^ 1]);
}

static boolean Rejected(fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2)
{
    int s1 = (int) (R_PointInSubsector(x1, y1)->sector - sectors);
    int s2 = (int) (R_PointInSubsector(x2, y2)->sector - sectors);
    int pnum = s1 * numsectors + s2;

    return (rejectmatrix[pnum >> 3] & (1 << (pnum & 7))) != 0;
}

static int Check(fixed_t x1, fixed_t y1, fixed_t eye, fixed_t x2, fixed_t y2,
                 fixed_t bottom, fixed_t top)
{
    sight_t sight;

    if (line_marks == NULL || marks_size != numlines || Rejected(x1, y1, x2, y2))
    {
        return false;
    }

    if (++mark == INT_MAX)
    {
        memset(line_marks, 0, marks_size * sizeof(*line_marks));
        mark = 1;
    }

    sight.eye = eye;
    sight.topslope = top - eye;
    sight.bottomslope = bottom - eye;
    sight.trace.x = x1;
    sight.trace.y = y1;
    sight.trace.dx = x2 - x1;
    sight.trace.dy = y2 - y1;
    sight.x2 = x2;
    sight.y2 = y2;

    return CrossBSPNode(&sight, numnodes - 1);
}

static fixed_t ToFixed(double value)
{
    return (fixed_t) floor(value * FRACUNIT + 0.5);
}

int K_Sight_Check(double x1, double y1, double eye, double x2, double y2, double bottom,
                  double top)
{
    return Check(ToFixed(x1), ToFixed(y1), ToFixed(eye), ToFixed(x2), ToFixed(y2),
                 ToFixed(bottom), ToFixed(top));
}

// The trace's slopes are heights at its end (P_CheckSight's units), so the
// autoaim's window is clamped there.
int K_Sight_Aim(double x1, double y1, double z, double x2, double y2, double bottom,
                double top)
{
    double reach = sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1)) * (100.0 / 160.0);

    top = SDL_min(top, z + reach);
    bottom = SDL_max(bottom, z - reach);

    return top > bottom && K_Sight_Check(x1, y1, z, x2, y2, bottom, top);
}

int K_Sight_Things(const mobj_t *looker, const mobj_t *target)
{
    return Check(looker->x, looker->y, looker->z + looker->height - (looker->height >> 2),
                 target->x, target->y, target->z, target->z + target->height);
}
