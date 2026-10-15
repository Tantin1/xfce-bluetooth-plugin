/* popup.c — núcleo del popup: ventana (crear/mostrar/ocultar/destruir),
 * posicionamiento y grab, operaciones en curso, helpers compartidos y
 * manejadores de la ventana. La fila de dispositivo vive en popup-rows.c;
 * las secciones y el refresco, en popup-sections.c; lo compartido se declara
 * en popup-private.h. */

#include "popup-private.h"

typedef struct {
    BtPopup   *popup;
    GtkWidget *button;
} GrabData;

static gboolean on_button_press (GtkWidget *widget, GdkEventButton *event, BtPopup *popup);
static gboolean on_focus_out    (GtkWidget *widget, GdkEventFocus *ev, BtPopup *popup);
static void     op_free         (gpointer p);

/* Callback de temporizador que agenda un refresco de UI y termina. Existe
 * para no castear schedule_refresh_ui a GSourceFunc: no devuelve nada, así
 * que el valor de retorno quedaría en basura y la fuente podría repetirse
 * para siempre. */
gboolean
deferred_refresh_cb (gpointer user_data)
{
    schedule_refresh_ui ((BtPopup *) user_data);
    return G_SOURCE_REMOVE;
}

/* ---------- grab diferido al mapear la ventana ---------- */

static gboolean
on_window_mapped (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    (void) event;
    GrabData   *gd      = user_data;
    BtPopup    *popup   = gd->popup;
    GdkDisplay *display = gtk_widget_get_display (widget);
    GdkSeat    *seat    = gdk_display_get_default_seat (display);

    /* Desconectarse enseguida: solo se ejecuta una vez por apertura. */
    g_signal_handlers_disconnect_by_func (widget, on_window_mapped, gd);

    GdkGrabStatus grab_status = gdk_seat_grab (
            seat, gtk_widget_get_window (widget),
            GDK_SEAT_CAPABILITY_ALL, TRUE, NULL, NULL, NULL, NULL);

    if (grab_status == GDK_GRAB_SUCCESS) {
        popup->press_handler =
            g_signal_connect (popup->window, "button-press-event",
                              G_CALLBACK (on_button_press), popup);
    } else {
        popup->press_handler =
            g_signal_connect (popup->window, "focus-out-event",
                              G_CALLBACK (on_focus_out), popup);
    }

    g_free (gd);
    return FALSE;   /* no consumir el evento */
}

/* ---------- posicionamiento ---------- */

static void
position_popup (BtPopup *popup, GtkWidget *button)
{
    GdkWindow      *gdk_win;
    GdkMonitor     *monitor;
    GdkRectangle    workarea;
    GtkRequisition  preferred;
    gint bx, by, bw, bh, pw, ph, x, y;

    gdk_win = gtk_widget_get_window (button);
    if (!gdk_win)
        return;
    gdk_window_get_origin (gdk_win, &bx, &by);
    bw = gtk_widget_get_allocated_width  (button);
    bh = gtk_widget_get_allocated_height (button);

    gtk_widget_get_preferred_size (popup->window, NULL, &preferred);
    pw = preferred.width;
    ph = preferred.height;

    monitor = gdk_display_get_monitor_at_point (
                  gtk_widget_get_display (button), bx + bw / 2, by + bh / 2);
    if (!monitor)
        return;
    gdk_monitor_get_workarea (monitor, &workarea);

    x = bx;
    y = by + bh;

    if (x + pw > workarea.x + workarea.width)
        x = workarea.x + workarea.width - pw;
    if (x < workarea.x)
        x = workarea.x;

    if (y + ph > workarea.y + workarea.height)
        y = by - ph;
    if (y < workarea.y)
        y = workarea.y;

    gtk_window_move (GTK_WINDOW (popup->window), x, y);
}

/* ---------- tabla de errores por dispositivo ---------- */

void
popup_set_error (BtPopup *popup, const gchar *device_path, const gchar *message)
{
    if (!popup || !popup->failed_devices || !device_path)
        return;
    g_hash_table_replace (popup->failed_devices,
                          g_strdup (device_path), g_strdup (message));
}

const gchar *
popup_get_error (BtPopup *popup, const gchar *device_path)
{
    if (!popup || !popup->failed_devices || !device_path)
        return NULL;
    return g_hash_table_lookup (popup->failed_devices, device_path);
}

void
popup_clear_error (BtPopup *popup, const gchar *device_path)
{
    if (!popup || !popup->failed_devices || !device_path)
        return;
    g_hash_table_remove (popup->failed_devices, device_path);
}

/* ---------- operaciones en curso ---------- */

static void
op_free (gpointer p)
{
    OpInProgress *op = p;
    if (!op) return;
    if (op->timeout_id) {
        g_source_remove (op->timeout_id);
        op->timeout_id = 0;
    }
    if (op->cancellable) {
        g_cancellable_cancel (op->cancellable);
        g_object_unref (op->cancellable);
    }
    g_free (op->uuid);
    g_free (op->device_path);
    g_free (op->adapter_path);
    g_free (op->name);
    g_free (op->action_label);
    g_slist_free (op->extra_disabled);
    g_free (op);
}

gboolean
op_running_for (BtPopup *popup, const gchar *device_path)
{
    if (!popup || !popup->ops_in_progress || !device_path)
        return FALSE;
    return g_hash_table_contains (popup->ops_in_progress, device_path);
}

