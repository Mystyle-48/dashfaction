#include "commands.h"
#include "BuildConfig.h"
#include "lazyban.h"
#include "main.h"
#include "misc.h"
#include "packfile.h"
#include "rf.h"
#include "spectate_mode.h"
#include "stdafx.h"
#include "utils.h"
#include <algorithm>
#include <CallHook.h>
#include <FunHook.h>
#include <RegsPatch.h>

// Note: limit should fit in int8_t
constexpr int CMD_LIMIT = 127;

rf::DcCommand* g_commands_buffer[CMD_LIMIT];
bool g_dbg_geometry_rendering_stats = false;
bool g_dbg_static_lights = false;

rf::Player* FindBestMatchingPlayer(const char* name)
{
    rf::Player* found_player;
    int num_found = 0;
    FindPlayer(StringMatcher().Exact(name), [&](rf::Player* player) {
        found_player = player;
        ++num_found;
    });
    if (num_found == 1)
        return found_player;

    num_found = 0;
    FindPlayer(StringMatcher().Infix(name), [&](rf::Player* player) {
        found_player = player;
        ++num_found;
    });

    if (num_found == 1)
        return found_player;
    else if (num_found > 1)
        rf::DcPrintf("Found %d players matching '%s'!", num_found, name);
    else
        rf::DcPrintf("Cannot find player matching '%s'", name);
    return nullptr;
}

#if SPLITSCREEN_ENABLE

DcCommand2 splitscreen_cmd{
    "splitscreen",
    []() {
        if (rf::is_net_game)
            SplitScreenStart(); /* FIXME: set player 2 controls */
        else
            rf::DcPrintf("Works only in multiplayer game!");
    },
    "Starts split screen mode",
};

#endif // SPLITSCREEN_ENABLE

DcCommand2 max_fps_cmd{
    "maxfps",
    [](std::optional<int> limit_opt) {
        if (limit_opt) {
#ifdef NDEBUG
            int new_limit = std::clamp<int>(limit_opt.value(), MIN_FPS_LIMIT, MAX_FPS_LIMIT);
#else
            int new_limit = limit_opt.value();
#endif
            g_game_config.maxFps = new_limit;
            g_game_config.save();
            rf::min_framerate = 1.0f / new_limit;
        }
        else
            rf::DcPrintf("Maximal FPS: %.1f", 1.0f / rf::min_framerate);
    },
    "Sets maximal FPS",
    "maxfps <limit>",
};

