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
#include "k_log.h"
#include "k_map.h"
#include "k_settings.h"

#define FRAME_INTERVAL_MS       16      // redraw at most ~60 times a second
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
static bool view_subsectors;
static bool view_centroids;
static std::vector<ImVec2> screen_points;   // scratch for polygon drawing

static const ImVec4 category_colors[K_LOG_NUMCATEGORIES] =
{
    ImVec4(0.65f, 0.65f, 0.65f, 1.0f),  // K_LOG_SYS
    ImVec4(0.45f, 0.75f, 0.95f, 1.0f),  // K_LOG_MAP
    ImVec4(0.95f, 0.80f, 0.35f, 1.0f),  // K_LOG_BRAIN
    ImVec4(0.50f, 0.85f, 0.50f, 1.0f),  // K_LOG_NAV
    ImVec4(0.75f, 0.60f, 0.95f, 1.0f),  // K_LOG_CTRL
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

static bool SubsectorOnScreen(const k_subsector_t *ss, const ImVec2 &center,
                              const ImVec2 &origin, const ImVec2 &size)
{
    ImVec2 top_left = MapToScreen(center, ss->min.x, ss->max.y);
    ImVec2 bottom_right = MapToScreen(center, ss->max.x, ss->min.y);

    return bottom_right.x >= origin.x && top_left.x <= origin.x + size.x
        && bottom_right.y >= origin.y && top_left.y <= origin.y + size.y;
}

// Leaves the polygon's screen points in screen_points. The y flip turns
// the map's counter-clockwise order into the clockwise order ImGui's
// anti-aliased fill expects.
static void ProjectSubsector(const k_map_t *map, const k_subsector_t *ss,
                             const ImVec2 &center)
{
    screen_points.clear();

    for (int i = 0; i < ss->num_points; ++i)
    {
        const k_vec2_t &point = map->points[ss->first_point + i];

        screen_points.push_back(MapToScreen(center, point.x, point.y));
    }
}

static void DrawSubsectors(ImDrawList *draw, const k_map_t *map,
                           const ImVec2 &center, const ImVec2 &origin,
                           const ImVec2 &size)
{
    for (int i = 0; i < map->num_subsectors; ++i)
    {
        const k_subsector_t *ss = &map->subsectors[i];

        if (ss->num_points < 3 || !SubsectorOnScreen(ss, center, origin, size))
        {
            continue;
        }

        ProjectSubsector(map, ss, center);
        draw->AddConvexPolyFilled(screen_points.data(), ss->num_points,
                                  SubsectorColor(i, 0.30f));
        draw->AddPolyline(screen_points.data(), ss->num_points,
                          ss->degenerate ? IM_COL32(255, 0, 255, 255)
                                         : IM_COL32(255, 255, 255, 40),
                          ImDrawFlags_Closed, SDL_max(1.0f, ui_scale));
    }
}

static void DrawCentroids(ImDrawList *draw, const k_map_t *map,
                          const ImVec2 &center)
{
    for (int i = 0; i < map->num_subsectors; ++i)
    {
        const k_subsector_t *ss = &map->subsectors[i];
        ImVec2 at = MapToScreen(center, ss->centroid.x, ss->centroid.y);

        if (ss->num_points == 0)
        {
            continue;
        }

        if (ss->centroid_mismatch)
        {
            draw->AddCircleFilled(at, 4.0f * ui_scale, IM_COL32(255, 60, 60, 255));
        }
        else
        {
            draw->AddCircleFilled(at, 1.5f * ui_scale, IM_COL32(230, 230, 230, 200));
        }
    }
}

static void DrawHoveredSubsector(ImDrawList *draw, const k_map_t *map,
                                 int index, const ImVec2 &center)
{
    const k_subsector_t *ss = &map->subsectors[index];
    double floor, ceiling;

    if (ss->num_points >= 3)
    {
        ProjectSubsector(map, ss, center);
        draw->AddConvexPolyFilled(screen_points.data(), ss->num_points,
                                  IM_COL32(255, 255, 255, 40));
        draw->AddPolyline(screen_points.data(), ss->num_points,
                          IM_COL32(255, 255, 255, 220), ImDrawFlags_Closed,
                          SDL_max(1.0f, ui_scale));
    }

    K_Map_SectorHeights(ss->sector, &floor, &ceiling);
    ImGui::SetTooltip("Subsector %d%s\nSector %d\nFloor %.0f, ceiling %.0f\n"
                      "%d vertices, area %.0f", index,
                      ss->degenerate ? " (degenerate)" : "", ss->sector,
                      floor, ceiling, ss->num_points, ss->area);
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
    ImGui::Checkbox("Follow player", &view_follow);
    ImGui::SameLine();
    ImGui::Checkbox("Plain lines", &view_plain_lines);

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Two-sided lines with no height change. The automap "
                          "only shows these with the map cheat.");
    }

    ImGui::SameLine();
    ImGui::Checkbox("Subsectors", &view_subsectors);
    ImGui::SameLine();
    ImGui::Checkbox("Centroids", &view_centroids);

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Red: the engine places this centroid in a "
                          "different subsector.");
    }

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();

    if (size.x < 1.0f || size.y < 1.0f)
    {
        return;
    }

    // Left button reserved for picking targets (M5).
    ImGui::InvisibleButton("canvas", size, ImGuiButtonFlags_MouseButtonLeft
                                         | ImGuiButtonFlags_MouseButtonRight
                                         | ImGuiButtonFlags_MouseButtonMiddle);

    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
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

    int hovered_subsector = -1;

    if (hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Right)
                && !ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
    {
        double x, y;

        ScreenToMap(center, io.MousePos, &x, &y);
        hovered_subsector = K_Map_SubsectorAt(x, y);
    }

    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

    if (view_subsectors)
    {
        DrawSubsectors(draw, map, center, origin, size);
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

    if (view_centroids)
    {
        DrawCentroids(draw, map, center);
    }

    if (hovered_subsector >= 0)
    {
        DrawHoveredSubsector(draw, map, hovered_subsector, center);
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
        StatRow("Polygon points", "%d", map->num_points);
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
            ImGui::SetTooltip("BSP leaves that are solid space behind their own "
                              "one-sided walls, e.g. inside a pillar. Harmless.");
        }

        ProblemRow("Degenerate", map->degenerate_subsectors);

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Subsectors whose floor lies behind one of their "
                              "two-sided segs (buggy nodes); their BSP region "
                              "is used. Shown with a magenta outline.");
        }
        ProblemRow("Unreached", map->unreached_subsectors);
        ProblemRow("Centroid mismatches", map->centroid_mismatches);
        StatRow("Build time", "%.1f ms", map->build_ms);
        ImGui::EndTable();
    }
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
            ImGui::TextDisabled("Priority list - arrives in M7");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Bot"))
        {
            ImGui::TextDisabled("Bot state - arrives in M6");
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
