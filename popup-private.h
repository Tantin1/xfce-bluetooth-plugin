/* popup-private.h — declaraciones internas compartidas entre los módulos
 * del popup (popup.c, popup-rows.c, popup-sections.c). NO incluir desde
 * plugin.c: la interfaz pública sigue siendo popup.h. */
#ifndef POPUP_PRIVATE_H
#define POPUP_PRIVATE_H

#include "popup.h"
#include "bt-obex.h"
#include <gdk/gdkkeysyms.h>
#include <glib/gi18n-lib.h>

/* Red de seguridad para operaciones que BlueZ nunca contesta (pasa cuando el
 * aparato desaparece a mitad del enlace). El pareo puede tardar lo que tarde
 * el usuario en tipear el PIN, así que es holgado a propósito. */
#define OP_TIMEOUT_MS 90000

/* Cooldown del botón Buscar entre reinicios del descubrimiento. */
#define SCAN_COOLDOWN_MS 5000

/* Tamaño del ícono de cada fila. */
#define ROW_ICON_SIZE 22

/* ================================================================
 * Operaciones en curso
 *
 * Igual que en el plugin de red, cuando el usuario aprieta Conectar,
 * Emparejar u Olvidar registramos la operación en popup->ops_in_progress
 * indexada por la ruta del dispositivo. La diferencia es que acá el
 * resultado llega por el callback de la llamada D-Bus, no por deducción
 * a partir del estado del aparato.
 * ================================================================ */

typedef enum {
    OP_CONNECT,
    OP_DISCONNECT,
    OP_PAIR,
    OP_REMOVE,
    OP_RECONNECT,           /* desconectar + pausa + conectar */
    OP_CONNECT_PROFILE,     /* un solo perfil: usa OpInProgress.uuid */
    OP_DISCONNECT_PROFILE
} OpKind;

typedef struct {
    OpKind        kind;
    gchar        *uuid;           /* solo para las operaciones de perfil */
    gchar        *device_path;
    gchar        *adapter_path;
    gchar        *name;
    GtkWidget    *expand_box;     /* expand a cerrar al confirmar (puede morir antes) */
    GtkWidget    *action_btn;     /* botón a restaurar si falla */
    gchar        *action_label;   /* texto original del botón */
    GSList       *extra_disabled; /* otros widgets deshabilitados a rehabilitar */
    guint         timeout_id;
    guint         serial;
    GCancellable *cancellable;
    BtPopup      *popup;
} OpInProgress;

/* Datos que viajan con el callback asíncrono. La validez del popup se
 * comprueba con una referencia débil a su ventana: si el plugin se sacó del
 * panel mientras la llamada estaba en vuelo, la referencia devuelve NULL y
 * el callback se retira sin tocar memoria liberada. */
typedef struct {
    GWeakRef  win_ref;
    BtPopup  *popup;
    gchar    *device_path;
    guint     serial;
} OpRef;

typedef struct {
    GDBusConnection *conn;
    gchar           *adapter_path;
} AdapterSwitchData;

typedef struct {
    BtPopup     *popup;
    GtkWidget   *expand_box;
    gchar       *device_path;
    gchar       *adapter_path;
    gchar       *address;
    gchar       *name;
    gboolean     paired;
    gboolean     connected;
    gboolean     trusted;
    gboolean     blocked;

    GtkWidget   *action_row;        /* fila con los botones principales */
    GtkWidget   *primary_btn;       /* Conectar / Desconectar / Emparejar */
    GtkWidget   *forget_btn;        /* Olvidar */
    GtkWidget   *reconnect_btn;     /* Reconectar, solo si está conectado */
    GtkWidget   *send_btn;          /* "Enviar archivo…", solo si el aparato
                                     * anuncia que sabe recibirlos */
    GtkWidget   *trust_check;       /* "Conectar automáticamente" (Trusted) */
    gulong       trust_handler;
    GtkWidget   *confirm_box;       /* "¿Olvidar este dispositivo?" */
    GtkWidget   *details_btn;
    GtkWidget   *details_box;
    GtkWidget   *details_arrow;
    GtkWidget   *row_box;           /* event_box de la fila, para el realce */
} RowData;

/* ---- popup.c (núcleo: ventana, operaciones, helpers) ---- */
gboolean  deferred_refresh_cb        (gpointer user_data);
void      op_start                   (BtPopup *popup, OpKind kind,
                                      const gchar *device_path,
                                      const gchar *adapter_path,
                                      const gchar *name,
                                      GtkWidget *expand_box,
                                      GtkWidget *action_btn,
                                      const gchar *action_label,
                                      GSList *extra_disabled);
/* Igual que op_start, para OP_CONNECT_PROFILE y OP_DISCONNECT_PROFILE. */
void      op_start_profile           (BtPopup *popup, OpKind kind,
                                      const gchar *device_path,
                                      const gchar *adapter_path,
                                      const gchar *name,
                                      const gchar *uuid,
                                      GtkWidget *expand_box,
                                      GtkWidget *action_btn,
                                      const gchar *action_label,
                                      GSList *extra_disabled);
gboolean  op_running_for             (BtPopup *popup, const gchar *device_path);
void      popup_set_error            (BtPopup *popup, const gchar *device_path,
                                      const gchar *message);
const gchar *popup_get_error         (BtPopup *popup, const gchar *device_path);
void      popup_clear_error          (BtPopup *popup, const gchar *device_path);
gboolean  on_bt_switch_toggled       (GtkSwitch *sw, gboolean state, gpointer user_data);
gboolean  on_discoverable_toggled    (GtkSwitch *sw, gboolean state, gpointer user_data);
void      adapter_switch_data_free   (AdapterSwitchData *d);
gboolean  on_adapter_switch_toggled  (GtkSwitch *sw, gboolean state, gpointer user_data);
void      on_refresh_clicked         (GtkWidget *btn, BtPopup *popup);
void      on_advanced_clicked        (GtkWidget *btn, gpointer user_data);
void      popup_send_file_flow       (BtPopup *popup, const gchar *address,
                                      const gchar *name);

/* ---- popup-rows.c (fila de dispositivo, expand y sus callbacks) ---- */
gchar     *row_fingerprint       (BtPopup *popup, BtDevice *dev);
GtkWidget *make_device_row       (BtDevice *dev, BtPopup *popup);
void       row_update_in_place   (GtkWidget *event_box, BtDevice *dev);
void       reopen_expand_for_device (BtPopup *popup, const gchar *device_path);
void       clear_open_highlight  (BtPopup *popup);

/* ---- popup-sections.c (secciones, refresco y reconstrucción) ---- */
void  rebuild_ui              (BtPopup *popup);
void  update_top_status       (BtPopup *popup);
void  update_adapters_section (BtPopup *popup);
void  schedule_refresh_ui     (BtPopup *popup);
void  on_bt_signal_popup      (gpointer user_data);

#endif /* POPUP_PRIVATE_H */