DcCommand2 debug_cmd{
    "debug",
    [](std::optional<std::string> type_opt) {

#ifdef NDEBUG
        if (rf::is_net_game) {
            rf::DcPrintf("This command is disabled in multiplayer!");
            return;
        }
#endif

        auto type = type_opt ? type_opt.value() : "";
        bool handled = false;
        static bool all_flags = false;

        if (type.empty()) {
            all_flags = !all_flags;
            handled = true;
            rf::DcPrintf("All debug flags are %s", all_flags ? "enabled" : "disabled");
        }

        auto handle_flag = [&](bool& flag_ref, const char* flag_name) {
            bool old_flag_value = flag_ref;
            if (type == flag_name) {
                flag_ref = !flag_ref;
                handled = true;
                rf::DcPrintf("Debug flag '%s' is %s", flag_name, flag_ref ? "enabled" : "disabled");
            }
            else if (type.empty()) {
                flag_ref = all_flags;
            }
            return old_flag_value != flag_ref;
        };

        // clang-format off
        auto &dg_thruster                  = AddrAsRef<bool>(0x0062F3AA);
        auto &dg_lights                    = AddrAsRef<bool>(0x0062FE19);
        auto &dg_push_and_climbing_regions = AddrAsRef<bool>(0x0062FE1A);
        auto &dg_geo_regions               = AddrAsRef<bool>(0x0062FE1B);
        auto &dg_glass                     = AddrAsRef<bool>(0x0062FE1C);
        auto &dg_movers                    = AddrAsRef<bool>(0x0062FE1D);
        auto &dg_entity_burn               = AddrAsRef<bool>(0x0062FE1E);
        auto &dg_movement_mode             = AddrAsRef<bool>(0x0062FE1F);
        auto &dg_perfomance                = AddrAsRef<bool>(0x0062FE20);
        auto &dg_perf_bar                  = AddrAsRef<bool>(0x0062FE21);
        auto &dg_waypoints                 = AddrAsRef<bool>(0x0064E39C);
        auto &dg_network                   = AddrAsRef<bool>(0x006FED24);
        auto &dg_particle_stats            = AddrAsRef<bool>(0x007B2758);
        auto &dg_weapon                    = AddrAsRef<bool>(0x007CAB59);
        auto &dg_events                    = AddrAsRef<bool>(0x00856500);
        auto &dg_triggers                  = AddrAsRef<bool>(0x0085683C);
        auto &dg_obj_rendering             = AddrAsRef<bool>(0x009BB5AC);
        // Geometry rendering flags
        auto& render_everything_see_through       = AddrAsRef<bool>(0x009BB594);
        auto& render_rooms_in_different_colors    = AddrAsRef<bool>(0x009BB598);
        auto& render_non_see_through_portal_faces = AddrAsRef<bool>(0x009BB59C);
        auto& render_lightmaps_only               = AddrAsRef<bool>(0x009BB5A4);
        auto& render_no_lightmaps                 = AddrAsRef<bool>(0x009BB5A8);
        // clang-format on

        handle_flag(dg_thruster, "thruster");
        // debug string at the left-top corner
        handle_flag(dg_lights, "light");
        handle_flag(g_dbg_static_lights, "light2");
        handle_flag(dg_push_and_climbing_regions, "push_climb_reg");
        handle_flag(dg_geo_regions, "geo_reg");
        handle_flag(dg_glass, "glass");
        handle_flag(dg_movers, "mover");
        handle_flag(dg_entity_burn, "ignite");
        handle_flag(dg_movement_mode, "movemode");
        handle_flag(dg_perfomance, "perf");
        handle_flag(dg_perf_bar, "perfbar");
        handle_flag(dg_waypoints, "waypoint");
        // network meter in left-top corner
        handle_flag(dg_network, "network");
        handle_flag(dg_particle_stats, "particlestats");
        // debug strings at the left side of the screen
        handle_flag(dg_weapon, "weapon");
        handle_flag(dg_events, "event");
        handle_flag(dg_triggers, "trigger");
        handle_flag(dg_obj_rendering, "objrender");
        handle_flag(g_dbg_geometry_rendering_stats, "roomstats");
        // geometry rendering
        bool geom_rendering_changed = false;
        geom_rendering_changed |= handle_flag(render_everything_see_through, "trans");
        geom_rendering_changed |= handle_flag(render_rooms_in_different_colors, "room");
        geom_rendering_changed |= handle_flag(render_non_see_through_portal_faces, "portal");
        geom_rendering_changed |= handle_flag(render_lightmaps_only, "lightmap");
        geom_rendering_changed |= handle_flag(render_no_lightmaps, "nolightmap");

        // Clear geometry cache (needed for geometry rendering flags)
        if (geom_rendering_changed) {
            auto geom_cache_clear = (void (*)())0x004F0B90;
            geom_cache_clear();
        }

        if (!handled)
            rf::DcPrintf("Invalid debug flag: %s", type.c_str());
    },
    nullptr,
    "debug [thruster | light | light2 | push_climb_reg | geo_reg | glass | mover | ignite | movemode | perf | "
    "perfbar | waypoint | network | particlestats | weapon | event | trigger | objrender | roomstats | trans | "
    "room | portal | lightmap | nolightmap]",
};

void DebugRender3d()
{
    const auto dbg_waypoints = AddrAsRef<void()>(0x00468F00);
    const auto dbg_internal_lights = AddrAsRef<void()>(0x004DB830);

    dbg_waypoints();
    if (g_dbg_static_lights)
        dbg_internal_lights();
}

void DebugRender2d()
{
    const auto dbg_rendering_stats = AddrAsRef<void()>(0x004D36B0);
    const auto dbg_particle_stats = AddrAsRef<void()>(0x004964E0);
    if (g_dbg_geometry_rendering_stats)
        dbg_rendering_stats();
    dbg_particle_stats();
}

