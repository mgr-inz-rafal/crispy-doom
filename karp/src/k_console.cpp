//
// DESCRIPTION:
//	Karp Console: a second SDL window running Dear ImGui. Status line on
//	top, map canvas and side tabs in the middle, rolling log at the
//	bottom (karp/docs/plan.md, 3.7).
//
//	Talks only to Karp's public headers, never to engine headers.
//

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "SDL.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include "k_brain.h"
#include "k_console.h"
#include "k_graph.h"
#include "k_jump.h"
#include "k_log.h"
#include "k_map.h"
#include "k_nav.h"
#include "k_path.h"
#include "k_settings.h"

#define FRAME_INTERVAL_MS       16      // redraw at most ~60 times a second
#define TICS_PER_SECOND         35.0    // the game's TICRATE
#define PLACEMENT_SAVE_DELAY_MS 1000    // save once the window is still

// At 100% display scaling; multiplied by the display's scale factor.
#define DEFAULT_WIDTH   960
#define DEFAULT_HEIGHT  720
#define MIN_WIDTH       480
#define MIN_HEIGHT      360

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Window *game_window;
static float ui_scale = 1.0f;

static Uint32 last_frame_time;
static Uint32 placement_change_time;
static bool placement_pending;

static float log_height;
static bool log_show[K_LOG_NUMCATEGORIES];
static ImGuiTextFilter log_filter;
static std::vector<int> log_rows;   // entries passing the filters
static unsigned int log_rows_serial;
static bool log_rows_valid;
static bool log_at_bottom = true;
static bool log_scroll_to_bottom;

static unsigned int view_generation;    // map the view was last fitted to
static double view_x, view_y;           // map point at the canvas centre
static double view_scale;               // pixels per map unit
static bool view_follow;
static bool view_plain_lines = true;
static bool view_cells;
static bool view_centroids;
static bool view_graph;
static std::vector<ImVec2> screen_points;   // scratch for polygon drawing

static const ImVec4 category_colors[K_LOG_NUMCATEGORIES] =
{
    ImVec4(0.65f, 0.65f, 0.65f, 1.0f),  // K_LOG_SYS
    ImVec4(0.45f, 0.75f, 0.95f, 1.0f),  // K_LOG_MAP
    ImVec4(0.95f, 0.80f, 0.35f, 1.0f),  // K_LOG_BRAIN
    ImVec4(0.50f, 0.85f, 0.50f, 1.0f),  // K_LOG_NAV
    ImVec4(0.75f, 0.60f, 0.95f, 1.0f),  // K_LOG_CTRL
    ImVec4(0.40f, 0.85f, 0.90f, 1.0f),  // K_LOG_PLAYER
};

static const ImVec4 warning_color = ImVec4(1.00f, 0.85f, 0.30f, 1.0f);
static const ImVec4 error_color = ImVec4(1.00f, 0.40f, 0.35f, 1.0f);

static float ClampFloat(float value, float low, float high)
{
    // low wins when the range is empty
    return SDL_max(low, SDL_min(value, high));
}

//
// Placement
//

// The title bar sits just above this strip: while it overlaps a display,
// the window can always be grabbed and moved.
static bool TopStripOnSomeDisplay(const SDL_Rect &rect)
{
    SDL_Rect strip = { rect.x, rect.y, rect.w, 32 };
    SDL_Rect bounds, overlap;

    for (int i = 0; i < SDL_GetNumVideoDisplays(); ++i)
    {
        if (SDL_GetDisplayUsableBounds(i, &bounds) == 0
         && SDL_IntersectRect(&strip, &bounds, &overlap)
         && overlap.w >= 64 && overlap.h >= 16)
        {
            return true;
        }
    }

    return false;
}

// The game window's display, or the primary one.
static int GameDisplay(void)
{
    int display = game_window ? SDL_GetWindowDisplayIndex(game_window) : 0;

    return display >= 0 ? display : 0;
}

// Next to a windowed game if there is room on its display, otherwise
// centred on that display. The game window itself is never moved.
static SDL_Rect DefaultRect(void)
{
    float scale = ImGui_ImplSDL2_GetContentScaleForDisplay(GameDisplay());
    int default_w = (int) (DEFAULT_WIDTH * scale);
    int default_h = (int) (DEFAULT_HEIGHT * scale);
    SDL_Rect usable = { 0, 0, default_w, default_h };
    SDL_Rect rect;
    int top = 0, left = 0, bottom = 0, right = 0;

    SDL_GetDisplayUsableBounds(GameDisplay(), &usable);

    if (game_window != NULL)
    {
        // The console's borders are unknown until it exists; assume they
        // match the game window's.
        SDL_GetWindowBordersSize(game_window, &top, &left, &bottom, &right);

        if (!(SDL_GetWindowFlags(game_window) & SDL_WINDOW_FULLSCREEN))
        {
            int game_x, game_y, game_w, game_h;

            SDL_GetWindowPosition(game_window, &game_x, &game_y);
            SDL_GetWindowSize(game_window, &game_w, &game_h);

            rect.x = game_x + game_w + right + left;
            rect.w = SDL_min(default_w, usable.x + usable.w - right - rect.x);
            rect.h = SDL_min(default_h, usable.h - top - bottom);

            if (rect.w >= (int) (MIN_WIDTH * scale))
            {
                rect.y = SDL_max(usable.y + top,
                                 SDL_min(game_y, usable.y + usable.h - bottom - rect.h));
                return rect;
            }
        }
    }

    rect.w = SDL_min(default_w, usable.w - left - right);
    rect.h = SDL_min(default_h, usable.h - top - bottom);
    rect.x = usable.x + (usable.w - rect.w) / 2;
    rect.y = usable.y + (usable.h - rect.h) / 2;
    return rect;
}

// Polled every frame rather than driven by window events: while a window
// is being maximized SDL can report the new size before the maximized
// flag, but between event pumps the two always agree.
static void TrackPlacement(void)
{
    Uint32 flags = SDL_GetWindowFlags(window);
    int changed = 0;
    int x, y, w, h;

    if (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED))
    {
        return;
    }

    // Keep the normal rect while maximized, so un-maximizing after a
    // restart goes back to it.
    if (!(flags & SDL_WINDOW_MAXIMIZED))
    {
        SDL_GetWindowPosition(window, &x, &y);
        SDL_GetWindowSize(window, &w, &h);
        changed |= K_Settings_SetInt("console_x", x);
        changed |= K_Settings_SetInt("console_y", y);
        changed |= K_Settings_SetInt("console_w", w);
        changed |= K_Settings_SetInt("console_h", h);
    }

    changed |= K_Settings_SetInt("console_maximized",
                                 (flags & SDL_WINDOW_MAXIMIZED) != 0);

    if (changed)
    {
        placement_change_time = SDL_GetTicks();
        placement_pending = true;
    }
}

