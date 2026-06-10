#pragma once
#include <QKeyEvent>
#include <SDL/SDL_keysym.h>

inline int qtKeyToSDLKeysym(int qtKey) {
    switch (qtKey) {
        case Qt::Key_Left:      return SDLK_LEFT;
        case Qt::Key_Right:     return SDLK_RIGHT;
        case Qt::Key_Up:        return SDLK_UP;
        case Qt::Key_Down:      return SDLK_DOWN;
        case Qt::Key_PageUp:    return SDLK_PAGEUP;
        case Qt::Key_PageDown:  return SDLK_PAGEDOWN;
        case Qt::Key_Home:      return SDLK_HOME;
        case Qt::Key_End:       return SDLK_END;
        case Qt::Key_Insert:    return SDLK_INSERT;
        case Qt::Key_Delete:    return SDLK_DELETE;
        case Qt::Key_Backspace: return SDLK_BACKSPACE;
        case Qt::Key_Return:    return SDLK_RETURN;
        case Qt::Key_Enter:     return SDLK_RETURN;
        case Qt::Key_Escape:    return SDLK_ESCAPE;
        case Qt::Key_Tab:       return SDLK_TAB;
        case Qt::Key_Backtab:   return SDLK_TAB;
        case Qt::Key_Space:     return SDLK_SPACE;
        case Qt::Key_F1:        return SDLK_F1;
        case Qt::Key_F2:        return SDLK_F2;
        case Qt::Key_F3:        return SDLK_F3;
        case Qt::Key_F4:        return SDLK_F4;
        case Qt::Key_F5:        return SDLK_F5;
        case Qt::Key_F6:        return SDLK_F6;
        case Qt::Key_F7:        return SDLK_F7;
        case Qt::Key_F8:        return SDLK_F8;
        case Qt::Key_F9:        return SDLK_F9;
        case Qt::Key_F10:       return SDLK_F10;
        case Qt::Key_F11:       return SDLK_F11;
        case Qt::Key_F12:       return SDLK_F12;
    }
    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) return 'a' + (qtKey - Qt::Key_A);
    if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9) return '0' + (qtKey - Qt::Key_0);
    switch (qtKey) {
        case Qt::Key_Comma:        return SDLK_COMMA;
        case Qt::Key_Period:       return SDLK_PERIOD;
        case Qt::Key_Semicolon:    return SDLK_SEMICOLON;
        case Qt::Key_Colon:        return SDLK_COLON;
        case Qt::Key_Less:         return SDLK_LESS;
        case Qt::Key_Greater:      return SDLK_GREATER;
        case Qt::Key_BracketLeft:  return SDLK_LEFTBRACKET;
        case Qt::Key_BracketRight: return SDLK_RIGHTBRACKET;
        case Qt::Key_ParenLeft:    return SDLK_LEFTPAREN;
        case Qt::Key_ParenRight:   return SDLK_RIGHTPAREN;
        case Qt::Key_Minus:        return SDLK_MINUS;
        case Qt::Key_Plus:         return SDLK_PLUS;
        case Qt::Key_Equal:        return SDLK_EQUALS;
        case Qt::Key_Slash:        return SDLK_SLASH;
        case Qt::Key_Asterisk:     return SDLK_ASTERISK;
        case Qt::Key_QuoteLeft:    return SDLK_BACKQUOTE;
    }
    return 0;
}

extern "C" {
extern int key, rawkey, shiftpressed;
}

inline void setGoatKeys(QKeyEvent *e) {
    int sdlKey = qtKeyToSDLKeysym(e->key());
    QString txt = e->text();
    int asciiKey = 0;
    if (!txt.isEmpty()) {
        ushort u = txt.at(0).unicode();
        if (u < 128) asciiKey = u;
    }
    bool shiftLike = (e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)) != 0;
    rawkey = sdlKey;
    key = asciiKey;
    shiftpressed = shiftLike ? 1 : 0;
}

inline void clearGoatKeys() {
    rawkey = 0;
    key = 0;
    shiftpressed = 0;
}

// Map an X11 / xkb keycode (Linux nativeScanCode) for a key in the QWERTY
// note-input layout (bottom row Z..M + S D G H J sharps, top row Q..U + 2
// 3 5 6 7 sharps) to the corresponding SDL keysym. Returns 0 if the
// scancode isn't a known note position. Windows / macOS use different
// scan-code numbering; only Linux is mapped for now (the user reported
// the issue on Linux + alternative layout).
inline int qwertyScancodeToNoteSDL(int scancode) {
    switch (scancode) {
    // Bottom row: Z X C V B N M
    case 52: return SDLK_z; case 53: return SDLK_x; case 54: return SDLK_c;
    case 55: return SDLK_v; case 56: return SDLK_b; case 57: return SDLK_n;
    case 58: return SDLK_m;
    // Sharps on home row: S D F G H J K L
    case 39: return SDLK_s; case 40: return SDLK_d; case 41: return SDLK_f;
    case 42: return SDLK_g; case 43: return SDLK_h; case 44: return SDLK_j;
    case 45: return SDLK_k; case 46: return SDLK_l;
    // Top row: Q W E R T Y U I O P
    case 24: return SDLK_q; case 25: return SDLK_w; case 26: return SDLK_e;
    case 27: return SDLK_r; case 28: return SDLK_t; case 29: return SDLK_y;
    case 30: return SDLK_u; case 31: return SDLK_i; case 32: return SDLK_o;
    case 33: return SDLK_p;
    // Number row sharps: 1 2 3 4 5 6 7 8 9 0
    case 10: return SDLK_1; case 11: return SDLK_2; case 12: return SDLK_3;
    case 13: return SDLK_4; case 14: return SDLK_5; case 15: return SDLK_6;
    case 16: return SDLK_7; case 17: return SDLK_8; case 18: return SDLK_9;
    case 19: return SDLK_0;
    default: return 0;
    }
}
