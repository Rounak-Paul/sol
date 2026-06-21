// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol-plugin-themes — Built-in editor themes for Sol.
 *
 * Every theme extends the Glass baseline (com.sol.theme.glass) with CSS
 * overrides. Surface RGB values are pushed to 20–60 per dominant hue channel
 * so tints remain visible through the frosted-glass semi-transparency.
 *
 * Themes implemented (inspired by popular Neovim colorschemes):
 *
 *   Deep Ocean     — custom teal/cyan
 *   Amethyst       — custom violet/plum
 *   Graphite       — custom neutral monochrome
 *   Ember Glass    — custom amber/rust
 *   Gruvbox Dark   — warm retro brown/orange (gruvbox)
 *   Nord           — arctic blue/slate (nord)
 *   Tokyo Night    — cool indigo/purple (tokyonight)
 *   Catppuccin     — pastel mauve/lavender (catppuccin mocha)
 *   Dracula        — purple/pink cyberpunk (dracula)
 *   One Dark       — muted blue-grey (onedark)
 *   Rosé Pine      — dusty rose/pine (rose-pine)
 *   Everforest     — muted green/sage (everforest)
 *   Kanagawa       — deep Japanese ink/wave (kanagawa)
 *   Carbonfox      — charcoal/orange (nightfox carbonfox)
 *   Monokai Pro    — saturated pink/green (monokai-pro)
 *   Sonokai        — vivid mixed accent (sonokai)
 */

#include "sol_plugin.h"
#include "sol_plugin_ctx.h"
#include "sol_theme.h"

/* ------------------------------------------------------------------ */
/* Shared CSS template macro                                            */
/* ------------------------------------------------------------------ */

/*
 * SW_CSS(tb,ts,th,sp,sr,st,tp,ta,bb,bg,gl,bl,bs,bc,kw,cm,str,num,fn,ty,pr,op,tg,
 *        acc,sprim,ssec)
 *
 * Generates the full theme CSS override block from palette tokens:
 *
 *   tb   = titlebar background rgba
 *   ts   = titlebar title color hex
 *   th   = titlebar menu item hover rgba
 *   sp   = splitter rgba color
 *   sr   = status-bar background rgba
 *   st   = status-bar text hex
 *   tp   = tree panel background rgba
 *   ta   = tree arrow-open hex / accent
 *   bb   = buffer-body background rgba
 *   bg   = buffer gutter rgba
 *   gl   = gutter line hex (dim)
 *   bl   = buffer-line / plain text hex
 *   bs   = buffer selection rgba
 *   bc   = buffer caret hex (accent)
 *   kw   = keyword hex
 *   cm   = comment hex (dim)
 *   str  = string hex
 *   num  = number hex
 *   fn   = function hex
 *   ty   = type hex
 *   pr   = property hex
 *   op   = operator hex
 *   tg   = tag hex
 *   acc  = accent rgba for buttons/active-bg
 *   sprim = sw-root / pm-root bg rgba
 *   ssec = sw-left bg rgba
 */

/* Helper macro to generate all theme-common CSS from basic palette tokens.
 * Strings are concatenated by the C preprocessor (adjacent string literals). */
#define THEME_CSS(\
    /* chrome */ TB, TS, TC, TM, TCH, TCL,\
    /* splitter */ SP,\
    /* status */ SR, ST, SBK, SBC, SBL,\
    /* tree panel */ TP, TT, TRH, TSR, TAD, TAO, TNA, TND,\
    /* tabs row */ BT,\
    /* tab bg, active tab */ BTAB, BTAA,\
    /* tab text, active text */ BTXT, BTXA,\
    /* buffer body, gutter, gutter-line */ BB, BG, GL,\
    /* plain text, selection, caret */ BL, BSL, BC,\
    /* scrollbar */ SB,\
    /* syntax */ KW, CM, STR, NUM, FN, TY, PR, OP, TG,\
    /* ui accent rgba */ UA,\
    /* fp/pm/sw root, left panel, hover, selected */ FR, FL, FH, FS)\