DcCommand2 spectate_cmd{
    "spectate",
    [](std::optional<std::string> player_name) {
        if (rf::is_net_game) {
            rf::Player* player;
            if (player_name && player_name.value() == "false")
                player = nullptr;
            else if (player_name)
                player = FindBestMatchingPlayer(player_name.value().c_str());
            else
                player = nullptr;

            if (player)
                SpectateModeSetTargetPlayer(player);
        }
        else
            rf::DcPrint("Works only in multiplayer game!", nullptr);
    },
    "Starts spectating mode",
    "spectate <player_name/false>",
};

DcCommand2 antialiasing_cmd{
    "antialiasing",
    []() {
        if (!g_game_config.msaa)
            rf::DcPrintf("Anti-aliasing is not supported");
        else {
            DWORD enabled = 0;
            rf::gr_d3d_device->GetRenderState(D3DRS_MULTISAMPLEANTIALIAS, &enabled);
            enabled = !enabled;
            rf::gr_d3d_device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, enabled);
            rf::DcPrintf("Anti-aliasing is %s", enabled ? "enabled" : "disabled");
        }
    },
    "Toggles anti-aliasing",
};

DcCommand2 input_mode_cmd{
    "inputmode",
    []() {
        g_game_config.directInput = !g_game_config.directInput;
        rf::direct_input_disabled = !g_game_config.directInput;
        g_game_config.save();
        if (g_game_config.directInput)
            rf::DcPrintf("DirectInput is enabled");
        else
            rf::DcPrintf("DirectInput is disabled");
    },
    "Toggles input mode",
};

#if CAMERA_1_3_COMMANDS

static int CanPlayerFireHook(rf::Player* player)
{
    if (!(player->flags & 0x10))
        return 0;
    if (rf::is_net_game && (player->camera->type == rf::CAM_FREELOOK || player->camera->player != player))
        return 0;
    return 1;
}

#endif // if CAMERA_1_3_COMMANDS

DcCommand2 ms_cmd{
    "ms",
    [](std::optional<float> value_opt) {
        if (value_opt) {
            float value = value_opt.value();
            value = std::clamp(value, 0.0f, 1.0f);
            rf::local_player->config.controls.mouse_sensitivity = value;
        }
        rf::DcPrintf("Mouse sensitivity: %.2f", rf::local_player->config.controls.mouse_sensitivity);
    },
    "Sets mouse sensitivity",
    "ms <value>",
};

CallHook<void(bool)> GlareRenderAllCorona_hook{
    0x0043233E,
    [](bool reflections) {
        if (g_game_config.glares)
            GlareRenderAllCorona_hook.CallTarget(reflections);
    },
};

DcCommand2 vli_cmd{
    "vli",
    []() {
        g_game_config.glares = !g_game_config.glares;
        g_game_config.save();
        rf::DcPrintf("Volumetric lightining is %s.", g_game_config.glares ? "enabled" : "disabled");
    },
    "Toggles volumetric lightining",
};

DcCommand2 level_sounds_cmd{
    "levelsounds",
    [](std::optional<float> volume) {
        if (volume) {
            float vol_scale = std::clamp(volume.value(), 0.0f, 1.0f);
            SetPlaySoundEventsVolumeScale(vol_scale);

            g_game_config.levelSoundVolume = vol_scale;
            g_game_config.save();
        }
        rf::DcPrintf("Level sound volume: %.1f", g_game_config.levelSoundVolume);
    },
    "Sets level sounds volume scale",
    "levelsounds <volume>",
};

DcCommand2 player_count_cmd{
    "playercount",
    []() {
        if (!rf::is_net_game)
            return;

        int player_count = 0;
        rf::Player* player = rf::player_list;
        while (player) {
            if (player != rf::player_list)
                ++player_count;
            player = player->next;
            if (player == rf::player_list)
                break;
        }
        rf::DcPrintf("Player count: %d\n", player_count);
    },
    "Get player count",
};

DcCommand2 find_level_cmd{
    "findlevel",
    [](std::string pattern) {
        PackfileFindMatchingFiles(StringMatcher().Infix(pattern).Suffix(".rfl"), [](const char* name) {
            // Print all matching filenames
            rf::DcPrintf("%s\n", name);
        });
    },
    "Find a level by a filename fragment",
    "findlevel <query>",
};

