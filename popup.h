#ifndef POPUP_H
#define POPUP_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include "bt-dbus.h"

/* Mensajes de diagnóstico de Wayland. Silenciosos por omisión; se ven
 * lanzando el panel con G_MESSAGES_DEBUG=xfce-bluetooth-plugin. Usan su
 * propio dominio para no mezclarse con los de los otros plugins, que en
 * Wayland viven en el mismo proceso. */
#define WL_DEBUG(...) g_log ("xfce-bluetooth-plugin", G_LOG_LEVEL_DEBUG, __VA_ARGS__)

/* Estilo de la ventana emergente (opción de Propiedades). Los números se
 * guardan en la configuración: no reordenar. */
typedef enum {
    BT_POPUP_STYLE_AUTO     = 0,  /* flotante y redondeada si el tema redondea */
    BT_POPUP_STYLE_FLOATING = 1,  /* siempre despegada, esquinas del tema */
    BT_POPUP_STYLE_ATTACHED = 2   /* esquinas rectas, pegada al panel */
} BtPopupStyle;

typedef struct {
    GtkWidget *window;
    GtkWidget *top_box;
    GtkWidget *content_box;
    GtkWidget *scroll;
    gulong     press_handler;
    GtkWidget *current_expand_box;
    gint       popup_width;
    gint       popup_height;
    GtkWidget *button;
    GSList    *adapter_switches;   /* GtkSwitch* de cada adaptador */

    /* Conexión DBus prestada (no liberar). */
    GDBusConnection *conn;

    /* Construcción perezosa: TRUE si la UI ya fue armada al menos una vez. */
    gboolean   ui_built;

    /* Estado superior: ícono, label y switch global. Se actualizan in-place. */
    GtkWidget *status_icon;
    GtkWidget *status_label;
    GtkWidget *bt_switch;
    gulong     bt_switch_handler;

    /* Botón "Buscar" + su label, para cambiar el texto sin reconstruir. */
    GtkWidget *refresh_button;
    GtkWidget *refresh_label;

    /* Switch "Visible para otros dispositivos" de la zona fija inferior. */
    GtkWidget *discoverable_switch;
    gulong     discoverable_handler;

    /* IDs de suscripción a señales DBus de BlueZ. */
    guint     *signal_ids;

    /* Operaciones en curso, indexadas por ruta de objeto del dispositivo.
     * A diferencia del plugin de red no hace falta adivinar el resultado:
     * BlueZ contesta, así que cada entrada se resuelve en su callback. */
    GHashTable *ops_in_progress;
    guint       op_serial;         /* discrimina operaciones recicladas */

    /* Rutas de dispositivos cuya última operación falló → mensaje de error
     * a mostrar dentro del expand. Se limpia al conectar bien o al olvidar. */
    GHashTable *failed_devices;

    /* Estado del descubrimiento. */
    gboolean   scanning;
    gboolean   discovery_active;   /* TRUE si nosotros lo arrancamos */
    guint      scan_timeout_id;

    gboolean   show_separators;
    gboolean   show_unnamed;       /* mostrar aparatos que solo informan MAC */

    /* Puntero opaco al BtPlugin del panel. Usado para el spinner del botón. */
    gpointer   plugin_ref;

    /* Cerrado para el programa pero todavía a la vista (fundido de salida).
     * Va primero en el cierre lógico: en Wayland, soltar el acaparamiento
     * puede disparar "grab-broken-event" en el acto, y soltar el botón del
     * panel puede volver a entrar al cierre; con la marca puesta no pasa
     * nada. Mientras está puesta, la ventana no ataja clics ni teclas. */
    gboolean   closing;
    guint      close_tick_id;     /* función por cuadro del fundido de salida */
    guint      close_timeout_id;  /* tope: termina aunque no lleguen cuadros */
    gint64     close_start;       /* momento del primer cuadro (0 = todavía no) */
    gdouble    close_from;        /* opacidad con la que arrancó */
    /* Fundido de entrada (popup_fade_begin): función por cuadro que sube la
     * opacidad del marco, y el momento del primer cuadro (0 = todavía no). */
    guint      fade_tick_id;
    gint64     fade_start;

    /* Aspecto del tema (ver popup_update_rounding en popup.c). frame: caja
     * con nombre de componente "popover" que contiene todo; el tema le pone
     * fondo, borde, esquinas y colores de letra. rounded: TRUE si la ventana
     * tiene transparencia y se ven las esquinas redondeadas. outer_box: la
     * caja de adentro. composited_handler: aviso de que el compositor se
     * prendió o se apagó. */
    GtkWidget *frame;              /* marco visible, componente "menu" */
    GtkWidget *frame_popover;      /* "popover" que nunca se muestra: radio del tema */
    GtkWidget *outer_box;
    gboolean   rounded;
    gulong     composited_handler;
    gint       style;              /* BtPopupStyle, lo fija el plugin */
    gboolean   floating;           /* decidido al abrir (popup_update_rounding) */

    /* Zona fija de abajo ("Visible para otros dispositivos" y "Configuración
     * avanzada"), en su propia caja para poder rearmarla tras un cambio de
     * tema (ver on_theme_changed en popup.c). */
    GtkWidget *bottom_box;

    /* X11: tamaño con el que se ubicó la ventana y el plugin, para volver a
     * ubicarla si al aparecer mide otra cosa (ver on_popup_size_allocate). */
    gpointer   panel_plugin;
    gint       placed_w, placed_h;

    /* El tema cambió con la ventana cerrada: rearmar antes de la próxima
     * apertura. La configuración de GTK vive más que el popup: el aviso se
     * desconecta al destruirlo. */
    gboolean     theme_dirty;
    GtkSettings *theme_settings;
    gulong       theme_handler;
} BtPopup;