\
    /* Chrome */\
    ".ca-titlebar { background: " TB "; }"\
    ".ca-titlebar-title { color: " TS "; }"\
    ".ca-titlebar-menu-item { color: " TC "; }"\
    ".ca-titlebar-menu-item:hover { background: " TM "; color: " TS "; }"\
    ".ca-titlebar-control { color: " TCH "; }"\
    ".ca-titlebar-control:hover { background: " TM "; }"\
    ".ca-titlebar-close { color: " TCL "; }"\
    ".ca-titlebar-close:hover { background: " TM "; }"\
    /* Splitter */\
    "splitter { background: transparent; color: " SP "; }"\
    /* Status bar */\
    ".status-bar { background: " SR "; }"\
    ".status-bar-text { color: " ST "; }"\
    ".status-bar-badge-key { background: " SBK "; }"\
    ".status-bar-badge-command { background: " SBC "; }"\
    ".status-bar-badge-leader { background: " SBL "; }"\
    /* File tree */\
    ".tree-panel, .plugin-side-panel { background: " TP "; }"\
    ".tree-section-header { background: transparent; }"\
    ".tree-section-title { color: " TT "; letter-spacing: 0.9px; }"\
    ".tree-row:hover { background: " TRH "; }"\
    ".tree-sticky-row { background: " TSR "; corner-radius: 0px; }"\
    ".tree-sticky-row:hover { background: " TRH "; }"\
    ".tree-arrow { color: " TAD "; }"\
    ".tree-arrow-open { color: " TAO "; }"\
    ".tree-name { color: " TNA "; }"\
    ".tree-name-dir { color: " TND "; }"\
    /* Buffer tabs */\
    ".buffer-tabs-row { height: 30px; padding: 3px 6px; gap: 4px; align-items: center; background: " BT "; }"\
    ".buffer-tab { height: 24px; padding: 0px 4px 0px 10px; background: transparent; corner-radius: 0px; }"\
    ".buffer-tab:hover { background: " UA "; }"\
    ".buffer-tab-active { background: " BTAA "; }"\
    ".buffer-tab-text { color: " TAD "; }"\
    ".buffer-tab-text-active { color: " TND "; }"\
    ".buffer-tab-close:hover { background: " UA "; }"\
    /* Buffer body */\
    ".buffer-body { background: " BB "; }"\
    ".buffer-gutter-col { background: " BG "; }"\
    ".buffer-gutter-line { color: " GL "; }"\
    ".buffer-line, .buffer-body-text, .hl-plain { color: " BL "; }"\
    ".buffer-selection { background: " BSL "; }"\
    ".buffer-caret { background: " BC "; }"\
    ".buffer-scrollbar { background: " SB "; }"\
    ".buffer-hscrollbar { background: " SB "; }"\
    ".buffer-scrollbar-thumb, .buffer-scrollbar-thumb-active,"\
    ".buffer-hscrollbar-thumb, .buffer-hscrollbar-thumb-active"\
    " { background: " UA "; corner-radius: 0px; width: 9px; }"\
    ".buffer-hscrollbar-thumb, .buffer-hscrollbar-thumb-active { height: 9px; }"\
    ".buffer-scrollbar-thumb:hover, .buffer-scrollbar-thumb-active,"\
    ".buffer-hscrollbar-thumb:hover, .buffer-hscrollbar-thumb-active"\
    " { background: " SP "; }"\
    /* Syntax */\
    ".hl-keyword, .hl-macro { color: " KW "; }"\
    ".hl-comment { color: " CM "; }"\
    ".hl-string, .hl-regex { color: " STR "; }"\
    ".hl-number, .hl-constant, .hl-escape { color: " NUM "; }"\
    ".hl-function, .hl-constructor { color: " FN "; }"\
    ".hl-type, .hl-namespace { color: " TY "; }"\
    ".hl-property, .hl-attribute, .hl-parameter { color: " PR "; }"\
    ".hl-operator, .hl-label { color: " OP "; }"\
    ".hl-tag { color: " TG "; }"\
    /* Welcome */\
    ".welcome-pane { background: " BB "; padding: 60px 72px; align-items: center; }"\
    ".welcome-title { color: " TND "; }"\
    ".welcome-subtitle, .welcome-section-label { color: " CM "; }"\
    ".welcome-desc { color: " TNA "; }"\
    ".welcome-hr { background: " UA "; }"\
    ".welcome-btn { corner-radius: 0px; background: " UA "; color: " TNA "; }"\
    ".welcome-btn-primary { corner-radius: 0px; background: " FS "; color: " TND "; }"\
    ".welcome-btn:hover, .welcome-btn-primary:hover { background: " FH "; }"\
    /* File picker + search */\
    ".fp-root, .search-root-window { background: " FR "; }"\
    ".fp-toolbar, .fp-footer, .fp-colhdr, .search-header, .search-footer { background: " FL "; }"\
    ".fp-list, .search-results { background: " BB "; }"\
    ".fp-row, .search-result { corner-radius: 0px; }"\
    ".fp-row:hover, .search-result:hover { background: " FH "; }"\
    ".fp-row-selected, .search-result-selected { background: " FS "; }"\
    ".fp-new-folder-input, .search-input { background: " BB "; corner-radius: 0px; color: " BL "; }"\
    ".search-result-line { color: " BC "; }"\
    /* Plugin manager / settings */\
    ".pm-root, .sw-root { background: " FR "; }"\
    ".pm-left, .sw-left { background: " FL "; }"\
    ".pm-search-row { background: " FL "; }"\
    ".pm-search-input, .sw-scale-input, .sw-select { background: " BB "; corner-radius: 0px; }"\
    ".pm-item, .sw-tab-btn { corner-radius: 0px; }"\
    ".pm-item:hover, .sw-tab-btn:hover { background: " FH "; }"\
    ".pm-item-selected, .sw-tab-btn-active { background: " FS "; }"\
    ".pm-right, .sw-right { background: " BB "; }"\
    ".pm-btn, .pm-btn-enable, .pm-btn-disable { corner-radius: 0px; background: " UA "; }"\
    ".pm-btn:hover { background: " FH "; }"\
    ".sw-hr, .pm-hr { background: " GL "; }"\
    /* Command flow popup */\
    ".cf-panel { background: " FR "; corner-radius: 0px; shadow-offset-y: 6px; shadow-blur: 18px; shadow-color: rgba(0,0,0,0.46); }"\
    ".cf-row { corner-radius: 0px; }"\
    ".cf-row:hover { background: transparent; }"\
    ".cf-row-key { background: " UA "; corner-radius: 0px; }"\
    ".cf-row-key-text { color: " BC "; }"\
    /* SCM */\
    ".scm-root, .scm-view { background: " BB "; }"\
    ".scm-toolbar, .scm-repository, .scm-commit-box, .scm-section-header { background: " FL "; }"\
    ".scm-file-row:hover, .scm-commit-row:hover, .scm-branch-row:hover { background: " FH "; }"\
    ".scm-commit-input, .scm-branch-input { background: " BB "; corner-radius: 0px; }"\
    ".scm-branch-row-current { background: " FS "; }"\
    /* Terminal */\
    ".term-panel, .term-filler { background: " SR "; }"\
    ".term-viewport { background: " SR "; padding: 4px 6px; }"\
    ".term-header { background: " FR "; }"\
    ".term-tab { background: " FR "; color: " CM "; }"\
    ".term-tab-active { background: " SR "; color: " TNA "; }"\
    ".term-cursor-focused { background: " BC "; }"