// Saving soon after a change (rather than only at exit) keeps the placement
// even if the game crashes or is killed.
static void SavePlacementWhenStill(Uint32 now)
{
    if (placement_pending
     && now - placement_change_time >= PLACEMENT_SAVE_DELAY_MS)
    {
        K_Settings_SaveIfDirty();
        placement_pending = false;
    }
}

//
// Log pane
//

static void RefreshLogRows(void)
{
    if (log_rows_valid && log_rows_serial == K_Log_Serial())
    {
        return;
    }

    log_rows.clear();

    for (int i = 0; i < K_Log_Count(); ++i)
    {
        const k_logentry_t *entry = K_Log_Entry(i);

        if (log_show[entry->category] && log_filter.PassFilter(entry->text))
        {
            log_rows.push_back(i);
        }
    }

    log_rows_serial = K_Log_Serial();
    log_rows_valid = true;
}

static void CopyLogToClipboard(void)
{
    std::string text;
    char time[16];
    char line[K_LOG_LINE_LEN + 64];

    RefreshLogRows();

    for (int row : log_rows)
    {
        const k_logentry_t *entry = K_Log_Entry(row);

        K_Log_FormatTime(entry->leveltime, time, sizeof(time));

        if (entry->repeat > 1)
        {
            snprintf(line, sizeof(line), "%s %-5s %s (x%d)\n", time,
                     K_Log_CategoryName(entry->category), entry->text,
                     entry->repeat);
        }
        else
        {
            snprintf(line, sizeof(line), "%s %-5s %s\n", time,
                     K_Log_CategoryName(entry->category), entry->text);
        }

        text += line;
    }

    ImGui::SetClipboardText(text.c_str());
}

static void DrawLogToolbar(void)
{
    bool changed = false;

    for (int i = 0; i < K_LOG_NUMCATEGORIES; ++i)
    {
        k_logcat_t category = (k_logcat_t) i;

        ImGui::PushStyleColor(ImGuiCol_Text, category_colors[i]);

        if (ImGui::Checkbox(K_Log_CategoryName(category), &log_show[i]))
        {
            changed = true;

            // CTRL is high-volume, so it is only recorded at all while shown.
            if (category == K_LOG_CTRL)
            {
                K_Log_SetCategoryEnabled(category, log_show[i]);
            }
        }

        ImGui::PopStyleColor();

        if (category == K_LOG_CTRL && ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Controller details. High volume, so only "
                              "recorded while shown.");
        }

        ImGui::SameLine();
    }

    changed |= log_filter.Draw("Filter", 160.0f * ui_scale);

    ImGui::SameLine();

    if (ImGui::Button("Clear"))
    {
        K_Log_Clear();
    }

    ImGui::SameLine();

    if (ImGui::Button("Copy"))
    {
        CopyLogToClipboard();
    }

    if (!log_at_bottom)
    {
        ImGui::SameLine();

        if (ImGui::Button("Jump to latest"))
        {
            log_scroll_to_bottom = true;
        }
    }

    if (changed)
    {
        log_rows_valid = false;
    }
}

static void DrawLogLine(const k_logentry_t *entry, float category_x,
                        float text_x)
{
    ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    char time[16];

    if (entry->level == K_LOG_ERROR)
    {
        color = error_color;
    }
    else if (entry->level == K_LOG_WARN)
    {
        color = warning_color;
    }

    K_Log_FormatTime(entry->leveltime, time, sizeof(time));
    ImGui::TextDisabled("%s", time);
    ImGui::SameLine(category_x);
    ImGui::TextColored(category_colors[entry->category], "%s",
                       K_Log_CategoryName(entry->category));
    ImGui::SameLine(text_x);

    if (entry->repeat > 1)
    {
        ImGui::TextColored(color, "%s (x%d)", entry->text, entry->repeat);
    }
    else
    {
        ImGui::TextColored(color, "%s", entry->text);
    }
}

