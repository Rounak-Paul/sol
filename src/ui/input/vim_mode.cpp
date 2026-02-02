#include "vim_mode.h"
#include "core/text/text_buffer.h"
#include "core/text/undo_tree.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace sol {

VimMode::VimMode() {
}

const char* VimMode::GetIndicator() const {
    switch (m_State) {
        case VimState::Normal: return "NORMAL";
        case VimState::Insert: return "INSERT";
        case VimState::Visual: return "VISUAL";
        case VimState::VisualLine: return "V-LINE";
        case VimState::VisualBlock: return "V-BLOCK";
        case VimState::Command: return ":";
        case VimState::Search: return m_SearchForward ? "/" : "?";
        case VimState::Replace: return "REPLACE";
        case VimState::Operator: return "OPERATOR";
        default: return "";
    }
}

void VimMode::OnActivate(EditorState& state) {
    m_State = VimState::Normal;
    ResetCount();
    m_PendingOperator = VimOperator::None;
    m_PendingKeys.clear();
}

void VimMode::OnDeactivate(EditorState& state) {
    m_State = VimState::Normal;
}

void VimMode::RenderUI(EditorState& state) {
    // Render command line if in command/search mode
    if (m_State == VimState::Command || m_State == VimState::Search) {
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetTextLineHeightWithSpacing() - 4);
        
        char prefix = (m_State == VimState::Command) ? ':' : (m_SearchForward ? '/' : '?');
        std::string cmdLine = std::string(1, prefix) + m_CommandBuffer + "_";
        
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", cmdLine.c_str());
    }
    
    // Show count if entering
    if (m_HasCount && m_State == VimState::Normal) {
        std::string countStr = std::to_string(m_Count);
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetTextLineHeightWithSpacing() - 4);
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", countStr.c_str());
    }
}

InputResult VimMode::HandleKeyboard(EditorState& state) {
    switch (m_State) {
        case VimState::Normal:
            return HandleNormalMode(state);
        case VimState::Insert:
            return HandleInsertMode(state);
        case VimState::Visual:
        case VimState::VisualLine:
        case VimState::VisualBlock:
            return HandleVisualMode(state);
        case VimState::Command:
            return HandleCommandMode(state);
        case VimState::Search:
            return HandleSearchMode(state);
        case VimState::Operator:
            return HandleOperatorPending(state);
        default:
            return {};
    }
}