/* ------------------------------------------------------------------ */
/* Theme palette definitions                                           */
/* ------------------------------------------------------------------ */

/*
 * Deep Ocean — custom cool navy/teal.
 */
static const char k_ocean_css[] = THEME_CSS(
    "rgba(5,18,38,0.91)",    /* TB  titlebar bg */
    "#4a9ab8",               /* TS  title color */
    "#5ab4cc",               /* TC  menu item */
    "rgba(40,160,200,0.16)", /* TM  menu hover */
    "#3a7898",               /* TCH control */
    "#50a8c0",               /* TCL close */
    "rgba(0,200,230,0.80)",  /* SP  splitter */
    "rgba(3,12,28,0.97)",    /* SR  status bar */
    "#3a7898",               /* ST  status text */
    "rgba(20,130,180,0.44)", /* SBK badge key */
    "rgba(16,150,120,0.36)", /* SBC badge cmd */
    "rgba(140,120,30,0.34)", /* SBL badge leader */
    "rgba(6,20,44,0.93)",    /* TP  tree panel */
    "#3a7898",               /* TT  section title */
    "rgba(30,170,210,0.12)", /* TRH row hover */
    "rgba(8,28,56,0.94)",    /* TSR sticky row */
    "#1e5470",               /* TAD arrow dim */
    "#00c8e0",               /* TAO arrow open */
    "#80c0d4",               /* TNA name */
    "#c4e4f0",               /* TND name-dir */
    "rgba(5,16,36,0.84)",    /* BT  tabs row */
    "rgba(6,16,30,0.60)",    /* BTAB tab bg (unused) */
    "rgba(12,90,140,0.56)",  /* BTAA active tab */
    "#2a5878",               /* BTXT tab text */
    "#d0ecf8",               /* BTXA active tab text */
    "rgba(4,14,30,0.91)",    /* BB  buffer body */
    "rgba(5,18,38,0.82)",    /* BG  gutter */
    "#143050",               /* GL  gutter line */
    "#c0dce8",               /* BL  plain text */
    "rgba(10,80,130,0.70)",  /* BSL selection */
    "#00d4e8",               /* BC  caret */
    "rgba(5,16,36,0.28)",    /* SB  scrollbar */
    "#00c8e0",               /* KW  keyword */
    "#1e5468",               /* CM  comment */
    "#70d090",               /* STR string */
    "#58d0b8",               /* NUM number */
    "#48b0f0",               /* FN  function */
    "#28d8c8",               /* TY  type */
    "#80bcd0",               /* PR  property */
    "#40a8c0",               /* OP  operator */
    "#d05868",               /* TG  tag */
    "rgba(20,140,180,0.35)", /* UA  ui accent */
    "rgba(5,16,36,0.88)",    /* FR  fp/pm/sw root */
    "rgba(6,22,48,0.86)",    /* FL  left panel */
    "rgba(30,155,200,0.13)", /* FH  hover */
    "rgba(10,80,130,0.46)"   /* FS  selected */
);

/*
 * Amethyst — custom violet/plum.
 */
static const char k_amethyst_css[] = THEME_CSS(
    "rgba(22,8,48,0.91)",
    "#8050b8", "#9868cc", "rgba(140,70,220,0.17)", "#6840a0", "#b05898",
    "rgba(190,90,255,0.82)",
    "rgba(14,4,32,0.97)", "#6840a0",
    "rgba(110,55,200,0.46)", "rgba(60,80,180,0.38)", "rgba(160,100,30,0.34)",
    "rgba(24,9,52,0.93)", "#6840a0", "rgba(140,70,220,0.12)",
    "rgba(30,12,64,0.94)", "#40206a", "#c050f8", "#b890d8", "#e0cef8",
    "rgba(20,7,44,0.84)", "rgba(20,7,44,0.60)", "rgba(70,22,140,0.60)",
    "#503080", "#eedcfc",
    "rgba(16,5,36,0.91)", "rgba(20,7,44,0.82)", "#2a1048",
    "#d8cce8", "rgba(80,22,160,0.66)", "#c050f8",
    "rgba(18,6,40,0.28)",
    "#c050f8", "#38186a", "#90d060", "#f0b060", "#80a8fc",
    "#50d8c8", "#c0a8e0", "#a080d0", "#f06080",
    "rgba(110,55,195,0.38)",
    "rgba(20,7,44,0.88)", "rgba(26,10,56,0.86)",
    "rgba(130,60,210,0.13)", "rgba(70,20,140,0.46)"
);

/*
 * Graphite — custom neutral monochrome.
 */