/* Devuelve los widgets del expand a su estado utilizable. */
static void
op_restore_widgets (OpInProgress *op)
{
    if (op->action_btn && GTK_IS_BUTTON (op->action_btn)) {
        gtk_widget_set_sensitive (op->action_btn, TRUE);
        if (op->action_label)
            gtk_button_set_label (GTK_BUTTON (op->action_btn), op->action_label);
    }
    for (GSList *l = op->extra_disabled; l; l = l->next) {
        GtkWidget *w = l->data;
        if (w && GTK_IS_WIDGET (w))
            gtk_widget_set_sensitive (w, TRUE);
    }
}

/* Muestra (o crea) el label de error dentro del expand abierto. */
static void
op_show_error_inline (OpInProgress *op, const gchar *message)
{
    if (!op->expand_box || !GTK_IS_WIDGET (op->expand_box))
        return;

    GtkWidget *err_label = g_object_get_data (G_OBJECT (op->expand_box),
                                              "error-label");
    if (!err_label) {
        err_label = gtk_label_new (NULL);
        gtk_style_context_add_class (gtk_widget_get_style_context (err_label),
                                     "error");
        gtk_label_set_xalign    (GTK_LABEL (err_label), 0.0);
        gtk_label_set_line_wrap (GTK_LABEL (err_label), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (err_label), 34);
        gtk_box_pack_start (GTK_BOX (op->expand_box), err_label, FALSE, FALSE, 0);
        gtk_box_reorder_child (GTK_BOX (op->expand_box), err_label, 0);
        g_object_set_data (G_OBJECT (op->expand_box), "error-label", err_label);
    }
    gtk_label_set_text (GTK_LABEL (err_label), message);
    gtk_widget_show (err_label);
}

static void
op_remove (BtPopup *popup, const gchar *device_path)
{
    g_hash_table_remove (popup->ops_in_progress, device_path);

    /* Apagar el spinner del panel si ya no queda nada en curso. */
    if (popup->plugin_ref &&
        g_hash_table_size (popup->ops_in_progress) == 0)
        bt_plugin_set_busy (popup->plugin_ref, FALSE);
}

static void op_launch (OpInProgress *op);

/* Callback de resultado de BlueZ. Acá está la diferencia grande con el
 * plugin de red: no hay que inferir nada del estado del aparato, la llamada
 * devuelve éxito o el error concreto. */
static void
on_op_result (gboolean success, const gchar *error_name,
              const gchar *error_message, gpointer user_data)
{
    OpRef *ref = user_data;

    /* ¿Sigue viva la ventana del popup? Si el plugin se sacó del panel
     * mientras la llamada estaba en vuelo, acá salimos sin tocar nada. */
    GObject *win = g_weak_ref_get (&ref->win_ref);
    if (!win) {
        g_weak_ref_clear (&ref->win_ref);
        g_free (ref->device_path);
        g_free (ref);
        return;
    }

    BtPopup      *popup = ref->popup;
    OpInProgress *op    = g_hash_table_lookup (popup->ops_in_progress,
                                               ref->device_path);

    /* La operación ya no está (popup cerrado, fila destruida) o fue
     * reemplazada por otra más nueva: este resultado es basura vieja. */
    if (!op || op->serial != ref->serial) {
        g_object_unref (win);
        g_weak_ref_clear (&ref->win_ref);
        g_free (ref->device_path);
        g_free (ref);
        return;
    }

    gboolean cancelled = op->cancellable &&
                         g_cancellable_is_cancelled (op->cancellable);

    if (success) {
        popup_clear_error (popup, op->device_path);

        if (op->kind == OP_PAIR) {
            /* Emparejado: lo marcamos de confianza (equivale al "conectar
             * automáticamente" del plugin de red) y encadenamos la conexión,
             * que es lo que el usuario realmente quería. */
            gchar     *dev_path = g_strdup (op->device_path);
            gchar     *adp_path = g_strdup (op->adapter_path);
            gchar     *name     = g_strdup (op->name);
            GtkWidget *expand   = op->expand_box;
            GtkWidget *btn      = op->action_btn;
            gchar     *label    = g_strdup (op->action_label);

            bt_device_set_trusted (popup->conn, dev_path, TRUE);
            op_remove (popup, dev_path);
            op_start (popup, OP_CONNECT, dev_path, adp_path, name,
                      expand, btn, label, NULL);

            g_free (label); g_free (name);
            g_free (adp_path); g_free (dev_path);
        } else {
            /* Conectar / desconectar / olvidar: cerramos el expand y dejamos
             * que el refresco redibuje la fila con el estado nuevo. */
            if (op->expand_box && GTK_IS_WIDGET (op->expand_box)) {
                gtk_widget_hide (op->expand_box);
                if (popup->current_expand_box == op->expand_box)
                    popup->current_expand_box = NULL;
            }
            clear_open_highlight (popup);
            op_remove (popup, ref->device_path);
            schedule_refresh_ui (popup);
        }
    } else {
        const gchar *msg = cancelled
            ? _("The device did not answer. Make sure it is on and nearby.")
            : bt_error_to_message (error_name, error_message);

        /* Con un perfil puntual, "no disponible" no quiere decir que el
         * aparato no lo tenga (si no lo tuviera, ni se ofrecería): quiere
         * decir que ESTE equipo no tiene registrado quién lo atienda. En
         * audio eso es el servidor de sonido. El mensaje genérico ("el
         * aparato no admite esta operación") mandaría a buscar mal. */
        if (!cancelled &&
            (op->kind == OP_CONNECT_PROFILE ||
             op->kind == OP_DISCONNECT_PROFILE) &&
            error_name && g_str_has_suffix (error_name, ".NotAvailable"))
            msg = _("This computer has no service for that profile. "
                    "For audio profiles, the sound server provides it.");

        popup_set_error (popup, op->device_path, msg);
        op_restore_widgets (op);
        op_show_error_inline (op, msg);
        op_remove (popup, ref->device_path);
        /* Sin schedule_refresh_ui: la fila no cambió de estado y el expand
         * sigue abierto con el mensaje. update_adapters_section tampoco
         * reconstruiría (regla de congelar filas con expand abierto). */
        update_top_status (popup);
    }

    g_object_unref (win);
    g_weak_ref_clear (&ref->win_ref);
    g_free (ref->device_path);
    g_free (ref);
}