InputResult VimMode::HandleNormalMode(EditorState& state) {
    InputResult result;
    ImGuiIO& io = ImGui::GetIO();
    
    // Handle counts (digits)
    for (int digit = 0; digit <= 9; ++digit) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + digit))) {
            if (digit == 0 && !m_HasCount) {
                // 0 without count = go to line start
                auto newPos = TextOps::LineStart(*state.buffer, state.cursorPos);
                result.handled = true;
                result.cursorMoved = true;
                result.newCursorPos = newPos;
                return result;
            }
            AddCount(digit);
            result.handled = true;
            return result;
        }
    }
    
    int count = GetCount();
    
    // Basic motions
    if (ImGui::IsKeyPressed(ImGuiKey_H) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        size_t newPos = state.cursorPos;
        for (int i = 0; i < count && newPos > 0; ++i) --newPos;
        result.handled = true;
        result.cursorMoved = true;
        result.newCursorPos = newPos;
        ResetCount();
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_L) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        size_t newPos = state.cursorPos;
        size_t lineEnd = TextOps::LineEnd(*state.buffer, state.cursorPos);
        for (int i = 0; i < count && newPos < lineEnd; ++i) ++newPos;
        result.handled = true;
        result.cursorMoved = true;
        result.newCursorPos = newPos;
        ResetCount();
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_J) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        auto [line, col] = state.buffer->PosToLineCol(state.cursorPos);
        size_t newLine = std::min(line + count, state.buffer->LineCount() - 1);
        std::string targetLine = state.buffer->Line(newLine);
        size_t newCol = std::min(col, targetLine.length());
        result.handled = true;
        result.cursorMoved = true;
        result.newCursorPos = state.buffer->LineColToPos(newLine, newCol);
        ResetCount();
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_K) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        auto [line, col] = state.buffer->PosToLineCol(state.cursorPos);
        size_t newLine = (line > static_cast<size_t>(count)) ? line - count : 0;
        std::string targetLine = state.buffer->Line(newLine);
        size_t newCol = std::min(col, targetLine.length());
        result.handled = true;
        result.cursorMoved = true;
        result.newCursorPos = state.buffer->LineColToPos(newLine, newCol);
        ResetCount();
        return result;
    }
    
    // Word motions
    if (ImGui::IsKeyPressed(ImGuiKey_W)) {
        size_t newPos = state.cursorPos;
        for (int i = 0; i < count; ++i) {
            newPos = TextOps::NextWord(*state.buffer, newPos);
        }
        result.handled = true;
        result.cursorMoved = true;
        result.newCursorPos = newPos;
        ResetCount();
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_B)) {
        size_t newPos = state.cursorPos;
        for (int i = 0; i < count; ++i) {
            newPos = TextOps::PrevWord(*state.buffer, newPos);
        }
        result.handled = true;
        result.cursorMoved = true;
        result.newCursorPos = newPos;
        ResetCount();
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_E)) {
        size_t newPos = state.cursorPos;
        for (int i = 0; i < count; ++i) {
            newPos = TextOps::WordEnd(*state.buffer, newPos);
        }
        result.handled = true;
        result.cursorMoved = true;
        result.newCursorPos = newPos;
        ResetCount();
        return result;
    }
    
    // Line motions
    if (ImGui::IsKeyPressed(ImGuiKey_6) && io.KeyShift) {  // ^
        result.handled = true;
        result.cursorMoved = true;
        result.newCursorPos = TextOps::FirstNonWhitespace(*state.buffer, state.cursorPos);
        ResetCount();
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_4) && io.KeyShift) {  // $
        result.handled = true;
        result.cursorMoved = true;
        result.newCursorPos = TextOps::LineEnd(*state.buffer, state.cursorPos);
        ResetCount();
        return result;
    }
    
    // Insert mode entry
    if (ImGui::IsKeyPressed(ImGuiKey_I)) {
        if (io.KeyShift) {  // I = insert at line start
            result.newCursorPos = TextOps::FirstNonWhitespace(*state.buffer, state.cursorPos);
        }
        EnterInsertMode(state, false);
        result.handled = true;
        ResetCount();
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_A)) {
        if (io.KeyShift) {  // A = append at line end
            result.newCursorPos = TextOps::LineEnd(*state.buffer, state.cursorPos);
        } else if (state.cursorPos < state.buffer->Length()) {
            result.newCursorPos = state.cursorPos + 1;
        }
        EnterInsertMode(state, true);
        result.handled = true;
        ResetCount();
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_O)) {
        // Open new line below
        size_t lineEnd = TextOps::LineEnd(*state.buffer, state.cursorPos);
        if (state.undoTree) {
            TextOps::Insert(*state.buffer, *state.undoTree, lineEnd, "\n", state.cursorPos);
        } else {
            state.buffer->Insert(lineEnd, "\n");
        }
        result.newCursorPos = lineEnd + 1;
        result.textChanged = true;
        EnterInsertMode(state, false);
        result.handled = true;
        ResetCount();
        return result;
    }
    
    // Visual mode
    if (ImGui::IsKeyPressed(ImGuiKey_V)) {
        if (io.KeyShift) {
            EnterVisualMode(state, VimState::VisualLine);
        } else {
            EnterVisualMode(state, VimState::Visual);
        }
        result.handled = true;
        ResetCount();
        return result;
    }
    
    // Operators
    if (ImGui::IsKeyPressed(ImGuiKey_D)) {
        if (m_PendingKeys == "d") {
            // dd - delete line
            auto [line, col] = state.buffer->PosToLineCol(state.cursorPos);
            size_t start = state.buffer->LineStart(line);
            size_t end = (line < state.buffer->LineCount() - 1) 
                ? state.buffer->LineStart(line + 1) 
                : state.buffer->Length();
            
            m_Register = state.buffer->Substring(start, end - start);
            m_RegisterIsLine = true;
            
            if (state.undoTree) {
                TextOps::Delete(*state.buffer, *state.undoTree, start, end - start, state.cursorPos);
            } else {
                state.buffer->Delete(start, end - start);
            }
            
            result.textChanged = true;
            result.newCursorPos = start;
            m_PendingKeys.clear();
            m_PendingOperator = VimOperator::None;
        } else {
            m_PendingOperator = VimOperator::Delete;
            m_PendingKeys = "d";
            m_State = VimState::Operator;
        }
        result.handled = true;
        ResetCount();
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_C)) {
        if (m_PendingKeys == "c") {
            // cc - change line
            auto [line, col] = state.buffer->PosToLineCol(state.cursorPos);
            size_t start = TextOps::FirstNonWhitespace(*state.buffer, state.cursorPos);
            size_t end = TextOps::LineEnd(*state.buffer, state.cursorPos);
            
            if (start < end) {
                m_Register = state.buffer->Substring(start, end - start);
                if (state.undoTree) {
                    TextOps::Delete(*state.buffer, *state.undoTree, start, end - start, state.cursorPos);
                } else {
                    state.buffer->Delete(start, end - start);
                }
                result.textChanged = true;
            }
            
            result.newCursorPos = start;
            EnterInsertMode(state, false);
            m_PendingKeys.clear();
            m_PendingOperator = VimOperator::None;
        } else {
            m_PendingOperator = VimOperator::Change;
            m_PendingKeys = "c";
            m_State = VimState::Operator;
        }
        result.handled = true;
        ResetCount();
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
        if (m_PendingKeys == "y") {
            // yy - yank line
            auto [line, col] = state.buffer->PosToLineCol(state.cursorPos);
            size_t start = state.buffer->LineStart(line);
            size_t end = (line < state.buffer->LineCount() - 1) 
                ? state.buffer->LineStart(line + 1) 
                : state.buffer->Length();
            
            m_Register = state.buffer->Substring(start, end - start);
            m_RegisterIsLine = true;
            m_PendingKeys.clear();
            m_PendingOperator = VimOperator::None;
        } else {
            m_PendingOperator = VimOperator::Yank;
            m_PendingKeys = "y";
            m_State = VimState::Operator;
        }
        result.handled = true;
        ResetCount();
        return result;
    }
    
    // Paste
    if (ImGui::IsKeyPressed(ImGuiKey_P)) {
        if (!m_Register.empty()) {
            size_t insertPos = state.cursorPos;
            if (m_RegisterIsLine) {
                // Paste line below
                size_t lineEnd = TextOps::LineEnd(*state.buffer, state.cursorPos);
                if (lineEnd < state.buffer->Length()) {
                    insertPos = lineEnd + 1;  // After newline
                } else {
                    // At end of file, add newline first
                    if (state.undoTree) {
                        TextOps::Insert(*state.buffer, *state.undoTree, lineEnd, "\n", state.cursorPos);
                    } else {
                        state.buffer->Insert(lineEnd, "\n");
                    }
                    insertPos = lineEnd + 1;
                }
            } else {
                insertPos = state.cursorPos + 1;
            }
            
            if (state.undoTree) {
                TextOps::Insert(*state.buffer, *state.undoTree, insertPos, m_Register, state.cursorPos);
            } else {
                state.buffer->Insert(insertPos, m_Register);
            }
            
            result.textChanged = true;
            result.newCursorPos = insertPos;
        }
        result.handled = true;
        ResetCount();
        return result;
    }
    
    // Undo/Redo
    if (ImGui::IsKeyPressed(ImGuiKey_U)) {
        if (state.undoTree) {
            auto newPos = TextOps::Undo(*state.buffer, *state.undoTree);
            if (newPos) {
                result.textChanged = true;
                result.newCursorPos = *newPos;
            }
        }
        result.handled = true;
        ResetCount();
        return result;
    }
    
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R)) {
        if (state.undoTree) {
            auto newPos = TextOps::Redo(*state.buffer, *state.undoTree);
            if (newPos) {
                result.textChanged = true;
                result.newCursorPos = *newPos;
            }
        }
        result.handled = true;
        ResetCount();
        return result;
    }
    
    // Go to line
    if (ImGui::IsKeyPressed(ImGuiKey_G)) {
        if (io.KeyShift) {  // G = go to end or line N
            size_t targetLine = m_HasCount ? static_cast<size_t>(m_Count - 1) : state.buffer->LineCount() - 1;
            targetLine = std::min(targetLine, state.buffer->LineCount() - 1);
            result.newCursorPos = state.buffer->LineStart(targetLine);
            result.cursorMoved = true;
        } else if (m_PendingKeys == "g") {  // gg = go to start
            size_t targetLine = m_HasCount ? static_cast<size_t>(m_Count - 1) : 0;
            targetLine = std::min(targetLine, state.buffer->LineCount() - 1);
            result.newCursorPos = state.buffer->LineStart(targetLine);
            result.cursorMoved = true;
            m_PendingKeys.clear();
        } else {
            m_PendingKeys = "g";
        }
        result.handled = true;
        ResetCount();
        return result;
    }
    
    // Search
    if (ImGui::IsKeyPressed(ImGuiKey_Slash)) {
        m_State = VimState::Search;
        m_SearchForward = true;
        m_CommandBuffer.clear();
        result.handled = true;
        ResetCount();
        return result;
    }
    
    // Command mode
    if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Semicolon)) {  // :
        m_State = VimState::Command;
        m_CommandBuffer.clear();
        result.handled = true;
        ResetCount();
        return result;
    }
    
    // x = delete char under cursor
    if (ImGui::IsKeyPressed(ImGuiKey_X)) {
        if (state.cursorPos < state.buffer->Length()) {
            char c = state.buffer->At(state.cursorPos);
            if (c != '\n') {
                m_Register = std::string(1, c);
                m_RegisterIsLine = false;
                if (state.undoTree) {
                    TextOps::Delete(*state.buffer, *state.undoTree, state.cursorPos, 1, state.cursorPos);
                } else {
                    state.buffer->Delete(state.cursorPos, 1);
                }
                result.textChanged = true;
            }
        }
        result.handled = true;
        ResetCount();
        return result;
    }
    
    // Matching bracket
    if (ImGui::IsKeyPressed(ImGuiKey_5) && io.KeyShift) {  // %
        size_t newPos = TextOps::MatchingBracket(*state.buffer, state.cursorPos);
        if (newPos != state.cursorPos) {
            result.newCursorPos = newPos;
            result.cursorMoved = true;
        }
        result.handled = true;
        ResetCount();
        return result;
    }
    
    return result;
}

