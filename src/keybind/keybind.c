/*
 * Sol IDE - Default Keybindings
 */

#include "sol.h"

void sol_keybind_defaults(sol_keymap* km) {
    if (!km) return;
    
    /* ===== File Operations ===== */
    sol_keymap_bind(km, "ctrl+s", "file.save", NULL);
    sol_keymap_bind(km, "ctrl+shift+s", "file.saveAs", NULL);
    sol_keymap_bind(km, "ctrl+o", "file.open", NULL);
    sol_keymap_bind(km, "alt+o", "file.openFolder", NULL);
    sol_keymap_bind(km, "ctrl+n", "file.new", NULL);
    sol_keymap_bind(km, "ctrl+w", "view.close", NULL);
    sol_keymap_bind(km, "ctrl+q", "app.quit", NULL);
    
    /* ===== Edit Operations ===== */
    sol_keymap_bind(km, "ctrl+z", "edit.undo", NULL);
    sol_keymap_bind(km, "ctrl+shift+z", "edit.redo", NULL);
    sol_keymap_bind(km, "ctrl+y", "edit.redo", NULL);
    sol_keymap_bind(km, "ctrl+x", "edit.cut", NULL);
    sol_keymap_bind(km, "ctrl+c", "edit.copy", NULL);
    sol_keymap_bind(km, "ctrl+v", "edit.paste", NULL);
    sol_keymap_bind(km, "ctrl+a", "edit.selectAll", NULL);
    sol_keymap_bind(km, "ctrl+d", "edit.selectWord", NULL);
    sol_keymap_bind(km, "ctrl+l", "edit.selectLine", NULL);
    sol_keymap_bind(km, "ctrl+/", "edit.toggleComment", NULL);
    sol_keymap_bind(km, "ctrl+]", "edit.indent", NULL);
    sol_keymap_bind(km, "ctrl+[", "edit.outdent", NULL);
    sol_keymap_bind(km, "ctrl+shift+k", "edit.deleteLine", NULL);
    sol_keymap_bind(km, "alt+up", "edit.moveLineUp", NULL);
    sol_keymap_bind(km, "alt+down", "edit.moveLineDown", NULL);
    sol_keymap_bind(km, "ctrl+shift+d", "edit.duplicateLine", NULL);
    sol_keymap_bind(km, "ctrl+enter", "edit.insertLineBelow", NULL);
    sol_keymap_bind(km, "ctrl+shift+enter", "edit.insertLineAbove", NULL);
    
    /* ===== Navigation ===== */
    sol_keymap_bind(km, "ctrl+g", "navigate.gotoLine", NULL);
    sol_keymap_bind(km, "ctrl+p", "navigate.quickOpen", NULL);  /* File picker */
    sol_keymap_bind(km, "ctrl+shift+p", "command.palette", NULL);
    sol_keymap_bind(km, "ctrl+shift+o", "navigate.gotoSymbol", NULL);
    sol_keymap_bind(km, "ctrl+shift+f", "search.inFiles", NULL);
    sol_keymap_bind(km, "ctrl+f", "search.find", NULL);
    sol_keymap_bind(km, "ctrl+h", "search.replace", NULL);
    sol_keymap_bind(km, "f3", "search.findNext", NULL);
    sol_keymap_bind(km, "shift+f3", "search.findPrev", NULL);
    sol_keymap_bind(km, "ctrl+home", "navigate.top", NULL);
    sol_keymap_bind(km, "ctrl+end", "navigate.bottom", NULL);
    
    /* ===== View/Panel ===== */
    sol_keymap_bind(km, "ctrl+b", "view.toggleSidebar", NULL);
    sol_keymap_bind(km, "ctrl+`", "view.toggleTerminal", NULL);
    sol_keymap_bind(km, "ctrl+j", "view.togglePanel", NULL);
    sol_keymap_bind(km, "ctrl+\\", "view.splitRight", NULL);
    sol_keymap_bind(km, "ctrl+shift+\\", "view.splitDown", NULL);
    sol_keymap_bind(km, "ctrl+1", "view.focusEditor1", NULL);
    sol_keymap_bind(km, "ctrl+2", "view.focusEditor2", NULL);
    sol_keymap_bind(km, "ctrl+3", "view.focusEditor3", NULL);
    sol_keymap_bind(km, "f11", "view.toggleFullscreen", NULL);
    sol_keymap_bind(km, "ctrl+shift+e", "view.focusFileTree", NULL);
    
    /* ===== Tab Navigation ===== */
    sol_keymap_bind(km, "ctrl+tab", "tab.next", NULL);
    sol_keymap_bind(km, "ctrl+shift+tab", "tab.prev", NULL);
    sol_keymap_bind(km, "alt+1", "tab.goto1", NULL);
    sol_keymap_bind(km, "alt+2", "tab.goto2", NULL);
    sol_keymap_bind(km, "alt+3", "tab.goto3", NULL);
    sol_keymap_bind(km, "alt+4", "tab.goto4", NULL);
    sol_keymap_bind(km, "alt+5", "tab.goto5", NULL);
    sol_keymap_bind(km, "alt+6", "tab.goto6", NULL);
    sol_keymap_bind(km, "alt+7", "tab.goto7", NULL);
    sol_keymap_bind(km, "alt+8", "tab.goto8", NULL);
    sol_keymap_bind(km, "alt+9", "tab.gotoLast", NULL);
    
    /* ===== LSP/Code Intelligence ===== */
    sol_keymap_bind(km, "f12", "lsp.gotoDefinition", NULL);
    sol_keymap_bind(km, "alt+f12", "lsp.peekDefinition", NULL);
    sol_keymap_bind(km, "shift+f12", "lsp.findReferences", NULL);
    sol_keymap_bind(km, "f2", "lsp.rename", NULL);
    sol_keymap_bind(km, "ctrl+.", "lsp.codeAction", NULL);
    sol_keymap_bind(km, "ctrl+space", "lsp.triggerCompletion", NULL);
    sol_keymap_bind(km, "ctrl+shift+space", "lsp.triggerSignature", NULL);
    sol_keymap_bind(km, "ctrl+k ctrl+i", "lsp.showHover", NULL);
    
    /* ===== Vim-like Leader Sequences ===== */
    /* Using space as leader (like Doom Emacs/SpaceVim) */
    sol_keymap_bind(km, "space f f", "navigate.quickOpen", NULL);  /* Find file */
    sol_keymap_bind(km, "space f s", "file.save", NULL);           /* File save */
    sol_keymap_bind(km, "space f S", "file.saveAll", NULL);        /* Save all */
    sol_keymap_bind(km, "space f r", "file.recentFiles", NULL);    /* Recent files */
    sol_keymap_bind(km, "space f t", "view.focusFileTree", NULL);  /* File tree */
    
    sol_keymap_bind(km, "space b b", "buffer.switch", NULL);       /* Buffer list */
    sol_keymap_bind(km, "space b d", "buffer.close", NULL);        /* Buffer delete */
    sol_keymap_bind(km, "space b n", "tab.next", NULL);            /* Next buffer */
    sol_keymap_bind(km, "space b p", "tab.prev", NULL);            /* Prev buffer */
    
    sol_keymap_bind(km, "space w v", "view.splitRight", NULL);     /* Vertical split */
    sol_keymap_bind(km, "space w s", "view.splitDown", NULL);      /* Horizontal split */
    sol_keymap_bind(km, "space w d", "view.close", NULL);          /* Close window */
    sol_keymap_bind(km, "space w h", "view.focusLeft", NULL);      /* Focus left */
    sol_keymap_bind(km, "space w l", "view.focusRight", NULL);     /* Focus right */
    sol_keymap_bind(km, "space w j", "view.focusDown", NULL);      /* Focus down */
    sol_keymap_bind(km, "space w k", "view.focusUp", NULL);        /* Focus up */
    
    sol_keymap_bind(km, "space s s", "search.find", NULL);         /* Search */
    sol_keymap_bind(km, "space s p", "search.inFiles", NULL);      /* Search project */
    sol_keymap_bind(km, "space s r", "search.replace", NULL);      /* Search/replace */
    
    sol_keymap_bind(km, "space g g", "git.status", NULL);          /* Git status */
    sol_keymap_bind(km, "space g s", "git.stage", NULL);           /* Stage file */
    sol_keymap_bind(km, "space g c", "git.commit", NULL);          /* Commit */
    sol_keymap_bind(km, "space g p", "git.push", NULL);            /* Push */
    sol_keymap_bind(km, "space g l", "git.log", NULL);             /* Log */
    sol_keymap_bind(km, "space g d", "git.diff", NULL);            /* Diff */
    
    sol_keymap_bind(km, "space c a", "lsp.codeAction", NULL);      /* Code action */
    sol_keymap_bind(km, "space c r", "lsp.rename", NULL);          /* Rename */
    sol_keymap_bind(km, "space c d", "lsp.gotoDefinition", NULL);  /* Definition */
    sol_keymap_bind(km, "space c f", "lsp.format", NULL);          /* Format */
    
    sol_keymap_bind(km, "space t t", "view.toggleTerminal", NULL); /* Terminal */
    sol_keymap_bind(km, "space t n", "terminal.new", NULL);        /* New terminal */
    
    sol_keymap_bind(km, "space h h", "help.documentation", NULL);  /* Help */
    sol_keymap_bind(km, "space h k", "help.keybindings", NULL);    /* Show keybindings */
    
    /* ===== Quick Access (double press) ===== */
    sol_keymap_bind(km, "space space", "command.palette", NULL);   /* Command palette */
    
    /* ===== Debug ===== */
    sol_keymap_bind(km, "f5", "debug.start", NULL);
    sol_keymap_bind(km, "shift+f5", "debug.stop", NULL);
    sol_keymap_bind(km, "f9", "debug.toggleBreakpoint", NULL);
    sol_keymap_bind(km, "f10", "debug.stepOver", NULL);
    sol_keymap_bind(km, "f11", "debug.stepInto", NULL);
    sol_keymap_bind(km, "shift+f11", "debug.stepOut", NULL);
}
