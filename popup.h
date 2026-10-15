#ifndef POPUP_H
#define POPUP_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include "bt-dbus.h"

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

#endif /* POPUP_H */