/* Red de seguridad: BlueZ normalmente contesta, pero si el aparato se apaga
 * a mitad del enlace la llamada puede quedar colgada. Cancelamos: eso hace
 * que on_op_result entre por la rama de error. */
static gboolean
op_timeout_cb (gpointer user_data)
{
    OpInProgress *op = user_data;
    op->timeout_id = 0;
    if (op->cancellable)
        g_cancellable_cancel (op->cancellable);
    return G_SOURCE_REMOVE;
}

static void
op_launch (OpInProgress *op)
{
    BtPopup *popup = op->popup;

    OpRef *ref = g_new0 (OpRef, 1);
    g_weak_ref_init (&ref->win_ref, popup->window);
    ref->popup       = popup;
    ref->device_path = g_strdup (op->device_path);
    ref->serial      = op->serial;

    switch (op->kind) {
        case OP_CONNECT:
            bt_device_connect_async (popup->conn, op->device_path,
                                     op->cancellable, on_op_result, ref);
            break;
        case OP_DISCONNECT:
            bt_device_disconnect_async (popup->conn, op->device_path,
                                        op->cancellable, on_op_result, ref);
            break;
        case OP_PAIR:
            bt_device_pair_async (popup->conn, op->device_path,
                                  op->cancellable, on_op_result, ref);
            break;
        case OP_REMOVE:
            bt_device_remove_async (popup->conn, op->adapter_path,
                                    op->device_path, op->cancellable,
                                    on_op_result, ref);
            break;
        case OP_RECONNECT:
            /* Sin cancelable a propósito (ver bt-dbus.h): la secuencia
             * termina aunque la ventana se cierre. Si la ventana ya no
             * espera el resultado, on_op_result lo descarta por el serial. */
            bt_device_reconnect_async (popup->conn, op->device_path,
                                       on_op_result, ref);
            break;
        case OP_CONNECT_PROFILE:
            bt_device_connect_profile_async (popup->conn, op->device_path,
                                             op->uuid, op->cancellable,
                                             on_op_result, ref);
            break;
        case OP_DISCONNECT_PROFILE:
            bt_device_disconnect_profile_async (popup->conn, op->device_path,
                                                op->uuid, op->cancellable,
                                                on_op_result, ref);
            break;
    }
}

static void
op_start_full (BtPopup *popup, OpKind kind,
               const gchar *device_path, const gchar *adapter_path,
               const gchar *name, const gchar *uuid,
               GtkWidget *expand_box, GtkWidget *action_btn,
               const gchar *action_label, GSList *extra_disabled);

void
op_start (BtPopup *popup, OpKind kind,
          const gchar *device_path, const gchar *adapter_path,
          const gchar *name,
          GtkWidget *expand_box, GtkWidget *action_btn,
          const gchar *action_label, GSList *extra_disabled)
{
    op_start_full (popup, kind, device_path, adapter_path, name, NULL,
                   expand_box, action_btn, action_label, extra_disabled);
}

void
op_start_profile (BtPopup *popup, OpKind kind,
                  const gchar *device_path, const gchar *adapter_path,
                  const gchar *name, const gchar *uuid,
                  GtkWidget *expand_box, GtkWidget *action_btn,
                  const gchar *action_label, GSList *extra_disabled)
{
    op_start_full (popup, kind, device_path, adapter_path, name, uuid,
                   expand_box, action_btn, action_label, extra_disabled);
}

