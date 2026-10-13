// Diálogo del sistema en pantalla (sceUtilityMsgDialog), compartido entre el HLE, que lo abre, lee los
// mandos y devuelve la respuesta al juego, y el overlay, que lo dibuja y recoge el ratón.
#pragma once
#include <string>

struct SystemDialog {
    bool visible = false;
    std::string text;                    // UTF-8
    bool yesno = false;                  // botones Sí/No
    bool ok = false;                     // botón Aceptar
    bool cancel = false;                 // se puede cerrar con ○ (Atrás)
    int choice = 1;                      // con Sí/No: 1 = Sí, 0 = No
    std::string ok_label, cancel_label;  // textos propios del juego (vacíos = los del sistema)
    int clicked = -1;                    // respuesta con ratón o teclado: 1 Sí/Aceptar, 2 No, 3 Atrás
};

extern SystemDialog g_dialog;