static const char k_graphite_css[] = THEME_CSS(
    "rgba(14,15,17,0.91)",
    "#7a8290", "#8890a0", "rgba(155,162,175,0.14)", "#6a7280", "#c06868",
    "rgba(200,208,218,0.72)",
    "rgba(10,10,12,0.97)", "#6a7280",
    "rgba(105,115,130,0.42)", "rgba(75,115,85,0.34)", "rgba(130,115,55,0.34)",
    "rgba(16,17,20,0.93)", "#6a7280", "rgba(155,165,180,0.11)",
    "rgba(20,22,26,0.94)", "#484e58", "#b0b8c4", "#8890a0", "#ccd4dc",
    "rgba(13,14,17,0.84)", "rgba(13,14,17,0.60)", "rgba(64,70,82,0.65)",
    "#484e58", "#e8ecf2",
    "rgba(11,12,14,0.91)", "rgba(14,15,17,0.82)", "#2c3038",
    "#d4d8e0", "rgba(65,72,86,0.75)", "#d8dce4",
    "rgba(13,14,17,0.28)",
    "#ccd2da", "#484e58", "#a0b098", "#b8aa90", "#a0b8cc",
    "#88aabb", "#98a0b0", "#7888a0", "#b88888",
    "rgba(112,122,138,0.38)",
    "rgba(13,14,17,0.88)", "rgba(16,17,20,0.86)",
    "rgba(148,156,172,0.11)", "rgba(62,68,82,0.52)"
);

/*
 * Ember Glass — custom amber/rust.
 */
static const char k_ember_css[] = THEME_CSS(
    "rgba(40,14,4,0.91)",
    "#a86028", "#c07838", "rgba(200,100,30,0.17)", "#985018", "#d05830",
    "rgba(248,140,40,0.82)",
    "rgba(26,8,2,0.97)", "#985018",
    "rgba(185,95,30,0.46)", "rgba(95,135,50,0.36)", "rgba(195,70,30,0.34)",
    "rgba(44,16,4,0.93)", "#985018", "rgba(200,100,28,0.13)",
    "rgba(56,20,5,0.94)", "#622810", "#f08820", "#c89858", "#f0cc80",
    "rgba(36,12,3,0.84)", "rgba(36,12,3,0.60)", "rgba(110,40,8,0.62)",
    "#7a3810", "#f8e0a0",
    "rgba(30,10,2,0.91)", "rgba(36,12,3,0.82)", "#401c08",
    "#ecd8a8", "rgba(130,48,10,0.70)", "#f08820",
    "rgba(34,12,3,0.28)",
    "#f08820", "#602810", "#a0c860", "#f8b850", "#e8d060",
    "#50cca0", "#d8b070", "#c08840", "#e04828",
    "rgba(175,82,26,0.38)",
    "rgba(36,12,3,0.88)", "rgba(44,16,4,0.86)",
    "rgba(188,95,24,0.13)", "rgba(110,40,8,0.48)"
);

/*
 * Gruvbox Dark — warm retro brown/orange palette (gruvbox dark medium).
 * bg0=#282828 fg1=#ebdbb2 yellow=#d79921 orange=#d65d0e green=#98971a
 * blue=#458588 aqua=#689d6a red=#cc241d purple=#b16286
 */
static const char k_gruvbox_css[] = THEME_CSS(
    "rgba(32,28,24,0.91)",
    "#d79921", "#d5c4a1", "rgba(168,153,132,0.18)", "#928374", "#cc241d",
    "rgba(214,93,14,0.85)",
    "rgba(20,18,14,0.97)", "#928374",
    "rgba(215,153,33,0.44)", "rgba(104,157,106,0.38)", "rgba(177,98,134,0.36)",
    "rgba(40,36,30,0.93)", "#928374", "rgba(168,153,132,0.12)",
    "rgba(50,46,38,0.94)", "#504945", "#d79921", "#d5c4a1", "#ebdbb2",
    "rgba(32,28,22,0.84)", "rgba(32,28,22,0.60)", "rgba(80,60,36,0.65)",
    "#7c6f64", "#ebdbb2",
    "rgba(24,22,18,0.91)", "rgba(40,36,30,0.82)", "#3c3836",
    "#ebdbb2", "rgba(80,60,36,0.72)", "#d79921",
    "rgba(32,28,22,0.28)",
    "#d79921", "#665c54", "#98971a", "#d65d0e", "#fabd2f",
    "#689d6a", "#d5c4a1", "#d65d0e", "#cc241d",
    "rgba(168,153,132,0.28)",
    "rgba(32,28,22,0.88)", "rgba(40,36,30,0.86)",
    "rgba(168,153,132,0.12)", "rgba(80,60,36,0.52)"
);

/*
 * Nord — arctic blue/slate palette (nord0–nord15).
 * nord0=#2e3440 nord3=#4c566a nord8=#88c0d0 nord11=#bf616a nord13=#ebcb8b
 */
static const char k_nord_css[] = THEME_CSS(
    "rgba(36,42,54,0.91)",
    "#88c0d0", "#81a1c1", "rgba(136,192,208,0.15)", "#5e81ac", "#bf616a",
    "rgba(136,192,208,0.80)",
    "rgba(22,26,34,0.97)", "#5e81ac",
    "rgba(94,129,172,0.44)", "rgba(163,190,140,0.38)", "rgba(235,203,139,0.36)",
    "rgba(46,52,64,0.93)", "#4c566a", "rgba(136,192,208,0.12)",
    "rgba(58,66,82,0.94)", "#3b4252", "#88c0d0", "#d8dee9", "#eceff4",
    "rgba(36,40,52,0.84)", "rgba(36,40,52,0.60)", "rgba(67,76,94,0.65)",
    "#4c566a", "#eceff4",
    "rgba(30,34,46,0.91)", "rgba(40,46,58,0.82)", "#3b4252",
    "#d8dee9", "rgba(67,76,94,0.72)", "#88c0d0",
    "rgba(36,40,52,0.28)",
    "#81a1c1", "#4c566a", "#a3be8c", "#b48ead", "#88c0d0",
    "#8fbcbb", "#d8dee9", "#81a1c1", "#bf616a",
    "rgba(94,129,172,0.30)",
    "rgba(36,40,52,0.88)", "rgba(46,52,64,0.86)",
    "rgba(136,192,208,0.11)", "rgba(67,76,94,0.52)"
);

