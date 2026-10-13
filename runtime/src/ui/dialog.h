// Diálogo del sistema en pantalla, compartido entre el HLE, que lo abre, lee los mandos y devuelve la
// respuesta al juego, y el overlay, que lo dibuja y recoge el ratón. Lo usan los mensajes
// (sceUtilityMsgDialog) y la pantalla de partidas guardadas (sceUtilitySavedata: Guardar/Cargar).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct SystemDialog {
    bool visible = false;
    std::string text;                    // UTF-8
    bool yesno = false;                  // botones Sí/No
    bool ok = false;                     // botón Aceptar
    bool cancel = false;                 // se puede cerrar con ○ (Atrás)
    int choice = 1;                      // con Sí/No: 1 = Sí, 0 = No
    std::string ok_label, cancel_label;  // textos propios del juego (vacíos = los del sistema)
    int clicked = -1;                    // respuesta con ratón o teclado: 1 Sí/Aceptar, 2 No, 3 Atrás

    // Pantalla de partidas: con banner ("Guardar"/"Cargar") el mensaje va a la derecha, con el icono
    // de la partida y sus datos a la izquierda.
    std::string banner;
    std::vector<uint8_t> icon_png;  // ICON0.PNG de la partida (vacío = sin icono)
    int icon_serial = 0;            // cambia cada vez que cambia el icono
    bool icon_dim = false;          // partida que aún no existe: icono atenuado
    std::string info;               // título de la partida, fecha y tamaño
    bool busy = false;              // guardando o cargando: sin botones
};

extern SystemDialog g_dialog;

// Respuesta automática sin ventana o con CARS_DIALOG_ANSWER=yes|no: 1 = Sí, 2 = No; -1 = preguntar.
int dialog_auto_answer();