InputResult VimMode::HandleInsertMode(EditorState& state) {
    InputResult result;
    
    // Escape to exit insert mode
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ExitToNormal(state);
        // Move cursor back one if not at start
        if (state.cursorPos > 0) {
            result.newCursorPos = state.cursorPos - 1;
        }
        result.handled = true;
        return result;
    }
    
    // Handle editing keys in insert mode
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        if (state.cursorPos > 0) {
            if (state.undoTree) {
                TextOps::Delete(*state.buffer, *state.undoTree, state.cursorPos - 1, 1, state.cursorPos);
            } else {
                state.buffer->Delete(state.cursorPos - 1, 1);
            }
            result.textChanged = true;
            result.newCursorPos = state.cursorPos - 1;
        }
        result.handled = true;
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        if (state.cursorPos < state.buffer->Length()) {
            if (state.undoTree) {
                TextOps::Delete(*state.buffer, *state.undoTree, state.cursorPos, 1, state.cursorPos);
            } else {
                state.buffer->Delete(state.cursorPos, 1);
            }
            result.textChanged = true;
        }
        result.handled = true;
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        if (state.undoTree) {
            TextOps::Insert(*state.buffer, *state.undoTree, state.cursorPos, "\n", state.cursorPos);
        } else {
            state.buffer->Insert(state.cursorPos, "\n");
        }
        result.textChanged = true;
        result.newCursorPos = state.cursorPos + 1;
        result.handled = true;
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        std::string tab = "    ";
        if (state.undoTree) {
            TextOps::Insert(*state.buffer, *state.undoTree, state.cursorPos, tab, state.cursorPos);
        } else {
            state.buffer->Insert(state.cursorPos, tab);
        }
        result.textChanged = true;
        result.newCursorPos = state.cursorPos + 4;
        result.handled = true;
        return result;
    }
    
    // Arrow keys for navigation in insert mode
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && state.cursorPos > 0) {
        result.newCursorPos = state.cursorPos - 1;
        result.cursorMoved = true;
        result.handled = true;
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && state.cursorPos < state.buffer->Length()) {
        result.newCursorPos = state.cursorPos + 1;
        result.cursorMoved = true;
        result.handled = true;
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        auto [line, col] = state.buffer->PosToLineCol(state.cursorPos);
        if (line > 0) {
            std::string prevLine = state.buffer->Line(line - 1);
            size_t newCol = std::min(col, prevLine.length());
            result.newCursorPos = state.buffer->LineColToPos(line - 1, newCol);
            result.cursorMoved = true;
        }
        result.handled = true;
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        auto [line, col] = state.buffer->PosToLineCol(state.cursorPos);
        if (line < state.buffer->LineCount() - 1) {
            std::string nextLine = state.buffer->Line(line + 1);
            size_t newCol = std::min(col, nextLine.length());
            result.newCursorPos = state.buffer->LineColToPos(line + 1, newCol);
            result.cursorMoved = true;
        }
        result.handled = true;
        return result;
    }
    
    return result;
}