/*
 * Tokyo Night — cool indigo/purple (tokyonight night variant).
 * bg=#1a1b26 fg=#c0caf5 blue=#7aa2f7 cyan=#7dcfff purple=#bb9af7
 * green=#9ece6a red=#f7768e orange=#ff9e64 yellow=#e0af68
 */
static const char k_tokyonight_css[] = THEME_CSS(
    "rgba(22,22,36,0.91)",
    "#7aa2f7", "#c0caf5", "rgba(122,162,247,0.16)", "#565f89", "#f7768e",
    "rgba(125,207,255,0.82)",
    "rgba(12,12,24,0.97)", "#565f89",
    "rgba(122,162,247,0.42)", "rgba(158,206,106,0.36)", "rgba(224,175,104,0.36)",
    "rgba(28,29,50,0.93)", "#565f89", "rgba(122,162,247,0.12)",
    "rgba(36,38,62,0.94)", "#3b3f61", "#7aa2f7", "#c0caf5", "#cdd6f4",
    "rgba(22,22,40,0.84)", "rgba(22,22,40,0.60)", "rgba(52,56,92,0.65)",
    "#3b3f61", "#cdd6f4",
    "rgba(16,17,32,0.91)", "rgba(24,24,42,0.82)", "#292e42",
    "#c0caf5", "rgba(52,56,92,0.75)", "#7aa2f7",
    "rgba(22,22,40,0.28)",
    "#bb9af7", "#565f89", "#9ece6a", "#ff9e64", "#7aa2f7",
    "#2ac3de", "#c0caf5", "#89ddff", "#f7768e",
    "rgba(122,162,247,0.28)",
    "rgba(22,22,40,0.88)", "rgba(28,29,50,0.86)",
    "rgba(122,162,247,0.13)", "rgba(52,56,92,0.52)"
);

/*
 * Catppuccin Mocha — pastel mauve/lavender.
 * base=#1e1e2e surface0=#313244 mauve=#cba6f7 blue=#89b4fa
 * green=#a6e3a1 red=#f38ba8 yellow=#f9e2af peach=#fab387 teal=#94e2d5
 */
static const char k_catppuccin_css[] = THEME_CSS(
    "rgba(24,24,37,0.91)",
    "#cba6f7", "#cdd6f4", "rgba(203,166,247,0.16)", "#6c7086", "#f38ba8",
    "rgba(203,166,247,0.80)",
    "rgba(12,12,20,0.97)", "#6c7086",
    "rgba(203,166,247,0.42)", "rgba(166,227,161,0.36)", "rgba(249,226,175,0.36)",
    "rgba(30,30,46,0.93)", "#6c7086", "rgba(203,166,247,0.12)",
    "rgba(40,40,56,0.94)", "#45475a", "#cba6f7", "#cdd6f4", "#f5f5f7",
    "rgba(24,24,40,0.84)", "rgba(24,24,40,0.60)", "rgba(69,71,90,0.65)",
    "#45475a", "#f5f5f7",
    "rgba(18,18,32,0.91)", "rgba(24,24,38,0.82)", "#313244",
    "#cdd6f4", "rgba(69,71,90,0.75)", "#cba6f7",
    "rgba(24,24,40,0.28)",
    "#cba6f7", "#585b70", "#a6e3a1", "#fab387", "#89b4fa",
    "#94e2d5", "#cdd6f4", "#89dceb", "#f38ba8",
    "rgba(137,180,250,0.30)",
    "rgba(24,24,38,0.88)", "rgba(30,30,46,0.86)",
    "rgba(203,166,247,0.12)", "rgba(69,71,90,0.52)"
);

/*
 * Dracula — purple/pink cyberpunk.
 * bg=#282a36 fg=#f8f8f2 pink=#ff79c6 purple=#bd93f9 cyan=#8be9fd
 * green=#50fa7b orange=#ffb86c red=#ff5555 yellow=#f1fa8c
 */
static const char k_dracula_css[] = THEME_CSS(
    "rgba(32,34,46,0.91)",
    "#bd93f9", "#f8f8f2", "rgba(189,147,249,0.17)", "#6272a4", "#ff5555",
    "rgba(255,121,198,0.85)",
    "rgba(18,20,30,0.97)", "#6272a4",
    "rgba(189,147,249,0.44)", "rgba(80,250,123,0.36)", "rgba(241,250,140,0.36)",
    "rgba(40,42,56,0.93)", "#6272a4", "rgba(189,147,249,0.13)",
    "rgba(50,52,68,0.94)", "#44475a", "#bd93f9", "#f8f8f2", "#ffffff",
    "rgba(32,34,46,0.84)", "rgba(32,34,46,0.60)", "rgba(68,71,90,0.65)",
    "#44475a", "#ffffff",
    "rgba(26,28,40,0.91)", "rgba(40,42,56,0.82)", "#44475a",
    "#f8f8f2", "rgba(68,71,90,0.75)", "#ff79c6",
    "rgba(32,34,46,0.28)",
    "#ff79c6", "#6272a4", "#f1fa8c", "#ffb86c", "#50fa7b",
    "#8be9fd", "#f8f8f2", "#ff79c6", "#ff5555",
    "rgba(189,147,249,0.30)",
    "rgba(32,34,46,0.88)", "rgba(40,42,56,0.86)",
    "rgba(189,147,249,0.13)", "rgba(68,71,90,0.52)"
);