static void DrawLogLines(void)
{
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float category_x = ImGui::CalcTextSize("00:00.0").x + spacing;
    float text_x = category_x + ImGui::CalcTextSize("BRAIN").x + spacing;
    ImGuiListClipper clipper;

    RefreshLogRows();

    ImGui::BeginChild("lines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);

    clipper.Begin((int) log_rows.size());

    while (clipper.Step())
    {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
        {
            DrawLogLine(K_Log_Entry(log_rows[row]), category_x, text_x);
        }
    }

    clipper.End();

    // Follow new lines only while the view is at the bottom, so scrolling
    // up to read something keeps it in place.
    if (log_scroll_to_bottom || ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
    {
        ImGui::SetScrollHereY(1.0f);
    }

    log_scroll_to_bottom = false;
    log_at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();

    ImGui::EndChild();
}

static void DrawLogPane(float height)
{
    ImGui::BeginChild("log", ImVec2(0.0f, height), ImGuiChildFlags_Borders);
    DrawLogToolbar();
    ImGui::Separator();
    DrawLogLines();
    ImGui::EndChild();
}

//
// Map view
//

// Automap colours (am_map.c): red walls, brown floor steps, yellow ceiling
// steps, grey for lines the automap only shows with the map cheat.
static ImU32 LineColor(k_lineclass_t line_class)
{
    switch (line_class)
    {
        case K_LINE_ONE_SIDED:
            return IM_COL32(220, 50, 50, 255);

        case K_LINE_FLOOR_STEP:
            return IM_COL32(190, 130, 80, 255);

        case K_LINE_CEILING_STEP:
            return IM_COL32(230, 210, 60, 255);

        default:
            return IM_COL32(95, 95, 95, 255);
    }
}

// Y is flipped: map y points north, screen y points down.
static ImVec2 MapToScreen(const ImVec2 &center, double x, double y)
{
    return ImVec2((float) (center.x + (x - view_x) * view_scale),
                  (float) (center.y - (y - view_y) * view_scale));
}

static void ScreenToMap(const ImVec2 &center, const ImVec2 &pos,
                        double *x, double *y)
{
    *x = view_x + (pos.x - center.x) / view_scale;
    *y = view_y - (pos.y - center.y) / view_scale;
}

static void FitView(const k_map_t *map, const ImVec2 &size)
{
    double width = SDL_max(map->max.x - map->min.x, 1.0);
    double height = SDL_max(map->max.y - map->min.y, 1.0);

    view_x = (map->min.x + map->max.x) * 0.5;
    view_y = (map->min.y + map->max.y) * 0.5;
    view_scale = SDL_min(size.x / width, size.y / height) * 0.95;
}

// Golden-ratio hue steps keep neighbouring subsectors apart in colour.
static ImU32 SubsectorColor(int index, float alpha)
{
    float r, g, b;

    ImGui::ColorConvertHSVtoRGB(fmodf(index * 0.618034f, 1.0f), 0.6f, 0.9f,
                                r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, alpha));
}

static bool CellOnScreen(const k_cell_t *cell, const ImVec2 &center,
                         const ImVec2 &origin, const ImVec2 &size)
{
    ImVec2 top_left = MapToScreen(center, cell->min.x, cell->max.y);
    ImVec2 bottom_right = MapToScreen(center, cell->max.x, cell->min.y);

    return bottom_right.x >= origin.x && top_left.x <= origin.x + size.x
        && bottom_right.y >= origin.y && top_left.y <= origin.y + size.y;
}

// Leaves the cell's screen points in screen_points. The y flip turns the
// map's counter-clockwise order into the clockwise order ImGui's
// anti-aliased fill expects.
static void ProjectCell(const k_map_t *map, const k_cell_t *cell,
                        const ImVec2 &center)
{
    screen_points.clear();

    for (int i = 0; i < cell->num_points; ++i)
    {
        const k_vec2_t &point = map->points[cell->first_point + i];

        screen_points.push_back(MapToScreen(center, point.x, point.y));
    }
}

// Cells of the same subsector share its colour; their outlines show where
// a subsector's floor had to be split.
static void DrawCells(ImDrawList *draw, const k_map_t *map,
                      const ImVec2 &center, const ImVec2 &origin,
                      const ImVec2 &size)
{
    for (int i = 0; i < map->num_cells; ++i)
    {
        const k_cell_t *cell = &map->cells[i];

        if (!CellOnScreen(cell, center, origin, size))
        {
            continue;
        }

        ProjectCell(map, cell, center);
        draw->AddConvexPolyFilled(screen_points.data(), cell->num_points,
                                  SubsectorColor(cell->subsector, 0.30f));
        draw->AddPolyline(screen_points.data(), cell->num_points,
                          map->subsectors[cell->subsector].foreign_floor
                              ? IM_COL32(255, 0, 255, 255)
                              : IM_COL32(255, 255, 255, 40),
                          ImDrawFlags_Closed, SDL_max(1.0f, ui_scale));
    }
}

static void DrawCentroids(ImDrawList *draw, const k_map_t *map,
                          const ImVec2 &center)
{
    for (int i = 0; i < map->num_cells; ++i)
    {
        const k_cell_t *cell = &map->cells[i];
        ImVec2 at = MapToScreen(center, cell->centroid.x, cell->centroid.y);

        if (cell->centroid_mismatch)
        {
            draw->AddCircleFilled(at, 4.0f * ui_scale, IM_COL32(255, 60, 60, 255));
        }
        else
        {
            draw->AddCircleFilled(at, 1.5f * ui_scale, IM_COL32(230, 230, 230, 200));
        }
    }
}

static const ImU32 teleport_color = IM_COL32(200, 100, 255, 255);
static const ImU32 jump_color = IM_COL32(255, 150, 40, 255);

static void DrawDashedLine(ImDrawList *draw, const ImVec2 &a, const ImVec2 &b,
                           ImU32 color, float thickness)
{
    float dx = b.x - a.x, dy = b.y - a.y;
    float length = sqrtf(dx * dx + dy * dy);
    float dash = 6.0f * ui_scale;

    for (float t = 0.0f; t < length; t += 2.0f * dash)
    {
        float t1 = SDL_min(t + dash, length);

        draw->AddLine(ImVec2(a.x + dx * t / length, a.y + dy * t / length),
                      ImVec2(a.x + dx * t1 / length, a.y + dy * t1 / length),
                      color, thickness);
    }
}

// Each teleporter: a dashed line from its lines to its destination.
static void DrawTeleporters(ImDrawList *draw, const k_graph_t *graph,
                            const ImVec2 &center)
{
    float thickness = SDL_max(1.0f, ui_scale);

    for (int i = 0; i < graph->num_teleports; ++i)
    {
        const k_teleport_t *teleport = &graph->teleports[i];

        if (teleport->cell < 0)
        {
            continue;
        }

        ImVec2 to = MapToScreen(center, teleport->to.x, teleport->to.y);

        DrawDashedLine(draw, MapToScreen(center, (teleport->min.x + teleport->max.x) * 0.5,
                                         (teleport->min.y + teleport->max.y) * 0.5),
                       to, teleport_color, thickness);
        draw->AddCircle(to, 5.0f * ui_scale, teleport_color, 0, thickness);
    }
}

static void DrawNavGraph(ImDrawList *draw, const k_map_t *map,
                         const k_graph_t *graph, const ImVec2 &center)
{
    float thickness = SDL_max(1.0f, ui_scale);

    for (int i = 0; i < graph->num_portals; ++i)
    {
        const k_portal_t *portal = &graph->portals[i];

        // A jump: dashed from the run-up's start to the touchdown, a ring
        // there; faint while it does not work.
        if (portal->jump >= 0)
        {
            bool works = K_Graph_CanCross(i, portal->cell[0]) == K_CROSS_JUMP;
            ImU32 color = works ? jump_color
                                : (jump_color & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 90);
            ImVec2 to = MapToScreen(center, portal->b.x, portal->b.y);

            DrawDashedLine(draw, MapToScreen(center, portal->a.x, portal->a.y), to, color,
                           thickness);
            draw->AddCircle(to, 4.0f * ui_scale, color, 0, thickness);
            continue;
        }

        k_crossing_t forward_crossing = K_Graph_CanCross(i, portal->cell[0]);
        k_crossing_t backward_crossing = K_Graph_CanCross(i, portal->cell[1]);
        bool forward = K_Graph_Passable(forward_crossing) != 0;
        bool backward = K_Graph_Passable(backward_crossing) != 0;
        bool teleport = forward_crossing == K_CROSS_TELEPORT
                     || backward_crossing == K_CROSS_TELEPORT;
        bool action = (forward && forward_crossing != K_CROSS_OPEN)
                   || (backward && backward_crossing != K_CROSS_OPEN);
        ImU32 color = teleport ? teleport_color
                    : action ? IM_COL32(70, 150, 255, 255)
                    : forward && backward ? IM_COL32(80, 200, 90, 255)
                    : forward || backward ? IM_COL32(235, 200, 50, 255)
                    : IM_COL32(230, 60, 60, 255);
        ImU32 link_color = (color & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 110);
        const k_vec2_t &c0 = map->cells[portal->cell[0]].centroid;
        const k_vec2_t &c1 = map->cells[portal->cell[1]].centroid;
        ImVec2 mid = MapToScreen(center, (portal->a.x + portal->b.x) * 0.5,
                                 (portal->a.y + portal->b.y) * 0.5);

        draw->AddLine(MapToScreen(center, c0.x, c0.y), mid, link_color, thickness);
        draw->AddLine(mid, MapToScreen(center, c1.x, c1.y), link_color, thickness);
        draw->AddLine(MapToScreen(center, portal->a.x, portal->a.y),
                      MapToScreen(center, portal->b.x, portal->b.y),
                      color, 2.0f * thickness);

        // One way only: an arrowhead at the portal, pointing the way the
        // player can go.
        if (forward != backward)
        {
            const k_vec2_t &target = forward ? c1 : c0;
            ImVec2 to = MapToScreen(center, target.x, target.y);
            float dx = to.x - mid.x, dy = to.y - mid.y;
            float length = sqrtf(dx * dx + dy * dy);
            float size = 6.0f * ui_scale;

            if (length > 0.0f)
            {
                dx /= length;
                dy /= length;
                draw->AddTriangleFilled(
                    ImVec2(mid.x + dx * size, mid.y + dy * size),
                    ImVec2(mid.x - dy * size * 0.6f, mid.y + dx * size * 0.6f),
                    ImVec2(mid.x + dy * size * 0.6f, mid.y - dx * size * 0.6f),
                    color);
            }
        }
    }

    DrawTeleporters(draw, graph, center);
}

// Solid things as the squares that stop the player: barrels green, other
// things grey, faint once gone (a barrel blew up).
static void DrawBlocks(ImDrawList *draw, const k_map_t *map, const ImVec2 &center)
{
    float thickness = SDL_max(1.0f, ui_scale);

    for (int i = 0; i < map->num_blocks; ++i)
    {
        const k_block_t *block = &map->blocks[i];
        bool stands = K_Map_BlockStands(i) != 0;
        ImU32 color = block->barrel ? IM_COL32(120, 200, 80, 255)
                                    : IM_COL32(190, 190, 200, 255);
        ImVec2 top_left = MapToScreen(center, block->at.x - block->radius,
                                      block->at.y + block->radius);
        ImVec2 bottom_right = MapToScreen(center, block->at.x + block->radius,
                                          block->at.y - block->radius);

        if (!stands)
        {
            color = (color & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 70);
        }
        else
        {
            draw->AddRectFilled(top_left, bottom_right,
                                (color & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 60));
        }

        draw->AddRect(top_left, bottom_right, color, 0.0f, 0, thickness);
    }
}

// "4 neighbours: 3 open, 1 step too high"
static std::string DescribeNeighbours(const k_graph_t *graph, int index)
{
    int counts[K_NUM_CROSSINGS] = { 0 };
    int first = graph->first_link[index];
    int count = graph->link_count[index];
    const char *separator = ": ";
    char part[64];
    std::string text;

    for (int i = 0; i < count; ++i)
    {
        ++counts[K_Graph_CanCross(graph->links[first + i].portal, index)];
    }

    snprintf(part, sizeof(part), "%d neighbours", count);
    text = part;

    for (int c = 0; c < K_NUM_CROSSINGS; ++c)
    {
        if (counts[c] > 0)
        {
            snprintf(part, sizeof(part), "%s%d %s", separator, counts[c],
                     K_Graph_CrossingName((k_crossing_t) c));
            text += part;
            separator = ", ";
        }
    }

    return text;
}

static void DrawHoveredCell(ImDrawList *draw, const k_map_t *map, int index,
                            const ImVec2 &center)
{
    const k_cell_t *cell = &map->cells[index];
    const k_subsector_t *ss = &map->subsectors[cell->subsector];
    const k_graph_t *graph = K_Graph_Get();
    std::string neighbours;
    char cell_of[32] = "";
    char under[64] = "";
    double floor, ceiling;

    if (graph->generation == map->generation)
    {
        neighbours = DescribeNeighbours(graph, index);
    }

    if (ss->num_cells > 1)
    {
        snprintf(cell_of, sizeof(cell_of), ", cell %d of %d",
                 index - ss->first_cell + 1, ss->num_cells);
    }

    if (cell->block >= 0)
    {
        snprintf(under, sizeof(under), "\nUnder %s %d (%s)",
                 map->blocks[cell->block].barrel ? "barrel" : "solid thing", cell->block,
                 K_Map_CellBlock(index) >= 0 ? "standing" : "gone");
    }

    ProjectCell(map, cell, center);
    draw->AddConvexPolyFilled(screen_points.data(), cell->num_points,
                              IM_COL32(255, 255, 255, 40));
    draw->AddPolyline(screen_points.data(), cell->num_points,
                      IM_COL32(255, 255, 255, 220), ImDrawFlags_Closed,
                      SDL_max(1.0f, ui_scale));

    K_Map_SectorHeights(ss->sector, &floor, &ceiling);
    ImGui::SetTooltip("Subsector %d%s%s\nSector %d\nFloor %.0f, ceiling %.0f\n"
                      "%d vertices, area %.0f%s\n%s", cell->subsector, cell_of,
                      ss->foreign_floor ? " (other-sector floor)" : "", ss->sector,
                      floor, ceiling, cell->num_points, cell->area, under,
                      neighbours.c_str());
}

static void DrawPlayerArrow(ImDrawList *draw, const ImVec2 &center,
                            const k_pose_t &player)
{
    // Twice the player's width, but never too small to see.
    float length = SDL_max(16.0f * ui_scale, (float) (64.0 * view_scale));
    float c = (float) cos(player.angle) * length;
    float s = (float) sin(player.angle) * length;
    ImVec2 at = MapToScreen(center, player.x, player.y);

    // Arrow shape pointing along +x, rotated to the player's facing; screen
    // y is flipped.
    auto point = [&](float fx, float fy)
    {
        return ImVec2(at.x + fx * c - fy * s, at.y - (fx * s + fy * c));
    };

    ImVec2 tip = point(0.5f, 0.0f);
    ImVec2 left = point(-0.5f, 0.35f);
    ImVec2 notch = point(-0.25f, 0.0f);
    ImVec2 right = point(-0.5f, -0.35f);
    ImU32 color = IM_COL32(255, 255, 255, 255);

    draw->AddTriangleFilled(tip, left, notch, color);
    draw->AddTriangleFilled(tip, notch, right, color);
}

// Blue for a route the player can take, orange for one that is blocked.
static ImU32 RouteColor(const k_path_t *path, int alpha)
{
    return path->result == K_PATH_FOUND ? IM_COL32(60, 200, 255, alpha)
                                        : IM_COL32(255, 140, 50, alpha);
}

// The legs taken carefully, near an edge: mint on a blue route, pale amber
// on an orange one.
static ImU32 CarefulColor(const k_path_t *path)
{
    return path->result == K_PATH_FOUND ? IM_COL32(70, 235, 160, 255)
                                        : IM_COL32(255, 210, 120, 255);
}

static void DrawRouteCells(ImDrawList *draw, const k_map_t *map,
                           const k_path_t *path, const ImVec2 &center)
{
    for (int i = 0; i < path->num_cells; ++i)
    {
        const k_cell_t *cell = &map->cells[path->cells[i]];

        ProjectCell(map, cell, center);
        draw->AddConvexPolyFilled(screen_points.data(), cell->num_points,
                                  RouteColor(path, 45));
    }
}

// The line to follow, its waypoints, and on a blocked route the crossing
// that blocks it, in red. A teleport or a jump is dashed. Legs taken
// carefully, and the waypoints between them, stand out in their own colour.
static void DrawRoute(ImDrawList *draw, const k_path_t *path,
                      const ImVec2 &center)
{
    float thickness = SDL_max(1.0f, ui_scale);
    ImU32 color = RouteColor(path, 255);
    ImU32 careful = CarefulColor(path);
    int first = 0;

    screen_points.clear();

    for (int i = 0; i < path->num_points; ++i)
    {
        screen_points.push_back(MapToScreen(center, path->points[i].x,
                                            path->points[i].y));
    }

    // The walked stretches between the teleports and jumps.
    for (int i = 0; i < path->num_points; ++i)
    {
        if (i < path->num_points - 1 && path->legs[i] == K_LEG_WALK)
        {
            continue;
        }

        if (i > first)
        {
            draw->AddPolyline(&screen_points[first], i - first + 1, color,
                              ImDrawFlags_None, 2.0f * thickness);
        }

        if (i < path->num_points - 1)
        {
            DrawDashedLine(draw, screen_points[i], screen_points[i + 1],
                           path->legs[i] == K_LEG_JUMP ? jump_color : teleport_color,
                           thickness);
        }

        first = i + 1;
    }

    for (int i = 0; i < path->num_points - 1; ++i)
    {
        if (path->legs[i] == K_LEG_WALK && K_Nav_CarefulLeg(i))
        {
            draw->AddLine(screen_points[i], screen_points[i + 1], careful, 2.0f * thickness);
        }
    }

    for (int i = 1; i < path->num_points - 1; ++i)
    {
        draw->AddCircleFilled(screen_points[i], 2.5f * ui_scale,
                              K_Nav_CarefulLeg(i - 1) && K_Nav_CarefulLeg(i) ? careful : color);
    }

    const k_navstatus_t *nav = K_Nav_GetStatus();

    // The waypoint the bot is driving to.
    if (nav->state == K_NAV_DRIVING && nav->target < path->num_points - 1)
    {
        draw->AddCircle(screen_points[nav->target], 5.0f * ui_scale,
                        IM_COL32(255, 255, 255, 255), 0, thickness);
    }

    // A gap inside a cell has no portal to show: a ring round it instead.
    if (path->result == K_PATH_BLOCKED && path->blocked_pinch)
    {
        draw->AddCircle(MapToScreen(center, path->blocked_at.x, path->blocked_at.y),
                        8.0f * ui_scale, IM_COL32(255, 40, 40, 255), 0, 2.0f * thickness);
    }
    else if (path->result == K_PATH_BLOCKED)
    {
        const k_portal_t *portal = &K_Graph_Get()->portals[path->blocked_portal];

        draw->AddLine(MapToScreen(center, portal->a.x, portal->a.y),
                      MapToScreen(center, portal->b.x, portal->b.y),
                      IM_COL32(255, 40, 40, 255), 4.0f * thickness);
    }
}

// Green when a route leads there, red otherwise.
static void DrawGoal(ImDrawList *draw, const k_path_t *path,
                     const ImVec2 &center)
{
    ImVec2 at = MapToScreen(center, path->goal.x, path->goal.y);
    ImU32 color = path->result == K_PATH_FOUND ? IM_COL32(80, 230, 100, 255)
                                               : IM_COL32(255, 60, 60, 255);

    draw->AddCircle(at, 7.0f * ui_scale, color, 0, 2.0f * SDL_max(1.0f, ui_scale));
    draw->AddCircleFilled(at, 2.0f * ui_scale, color);
}

static void DrawMapView(void)
{
    const k_map_t *map = K_Map_Get();
    ImGuiIO &io = ImGui::GetIO();
    k_pose_t player;
    bool have_player = K_Map_GetPlayer(&player) != 0;
    bool fit;

    if (map->loaded)
    {
        ImGui::TextUnformatted(map->name);
        ImGui::SameLine();
        ImGui::TextDisabled("%d lines", map->num_lines);
        ImGui::SameLine();
    }

    fit = ImGui::Button("Fit");
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &view_follow);

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Keep the player centred. Panning turns it off.");
    }

    ImGui::SameLine();
    ImGui::Checkbox("Plain lines", &view_plain_lines);

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Two-sided lines with no height change. The automap "
                          "only shows these with the map cheat.");
    }

    ImGui::SameLine();
    ImGui::Checkbox("Cells", &view_cells);

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Convex pieces of floor, coloured by subsector: the "
                          "nodes of the navigation graph.");
    }

    ImGui::SameLine();
    ImGui::Checkbox("Centroids", &view_centroids);

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Red: the engine places this centroid in a "
                          "different subsector.");
    }

    ImGui::SameLine();
    ImGui::Checkbox("Graph", &view_graph);

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Where neighbouring cells meet, from live sector "
                          "heights.\nGreen: open both ways. Yellow: one way "
                          "only (arrow), e.g. a drop.\nBlue: a door or lift "
                          "the bot can work.\nRed: blocked, e.g. a locked door, "
                          "a step too high or a solid thing.");
    }

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();

    if (size.x < 1.0f || size.y < 1.0f)
    {
        return;
    }

    // Left click sends the bot somewhere; right or middle drag pans.
    ImGui::InvisibleButton("canvas", size, ImGuiButtonFlags_MouseButtonLeft
                                         | ImGuiButtonFlags_MouseButtonRight
                                         | ImGuiButtonFlags_MouseButtonMiddle);

    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImVec2 center(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);
    ImDrawList *draw = ImGui::GetWindowDrawList();

    if (!map->loaded)
    {
        const char *text = "No level loaded";
        ImVec2 text_size = ImGui::CalcTextSize(text);

        draw->AddText(ImVec2(center.x - text_size.x * 0.5f,
                             center.y - text_size.y * 0.5f),
                      ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
        return;
    }

    if (fit || view_generation != map->generation)
    {
        FitView(map, size);
        view_generation = map->generation;
    }

    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Right)
                || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
    {
        view_x -= io.MouseDelta.x / view_scale;
        view_y += io.MouseDelta.y / view_scale;
        view_follow = false;
    }

    // Zoom around the point under the cursor.
    if (hovered && io.MouseWheel != 0.0f)
    {
        double x, y;

        ScreenToMap(center, io.MousePos, &x, &y);
        view_scale = SDL_max(0.005, SDL_min(view_scale * pow(1.2, io.MouseWheel),
                                            50.0));
        view_x = x - (io.MousePos.x - center.x) / view_scale;
        view_y = y + (io.MousePos.y - center.y) / view_scale;
    }

    if (view_follow && have_player)
    {
        view_x = player.x;
        view_y = player.y;
    }

    int hovered_cell = -1;

    if (hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Right)
                && !ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
    {
        double x, y;

        ScreenToMap(center, io.MousePos, &x, &y);
        hovered_cell = K_Map_CellAt(x, y);
    }

    if (clicked)
    {
        double x, y;

        ScreenToMap(center, io.MousePos, &x, &y);
        K_Brain_RequestGoto(x, y);
    }

    const k_path_t *path = K_Nav_GetPath();

    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

    if (view_cells)
    {
        DrawCells(draw, map, center, origin, size);
    }

    if (path != NULL)
    {
        DrawRouteCells(draw, map, path, center);
    }

    // Plain lines first, so walls and steps are drawn on top of them.
    float thickness = SDL_max(1.0f, ui_scale);

    for (int pass = 0; pass < 2; ++pass)
    {
        for (int i = 0; i < map->num_lines; ++i)
        {
            const k_line_t *line = &map->lines[i];
            k_lineclass_t line_class = K_Map_LineClass(i);
            bool plain = line_class == K_LINE_TWO_SIDED;

            if (plain != (pass == 0) || (plain && !view_plain_lines))
            {
                continue;
            }

            draw->AddLine(MapToScreen(center, line->v1.x, line->v1.y),
                          MapToScreen(center, line->v2.x, line->v2.y),
                          LineColor(line_class), thickness);
        }
    }

    DrawBlocks(draw, map, center);

    const k_graph_t *graph = K_Graph_Get();

    if (view_graph && graph->generation == map->generation)
    {
        DrawNavGraph(draw, map, graph, center);
    }

    if (view_centroids)
    {
        DrawCentroids(draw, map, center);
    }

    if (path != NULL)
    {
        DrawRoute(draw, path, center);
        DrawGoal(draw, path, center);
    }

    if (hovered_cell >= 0)
    {
        DrawHoveredCell(draw, map, hovered_cell, center);
    }

    if (have_player)
    {
        DrawPlayerArrow(draw, center, player);
    }

    if (hovered)
    {
        char text[64];
        double x, y;

        ScreenToMap(center, io.MousePos, &x, &y);
        snprintf(text, sizeof(text), "%.0f, %.0f", x, y);
        draw->AddText(ImVec2(origin.x + 6.0f * ui_scale,
                             origin.y + size.y - ImGui::GetTextLineHeight()
                             - 4.0f * ui_scale),
                      ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
    }

    draw->PopClipRect();
}