InputResult VimMode::HandleVisualMode(EditorState& state) {
    InputResult result;
    
    // Escape to exit visual mode
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ExitToNormal(state);
        result.selectionChanged = true;
        result.newSelectionStart = state.cursorPos;
        result.newSelectionEnd = state.cursorPos;
        result.handled = true;
        return result;
    }
    
    // Handle motions (same as normal mode but extends selection)
    // Simplified - just handle basic motions
    size_t newPos = state.cursorPos;
    bool moved = false;
    
    if (ImGui::IsKeyPressed(ImGuiKey_H) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        if (newPos > 0) { --newPos; moved = true; }
    } else if (ImGui::IsKeyPressed(ImGuiKey_L) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        if (newPos < state.buffer->Length()) { ++newPos; moved = true; }
    } else if (ImGui::IsKeyPressed(ImGuiKey_J) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        auto [line, col] = state.buffer->PosToLineCol(state.cursorPos);
        if (line < state.buffer->LineCount() - 1) {
            std::string nextLine = state.buffer->Line(line + 1);
            size_t newCol = std::min(col, nextLine.length());
            newPos = state.buffer->LineColToPos(line + 1, newCol);
            moved = true;
        }
    } else if (ImGui::IsKeyPressed(ImGuiKey_K) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        auto [line, col] = state.buffer->PosToLineCol(state.cursorPos);
        if (line > 0) {
            std::string prevLine = state.buffer->Line(line - 1);
            size_t newCol = std::min(col, prevLine.length());
            newPos = state.buffer->LineColToPos(line - 1, newCol);
            moved = true;
        }
    } else if (ImGui::IsKeyPressed(ImGuiKey_W)) {
        newPos = TextOps::NextWord(*state.buffer, state.cursorPos);
        moved = true;
    } else if (ImGui::IsKeyPressed(ImGuiKey_B)) {
        newPos = TextOps::PrevWord(*state.buffer, state.cursorPos);
        moved = true;
    }
    
    if (moved) {
        result.handled = true;
        result.cursorMoved = true;
        result.newCursorPos = newPos;
        result.selectionChanged = true;
        result.newSelectionStart = m_VisualAnchor;
        result.newSelectionEnd = newPos;
        return result;
    }
    
    // Operators on selection
    if (ImGui::IsKeyPressed(ImGuiKey_D) || ImGui::IsKeyPressed(ImGuiKey_X)) {
        size_t start = std::min(m_VisualAnchor, state.cursorPos);
        size_t end = std::max(m_VisualAnchor, state.cursorPos);
        
        if (m_State == VimState::VisualLine) {
            auto [startLine, _1] = state.buffer->PosToLineCol(start);
            auto [endLine, _2] = state.buffer->PosToLineCol(end);
            start = state.buffer->LineStart(startLine);
            end = (endLine < state.buffer->LineCount() - 1) 
                ? state.buffer->LineStart(endLine + 1) 
                : state.buffer->Length();
            m_RegisterIsLine = true;
        } else {
            ++end;  // Include char under cursor
            m_RegisterIsLine = false;
        }
        
        m_Register = state.buffer->Substring(start, end - start);
        
        if (state.undoTree) {
            TextOps::Delete(*state.buffer, *state.undoTree, start, end - start, state.cursorPos);
        } else {
            state.buffer->Delete(start, end - start);
        }
        
        result.textChanged = true;
        result.newCursorPos = start;
        ExitToNormal(state);
        result.selectionChanged = true;
        result.newSelectionStart = start;
        result.newSelectionEnd = start;
        result.handled = true;
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
        size_t start = std::min(m_VisualAnchor, state.cursorPos);
        size_t end = std::max(m_VisualAnchor, state.cursorPos) + 1;
        
        if (m_State == VimState::VisualLine) {
            auto [startLine, _1] = state.buffer->PosToLineCol(start);
            auto [endLine, _2] = state.buffer->PosToLineCol(end);
            start = state.buffer->LineStart(startLine);
            end = (endLine < state.buffer->LineCount() - 1) 
                ? state.buffer->LineStart(endLine + 1) 
                : state.buffer->Length();
            m_RegisterIsLine = true;
        } else {
            m_RegisterIsLine = false;
        }
        
        m_Register = state.buffer->Substring(start, end - start);
        
        ExitToNormal(state);
        result.selectionChanged = true;
        result.newSelectionStart = state.cursorPos;
        result.newSelectionEnd = state.cursorPos;
        result.handled = true;
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_C)) {
        size_t start = std::min(m_VisualAnchor, state.cursorPos);
        size_t end = std::max(m_VisualAnchor, state.cursorPos) + 1;
        
        m_Register = state.buffer->Substring(start, end - start);
        m_RegisterIsLine = false;
        
        if (state.undoTree) {
            TextOps::Delete(*state.buffer, *state.undoTree, start, end - start, state.cursorPos);
        } else {
            state.buffer->Delete(start, end - start);
        }
        
        result.textChanged = true;
        result.newCursorPos = start;
        EnterInsertMode(state, false);
        result.selectionChanged = true;
        result.newSelectionStart = start;
        result.newSelectionEnd = start;
        result.handled = true;
        return result;
    }
    
    return result;
}