DcCommandAlias find_map_cmd{
    "findmap",
    find_level_cmd,
};

auto& level_cmd = AddrAsRef<rf::DcCommand>(0x00637078);
DcCommandAlias map_cmd{
    "map",
    level_cmd,
};

void DcShowCmdHelp(rf::DcCommand* cmd)
{
    rf::dc_run = 0;
    rf::dc_help = 1;
    rf::dc_status = 0;
    auto handler = reinterpret_cast<void(__fastcall*)(rf::DcCommand*)>(cmd->func);
    handler(cmd);
}

int DcAutoCompleteGetComponent(int offset, std::string& result)
{
    const char *begin = rf::dc_cmd_line + offset, *end = nullptr, *next;
    if (begin[0] == '"') {
        ++begin;
        end = strchr(begin, '"');
        next = end ? strchr(end, ' ') : nullptr;
    }
    else
        end = next = strchr(begin, ' ');

    if (!end)
        end = rf::dc_cmd_line + rf::dc_cmd_line_len;

    size_t len = end - begin;
    result.assign(begin, len);

    return next ? next + 1 - rf::dc_cmd_line : -1;
}

void DcAutoCompletePutComponent(int offset, const std::string& component, bool finished)
{
    bool quote = component.find(' ') != std::string::npos;
    int max_len = std::size(rf::dc_cmd_line);
    int len;
    if (quote) {
        len = snprintf(rf::dc_cmd_line + offset, max_len - offset, "\"%s\"", component.c_str());
    }
    else {
        len = snprintf(rf::dc_cmd_line + offset, max_len - offset, "%s", component.c_str());
    }
    rf::dc_cmd_line_len = offset + len;
    if (finished)
        rf::dc_cmd_line_len += snprintf(rf::dc_cmd_line + rf::dc_cmd_line_len, max_len - rf::dc_cmd_line_len, " ");
}

template<typename T, typename F>
void DcAutoCompletePrintSuggestions(T& suggestions, F mapping_fun)
{
    for (auto item : suggestions) {
        rf::DcPrintf("%s\n", mapping_fun(item));
    }
}

void DcAutoCompleteUpdateCommonPrefix(std::string& common_prefix, const std::string& value, bool& first,
                                      bool case_sensitive = false)
{
    if (first) {
        first = false;
        common_prefix = value;
    }
    if (common_prefix.size() > value.size())
        common_prefix.resize(value.size());
    for (size_t i = 0; i < common_prefix.size(); ++i) {
        if ((case_sensitive && common_prefix[i] != value[i]) || tolower(common_prefix[i]) != tolower(value[i])) {
            common_prefix.resize(i);
            break;
        }
    }
}

void DcAutoCompleteLevel(int offset)
{
    std::string level_name;
    DcAutoCompleteGetComponent(offset, level_name);
    if (level_name.size() < 3)
        return;

    bool first = true;
    std::string common_prefix;
    std::vector<std::string> matches;
    PackfileFindMatchingFiles(StringMatcher().Prefix(level_name).Suffix(".rfl"), [&](const char* name) {
        auto ext = strrchr(name, '.');
        auto name_len = ext ? ext - name : strlen(name);
        std::string name_without_ext(name, name_len);
        matches.push_back(name_without_ext);
        DcAutoCompleteUpdateCommonPrefix(common_prefix, name_without_ext, first);
    });

    if (matches.size() == 1)
        DcAutoCompletePutComponent(offset, matches[0], true);
    else if (!matches.empty()) {
        DcAutoCompletePrintSuggestions(matches, [](std::string& name) { return name.c_str(); });
        DcAutoCompletePutComponent(offset, common_prefix, false);
    }
}

void DcAutoCompletePlayer(int offset)
{
    std::string player_name;
    DcAutoCompleteGetComponent(offset, player_name);
    if (player_name.size() < 1)
        return;

    bool first = true;
    std::string common_prefix;
    std::vector<rf::Player*> matching_players;
    FindPlayer(StringMatcher().Prefix(player_name), [&](rf::Player* player) {
        matching_players.push_back(player);
        DcAutoCompleteUpdateCommonPrefix(common_prefix, player->name.CStr(), first);
    });

    if (matching_players.size() == 1)
        DcAutoCompletePutComponent(offset, matching_players[0]->name.CStr(), true);
    else if (!matching_players.empty()) {
        DcAutoCompletePrintSuggestions(matching_players, [](rf::Player* player) {
            // Print player names
            return player->name.CStr();
        });
        DcAutoCompletePutComponent(offset, common_prefix, false);
    }
}