/*
 * One Dark Pro — muted blue-grey (atom one dark).
 * bg=#282c34 fg=#abb2bf blue=#61afef green=#98c379 red=#e06c75
 * yellow=#e5c07b cyan=#56b6c2 purple=#c678dd orange=#d19a66
 */
static const char k_onedark_css[] = THEME_CSS(
    "rgba(30,33,40,0.91)",
    "#61afef", "#abb2bf", "rgba(97,175,239,0.16)", "#5c6370", "#e06c75",
    "rgba(97,175,239,0.82)",
    "rgba(18,20,26,0.97)", "#5c6370",
    "rgba(97,175,239,0.42)", "rgba(152,195,121,0.36)", "rgba(229,192,123,0.36)",
    "rgba(36,40,50,0.93)", "#5c6370", "rgba(97,175,239,0.12)",
    "rgba(46,50,60,0.94)", "#3e4451", "#61afef", "#abb2bf", "#d0d5e0",
    "rgba(30,33,42,0.84)", "rgba(30,33,42,0.60)", "rgba(62,68,86,0.65)",
    "#3e4451", "#d0d5e0",
    "rgba(24,26,34,0.91)", "rgba(30,33,42,0.82)", "#3e4451",
    "#abb2bf", "rgba(62,68,86,0.75)", "#61afef",
    "rgba(30,33,42,0.28)",
    "#c678dd", "#5c6370", "#98c379", "#d19a66", "#61afef",
    "#56b6c2", "#abb2bf", "#56b6c2", "#e06c75",
    "rgba(97,175,239,0.28)",
    "rgba(30,33,42,0.88)", "rgba(36,40,50,0.86)",
    "rgba(97,175,239,0.12)", "rgba(62,68,86,0.52)"
);

/*
 * Rosé Pine — dusty rose/pine (rose-pine main).
 * base=#191724 surface=#1f1d2e gold=#f6c177 iris=#c4a7e7 pine=#31748f
 * foam=#9ccfd8 rose=#ebbcba love=#eb6f92 muted=#6e6a86
 */
static const char k_rosepine_css[] = THEME_CSS(
    "rgba(22,20,32,0.91)",
    "#c4a7e7", "#e0d9f0", "rgba(196,167,231,0.16)", "#6e6a86", "#eb6f92",
    "rgba(196,167,231,0.82)",
    "rgba(12,11,22,0.97)", "#6e6a86",
    "rgba(196,167,231,0.42)", "rgba(156,207,216,0.38)", "rgba(246,193,119,0.36)",
    "rgba(26,24,40,0.93)", "#6e6a86", "rgba(196,167,231,0.12)",
    "rgba(34,32,52,0.94)", "#44415a", "#c4a7e7", "#e0d9f0", "#f5eeff",
    "rgba(20,18,34,0.84)", "rgba(20,18,34,0.60)", "rgba(62,58,84,0.65)",
    "#44415a", "#f5eeff",
    "rgba(16,14,30,0.91)", "rgba(22,20,36,0.82)", "#393552",
    "#e0d9f0", "rgba(62,58,84,0.75)", "#c4a7e7",
    "rgba(20,18,34,0.28)",
    "#c4a7e7", "#6e6a86", "#9ccfd8", "#f6c177", "#9ccfd8",
    "#31748f", "#e0d9f0", "#c4a7e7", "#eb6f92",
    "rgba(196,167,231,0.28)",
    "rgba(20,18,34,0.88)", "rgba(26,24,40,0.86)",
    "rgba(196,167,231,0.12)", "rgba(62,58,84,0.52)"
);

/*
 * Everforest — muted green/sage (everforest dark hard).
 * bg0=#272e33 fg=#d3c6aa green=#a7c080 yellow=#dbbc7f orange=#e69875
 * red=#e67e80 purple=#d699b6 blue=#7fbbb3 aqua=#83c092 grey=#859289
 */
static const char k_everforest_css[] = THEME_CSS(
    "rgba(30,36,40,0.91)",
    "#a7c080", "#d3c6aa", "rgba(167,192,128,0.16)", "#859289", "#e67e80",
    "rgba(131,192,146,0.82)",
    "rgba(18,24,28,0.97)", "#859289",
    "rgba(127,187,179,0.42)", "rgba(167,192,128,0.38)", "rgba(219,188,127,0.36)",
    "rgba(36,44,48,0.93)", "#859289", "rgba(167,192,128,0.12)",
    "rgba(46,56,60,0.94)", "#374247", "#a7c080", "#d3c6aa", "#f0e6cc",
    "rgba(30,36,42,0.84)", "rgba(30,36,42,0.60)", "rgba(56,68,72,0.65)",
    "#374247", "#f0e6cc",
    "rgba(24,30,34,0.91)", "rgba(30,38,42,0.82)", "#3d4e54",
    "#d3c6aa", "rgba(56,68,72,0.75)", "#a7c080",
    "rgba(30,36,42,0.28)",
    "#a7c080", "#5c6a72", "#a7c080", "#dbbc7f", "#7fbbb3",
    "#83c092", "#d3c6aa", "#7fbbb3", "#e67e80",
    "rgba(127,187,179,0.30)",
    "rgba(30,36,42,0.88)", "rgba(36,44,48,0.86)",
    "rgba(167,192,128,0.12)", "rgba(56,68,72,0.52)"
);