InputResult VimMode::HandleCommandMode(EditorState& state) {
    InputResult result;
    result.handled = true;
    
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ExitToNormal(state);
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        // Execute command
        if (m_CommandBuffer == "w") {
            // TODO: Save file
        } else if (m_CommandBuffer == "q") {
            // TODO: Quit
        } else if (m_CommandBuffer == "wq") {
            // TODO: Save and quit
        }
        
        ExitToNormal(state);
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        if (!m_CommandBuffer.empty()) {
            m_CommandBuffer.pop_back();
        } else {
            ExitToNormal(state);
        }
        return result;
    }
    
    return result;
}

InputResult VimMode::HandleSearchMode(EditorState& state) {
    InputResult result;
    result.handled = true;
    
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ExitToNormal(state);
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        // Execute search
        if (!m_CommandBuffer.empty()) {
            m_LastSearch = m_CommandBuffer;
            size_t found = TextOps::FindString(*state.buffer, state.cursorPos, m_LastSearch, m_SearchForward);
            if (found != state.cursorPos) {
                result.newCursorPos = found;
                result.cursorMoved = true;
            }
        }
        ExitToNormal(state);
        return result;
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        if (!m_CommandBuffer.empty()) {
            m_CommandBuffer.pop_back();
        } else {
            ExitToNormal(state);
        }
        return result;
    }
    
    return result;
}