static void StatRow(const char *label, const char *fmt, ...)
{
    va_list args;

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

// A sanity-check count: highlighted when it is not zero.
static void ProblemRow(const char *label, int count)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);

    if (count > 0)
    {
        ImGui::TextColored(warning_color, "%d", count);
    }
    else
    {
        ImGui::Text("%d", count);
    }
}

static void DrawMapTab(void)
{
    const k_map_t *map = K_Map_Get();

    if (!map->loaded)
    {
        ImGui::TextDisabled("No level loaded");
        return;
    }

    if (ImGui::BeginTable("map_stats", 2, ImGuiTableFlags_RowBg))
    {
        StatRow("Map", "%s", map->name);
        StatRow("Lines", "%d", map->num_lines);
        StatRow("Sectors", "%d", map->num_sectors);
        StatRow("Subsectors", "%d", map->num_subsectors);
        StatRow("Nodes", "%d", map->num_nodes);
        StatRow("Floor cells", "%d", map->num_cells);

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Convex pieces of floor: the nodes of the "
                              "navigation graph.");
        }

        StatRow("Split subsectors", "%d", map->split_subsectors);

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Subsectors whose floor is not convex (e.g. a wall "
                              "corner inside), or holds a solid thing, so it is "
                              "split into several cells.");
        }

        StatRow("Solid things", "%d (%d barrels)", map->num_blocks, map->num_barrels);

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Solid things that are not monsters: decorations, "
                              "barrels. Cut out of the floor cells like pillars; "
                              "the cells under one are blocked while it stands.");
        }

        StatRow("Without area", "%d", map->no_area_subsectors);

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("BSP leaves with no usable floor: no area at all, "
                              "or hairline slivers left by vertex rounding. "
                              "Harmless.");
        }

        StatRow("Behind walls", "%d", map->behind_wall_subsectors);

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("BSP leaves that are all solid space behind walls, "
                              "e.g. inside a pillar. Harmless.");
        }

        StatRow("Other-sector floor", "%d", map->foreign_floor_subsectors);

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Subsectors with some floor that the surrounding "
                              "lines put in another sector: sector tricks or "
                              "buggy nodes. The engine, and so navigation, uses "
                              "the subsector's own sector there. Shown with a "
                              "magenta outline.");
        }

        ProblemRow("Unreached", map->unreached_subsectors);
        ProblemRow("Centroid mismatches", map->centroid_mismatches);
        StatRow("Build time", "%.1f ms", map->build_ms);
        ImGui::EndTable();
    }

    const k_graph_t *graph = K_Graph_Get();

    if (graph->generation != map->generation)
    {
        return;
    }

    ImGui::SeparatorText("Navigation graph");

    if (ImGui::BeginTable("graph_stats", 2, ImGuiTableFlags_RowBg))
    {
        StatRow("Portals", "%d", graph->num_portals - K_Jump_Count());
        StatRow("Jumps", "%d", K_Jump_Count());

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Straight runs over a gap to a floor that cannot be "
                              "climbed to from below: a running jump, or a step "
                              "across a gap narrower than the player. Dashed "
                              "orange on the graph overlay.");
        }

        ProblemRow("Holes", graph->holes);

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Stretches of cell border, away from walls, that "
                              "border no neighbour: holes in the navigation "
                              "mesh. %.0f units in total.", graph->hole_length);
        }

        StatRow("Isolated", "%d", graph->isolated_cells);

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Cells without neighbours, e.g. a closet reached "
                              "only by teleporter.");
        }

        StatRow("Components", "%d (largest %d)", graph->components,
                graph->largest_component);

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Groups of cells joined by portals, whether open "
                              "or not. Areas reached only by teleporter form "
                              "their own groups.");
        }

        StatRow("Build time", "%.1f ms", graph->build_ms);
        ImGui::EndTable();
    }
}