void DcAutoCompleteCommand(int offset)
{
    std::string cmd_name;
    int next_offset = DcAutoCompleteGetComponent(offset, cmd_name);
    if (cmd_name.size() < 2)
        return;

    bool first = true;
    std::string common_prefix;

    std::vector<rf::DcCommand*> matching_cmds;
    for (unsigned i = 0; i < rf::dc_num_commands; ++i) {
        rf::DcCommand* cmd = g_commands_buffer[i];
        if (!strnicmp(cmd->cmd_name, cmd_name.c_str(), cmd_name.size()) &&
            (next_offset == -1 || !cmd->cmd_name[cmd_name.size()])) {
            matching_cmds.push_back(cmd);
            DcAutoCompleteUpdateCommonPrefix(common_prefix, cmd->cmd_name, first);
        }
    }

    if (next_offset != -1) {
        if (matching_cmds.size() != 1)
            return;
        if (!stricmp(cmd_name.c_str(), "level") || !stricmp(cmd_name.c_str(), "levelsp"))
            DcAutoCompleteLevel(next_offset);
        else if (!stricmp(cmd_name.c_str(), "kick") || !stricmp(cmd_name.c_str(), "ban"))
            DcAutoCompletePlayer(next_offset);
        else if (!stricmp(cmd_name.c_str(), "rcon"))
            DcAutoCompleteCommand(next_offset);
        else
            DcShowCmdHelp(matching_cmds[0]);
    }
    else if (matching_cmds.size() > 1) {
        for (auto* cmd : matching_cmds) {
            if (cmd->descr)
                rf::DcPrintf("%s - %s", cmd->cmd_name, cmd->descr);
            else
                rf::DcPrintf("%s", cmd->cmd_name);
        }
        DcAutoCompletePutComponent(offset, common_prefix, false);
    }
    else if (matching_cmds.size() == 1)
        DcAutoCompletePutComponent(offset, matching_cmds[0]->cmd_name, true);
}

FunHook<void()> DcAutoCompleteInput_hook{
    0x0050A620,
    []() {
        // autocomplete on offset 0
        DcAutoCompleteCommand(0);
    },
};

RegsPatch DcRunCmd_CallHandlerPatch{
    0x00509DBB,
    [](auto& regs) {
        // Make sure command pointer is in ecx register to support thiscall handlers
        regs.ecx = regs.eax;
    },
};

void CommandsInit()
{
#if CAMERA_1_3_COMMANDS
    /* Enable camera1-3 in multiplayer and hook CanPlayerFire to disable shooting in camera2 */
    AsmWritter(0x00431280).nop(2);
    AsmWritter(0x004312E0).nop(2);
    AsmWritter(0x00431340).nop(2);
    AsmWritter(0x004A68D0).jmp(CanPlayerFireHook);
#endif // if CAMERA_1_3_COMMANDS

    // Change limit of commands
    ASSERT(rf::dc_num_commands == 0);
    WriteMemPtr(0x005099AC + 1, g_commands_buffer);
    WriteMem<u8>(0x00509A78 + 2, CMD_LIMIT);
    WriteMemPtr(0x00509A8A + 1, g_commands_buffer);
    WriteMemPtr(0x00509AB0 + 3, g_commands_buffer);
    WriteMemPtr(0x00509AE1 + 3, g_commands_buffer);
    WriteMemPtr(0x00509AF5 + 3, g_commands_buffer);
    WriteMemPtr(0x00509C8F + 1, g_commands_buffer);
    WriteMemPtr(0x00509DB4 + 3, g_commands_buffer);
    WriteMemPtr(0x00509E6F + 1, g_commands_buffer);
    WriteMemPtr(0x0050A648 + 4, g_commands_buffer);
    WriteMemPtr(0x0050A6A0 + 3, g_commands_buffer);

    DcRunCmd_CallHandlerPatch.Install();

    // Better console autocomplete
    DcAutoCompleteInput_hook.Install();

    // vli command support
    GlareRenderAllCorona_hook.Install();

    // Allow 'level' command outside of multiplayer game
    AsmWritter(0x00434FEC, 0x00434FF2).nop();
}

