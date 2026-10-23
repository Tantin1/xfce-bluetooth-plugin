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

    /* TRUE mientras corre popup_hide. En Wayland, soltar el acaparamiento
     * puede disparar "grab-broken-event" en el acto, y ese manejador vuelve
     * a llamar a popup_hide: la marca corta esa vuelta. */
    gboolean   hiding;
} BtPopup;

/* Ícono de un dispositivo, con cadena de respaldo hasta el genérico. */
GtkWidget *make_device_icon (const gchar *icon_name, gint icon_size);

/* Controlado desde el popup para mostrar/ocultar el spinner del panel. */
void bt_plugin_set_busy (gpointer bp_ptr, gboolean busy);

BtPopup *popup_create  (XfcePanelPlugin *plugin, GtkWidget *button);
void     popup_show    (BtPopup *popup, XfcePanelPlugin *plugin,
                        GtkWidget *button, GDBusConnection *conn,
                        gint popup_width, gint popup_height);
void     popup_hide    (BtPopup *popup);
void     popup_destroy (BtPopup *popup);

/* Para los carteles que aparecen solos (pareo, recepción): en Wayland la
 * ventana emergente abierta se queda con el teclado y el cartel no recibe lo
 * que se escribe, así que hay que cerrarla antes. En X11 no hace nada: ahí
 * alcanza con soltar el acaparamiento, como siempre. */
void     popup_close_for_dialog (void);

#endif /* POPUP_H */