static void DrawRouteStats(const k_path_t *path)
{
    if (ImGui::BeginTable("route", 2, ImGuiTableFlags_RowBg))
    {
        StatRow("Goal", "%.0f, %.0f", path->goal.x, path->goal.y);
        StatRow("Result", "%s", K_Path_ResultName(path->result));

        if (path->num_cells > 0)
        {
            StatRow("Cells", "%d", path->num_cells);
            StatRow("Waypoints", "%d", path->num_points - 1);
            StatRow("Length", "%.0f units", path->length);
        }

        StatRow("Search", "%d nodes, %.2f ms", path->searched, path->search_ms);
        ImGui::EndTable();
    }

    if (path->result != K_PATH_FOUND)
    {
        char text[K_LOG_LINE_LEN];

        K_Path_Describe(path, text, sizeof(text));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(warning_color, "%s", text);
        ImGui::PopTextWrapPos();
    }
}

static const char *NavStateName(k_navstate_t state)
{
    switch (state)
    {
        case K_NAV_DRIVING:
            return "driving";

        case K_NAV_ARRIVED:
            return "arrived";

        case K_NAV_FAILED:
            return "failed";

        default:
            return "idle";
    }
}

static void DrawDriving(void)
{
    const k_navstatus_t *nav = K_Nav_GetStatus();
    const k_path_t *path = K_Nav_GetPath();
    k_navtuning_t *tuning = K_Nav_Tuning();
    k_pose_t player;

    ImGui::SeparatorText("Driving");

    if (ImGui::BeginTable("driving", 2, ImGuiTableFlags_RowBg))
    {
        StatRow("State", "%s", NavStateName(nav->state));

        if (K_Map_GetPlayer(&player))
        {
            StatRow("Speed", "%.1f units/tic",
                    sqrt(player.momx * player.momx + player.momy * player.momy));
        }

        if (nav->state == K_NAV_DRIVING && path != NULL)
        {
            StatRow("Waypoint", "%d of %d", nav->target, path->num_points - 1);
            StatRow("Remaining", "%.0f units", nav->remaining);

            if (nav->action != K_ACTION_NONE)
            {
                StatRow("Action", "%s", nav->action_text);
            }
        }

        if (nav->state != K_NAV_IDLE || nav->tics > 0)
        {
            StatRow("Time", "%.1f s", nav->tics / TICS_PER_SECOND);
            StatRow("Replans", "%d", nav->replans);
            StatRow("Prediction error", "%.2f units", nav->prediction_error);

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("How far the player ended up from where the "
                                  "controller expected after each tic, averaged "
                                  "over about a second. Walls the player slides "
                                  "along make it grow.");
            }
        }

        ImGui::EndTable();
    }

    if (nav->state == K_NAV_FAILED)
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(warning_color, "%s", nav->problem);
        ImGui::PopTextWrapPos();
    }

    ImGui::SeparatorText("Controller");

    static const double min_speed = 2.0, max_speed = 16.7;
    static const double min_braking = 0.3, max_braking = 3.0;
    static const double min_radius = 4.0, max_radius = 64.0;
    static const double min_turn = 1.0, max_turn = 45.0;

    ImGui::PushItemWidth(-FLT_MIN);
    ImGui::TextUnformatted("Top speed");
    ImGui::SliderScalar("##speed", ImGuiDataType_Double, &tuning->max_speed,
                        &min_speed, &max_speed, "%.1f units/tic");
    ImGui::TextUnformatted("Braking");
    ImGui::SliderScalar("##braking", ImGuiDataType_Double, &tuning->braking,
                        &min_braking, &max_braking, "%.2f units/tic/tic");

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The deceleration assumed when slowing down for "
                          "corners and the goal. Higher brakes later.");
    }

    ImGui::TextUnformatted("Waypoint radius");
    ImGui::SliderScalar("##radius", ImGuiDataType_Double, &tuning->advance_radius,
                        &min_radius, &max_radius, "%.0f units");

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("A waypoint on the way counts as reached this close.");
    }

    ImGui::TextUnformatted("Turn rate");
    ImGui::SliderScalar("##turn", ImGuiDataType_Double, &tuning->max_turn,
                        &min_turn, &max_turn, "%.1f degrees/tic");
    ImGui::PopItemWidth();
}