InputResult VimMode::HandleOperatorPending(EditorState& state) {
    InputResult result;
    
    // Escape cancels operator
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_PendingOperator = VimOperator::None;
        m_PendingKeys.clear();
        m_State = VimState::Normal;
        result.handled = true;
        return result;
    }
    
    // Handle motion keys to complete the operator
    char motion = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_W)) motion = 'w';
    else if (ImGui::IsKeyPressed(ImGuiKey_B)) motion = 'b';
    else if (ImGui::IsKeyPressed(ImGuiKey_E)) motion = 'e';
    else if (ImGui::IsKeyPressed(ImGuiKey_0)) motion = '0';
    else if (ImGui::IsKeyPressed(ImGuiKey_4) && ImGui::GetIO().KeyShift) motion = '$';
    else if (ImGui::IsKeyPressed(ImGuiKey_6) && ImGui::GetIO().KeyShift) motion = '^';
    
    if (motion) {
        auto range = GetMotionRange(state, motion, GetCount());
        if (range) {
            ExecuteOperator(state, range->first, range->second);
            result.textChanged = (m_PendingOperator != VimOperator::Yank);
            result.newCursorPos = range->first;
        }
        
        m_PendingOperator = VimOperator::None;
        m_PendingKeys.clear();
        m_State = VimState::Normal;
        ResetCount();
        result.handled = true;
    }
    
    return result;
}