static void
op_start_full (BtPopup *popup, OpKind kind,
               const gchar *device_path, const gchar *adapter_path,
               const gchar *name, const gchar *uuid,
               GtkWidget *expand_box, GtkWidget *action_btn,
               const gchar *action_label, GSList *extra_disabled)
{
    if (!popup || !device_path)
        return;

    /* Una sola operación por dispositivo: la nueva reemplaza a la anterior
     * (op_free cancela la vieja, y su callback se descarta por el serial). */
    OpInProgress *op = g_new0 (OpInProgress, 1);
    op->kind         = kind;
    op->uuid         = g_strdup (uuid);
    op->device_path  = g_strdup (device_path);
    op->adapter_path = g_strdup (adapter_path);
    op->name         = g_strdup (name);
    op->expand_box   = expand_box;
    op->action_btn   = action_btn;
    op->action_label = g_strdup (action_label);
    op->extra_disabled = extra_disabled;
    op->cancellable  = g_cancellable_new ();
    op->serial       = ++popup->op_serial;
    op->popup        = popup;
    op->timeout_id   = g_timeout_add (OP_TIMEOUT_MS, op_timeout_cb, op);

    /* UI inmediata: botón en "…" y deshabilitado, igual que hacía do_connect. */
    if (action_btn && GTK_IS_BUTTON (action_btn)) {
        const gchar *busy_label;
        switch (kind) {
            case OP_CONNECT:
            case OP_CONNECT_PROFILE:    busy_label = _("Connecting…");    break;
            case OP_DISCONNECT:
            case OP_DISCONNECT_PROFILE: busy_label = _("Disconnecting…"); break;
            case OP_PAIR:               busy_label = _("Pairing…");       break;
            case OP_RECONNECT:          busy_label = _("Reconnecting…");  break;
            default:                    busy_label = _("Removing…");      break;
        }
        gtk_widget_set_sensitive (action_btn, FALSE);
        gtk_button_set_label (GTK_BUTTON (action_btn), busy_label);
    }
    for (GSList *l = extra_disabled; l; l = l->next) {
        GtkWidget *w = l->data;
        if (w && GTK_IS_WIDGET (w))
            gtk_widget_set_sensitive (w, FALSE);
    }

    /* Ocultar un error previo del expand. */
    if (expand_box && GTK_IS_WIDGET (expand_box)) {
        GtkWidget *prev = g_object_get_data (G_OBJECT (expand_box), "error-label");
        if (prev)
            gtk_widget_hide (prev);
    }
    popup_clear_error (popup, device_path);

    g_hash_table_replace (popup->ops_in_progress, g_strdup (device_path), op);

    if (popup->plugin_ref)
        bt_plugin_set_busy (popup->plugin_ref, TRUE);

    op_launch (op);
}

/* ---------- ícono de dispositivo (usado también por plugin.c) ---------- */

GtkWidget *
make_device_icon (const gchar *icon_name, gint icon_size)
{
    const gchar *names[4];
    gint n = 0;

    if (icon_name && *icon_name)
        names[n++] = icon_name;
    names[n++] = "bluetooth";
    names[n++] = "bluetooth-symbolic";
    names[n]   = NULL;

    GIcon     *gicon = g_themed_icon_new_from_names ((gchar **) names, -1);
    GtkWidget *img   = gtk_image_new_from_gicon (gicon, GTK_ICON_SIZE_BUTTON);
    g_object_unref (gicon);
    gtk_image_set_pixel_size (GTK_IMAGE (img), icon_size);
    return img;
}

/* ---------- switch global de Bluetooth ---------- */

gboolean
on_bt_switch_toggled (GtkSwitch *sw, gboolean state, gpointer user_data)
{
    (void) sw;
    BtPopup *popup = user_data;
    bt_set_all_powered_async (popup->conn, state);

    /* Al apagar, todo lo demás deja de tener sentido: cerramos el expand. */
    if (!state && popup->current_expand_box) {
        gtk_widget_hide (popup->current_expand_box);
        popup->current_expand_box = NULL;
        clear_open_highlight (popup);
    }

    /* Dar tiempo a que BlueZ aplique el cambio antes de redibujar. */
    g_timeout_add (400, deferred_refresh_cb, popup);
    return FALSE;   /* dejar que el switch actualice su estado visual */
}

/* ---------- switch "visible para otros dispositivos" ---------- */

gboolean
on_discoverable_toggled (GtkSwitch *sw, gboolean state, gpointer user_data)
{
    (void) sw;
    BtPopup *popup = user_data;
    bt_set_all_discoverable_async (popup->conn, state);
    g_timeout_add (400, deferred_refresh_cb, popup);
    return FALSE;
}

/* ---------- switch por adaptador ---------- */

void
adapter_switch_data_free (AdapterSwitchData *d)
{
    if (!d) return;
    g_free (d->adapter_path);
    g_free (d);
}

gboolean
on_adapter_switch_toggled (GtkSwitch *sw, gboolean state, gpointer user_data)
{
    (void) sw;
    AdapterSwitchData *d = user_data;
    bt_adapter_set_powered_async (d->conn, d->adapter_path, state);
    return FALSE;
}

/* ---------- botón Buscar ---------- */

static gboolean
on_refresh_reenable (gpointer popup_ptr)
{
    BtPopup *popup = popup_ptr;
    popup->scan_timeout_id = 0;
    popup->scanning = FALSE;
    if (popup->refresh_button && GTK_IS_WIDGET (popup->refresh_button)) {
        gtk_widget_set_sensitive (popup->refresh_button, TRUE);
        if (popup->refresh_label && GTK_IS_LABEL (popup->refresh_label))
            gtk_label_set_text (GTK_LABEL (popup->refresh_label), _("Search"));
    }
    return G_SOURCE_REMOVE;
}

void
on_refresh_clicked (GtkWidget *btn, BtPopup *popup)
{
    (void) btn;
    if (popup->scanning)
        return;

    popup->scanning = TRUE;
    gtk_widget_set_sensitive (popup->refresh_button, FALSE);
    if (popup->refresh_label && GTK_IS_LABEL (popup->refresh_label))
        gtk_label_set_text (GTK_LABEL (popup->refresh_label), _("Searching…"));

    /* NO parar antes de arrancar. StopDiscovery es asíncrono: cuando el
     * StartDiscovery de la línea siguiente leía el estado, Discovering todavía
     * decía TRUE y el arranque se salteaba, con lo cual el botón Buscar
     * terminaba apagando la búsqueda en vez de reiniciarla.
     * Pedir el descubrimiento de nuevo es inofensivo y suficiente: BlueZ
     * lleva la cuenta por cliente y vuelve a emitir los aparatos conocidos. */
    bt_start_discovery_all (popup->conn);
    popup->discovery_active = TRUE;

    if (popup->scan_timeout_id)
        g_source_remove (popup->scan_timeout_id);
    popup->scan_timeout_id = g_timeout_add (SCAN_COOLDOWN_MS,
                                            on_refresh_reenable, popup);
}