// The goal stack, top first: the top one is worked on; each goal below
// waits for the one above it.
static void DrawPrioritiesTab(void)
{
    char text[K_LOG_LINE_LEN * 2];
    int count = K_Brain_GoalCount();

    ImGui::PushTextWrapPos(0.0f);

    if (count == 0)
    {
        ImGui::TextDisabled("No goals");
    }

    for (int i = 0; i < count; ++i)
    {
        K_Brain_GoalText(i, text, sizeof(text));

        if (i == 0)
        {
            ImGui::TextColored(category_colors[K_LOG_BRAIN], "> %s", text);
        }
        else
        {
            ImGui::Text("  %s", text);
        }
    }

    if (K_Brain_FailedCount() > 0)
    {
        ImGui::SeparatorText("Failed this level");

        for (int i = 0; i < K_Brain_FailedCount(); ++i)
        {
            ImGui::TextColored(error_color, "%s", K_Brain_FailedText(i));
        }
    }

    ImGui::PopTextWrapPos();
}

static void DrawBotTab(void)
{
    const k_map_t *map = K_Map_Get();
    const k_path_t *path = K_Nav_GetPath();
    k_pose_t player;

    if (!map->loaded || !K_Map_GetPlayer(&player))
    {
        ImGui::TextDisabled("Not in a level");
        return;
    }

    int cell = player.cell;

    if (ImGui::BeginTable("player", 2, ImGuiTableFlags_RowBg))
    {
        StatRow("Position", "%.0f, %.0f", player.x, player.y);
        StatRow("Facing", "%.0f degrees", player.angle * (180.0 / 3.14159265358979323846));

        if (cell >= 0)
        {
            StatRow("Cell", "%d (subsector %d)", cell, map->cells[cell].subsector);
        }
        else
        {
            StatRow("Cell", "none");
        }

        ImGui::EndTable();
    }

    ImGui::SeparatorText("Route");

    if (path != NULL)
    {
        DrawRouteStats(path);
    }
    else
    {
        ImGui::TextDisabled("No goal - click the map");
    }

    DrawDriving();
}