InputResult VimMode::HandleTextInput(EditorState& state, const ImWchar* chars, int count) {
    InputResult result;
    
    if (m_State == VimState::Insert) {
        std::string text;
        for (int i = 0; i < count; ++i) {
            ImWchar c = chars[i];
            if (c == '\r') continue;
            
            if (c < 0x80) {
                text += static_cast<char>(c);
            } else if (c < 0x800) {
                text += static_cast<char>(0xC0 | (c >> 6));
                text += static_cast<char>(0x80 | (c & 0x3F));
            } else {
                text += static_cast<char>(0xE0 | (c >> 12));
                text += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                text += static_cast<char>(0x80 | (c & 0x3F));
            }
        }
        
        if (!text.empty()) {
            if (state.undoTree) {
                TextOps::Insert(*state.buffer, *state.undoTree, state.cursorPos, text, state.cursorPos);
            } else {
                state.buffer->Insert(state.cursorPos, text);
            }
            result.handled = true;
            result.textChanged = true;
            result.newCursorPos = state.cursorPos + text.length();
        }
    } else if (m_State == VimState::Command || m_State == VimState::Search) {
        for (int i = 0; i < count; ++i) {
            if (chars[i] < 128) {
                m_CommandBuffer += static_cast<char>(chars[i]);
            }
        }
        result.handled = true;
    }
    
    return result;
}