/*
 * Kanagawa — deep Japanese ink/wave (kanagawa wave).
 * bg=#1f1f28 fg=#dcd7ba dragonBlue=#658594 waveBlue=#223249 lotusBlue=#c7d7e0
 * sakuraPink=#d27e99 springGreen=#98bb6c carpYellow=#e6c384 waveAqua=#6a9589
 * oniViolet=#957fb8 crystalBlue=#7e9cd8 roninYellow=#ff9e3b
 */
static const char k_kanagawa_css[] = THEME_CSS(
    "rgba(22,22,30,0.91)",
    "#7e9cd8", "#dcd7ba", "rgba(126,156,216,0.16)", "#54546d", "#c34043",
    "rgba(101,133,148,0.82)",
    "rgba(12,12,18,0.97)", "#54546d",
    "rgba(126,156,216,0.42)", "rgba(152,187,108,0.36)", "rgba(230,195,132,0.36)",
    "rgba(26,26,36,0.93)", "#54546d", "rgba(126,156,216,0.12)",
    "rgba(34,34,46,0.94)", "#363646", "#7e9cd8", "#dcd7ba", "#f2ecbc",
    "rgba(20,20,30,0.84)", "rgba(20,20,30,0.60)", "rgba(54,54,80,0.65)",
    "#363646", "#f2ecbc",
    "rgba(16,16,26,0.91)", "rgba(22,22,34,0.82)", "#2a2a3a",
    "#dcd7ba", "rgba(54,54,80,0.75)", "#7e9cd8",
    "rgba(20,20,30,0.28)",
    "#957fb8", "#54546d", "#98bb6c", "#ff9e3b", "#7e9cd8",
    "#6a9589", "#dcd7ba", "#e6c384", "#c34043",
    "rgba(126,156,216,0.28)",
    "rgba(20,20,30,0.88)", "rgba(26,26,36,0.86)",
    "rgba(126,156,216,0.12)", "rgba(54,54,80,0.52)"
);

/*
 * Carbonfox — charcoal/orange (nightfox carbonfox).
 * bg=#161616 fg=#f2f4f8 orange=#3ddbd9 blue=#78a9ff red=#ee5396 green=#25be6a
 * yellow=#08bdba magenta=#be95ff cyan=#3ddbd9
 */
static const char k_carbonfox_css[] = THEME_CSS(
    "rgba(18,18,20,0.91)",
    "#78a9ff", "#f2f4f8", "rgba(120,169,255,0.16)", "#525252", "#ee5396",
    "rgba(61,219,217,0.80)",
    "rgba(10,10,12,0.97)", "#525252",
    "rgba(120,169,255,0.42)", "rgba(37,190,106,0.36)", "rgba(8,189,186,0.36)",
    "rgba(22,22,26,0.93)", "#525252", "rgba(120,169,255,0.12)",
    "rgba(30,30,34,0.94)", "#393939", "#78a9ff", "#f2f4f8", "#ffffff",
    "rgba(16,16,20,0.84)", "rgba(16,16,20,0.60)", "rgba(57,57,65,0.65)",
    "#393939", "#ffffff",
    "rgba(12,12,16,0.91)", "rgba(18,18,22,0.82)", "#262626",
    "#f2f4f8", "rgba(57,57,65,0.75)", "#78a9ff",
    "rgba(16,16,20,0.28)",
    "#be95ff", "#525252", "#25be6a", "#ff832b", "#78a9ff",
    "#3ddbd9", "#f2f4f8", "#3ddbd9", "#ee5396",
    "rgba(120,169,255,0.28)",
    "rgba(16,16,20,0.88)", "rgba(22,22,26,0.86)",
    "rgba(120,169,255,0.12)", "rgba(57,57,65,0.52)"
);

/*
 * Monokai Pro — saturated pink/green (monokai pro classic).
 * bg=#2d2a2e fg=#fcfcfa yellow=#ffd866 orange=#fc9867 red=#ff6188
 * purple=#ab9df2 green=#a9dc76 blue=#78dce8
 */
static const char k_monokai_css[] = THEME_CSS(
    "rgba(34,32,36,0.91)",
    "#ffd866", "#fcfcfa", "rgba(255,216,102,0.17)", "#727072", "#ff6188",
    "rgba(120,220,232,0.82)",
    "rgba(20,18,22,0.97)", "#727072",
    "rgba(171,157,242,0.42)", "rgba(169,220,118,0.36)", "rgba(255,216,102,0.36)",
    "rgba(40,38,42,0.93)", "#727072", "rgba(255,216,102,0.13)",
    "rgba(50,48,54,0.94)", "#5b595c", "#ffd866", "#fcfcfa", "#ffffff",
    "rgba(34,32,38,0.84)", "rgba(34,32,38,0.60)", "rgba(82,78,88,0.65)",
    "#5b595c", "#ffffff",
    "rgba(28,26,32,0.91)", "rgba(34,32,40,0.82)", "#403e41",
    "#fcfcfa", "rgba(82,78,88,0.75)", "#ff6188",
    "rgba(34,32,38,0.28)",
    "#ff6188", "#5b595c", "#a9dc76", "#fc9867", "#78dce8",
    "#ab9df2", "#fcfcfa", "#78dce8", "#ff6188",
    "rgba(171,157,242,0.30)",
    "rgba(34,32,38,0.88)", "rgba(40,38,42,0.86)",
    "rgba(255,216,102,0.13)", "rgba(82,78,88,0.52)"
);