//
// Layout
//

static void DrawStatusLine(void)
{
    ImGui::TextColored(category_colors[K_LOG_BRAIN], "KARP");
    ImGui::SameLine();
    ImGui::TextUnformatted(K_Brain_Status());
}

static void DrawTopArea(float height)
{
    float width = ImGui::GetContentRegionAvail().x;
    float side_width = SDL_min(300.0f * ui_scale, width * 0.45f);

    // The mouse wheel zooms the map, so the child must not scroll.
    ImGui::BeginChild("map", ImVec2(width - side_width
                                    - ImGui::GetStyle().ItemSpacing.x, height),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar
                    | ImGuiWindowFlags_NoScrollWithMouse);
    DrawMapView();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("side", ImVec2(0.0f, height), ImGuiChildFlags_Borders);

    if (ImGui::BeginTabBar("tabs"))
    {
        if (ImGui::BeginTabItem("Priorities"))
        {
            DrawPrioritiesTab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Bot"))
        {
            DrawBotTab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Map"))
        {
            DrawMapTab();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::EndChild();
}

static void DrawSplitter(float height)
{
    ImVec2 min, max;
    ImU32 color;
    float y;

    ImGui::InvisibleButton("##splitter",
                           ImVec2(ImGui::GetContentRegionAvail().x, height));

    if (ImGui::IsItemActive())
    {
        log_height -= ImGui::GetIO().MouseDelta.y;
    }

    color = ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_SeparatorActive
                             : ImGui::IsItemHovered() ? ImGuiCol_SeparatorHovered
                             : ImGuiCol_Separator);
    min = ImGui::GetItemRectMin();
    max = ImGui::GetItemRectMax();
    y = (min.y + max.y) * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, y), ImVec2(max.x, y),
                                        color, 2.0f * ui_scale);
}

static void DrawConsole(void)
{
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    float splitter_height = 6.0f * ui_scale;
    float available;

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Karp Console", NULL,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
               | ImGuiWindowFlags_NoSavedSettings
               | ImGuiWindowFlags_NoBringToFrontOnFocus);

    DrawStatusLine();
    ImGui::Separator();

    // Three items stacked below: top area, splitter, log pane.
    available = ImGui::GetContentRegionAvail().y - splitter_height
              - 2.0f * ImGui::GetStyle().ItemSpacing.y;
    log_height = ClampFloat(log_height, 80.0f * ui_scale,
                            available - 120.0f * ui_scale);

    DrawTopArea(SDL_max(1.0f, available - log_height));
    DrawSplitter(splitter_height);
    DrawLogPane(log_height);

    ImGui::End();
}