void CommandRegister(rf::DcCommand* cmd)
{
    if (rf::dc_num_commands < CMD_LIMIT)
        g_commands_buffer[rf::dc_num_commands++] = cmd;
    else
        ASSERT(false);
}

static void RegisterBuiltInCommand(const char* name, const char* description, uintptr_t addr)
{
    static std::vector<std::unique_ptr<rf::DcCommand>> builtin_commands;
    auto cmd = std::make_unique<rf::DcCommand>();
    cmd->cmd_name = name;
    cmd->descr = description;
    cmd->func = reinterpret_cast<rf::DcCmdHandler>(addr);
    builtin_commands.push_back(std::move(cmd));
    CommandRegister(builtin_commands.back().get());
}

void CommandsAfterGameInit()
{
    // Register RF builtin commands disabled in PC build

    // Server configuration commands
    RegisterBuiltInCommand("kill_limit", "Sets kill limit", 0x0046CBC0);
    RegisterBuiltInCommand("time_limit", "Sets time limit", 0x0046CC10);
    RegisterBuiltInCommand("geomod_limit", "Sets geomod limit", 0x0046CC70);
    RegisterBuiltInCommand("capture_limit", "Sets capture limit", 0x0046CCC0);

    // Misc commands
    RegisterBuiltInCommand("sound", "Toggle sound", 0x00434590);
    RegisterBuiltInCommand("difficulty", "Set game difficulty", 0x00434EB0);
    // RegisterBuiltInCommand("ms", "Set mouse sensitivity", 0x0043CE90);
    RegisterBuiltInCommand("level_info", "Show level info", 0x0045C210);
    RegisterBuiltInCommand("verify_level", "Verify level", 0x0045E1F0);
    RegisterBuiltInCommand("player_names", "Toggle player names on HUD", 0x0046CB80);
    RegisterBuiltInCommand("clients_count", "Show number of connected clients", 0x0046CD10);
    RegisterBuiltInCommand("kick_all", "Kick all clients", 0x0047B9E0);
    RegisterBuiltInCommand("timedemo", "Start timedemo", 0x004CC1B0);
    RegisterBuiltInCommand("frameratetest", "Start frame rate test", 0x004CC360);
    RegisterBuiltInCommand("system_info", "Show system information", 0x00525A60);
    RegisterBuiltInCommand("trilinear_filtering", "Toggle trilinear filtering", 0x0054F050);
    RegisterBuiltInCommand("detail_textures", "Toggle detail textures", 0x0054F0B0);

#ifdef DEBUG
    RegisterBuiltInCommand("drop_fixed_cam", "Drop a fixed camera", 0x0040D220);
    RegisterBuiltInCommand("orbit_cam", "Orbit camera around current target", 0x0040D2A0);
    RegisterBuiltInCommand("drop_clutter", "Drop any clutter", 0x0040F0A0);
    RegisterBuiltInCommand("glares_toggle", "toggle glares", 0x00414830);
    RegisterBuiltInCommand("set_vehicle_bounce", "set the elasticity of vehicle collisions", 0x004184B0);
    RegisterBuiltInCommand("set_mass", "set the mass of the targeted object", 0x004184F0);
    RegisterBuiltInCommand("set_skin", "Set the skin of the current player target", 0x00418610);
    RegisterBuiltInCommand("set_life", "Set the life of the player's current target", 0x00418680);
    RegisterBuiltInCommand("set_armor", "Set the armor of the player's current target", 0x004186E0);
    RegisterBuiltInCommand("drop_entity", "Drop any entity", 0x00418740);
    RegisterBuiltInCommand("set_weapon", "set the current weapon for the targeted entity", 0x00418AA0);
    RegisterBuiltInCommand("jump_height", "set the height the player can jump", 0x00418B80);
    RegisterBuiltInCommand("fall_factor", nullptr, 0x004282F0);
    RegisterBuiltInCommand("toggle_crouch", nullptr, 0x00430C50);
    RegisterBuiltInCommand("player_step", nullptr, 0x00433DB0);
    RegisterBuiltInCommand("difficulty", nullptr, 0x00434EB0);
    RegisterBuiltInCommand("mouse_cursor", "Sets the mouse cursor", 0x00435210);
    RegisterBuiltInCommand("fogme", "Fog everything", 0x004352E0);
    RegisterBuiltInCommand("mouse_look", nullptr, 0x0043CF30);
    RegisterBuiltInCommand("drop_item", "Drop any item", 0x00458530);
    RegisterBuiltInCommand("set_georegion_hardness", "Set default hardness for geomods", 0x004663E0);
    RegisterBuiltInCommand("make_myself", nullptr, 0x00475040);
    RegisterBuiltInCommand("splitscreen", nullptr, 0x00480AA0);
    RegisterBuiltInCommand("splitscreen_swap", "Swap splitscreen players", 0x00480AB0);
    RegisterBuiltInCommand("splitscreen_bot_test", "Start a splitscreen game in mk_circuits3.rfl", 0x00480AE0);
    RegisterBuiltInCommand("max_plankton", "set the max number of plankton bits", 0x00497FC0);
    RegisterBuiltInCommand("pcollide", "Toggle if player collides with the world", 0x004A0F60);
    RegisterBuiltInCommand("teleport", "Teleport to an x,y,z", 0x004A0FC0);
    RegisterBuiltInCommand("body", "Set player entity type", 0x004A11C0);
    RegisterBuiltInCommand("invulnerable", "Make self invulnerable", 0x004A12A0);
    RegisterBuiltInCommand("freelook", "Toggle between freelook and first person", 0x004A1340);
    RegisterBuiltInCommand("hide_target", "Hide current target, unhide if already hidden", 0x004A13D0);
    RegisterBuiltInCommand("set_primary", "Set default player primary weapon", 0x004A1430);
    RegisterBuiltInCommand("set_secondary", "Set default player secondary weapon", 0x004A14B0);
    RegisterBuiltInCommand("set_view", "Set camera view from entity with specified UID", 0x004A1540);
    RegisterBuiltInCommand("set_ammo", nullptr, 0x004A15C0);
    RegisterBuiltInCommand("endgame", "force endgame", 0x004A1640);
    RegisterBuiltInCommand("irc", "Set the color range of the infrared characters", 0x004AECE0);
    RegisterBuiltInCommand("irc_close", "Set the close color of the infrared characters", 0x004AEDB0);
    RegisterBuiltInCommand("irc_far", "Set the far color of the infrared characters", 0x004AEE20);
    // RegisterBuiltInCommand("pools", nullptr, 0x004B1050);
    RegisterBuiltInCommand("savegame", "Save the current game", 0x004B3410);
    RegisterBuiltInCommand("loadgame", "Load a game", 0x004B34C0);
    RegisterBuiltInCommand("show_obj_times", "Set the number of portal objects to show times for", 0x004D3250);
    // Some commands does not use dc_exec variables and were not added e.g. 004D3290
    RegisterBuiltInCommand("save_commands", "Print out console commands to the text file console.txt", 0x00509920);
    RegisterBuiltInCommand("script", "Runs a console script file (.vcs)", 0x0050B7D0);
    RegisterBuiltInCommand("screen_res", nullptr, 0x0050E400);
    RegisterBuiltInCommand("wfar", nullptr, 0x005183D0);
    RegisterBuiltInCommand("play_bik", nullptr, 0x00520A70);
#endif // DEBUG

    // Custom Dash Faction commands
    max_fps_cmd.Register();
    ms_cmd.Register();
    vli_cmd.Register();
    level_sounds_cmd.Register();
    player_count_cmd.Register();
    find_level_cmd.Register();
    find_map_cmd.Register();
    map_cmd.Register();
    spectate_cmd.Register();
    input_mode_cmd.Register();
    antialiasing_cmd.Register();
    debug_cmd.Register();

#if SPLITSCREEN_ENABLE
    splitscreen_cmd.Register();
#endif
}
