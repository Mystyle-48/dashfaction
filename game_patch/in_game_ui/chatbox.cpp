#include "hud_internal.h"
#include "../in_game_ui/hud.h"
#include "../rf/hud.h"
#include "../rf/graphics.h"
#include <patch_common/FunHook.h>
#include <patch_common/AsmOpcodes.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/CodeInjection.h>

bool g_big_chatbox = false;

constexpr int chat_msg_max_len = 224;
constexpr int chatbox_border_alpha = 0x30; // default is 77
constexpr int chatbox_bg_alpha = 0x40; // default is 128

FunHook<void(uint16_t)> multi_chat_say_add_char_hook{
    0x00444740,
    [](uint16_t key) {
        if (key)
            multi_chat_say_add_char_hook.CallTarget(key);
    },
};

FunHook<void(const char*, bool)> multi_chat_say_accept_hook{
    0x00444440,
    [](const char* msg, bool is_team_msg) {
        std::string msg_str{msg};
        if (msg_str.size() > chat_msg_max_len)
            msg_str.resize(chat_msg_max_len);
        multi_chat_say_accept_hook.CallTarget(msg_str.c_str(), is_team_msg);
    },
};

void ChatRender()
{
    if (!rf::chat_fully_visible_timer.valid() && !rf::chat_fade_out_timer.valid()) {
        return;
    }

    float fade_out = 1.0;
    if (rf::chat_fully_visible_timer.elapsed() && !rf::chat_fade_out_timer.valid()) {
        rf::chat_fully_visible_timer.invalidate();
        rf::chat_fade_out_timer.set(750);
    }
    if (rf::chat_fade_out_timer.valid()) {
        if (rf::chat_fade_out_timer.elapsed()) {
            rf::chat_fade_out_timer.invalidate();
            return;
        }
        fade_out = rf::chat_fade_out_timer.time_until() / 750.0f;
    }

    int chatbox_font = HudGetDefaultFont();
    int clip_w = rf::gr_clip_width();
    int font_h = rf::gr_get_font_height(chatbox_font);
    int border = g_big_chatbox ? 3 : 2;
    int box_w = clip_w - (g_big_chatbox ? 600 : 313);
    int box_h = 8 * font_h + 2 * border + 6;
    int content_w = box_w - 2 * border;
    int content_h = box_h - 2 * border;
    int box_y = 10;
    int box_x = (clip_w - box_w) / 2;

    int border_alpha = rf::scoreboard_visible ? 255 : chatbox_border_alpha;
    rf::gr_set_color_rgba(255, 255, 255, static_cast<int>(border_alpha * fade_out));
    HudRectBorder(box_x, box_y, box_w, box_h, border);
    int bg_alpha = rf::scoreboard_visible ? 255 : chatbox_bg_alpha;
    rf::gr_set_color_rgba(0, 0, 0, static_cast<int>(bg_alpha * fade_out));
    rf::gr_rect(box_x + border, box_y + border, content_w, content_h);
    int y = box_y + box_h - border - font_h - 5;

    int text_alpha = static_cast<int>(fade_out * 255.0);
    for (auto& msg : rf::chat_messages) {
        if (msg.name.empty() && msg.text.empty()) {
            break;
        }
        int x = box_x + border + 6;

        int name_w, name_h;
        rf::gr_get_string_size(&name_w, &name_h, msg.name.c_str(), -1, chatbox_font);
        if (msg.color_id == 0 || msg.color_id == 1) {
            if (msg.color_id == 0) {
                rf::gr_set_color_rgba(227, 48, 47, text_alpha);
            }
            else {
                rf::gr_set_color_rgba(117, 117, 254, text_alpha);
            }
            rf::gr_string(x, y, msg.name.c_str(), chatbox_font);
            rf::gr_set_color_rgba(255, 255, 255, text_alpha);
            x += name_w;
        }
        else if (msg.color_id == 2) {
            rf::gr_set_color_rgba(227, 48, 47, text_alpha);
        }
        else if (msg.color_id == 3) {
            rf::gr_set_color_rgba(117, 117, 254, text_alpha);
        }
        else if (msg.color_id == 4) {
            rf::gr_set_color_rgba(255, 255, 255, text_alpha);
        }
        else {
            rf::gr_set_color_rgba(52, 255, 57, text_alpha);
        }
        rf::gr_string(x, y, msg.text.c_str(), chatbox_font);
        y -= font_h;
    }
}