//
// Public interface
//

int K_Console_Open(SDL_Window *game)
{
    SDL_Rect rect;
    bool remembered = false;
    bool maximized = false;

    game_window = game;

    rect.x = K_Settings_GetInt("console_x", 0);
    rect.y = K_Settings_GetInt("console_y", 0);
    rect.w = K_Settings_GetInt("console_w", 0);
    rect.h = K_Settings_GetInt("console_h", 0);

    if (rect.w > 0 && rect.h > 0)
    {
        if (TopStripOnSomeDisplay(rect))
        {
            remembered = true;
            maximized = K_Settings_GetInt("console_maximized", 0) != 0;
        }
        else
        {
            K_Log(K_LOG_SYS, K_LOG_WARN, "remembered console position (%d,%d) "
                  "is off-screen - using the default placement", rect.x, rect.y);
        }
    }

    if (!remembered)
    {
        rect = DefaultRect();
    }

    // Hidden at first, so showing, maximizing and handing focus back to the
    // game happen in a controlled order.
    window = SDL_CreateWindow("Karp Console", rect.x, rect.y, rect.w, rect.h,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN
                            | SDL_WINDOW_ALLOW_HIGHDPI);

    if (window == NULL)
    {
        K_Log(K_LOG_SYS, K_LOG_ERROR, "could not create the console window: %s",
              SDL_GetError());
        return 0;
    }

    ui_scale = ImGui_ImplSDL2_GetContentScaleForWindow(window);

    if (ui_scale <= 0.0f)
    {
        ui_scale = 1.0f;
    }

    SDL_SetWindowMinimumSize(window, (int) (MIN_WIDTH * ui_scale),
                             (int) (MIN_HEIGHT * ui_scale));

    // No vsync: the game renderer already waits for it, and a second wait
    // every frame would halve the game's frame rate.
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    if (renderer == NULL)
    {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }

    if (renderer == NULL)
    {
        K_Log(K_LOG_SYS, K_LOG_ERROR, "could not create the console renderer: %s",
              SDL_GetError());
        SDL_DestroyWindow(window);
        window = NULL;
        return 0;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    // The mouse cursor is process-wide in SDL; leave it to the game.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.ScaleAllSizes(ui_scale);
    style.FontScaleDpi = ui_scale;

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    // Manual mode with no gamepads: the backend must not open the game's
    // controllers.
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, NULL, 0);
    ImGui_ImplSDLRenderer2_Init(renderer);

    log_height = 200.0f * ui_scale;

    for (int i = 0; i < K_LOG_NUMCATEGORIES; ++i)
    {
        log_show[i] = K_Log_CategoryEnabled((k_logcat_t) i) != 0;
    }

    SDL_ShowWindow(window);

    if (maximized)
    {
        SDL_MaximizeWindow(window);
    }

    // Showing a window gives it keyboard focus. Hand it back, so menu keys
    // reach the game straight away.
    if (game_window != NULL)
    {
        SDL_RaiseWindow(game_window);
    }

    K_Log(K_LOG_SYS, K_LOG_INFO, "console opened at (%d,%d) %dx%d%s - %s placement",
          rect.x, rect.y, rect.w, rect.h, maximized ? " maximized" : "",
          remembered ? "remembered" : "default");

    return 1;
}

void K_Console_Close(void)
{
    if (window == NULL)
    {
        return;
    }

    TrackPlacement();

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    renderer = NULL;
    window = NULL;
}

// The window an event belongs to, or 0 for events not tied to a window.
static Uint32 EventWindowID(const SDL_Event *event)
{
    switch (event->type)
    {
        case SDL_WINDOWEVENT:
            return event->window.windowID;

        case SDL_KEYDOWN:
        case SDL_KEYUP:
            return event->key.windowID;

        case SDL_TEXTEDITING:
            return event->edit.windowID;

        case SDL_TEXTINPUT:
            return event->text.windowID;

        case SDL_MOUSEMOTION:
            return event->motion.windowID;

        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            return event->button.windowID;

        case SDL_MOUSEWHEEL:
            return event->wheel.windowID;

        default:
            return 0;
    }
}

int K_Console_HandleEvent(SDL_Event *event)
{
    if (window == NULL || EventWindowID(event) != SDL_GetWindowID(window))
    {
        return 0;
    }

    // Hidden, not destroyed: everything stays set up, and there is no way
    // to reopen it in stage 1 anyway.
    if (event->type == SDL_WINDOWEVENT
     && event->window.event == SDL_WINDOWEVENT_CLOSE)
    {
        TrackPlacement();
        SDL_HideWindow(window);
        K_Log(K_LOG_SYS, K_LOG_INFO, "console closed - it reopens on the next run");
        return 1;
    }

    ImGui_ImplSDL2_ProcessEvent(event);
    return 1;
}

void K_Console_Frame(void)
{
    Uint32 now = SDL_GetTicks();

    if (window == NULL || now - last_frame_time < FRAME_INTERVAL_MS)
    {
        return;
    }

    last_frame_time = now;

    TrackPlacement();
    SavePlacementWhenStill(now);

    if (SDL_GetWindowFlags(window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED))
    {
        return;
    }

    ImGuiIO &io = ImGui::GetIO();

    // Text input is process-wide in SDL2; make sure it is on while an
    // ImGui text field is being edited.
    if (io.WantTextInput && !SDL_IsTextInputActive())
    {
        SDL_StartTextInput();
    }

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    DrawConsole();

    ImGui::Render();
    SDL_RenderSetScale(renderer, io.DisplayFramebufferScale.x,
                       io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
}
