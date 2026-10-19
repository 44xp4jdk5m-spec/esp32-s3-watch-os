# ── Reusable system UI widgets ────────────────────────────────────────
# Shared on-screen widgets used by multiple apps (keyboard, spinner, and
# the frosted action-sheet / warning modals). Depends on the OS core
# (dictionary, events, fonts, RTC time) and LVGL.

idf_component_register(
    SRCS
        # Existing widgets
        "keyboard/keyboard.c"
        "keyboard/kbd_icons.c"
        "spinner/spinner.c"
        "actionsheet/sheet.c"
        # Settings-family widgets — one folder per widget type
        "list/sw_list.c"
        "row/sw_row.c"
        "title/sw_title.c"
        "comment/sw_comment.c"
        "learn_more/sw_learn_more.c"
        "info/sw_info.c"
        "entry/sw_entry.c"
        "icon_btn/sw_icon_btn.c"
        "button/sw_button.c"
        "toggle/sw_toggle.c"
        "slider/sw_slider.c"
        "picker/sw_picker.c"
        "graph/sw_graph.c"
        "nav_header/sw_nav_header.c"
        "transition/sw_transition.c"
        "subpage/sw_subpage.c"
    INCLUDE_DIRS
        "."
        "keyboard"
        "spinner"
        "actionsheet"
        "theme"
        "list"
        "row"
        "title"
        "comment"
        "learn_more"
        "info"
        "entry"
        "icon_btn"
        "button"
        "toggle"
        "slider"
        "picker"
        "graph"
        "nav_header"
        "transition"
        "subpage"
    REQUIRES
        os
        lvgl__lvgl
)