FunHook<void()> multi_chat_render_hook{0x004773D0, ChatRender};

CodeInjection multi_chat_add_msg_max_width_injection{
    0x004788E3,
    [](auto& regs) {
        regs.esi = rf::gr_screen_width() - (g_big_chatbox ? 620 : 320);
    },
};

void ChatboxInputRender(rf::String::Pod label_pod, rf::String::Pod msg_pod)
{
    // Note: POD has to be used here because of differences between compilers ABI
    rf::String label{label_pod};
    rf::String msg{msg_pod};
    if (label.empty() && msg.empty()) {
        return;
    }

    int chatbox_font = HudGetDefaultFont();
    int clip_w = rf::gr_clip_width();
    int font_h = rf::gr_get_font_height(chatbox_font); // 12
    int border = g_big_chatbox ? 3 : 2;
    int box_w = clip_w - (g_big_chatbox ? 600 : 313);
    int content_w = box_w - 2 * border;
    int hist_box_y = 10;
    int hist_box_h = 8 * font_h + 2 * border + 6;
    int input_box_content_h = font_h + 3;
    int input_box_h = input_box_content_h + 2 * border;
    int input_box_y = hist_box_y + hist_box_h; // 116
    int input_box_x = (clip_w - box_w) / 2; // 157

    rf::String msg_shortened{msg};
    int msg_w, msg_h;
    rf::gr_get_string_size(&msg_w, &msg_h, msg_shortened.c_str(), -1, chatbox_font);
    int label_w, label_h;
    rf::gr_get_string_size(&label_w, &label_h, label.c_str(), -1, chatbox_font);
    int max_msg_w = -330 - label_w + rf::gr_screen_width();
    while (msg_w > max_msg_w) {
        msg_shortened.substr(&msg_shortened, 1, -1);
        rf::gr_get_string_size(&msg_w, &msg_h, msg_shortened.c_str(), -1, chatbox_font);
    }

    rf::gr_set_color_rgba(255, 255, 255, rf::scoreboard_visible ? 255 : chatbox_border_alpha);
    HudRectBorder(input_box_x, input_box_y, box_w, input_box_h, 2);

    rf::gr_set_color_rgba(0, 0, 0, rf::scoreboard_visible ? 255 : chatbox_bg_alpha);
    rf::gr_rect(input_box_x + border, input_box_y + border, content_w, input_box_content_h);

    rf::String label_and_msg;
    rf::String::concat(&label_and_msg, label, msg_shortened);
    int text_x = input_box_x + border + 4;
    int text_y = input_box_y + border + (g_big_chatbox ? 2 : 1);
    rf::gr_set_color_rgba(53, 207, 22, 255);
    rf::gr_string(text_x, text_y, label_and_msg.c_str(), chatbox_font);

    static rf::Timestamp cursor_blink_timestamp;
    static bool chatbox_cursor_visible = false;

    if (!cursor_blink_timestamp.valid() || cursor_blink_timestamp.elapsed()) {
        cursor_blink_timestamp.set(350);
        chatbox_cursor_visible = !chatbox_cursor_visible;
    }
    if (chatbox_cursor_visible) {
        int cursor_x = text_x + label_w + msg_w + 2;
        int cursor_y = text_y + msg_h + (g_big_chatbox ? -2 : 0);
        int cursor_w = g_big_chatbox ? 10 : 5;
        int cursor_h = g_big_chatbox ? 2 : 1;
        rf::gr_rect(cursor_x, cursor_y, cursor_w, cursor_h);
    }
}

FunHook<void(rf::String::Pod, rf::String::Pod)> ChatboxInputRender_hook{0x00478CA0, ChatboxInputRender};

void InstallChatboxPatches()
{
    // Fix game beeping every frame if chat input buffer is full
    multi_chat_say_add_char_hook.Install();

    // Change chat input limit to 224 (RF can support 255 safely but PF kicks if message is longer than 224)
    WriteMem<i32>(0x0044474A + 1, chat_msg_max_len);

    // Add chat message limit for say/teamsay commands
    multi_chat_say_accept_hook.Install();

    multi_chat_render_hook.Install();
    multi_chat_add_msg_max_width_injection.Install();
    ChatboxInputRender_hook.Install();
}

void SetBigChatbox(bool is_big)
{
    g_big_chatbox = is_big;
}