/* ---------- botón "Configuración avanzada" ---------- */

void
on_advanced_clicked (GtkWidget *btn, gpointer user_data)
{
    (void) btn; (void) user_data;

    static const gchar *candidates[] = {
        "blueman-manager", "blueberry", "gnome-control-center bluetooth",
        "bluedevil-wizard", NULL
    };

    for (gint i = 0; candidates[i]; i++) {
        gchar **argv = g_strsplit (candidates[i], " ", 2);
        gboolean found = (g_find_program_in_path (argv[0]) != NULL);
        g_strfreev (argv);
        if (found) {
            g_spawn_command_line_async (candidates[i], NULL);
            return;
        }
    }

    if (g_find_program_in_path ("bluetoothctl") &&
        g_find_program_in_path ("xterm"))
        g_spawn_command_line_async ("xterm -e bluetoothctl", NULL);
}

/* ---------- envío de archivos por Bluetooth ----------
 *
 * Tres decisiones de diseño que conviene no revertir sin leer esto:
 *
 * 1. El popup se CIERRA antes de abrir el selector de archivos. El popup
 *    acapara el puntero para poder cerrarse al clic de afuera, y ese
 *    acaparamiento pelea con cualquier subventana. Cerrarlo primero libera
 *    el acaparamiento y además es lo que el usuario espera: elegís "enviar",
 *    el panel desaparece y aparece el selector.
 *
 * 2. La ventana de progreso es INDEPENDIENTE, no parte del popup. Si viviera
 *    adentro del popup, el primer clic afuera se llevaría puesta la
 *    transferencia.
 *
 * 3. Cerrar la ventana de progreso cancela el envío. Es el único botón que
 *    hace falta: mientras transfiere dice "Cancelar", cuando termina dice
 *    "Cerrar", y las dos veces hace exactamente lo mismo.
 *
 * Costo aceptado: si se reinicia el panel a mitad de un envío, el envío se
 * corta, porque todo esto vive en el proceso del plugin.
 */

typedef struct {
    GtkWidget  *window;        /* NULL una vez que la ventana se destruyó */
    GtkWidget  *bar;
    GtkWidget  *status;
    GtkWidget  *button;
    BtObexSend *send;          /* NULL una vez que el envío terminó */
    gboolean    finished;
} SendUi;

static void
send_ui_free (SendUi *ui)
{
    g_free (ui);
}

/* Fin del envío. Llega siempre, una sola vez. Si la ventana ya no está, este
 * es el último en apagar la luz. */
static void
on_send_done (gboolean success, const gchar *message, gpointer user_data)
{
    SendUi *ui = user_data;

    ui->send     = NULL;      /* el objeto de envío ya no existe */
    ui->finished = TRUE;

    if (ui->window == NULL) {
        send_ui_free (ui);
        return;
    }

    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (ui->bar),
                                   success ? 1.0 : 0.0);
    gtk_progress_bar_set_text (GTK_PROGRESS_BAR (ui->bar), NULL);
    gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (ui->bar), FALSE);

    gtk_label_set_text (GTK_LABEL (ui->status),
                        message != NULL ? message : "");
    if (!success)
        gtk_style_context_add_class (
            gtk_widget_get_style_context (ui->status), "error");

    gtk_button_set_label (GTK_BUTTON (ui->button), _("Close"));
}

static void
on_send_progress (guint64 transferred, guint64 size, gpointer user_data)
{
    SendUi *ui = user_data;

    if (ui->window == NULL)
        return;

    if (size > 0) {
        gdouble fraction = (gdouble) transferred / (gdouble) size;
        gchar *done  = g_format_size (transferred);
        gchar *total = g_format_size (size);
        gchar *txt   = g_strdup_printf (_("%s of %s"), done, total);

        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (ui->bar),
                                       CLAMP (fraction, 0.0, 1.0));
        gtk_progress_bar_set_text (GTK_PROGRESS_BAR (ui->bar), txt);
        gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (ui->bar), TRUE);

        g_free (done);
        g_free (total);
        g_free (txt);

        gtk_label_set_text (GTK_LABEL (ui->status), _("Sending…"));
    } else {
        /* Todavía no sabemos el tamaño: el otro extremo no aceptó aún. */
        gtk_progress_bar_pulse (GTK_PROGRESS_BAR (ui->bar));
        gtk_label_set_text (GTK_LABEL (ui->status),
                            _("Waiting for the other device to accept…"));
    }
}

/* La ventana se cerró. Si el envío sigue vivo, cancelarlo. Poner window en
 * NULL ANTES de cancelar es importante: cancelar dispara el aviso de fin en
 * el acto, y ese aviso no debe intentar dibujar en una ventana que se está
 * destruyendo. */
static void
on_send_window_destroy (GtkWidget *win, gpointer user_data)
{
    (void) win;
    SendUi *ui = user_data;

    ui->window = NULL;

    if (ui->send != NULL) {
        BtObexSend *send = ui->send;
        ui->send = NULL;
        bt_obex_send_cancel (send);
        /* on_send_done ya corrió y liberó ui. */
        return;
    }

    if (ui->finished)
        send_ui_free (ui);
}