InputResult VimMode::HandleMouse(EditorState& state, ImVec2 mousePos, bool clicked, bool dragging, bool released) {
    // Mouse handling is the same for all modes
    return {};
}

std::optional<std::pair<size_t, size_t>> VimMode::GetMotionRange(EditorState& state, char motion, int count) {
    size_t start = state.cursorPos;
    size_t end = start;
    
    switch (motion) {
        case 'w':
            for (int i = 0; i < count; ++i) {
                end = TextOps::NextWord(*state.buffer, end);
            }
            break;
        case 'b':
            end = start;
            for (int i = 0; i < count; ++i) {
                start = TextOps::PrevWord(*state.buffer, start);
            }
            std::swap(start, end);
            break;
        case 'e':
            for (int i = 0; i < count; ++i) {
                end = TextOps::WordEnd(*state.buffer, end);
            }
            ++end;  // Include the last char
            break;
        case '0':
            start = TextOps::LineStart(*state.buffer, state.cursorPos);
            end = state.cursorPos;
            break;
        case '$':
            end = TextOps::LineEnd(*state.buffer, state.cursorPos);
            break;
        case '^':
            start = TextOps::FirstNonWhitespace(*state.buffer, state.cursorPos);
            if (start > state.cursorPos) {
                std::swap(start, end);
            } else {
                end = state.cursorPos;
            }
            break;
        default:
            return std::nullopt;
    }
    
    if (start > end) std::swap(start, end);
    return std::make_pair(start, end);
}

void VimMode::ExecuteOperator(EditorState& state, size_t start, size_t end) {
    if (start >= end) return;
    
    switch (m_PendingOperator) {
        case VimOperator::Delete:
            m_Register = state.buffer->Substring(start, end - start);
            m_RegisterIsLine = false;
            if (state.undoTree) {
                TextOps::Delete(*state.buffer, *state.undoTree, start, end - start, state.cursorPos);
            } else {
                state.buffer->Delete(start, end - start);
            }
            break;
            
        case VimOperator::Change:
            m_Register = state.buffer->Substring(start, end - start);
            m_RegisterIsLine = false;
            if (state.undoTree) {
                TextOps::Delete(*state.buffer, *state.undoTree, start, end - start, state.cursorPos);
            } else {
                state.buffer->Delete(start, end - start);
            }
            EnterInsertMode(state, false);
            break;
            
        case VimOperator::Yank:
            m_Register = state.buffer->Substring(start, end - start);
            m_RegisterIsLine = false;
            break;
            
        default:
            break;
    }
}

void VimMode::EnterInsertMode(EditorState& state, bool afterCursor) {
    m_State = VimState::Insert;
}

void VimMode::EnterVisualMode(EditorState& state, VimState visualType) {
    m_State = visualType;
    m_VisualAnchor = state.cursorPos;
}

void VimMode::ExitToNormal(EditorState& state) {
    m_State = VimState::Normal;
    m_PendingOperator = VimOperator::None;
    m_PendingKeys.clear();
    m_CommandBuffer.clear();
    ResetCount();
}

void VimMode::AddCount(int digit) {
    m_Count = m_Count * 10 + digit;
    m_HasCount = true;
}

void VimMode::PushJump(size_t pos) {
    if (m_JumpIndex < m_JumpList.size()) {
        m_JumpList.resize(m_JumpIndex);
    }
    m_JumpList.push_back(pos);
    m_JumpIndex = m_JumpList.size();
}

std::optional<size_t> VimMode::JumpBack() {
    if (m_JumpIndex > 0) {
        --m_JumpIndex;
        return m_JumpList[m_JumpIndex];
    }
    return std::nullopt;
}

std::optional<size_t> VimMode::JumpForward() {
    if (m_JumpIndex < m_JumpList.size() - 1) {
        ++m_JumpIndex;
        return m_JumpList[m_JumpIndex];
    }
    return std::nullopt;
}

} // namespace sol