/* Ícono de un dispositivo, con cadena de respaldo hasta el genérico. */
GtkWidget *make_device_icon (const gchar *icon_name, gint icon_size);

/* Controlado desde el popup para mostrar/ocultar el spinner del panel. */
void bt_plugin_set_busy (gpointer bp_ptr, gboolean busy);
/* El popup quedó cerrado para el programa: soltar el botón del panel. Se
 * llama al empezar el cierre, no al ocultar la ventana (la define plugin.c). */
void bt_plugin_popup_closed (gpointer bp_ptr);

BtPopup *popup_create  (XfcePanelPlugin *plugin, GtkWidget *button);
void     popup_show    (BtPopup *popup, XfcePanelPlugin *plugin,
                        GtkWidget *button, GDBusConnection *conn,
                        gint popup_width, gint popup_height);
/* popup_hide: cierre de golpe (cierres automáticos, cuando enseguida se
 * abre otra ventana, el compositor en Wayland). Si había un fundido de
 * salida en curso, lo termina.
 * popup_close: lo que cierra el usuario (Escape, clic afuera, botón del
 * panel, pérdida de foco): con fundido si se puede, si no como popup_hide.
 * popup_is_active: visible y no desvaneciéndose. Para preguntar «¿el menú
 * está abierto?»; para «¿la ventana está en pantalla?» sigue valiendo
 * gtk_widget_get_visible. */
void     popup_hide      (BtPopup *popup);
void     popup_close     (BtPopup *popup);
gboolean popup_is_active (BtPopup *popup);
void     popup_destroy   (BtPopup *popup);

/* Para los carteles que aparecen solos (pareo, recepción): en Wayland la
 * ventana emergente abierta se queda con el teclado y el cartel no recibe lo
 * que se escribe, así que hay que cerrarla antes. En X11 no hace nada: ahí
 * alcanza con soltar el acaparamiento, como siempre. */
void     popup_close_for_dialog (void);

#endif /* POPUP_H */