static SendUi *
send_window_new (const gchar *device_name, const gchar *filename)
{
    SendUi *ui = g_new0 (SendUi, 1);

    ui->window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title (GTK_WINDOW (ui->window), _("Send file"));
    gtk_window_set_resizable (GTK_WINDOW (ui->window), FALSE);
    gtk_window_set_position (GTK_WINDOW (ui->window), GTK_WIN_POS_CENTER);
    gtk_window_set_default_size (GTK_WINDOW (ui->window), 380, -1);
    gtk_container_set_border_width (GTK_CONTAINER (ui->window), 12);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add (GTK_CONTAINER (ui->window), box);

    gchar     *base  = g_path_get_basename (filename);
    GtkWidget *fname = gtk_label_new (base);
    g_free (base);
    gtk_label_set_xalign (GTK_LABEL (fname), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (fname), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars (GTK_LABEL (fname), 40);
    gtk_box_pack_start (GTK_BOX (box), fname, FALSE, FALSE, 0);

    gchar *to = g_strdup_printf (_("To: %s"), device_name);
    GtkWidget *dest = gtk_label_new (to);
    g_free (to);
    gtk_label_set_xalign (GTK_LABEL (dest), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (dest), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (dest), 40);
    gtk_style_context_add_class (gtk_widget_get_style_context (dest),
                                 "dim-label");
    gtk_box_pack_start (GTK_BOX (box), dest, FALSE, FALSE, 0);

    ui->bar = gtk_progress_bar_new ();
    gtk_box_pack_start (GTK_BOX (box), ui->bar, FALSE, FALSE, 0);

    ui->status = gtk_label_new (_("Connecting…"));
    gtk_label_set_xalign (GTK_LABEL (ui->status), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (ui->status), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (ui->status), 40);
    gtk_box_pack_start (GTK_BOX (box), ui->status, FALSE, FALSE, 0);

    GtkWidget *btn_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start (GTK_BOX (box), btn_row, FALSE, FALSE, 0);

    ui->button = gtk_button_new_with_label (_("Cancel"));
    gtk_box_pack_end (GTK_BOX (btn_row), ui->button, FALSE, FALSE, 0);
    g_signal_connect_swapped (ui->button, "clicked",
                              G_CALLBACK (gtk_widget_destroy), ui->window);

    g_signal_connect (ui->window, "destroy",
                      G_CALLBACK (on_send_window_destroy), ui);

    gtk_widget_show_all (ui->window);
    return ui;
}

/* Aviso cuando el sistema todavía no puede transferir. Reusa el diagnóstico
 * de bt-obex.c para decir exactamente qué falta. */
static void
show_obex_problem (BtObexState state)
{
    GtkWidget *dlg = gtk_message_dialog_new (
        NULL, 0, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "%s",
        bt_obex_state_message (state));

    gchar *hint = bt_obex_state_hint (state);
    if (hint != NULL) {
        gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dlg),
                                                  "%s", hint);
        g_free (hint);
    }
    g_signal_connect (dlg, "response", G_CALLBACK (gtk_widget_destroy), NULL);
    gtk_widget_show (dlg);
}

typedef struct {
    gchar *address;
    gchar *name;
} SendRequest;

static void
send_request_free (SendRequest *req)
{
    g_free (req->address);
    g_free (req->name);
    g_free (req);
}

static void
on_chooser_response (GtkWidget *dialog, gint response, gpointer user_data)
{
    SendRequest *req = user_data;

    if (response == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename (
                              GTK_FILE_CHOOSER (dialog));
        if (filename != NULL) {
            SendUi *ui = send_window_new (req->name, filename);

            ui->send = bt_obex_send_file (req->address, filename,
                                          on_send_progress, on_send_done, ui);

            /* NULL solo si no hubo bus de sesión: en ese caso el aviso de fin
             * nunca va a llegar, así que hay que cerrar el círculo a mano. */
            if (ui->send == NULL)
                on_send_done (FALSE, _("Could not reach the D-Bus session bus."),
                              ui);

            g_free (filename);
        }
    }

    send_request_free (req);
    gtk_widget_destroy (dialog);
}

/* Abre el selector. Diferido a un idle para que el cierre del popup (y con
 * él la liberación del acaparamiento del puntero) termine primero. */
static gboolean
open_chooser_idle (gpointer user_data)
{
    SendRequest *req = user_data;

    GtkWidget *dialog = gtk_file_chooser_dialog_new (
        _("Select a file to send"), NULL, GTK_FILE_CHOOSER_ACTION_OPEN,
        _("Cancel"), GTK_RESPONSE_CANCEL,
        _("Send"),   GTK_RESPONSE_ACCEPT,
        NULL);

    /* obexd necesita una ruta real en el disco, no un recurso remoto. */
    gtk_file_chooser_set_local_only (GTK_FILE_CHOOSER (dialog), TRUE);
    gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_CENTER);

    g_signal_connect (dialog, "response",
                      G_CALLBACK (on_chooser_response), req);
    gtk_widget_show_all (dialog);

    return G_SOURCE_REMOVE;
}