/*
 * Sonokai — vivid mixed accent (sonokai shusia variant).
 * bg=#2d2a2e fg=#e2e2e3 red=#f85e84 orange=#ef9062 yellow=#e7c664
 * green=#9ed06c blue=#76cce0 purple=#b39df3
 */
static const char k_sonokai_css[] = THEME_CSS(
    "rgba(34,32,38,0.91)",
    "#76cce0", "#e2e2e3", "rgba(118,204,224,0.17)", "#7f8490", "#f85e84",
    "rgba(118,204,224,0.82)",
    "rgba(20,18,24,0.97)", "#7f8490",
    "rgba(179,157,243,0.42)", "rgba(158,208,108,0.36)", "rgba(231,198,100,0.36)",
    "rgba(40,38,46,0.93)", "#7f8490", "rgba(118,204,224,0.13)",
    "rgba(50,48,58,0.94)", "#4a4a59", "#76cce0", "#e2e2e3", "#f2f2f3",
    "rgba(34,32,42,0.84)", "rgba(34,32,42,0.60)", "rgba(72,70,88,0.65)",
    "#4a4a59", "#f2f2f3",
    "rgba(26,24,34,0.91)", "rgba(34,32,44,0.82)", "#3a384a",
    "#e2e2e3", "rgba(72,70,88,0.75)", "#f85e84",
    "rgba(34,32,42,0.28)",
    "#f85e84", "#4a4a59", "#9ed06c", "#ef9062", "#76cce0",
    "#b39df3", "#e2e2e3", "#76cce0", "#f85e84",
    "rgba(179,157,243,0.28)",
    "rgba(34,32,42,0.88)", "rgba(40,38,46,0.86)",
    "rgba(118,204,224,0.13)", "rgba(72,70,88,0.52)"
);

/* ------------------------------------------------------------------ */
/* Theme table                                                          */
/* ------------------------------------------------------------------ */

static const struct {
    const char *id;
    const char *name;
    const char *css;
} k_themes[] = {
    { "com.sol.theme.ocean",       "Deep Ocean",     k_ocean_css       },
    { "com.sol.theme.amethyst",    "Amethyst",       k_amethyst_css    },
    { "com.sol.theme.graphite",    "Graphite",       k_graphite_css    },
    { "com.sol.theme.ember",       "Ember Glass",    k_ember_css       },
    { "com.sol.theme.gruvbox",     "Gruvbox Dark",   k_gruvbox_css     },
    { "com.sol.theme.nord",        "Nord",           k_nord_css        },
    { "com.sol.theme.tokyonight",  "Tokyo Night",    k_tokyonight_css  },
    { "com.sol.theme.catppuccin",  "Catppuccin",     k_catppuccin_css  },
    { "com.sol.theme.dracula",     "Dracula",        k_dracula_css     },
    { "com.sol.theme.onedark",     "One Dark",       k_onedark_css     },
    { "com.sol.theme.rosepine",    "Rosé Pine",      k_rosepine_css    },
    { "com.sol.theme.everforest",  "Everforest",     k_everforest_css  },
    { "com.sol.theme.kanagawa",    "Kanagawa",       k_kanagawa_css    },
    { "com.sol.theme.carbonfox",   "Carbonfox",      k_carbonfox_css   },
    { "com.sol.theme.monokai",     "Monokai Pro",    k_monokai_css     },
    { "com.sol.theme.sonokai",     "Sonokai",        k_sonokai_css     },
};

/* ------------------------------------------------------------------ */
/* Plugin lifecycle                                                     */
/* ------------------------------------------------------------------ */

/*
 * Register all theme overrides, each extending the Glass baseline.
 *
 * ctx  Plugin context for registration tracking.
 */
static bool themes_on_load(SolPluginCtx *ctx)
{
    for (size_t i = 0u; i < sizeof(k_themes) / sizeof(k_themes[0]); ++i) {
        sol_plugin_register_theme(ctx, &(SolThemeDesc){
            .id      = k_themes[i].id,
            .name    = k_themes[i].name,
            .base_id = "com.sol.theme.glass",
            .css     = k_themes[i].css,
        });
    }
    return true;
}

static void themes_on_unload(SolPluginCtx *ctx)
{
    (void)ctx;
}

/*
 * Query function exported by the plugin; called by the plugin manager to
 * discover capabilities and verify API compatibility.
 *
 * requested_api_version  API version the manager was compiled against.
 * out_api                Filled with this plugin's descriptor.
 * Returns  true if the plugin supports the requested API version.
 */
bool sol_plugin_query(uint32_t requested_api_version, SolPluginAPI *out_api)
{
    if (requested_api_version != SOL_PLUGIN_API_VERSION) return false;
    *out_api = (SolPluginAPI){
        .api_version  = SOL_PLUGIN_API_VERSION,
        .id           = "com.sol.themes",
        .display_name = "Sol Themes",
        .version      = "1.0.0",
        .after        = { NULL },
        .on_load      = themes_on_load,
        .on_unload    = themes_on_unload,
    };
    return true;
}