void
popup_send_file_flow (BtPopup *popup, const gchar *address, const gchar *name)
{
    if (address == NULL)
        return;

    /* Copiar ANTES de cerrar el popup. Las cadenas vienen de la fila del
     * aparato, y cerrar el popup es justamente lo único que podría llegar a
     * tocarla. Copiando primero, el resto de la función ya no depende de
     * nada que viva adentro de la ventana. */
    SendRequest *req = g_new0 (SendRequest, 1);
    req->address = g_strdup (address);
    req->name    = g_strdup (name != NULL ? name : address);

    BtObexState state = bt_obex_check ();

    /* Cerrar el popup pase lo que pase: aunque sea solo para mostrar el aviso
     * de que falta el demonio, el acaparamiento del puntero tiene que
     * soltarse antes de abrir cualquier otra ventana. */
    popup_hide (popup);

    if (state != BT_OBEX_READY) {
        show_obex_problem (state);
        send_request_free (req);
        return;
    }

    g_idle_add (open_chooser_idle, req);
}

/* ---------- event handlers de la ventana ---------- */

static gboolean
on_key_press (GtkWidget *widget, GdkEventKey *event, BtPopup *popup)
{
    (void) widget;

    if (event->keyval == GDK_KEY_Escape) {
        /* Primero cierra el expand abierto; recién con el segundo Escape se
         * cierra el popup entero. */
        if (popup->current_expand_box) {
            gtk_widget_hide (popup->current_expand_box);
            popup->current_expand_box = NULL;
            clear_open_highlight (popup);
            return TRUE;
        }
        popup_hide (popup);
        return TRUE;
    }
    return FALSE;
}

static gboolean
on_focus_out (GtkWidget *widget, GdkEventFocus *ev, BtPopup *popup)
{
    (void) widget; (void) ev;
    popup_hide (popup);
    return FALSE;
}

static gboolean
on_button_press (GtkWidget *widget, GdkEventButton *event, BtPopup *popup)
{
    (void) widget;
    gint w, h;
    gtk_window_get_size (GTK_WINDOW (popup->window), &w, &h);

    /* Click fuera de la ventana: cerrar. */
    if (event->x < 0 || event->y < 0 || event->x > w || event->y > h) {
        popup_hide (popup);
        return TRUE;
    }
    return FALSE;
}

/* ---------- API pública ---------- */

BtPopup *
popup_create (XfcePanelPlugin *plugin, GtkWidget *button)
{
    (void) plugin; (void) button;
    BtPopup   *popup = g_new0 (BtPopup, 1);
    GtkWidget *win, *outer_box, *scroll;

    /* Proveedor de estilos único para todo el plugin, registrado a nivel de
     * pantalla una sola vez (no uno por fila y por separador en cada
     * refresco, como sería lo natural y lo caro). */
    {
        static gboolean css_installed = FALSE;
        if (!css_installed) {
            GtkCssProvider *prov = gtk_css_provider_new ();
            gtk_css_provider_load_from_data (prov,
                ".bt-row:focus { background-color: mix(@theme_bg_color, @theme_fg_color, 0.05); }"
                ".bt-row-open { background-color: mix(@theme_bg_color, @theme_fg_color, 0.07); }"
                ".bt-row-open:focus { background-color: mix(@theme_bg_color, @theme_fg_color, 0.07); }"
                ".module-sep { background-color: mix(@theme_bg_color, @theme_fg_color, 0.3); min-height: 2px; }",
                -1, NULL);
            gtk_style_context_add_provider_for_screen (
                gdk_screen_get_default (), GTK_STYLE_PROVIDER (prov),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref (prov);
            css_installed = TRUE;
        }
    }

    win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated         (GTK_WINDOW (win), FALSE);
    gtk_window_set_skip_taskbar_hint (GTK_WINDOW (win), TRUE);
    gtk_window_set_skip_pager_hint   (GTK_WINDOW (win), TRUE);
    gtk_window_set_resizable         (GTK_WINDOW (win), FALSE);
    gtk_window_set_type_hint         (GTK_WINDOW (win),
                                      GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_window_set_keep_above        (GTK_WINDOW (win), TRUE);

    outer_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class (gtk_widget_get_style_context (outer_box),
                                 "popup");
    gtk_container_add (GTK_CONTAINER (win), outer_box);

    popup->top_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start (GTK_BOX (outer_box), popup->top_box, FALSE, FALSE, 0);

    scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start (GTK_BOX (outer_box), scroll, TRUE, TRUE, 0);
    popup->scroll = scroll;

    popup->content_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (scroll), popup->content_box);

    /* ---- Zona fija inferior ---- */
    GtkWidget *bottom_sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_end (GTK_BOX (outer_box), bottom_sep, FALSE, FALSE, 0);

    GtkWidget *adv_btn = gtk_button_new_with_label (_("Advanced settings…"));
    gtk_button_set_relief (GTK_BUTTON (adv_btn), GTK_RELIEF_NONE);
    gtk_widget_set_margin_start  (adv_btn, 8);
    gtk_widget_set_margin_end    (adv_btn, 8);
    gtk_widget_set_margin_top    (adv_btn, 4);
    gtk_widget_set_margin_bottom (adv_btn, 4);
    g_signal_connect (adv_btn, "clicked",
                      G_CALLBACK (on_advanced_clicked), NULL);
    gtk_box_pack_end (GTK_BOX (outer_box), adv_btn, FALSE, FALSE, 0);

    /* Fila "visible para otros dispositivos": el equivalente funcional del
     * "conectar a red oculta" del plugin de red, pero al revés — acá lo que
     * se controla es si NOSOTROS somos visibles. */
    GtkWidget *disc_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start  (disc_row, 12);
    gtk_widget_set_margin_end    (disc_row, 12);
    gtk_widget_set_margin_top    (disc_row, 4);
    gtk_widget_set_margin_bottom (disc_row, 2);

    GtkWidget *disc_label = gtk_label_new (_("Visible to other devices"));
    gtk_label_set_xalign (GTK_LABEL (disc_label), 0.0);
    gtk_box_pack_start (GTK_BOX (disc_row), disc_label, TRUE, TRUE, 0);

    popup->discoverable_switch = gtk_switch_new ();
    gtk_widget_set_valign (popup->discoverable_switch, GTK_ALIGN_CENTER);
    popup->discoverable_handler = g_signal_connect (
        popup->discoverable_switch, "state-set",
        G_CALLBACK (on_discoverable_toggled), popup);
    gtk_box_pack_start (GTK_BOX (disc_row), popup->discoverable_switch,
                        FALSE, FALSE, 0);
    gtk_box_pack_end (GTK_BOX (outer_box), disc_row, FALSE, FALSE, 0);

    /* La visibilidad ya no es permanente: se apaga sola al vencer el plazo
     * (ver BT_DISCOVERABLE_TIMEOUT_S en bt-dbus.h). Decirlo acá evita que el
     * usuario crea que el interruptor "se apagó solo" por un error. A
     * propósito NO se apaga al cerrar la ventana: lo normal es cerrarla e ir
     * a buscar el equipo desde el teléfono. */
    {
        gchar *tip = g_strdup_printf (
            _("Other devices can find this computer for %d minutes; "
              "then it turns off by itself."),
            BT_DISCOVERABLE_TIMEOUT_S / 60);
        gtk_widget_set_tooltip_text (disc_row, tip);
        g_free (tip);
    }

    g_signal_connect (win, "key-press-event", G_CALLBACK (on_key_press), popup);

    popup->window = win;
    popup->ops_in_progress = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    g_free, op_free);
    popup->failed_devices  = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    g_free, g_free);
    return popup;
}

void
popup_show (BtPopup *popup, XfcePanelPlugin *plugin, GtkWidget *button,
            GDBusConnection *conn, gint popup_width, gint popup_height)
{
    (void) plugin;

    popup->conn         = conn;
    popup->popup_width  = popup_width;
    popup->popup_height = popup_height;
    gtk_widget_set_size_request (popup->window, popup_width, -1);
    gtk_widget_set_size_request (popup->scroll, popup_width, popup_height);

    /* Instantánea: toda la construcción lee de una sola foto del bus. */
    bt_cache_begin (conn);
    if (!popup->ui_built) {
        rebuild_ui (popup);
    } else {
        update_top_status       (popup);
        update_adapters_section (popup);
    }
    bt_cache_end ();

    /* El popup gestiona sus propias señales mientras está abierto. */
    if (!popup->signal_ids)
        popup->signal_ids = bt_subscribe_signals (conn, on_bt_signal_popup,
                                                  popup);

    gtk_widget_realize (popup->window);
    position_popup (popup, button);
    gtk_widget_show_all (popup->window);

    popup->button = button;
    {
        GrabData *gd = g_new0 (GrabData, 1);
        gd->popup  = popup;
        gd->button = button;
        g_signal_connect (popup->window, "map-event",
                          G_CALLBACK (on_window_mapped), gd);
    }

    /* Arrancar el descubrimiento. A diferencia del RequestScan del plugin de
     * red esto NO es un disparo único: queda corriendo, así que popup_hide
     * tiene la obligación de pararlo (consume batería y degrada el audio
     * A2DP que esté sonando). */
    bt_start_discovery_all (conn);
    popup->discovery_active = TRUE;
}

void
popup_hide (BtPopup *popup)
{
    GdkDisplay *display;
    GdkSeat    *seat;

    if (!gtk_widget_get_visible (popup->window))
        return;

    display = gtk_widget_get_display (popup->window);
    seat    = gdk_display_get_default_seat (display);
    gdk_seat_ungrab (seat);

    if (popup->press_handler) {
        g_signal_handler_disconnect (popup->window, popup->press_handler);
        popup->press_handler = 0;
    }

    /* Parar el descubrimiento SIEMPRE. */
    if (popup->discovery_active && popup->conn) {
        bt_stop_discovery_all (popup->conn);
        popup->discovery_active = FALSE;
    }

    if (popup->signal_ids) {
        bt_unsubscribe_signals (popup->conn, popup->signal_ids);
        popup->signal_ids = NULL;
    }

    /* Cancelar operaciones en curso. Las llamadas ya enviadas a BlueZ siguen
     * su curso del lado del servicio, pero la UI deja de esperarlas: sus
     * callbacks no encuentran la entrada en la tabla y se retiran solos. */
    g_hash_table_remove_all (popup->ops_in_progress);
    if (popup->plugin_ref)
        bt_plugin_set_busy (popup->plugin_ref, FALSE);

    if (popup->scan_timeout_id) {
        g_source_remove (popup->scan_timeout_id);
        popup->scan_timeout_id = 0;
    }
    popup->scanning = FALSE;

    if (popup->current_expand_box) {
        gtk_widget_hide (popup->current_expand_box);
        popup->current_expand_box = NULL;
    }
    clear_open_highlight (popup);

    gtk_widget_hide (popup->window);
}

void
popup_destroy (BtPopup *popup)
{
    if (!popup) return;
    popup_hide (popup);
    if (popup->ops_in_progress)
        g_hash_table_destroy (popup->ops_in_progress);
    if (popup->failed_devices)
        g_hash_table_destroy (popup->failed_devices);
    g_slist_free (popup->adapter_switches);
    gtk_widget_destroy (popup->window);
    g_free (popup);
}
