/* popup-rows.c — una fila por dispositivo Bluetooth, con su panel
 * expandible y todos los callbacks de acción. Equivalente a popup-rows.c del
 * plugin de red, pero sin nada de contraseñas: BlueZ resuelve la
 * autenticación por el agente (bt-agent.c), no por un campo de texto en la
 * fila. */

#include "popup-private.h"

/* ---------- realce de la fila abierta ---------- */

void
clear_open_highlight (BtPopup *popup)
{
    if (!popup->content_box)
        return;

    GList *sections = gtk_container_get_children (GTK_CONTAINER (popup->content_box));
    for (GList *s = sections; s; s = s->next) {
        if (!GTK_IS_CONTAINER (s->data))
            continue;
        GList *kids = gtk_container_get_children (GTK_CONTAINER (s->data));
        for (GList *k = kids; k; k = k->next) {
            if (!GTK_IS_EVENT_BOX (k->data))
                continue;
            gtk_style_context_remove_class (
                gtk_widget_get_style_context (GTK_WIDGET (k->data)),
                "bt-row-open");
        }
        g_list_free (kids);
    }
    g_list_free (sections);
}

static void
row_data_free (RowData *rd)
{
    if (!rd) return;
    g_free (rd->device_path);
    g_free (rd->adapter_path);
    g_free (rd->address);
    g_free (rd->name);
    g_free (rd);
}

/* ---------- desplazamiento hasta el expand recién abierto ---------- */

static gboolean
scroll_to_expand_idle (gpointer user_data)
{
    GtkWidget *expand = user_data;
    if (!GTK_IS_WIDGET (expand))
        return G_SOURCE_REMOVE;

    GtkWidget *sw = gtk_widget_get_ancestor (expand, GTK_TYPE_SCROLLED_WINDOW);
    if (!sw)
        return G_SOURCE_REMOVE;

    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (
                             GTK_SCROLLED_WINDOW (sw));
    GtkWidget *child = gtk_bin_get_child (GTK_BIN (sw));
    if (!adj || !child)
        return G_SOURCE_REMOVE;

    gint y = 0;
    if (!gtk_widget_translate_coordinates (expand, child, 0, 0, NULL, &y))
        return G_SOURCE_REMOVE;

    GtkAllocation alloc;
    gtk_widget_get_allocation (expand, &alloc);

    gdouble page  = gtk_adjustment_get_page_size (adj);
    gdouble value = gtk_adjustment_get_value (adj);
    gdouble upper = gtk_adjustment_get_upper (adj);

    if (y + alloc.height > value + page) {
        gdouble target = y + alloc.height - page;
        gtk_adjustment_set_value (adj, CLAMP (target, 0, MAX (0, upper - page)));
    } else if (y < value) {
        gtk_adjustment_set_value (adj, y);
    }
    return G_SOURCE_REMOVE;
}

static void
scroll_to_expand_on_alloc (GtkWidget *expand, GdkRectangle *alloc,
                           gpointer user_data)
{
    (void) alloc; (void) user_data;
    g_signal_handlers_disconnect_by_func (expand, scroll_to_expand_on_alloc, NULL);
    g_idle_add (scroll_to_expand_idle, expand);
}

/* ---------- abrir / cerrar el expand ---------- */

/* Muestra un contenedor Y su contenido.
 *
 * Hacen falta las dos mitades y ninguna sirve sola:
 *   - gtk_widget_show() no es recursivo: deja los botones de adentro
 *     invisibles, con lo que el panel se despliega vacío;
 *   - gtk_widget_show_all() sobre el contenedor tampoco sirve, porque
 *     retorna de inmediato cuando el widget tiene puesto no_show_all — y se
 *     lo ponemos a propósito, para que el show_all de la reconstrucción de
 *     filas no abra todos los paneles de golpe.
 *
 * La salida es mostrar el contenedor a mano y hacer show_all sobre cada
 * hijo. Los hijos que llevan su propio no_show_all (confirm_box, details_box)
 * se saltean solos, así que siguen apareciendo únicamente cuando les toca. */
static void
box_show_deep (GtkWidget *box)
{
    if (!box || !GTK_IS_CONTAINER (box))
        return;

    GList *kids = gtk_container_get_children (GTK_CONTAINER (box));
    for (GList *k = kids; k; k = k->next)
        gtk_widget_show_all (GTK_WIDGET (k->data));
    g_list_free (kids);

    gtk_widget_show (box);
}

static void
row_toggle_expand (RowData *rd)
{
    BtPopup *popup = rd->popup;

    /* Ya estaba abierto: cerrar. */
    if (popup->current_expand_box == rd->expand_box) {
        gtk_widget_hide (rd->expand_box);
        popup->current_expand_box = NULL;
        clear_open_highlight (popup);
        return;
    }

    /* Cerrar el que hubiera abierto (solo uno a la vez). */
    if (popup->current_expand_box) {
        gtk_widget_hide (popup->current_expand_box);
        popup->current_expand_box = NULL;
    }
    clear_open_highlight (popup);

    /* Confirmación de "olvidar" y detalles siempre arrancan ocultos. */
    if (rd->confirm_box)
        gtk_widget_hide (rd->confirm_box);
    if (rd->details_box)
        gtk_widget_hide (rd->details_box);
    if (rd->details_arrow)
        gtk_image_set_from_icon_name (GTK_IMAGE (rd->details_arrow),
                                      "pan-down-symbolic", GTK_ICON_SIZE_MENU);

    box_show_deep (rd->expand_box);
    popup->current_expand_box = rd->expand_box;

    if (rd->row_box)
        gtk_style_context_add_class (gtk_widget_get_style_context (rd->row_box),
                                     "bt-row-open");

    g_signal_connect (rd->expand_box, "size-allocate",
                      G_CALLBACK (scroll_to_expand_on_alloc), NULL);
}

static gboolean
on_row_clicked (GtkWidget *event_box, GdkEventButton *event, gpointer rd_ptr)
{
    (void) event_box;
    if (event->type != GDK_BUTTON_PRESS || event->button != 1)
        return FALSE;
    row_toggle_expand ((RowData *) rd_ptr);
    return TRUE;
}

static gboolean
on_row_key_press (GtkWidget *event_box, GdkEventKey *event, gpointer rd_ptr)
{
    (void) event_box;
    if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter ||
        event->keyval == GDK_KEY_space) {
        row_toggle_expand ((RowData *) rd_ptr);
        return TRUE;
    }
    return FALSE;
}

void
reopen_expand_for_device (BtPopup *popup, const gchar *device_path)
{
    if (!popup || !device_path || !popup->content_box)
        return;

    GList *sections = gtk_container_get_children (GTK_CONTAINER (popup->content_box));
    for (GList *s = sections; s; s = s->next) {
        if (!GTK_IS_CONTAINER (s->data))
            continue;
        GList *kids = gtk_container_get_children (GTK_CONTAINER (s->data));
        for (GList *k = kids; k; k = k->next) {
            if (!GTK_IS_EVENT_BOX (k->data))
                continue;
            const gchar *p = g_object_get_data (G_OBJECT (k->data), "device-path");
            if (g_strcmp0 (p, device_path) != 0)
                continue;
            RowData *rd = g_object_get_data (G_OBJECT (k->data), "row-data");
            if (rd)
                row_toggle_expand (rd);
            g_list_free (kids);
            g_list_free (sections);
            return;
        }
        g_list_free (kids);
    }
    g_list_free (sections);
}

/* ---------- acciones ---------- */

/* Widgets que hay que deshabilitar mientras corre una operación, para que el
 * usuario no dispare dos cosas encimadas sobre el mismo aparato. */
static GSList *
collect_extras (RowData *rd)
{
    GSList *extras = NULL;
    if (rd->forget_btn && gtk_widget_get_sensitive (rd->forget_btn))
        extras = g_slist_prepend (extras, rd->forget_btn);
    if (rd->reconnect_btn && gtk_widget_get_sensitive (rd->reconnect_btn))
        extras = g_slist_prepend (extras, rd->reconnect_btn);
    if (rd->trust_check && gtk_widget_get_sensitive (rd->trust_check))
        extras = g_slist_prepend (extras, rd->trust_check);
    if (rd->send_btn && gtk_widget_get_sensitive (rd->send_btn))
        extras = g_slist_prepend (extras, rd->send_btn);
    for (GSList *l = extras; l; l = l->next)
        gtk_widget_set_sensitive (GTK_WIDGET (l->data), FALSE);
    return extras;
}

static void
on_primary_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    RowData *rd = rd_ptr;
    BtPopup *popup = rd->popup;

    if (op_running_for (popup, rd->device_path))
        return;

    if (rd->blocked) {
        /* Desbloquear no es una operación con resultado: es una propiedad. */
        bt_device_set_blocked (popup->conn, rd->device_path, FALSE);
        g_timeout_add (300, deferred_refresh_cb, popup);
        return;
    }

    OpKind kind;
    const gchar *label;
    if (rd->connected) {
        kind  = OP_DISCONNECT;
        label = _("Disconnect");
    } else if (rd->paired) {
        kind  = OP_CONNECT;
        label = _("Connect");
    } else {
        kind  = OP_PAIR;
        label = _("Pair");
    }

    GSList *extras = collect_extras (rd);
    op_start (popup, kind, rd->device_path, rd->adapter_path, rd->name,
              rd->expand_box, btn, label, extras);
}

/* Para las acciones que no salen del botón principal: el principal también
 * tiene que quedar deshabilitado mientras corren. */
static GSList *
collect_extras_with_primary (RowData *rd, GtkWidget *acting)
{
    GSList *extras = collect_extras (rd);
    extras = g_slist_remove (extras, acting);
    if (acting)
        gtk_widget_set_sensitive (acting, TRUE);   /* op_start lo deshabilita */
    if (rd->primary_btn && rd->primary_btn != acting &&
        gtk_widget_get_sensitive (rd->primary_btn)) {
        gtk_widget_set_sensitive (rd->primary_btn, FALSE);
        extras = g_slist_prepend (extras, rd->primary_btn);
    }
    return extras;
}

static void
on_reconnect_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    RowData *rd = rd_ptr;
    BtPopup *popup = rd->popup;

    if (op_running_for (popup, rd->device_path))
        return;

    GSList *extras = collect_extras_with_primary (rd, btn);
    op_start (popup, OP_RECONNECT, rd->device_path, rd->adapter_path,
              rd->name, rd->expand_box, btn, _("Reconnect"), extras);
}

static void
on_forget_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData *rd = rd_ptr;
    if (rd->action_row)  gtk_widget_hide (rd->action_row);
    if (rd->trust_check) gtk_widget_hide (rd->trust_check);
    box_show_deep (rd->confirm_box);
}

static void
on_forget_cancel_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData *rd = rd_ptr;
    if (rd->confirm_box) gtk_widget_hide (rd->confirm_box);
    if (rd->action_row)  gtk_widget_show_all (rd->action_row);
    if (rd->trust_check) gtk_widget_show_all (rd->trust_check);
}

static void
on_forget_confirm_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    RowData *rd = rd_ptr;
    BtPopup *popup = rd->popup;

    if (op_running_for (popup, rd->device_path))
        return;

    popup_clear_error (popup, rd->device_path);
    op_start (popup, OP_REMOVE, rd->device_path, rd->adapter_path, rd->name,
              rd->expand_box, btn, _("Forget"), NULL);
}

static void
on_trust_toggled (GtkToggleButton *check, gpointer rd_ptr)
{
    RowData *rd = rd_ptr;
    bt_device_set_trusted (rd->popup->conn, rd->device_path,
                           gtk_toggle_button_get_active (check));
}

/* ---------- enviar archivo ---------- */

/* Todo el trabajo vive en popup.c (popup_send_file_flow): cerrar el popup,
 * revisar que el sistema pueda transferir, pedir el archivo y abrir la
 * ventana de progreso. Acá solo se dispara. */
static void
on_send_file_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData *rd = rd_ptr;

    if (op_running_for (rd->popup, rd->device_path))
        return;

    popup_send_file_flow (rd->popup, rd->address, rd->name);
}

/* ---------- panel de detalles ---------- */

static void
details_add_row (GtkWidget *box, const gchar *label, const gchar *value)
{
    if (!value || !*value)
        return;

    GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget *l = gtk_label_new (label);
    gtk_label_set_xalign (GTK_LABEL (l), 0.0);
    gtk_widget_set_size_request (l, 110, -1);
    gtk_style_context_add_class (gtk_widget_get_style_context (l), "dim-label");
    gtk_box_pack_start (GTK_BOX (hbox), l, FALSE, FALSE, 0);

    GtkWidget *v = gtk_label_new (value);
    gtk_label_set_xalign (GTK_LABEL (v), 0.0);
    gtk_label_set_selectable (GTK_LABEL (v), TRUE);
    gtk_label_set_line_wrap  (GTK_LABEL (v), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (v), 26);
    gtk_box_pack_start (GTK_BOX (hbox), v, TRUE, TRUE, 0);

    gtk_box_pack_start (GTK_BOX (box), hbox, FALSE, FALSE, 0);
}

/* ---------- renombrar (alias) ----------
 *
 * El campo vive dentro de «Detalles», que se rellena de nuevo cada vez que
 * se abre. Por eso los datos que necesita el guardado (la fila y el nombre
 * que tenía al abrirse) van pegados al propio campo y no a la fila.
 *
 * Al guardar se cierra el panel desplegado a propósito: mientras está
 * abierto, la lista no se redibuja (regla de no destruir filas debajo del
 * cursor), y el usuario quiere ver el nombre nuevo en la lista. Cerrarlo
 * deja que el refresco recree esa fila con el título nuevo. Desde acá en
 * adelante no se toca rd: el refresco lo libera junto con la fila vieja. */
static void
on_alias_save (GtkWidget *entry_w, gpointer unused)
{
    (void) unused;
    RowData *rd = g_object_get_data (G_OBJECT (entry_w), "row-data");
    if (!rd)
        return;

    gchar       *alias   = g_strstrip (g_strdup (gtk_entry_get_text (
                                           GTK_ENTRY (entry_w))));
    const gchar *current = g_object_get_data (G_OBJECT (entry_w),
                                              "current-alias");

    /* Mismo nombre que ya tenía: nada que hacer. Vacío siempre se manda,
     * porque es la forma de pedir el nombre de fábrica. */
    if (*alias && g_strcmp0 (alias, current) == 0) {
        g_free (alias);
        return;
    }

    BtPopup *popup = rd->popup;
    bt_device_set_alias (popup->conn, rd->device_path, alias);
    g_free (alias);

    if (popup->current_expand_box == rd->expand_box) {
        gtk_widget_hide (rd->expand_box);
        popup->current_expand_box = NULL;
    }
    clear_open_highlight (popup);

    /* Darle tiempo a BlueZ a guardar y avisar antes de redibujar. */
    g_timeout_add (300, deferred_refresh_cb, popup);
}

static void
details_add_alias_editor (RowData *rd, const BtDevice *d)
{
    GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget *l = gtk_label_new (_("Name:"));
    gtk_label_set_xalign (GTK_LABEL (l), 0.0);
    gtk_widget_set_size_request (l, 110, -1);
    gtk_style_context_add_class (gtk_widget_get_style_context (l), "dim-label");
    gtk_box_pack_start (GTK_BOX (hbox), l, FALSE, FALSE, 0);

    GtkWidget *entry = gtk_entry_new ();
    gtk_entry_set_text (GTK_ENTRY (entry), d->name ? d->name : "");
    gtk_entry_set_max_length (GTK_ENTRY (entry), 64);
    gtk_entry_set_width_chars (GTK_ENTRY (entry), 12);
    gtk_widget_set_hexpand (entry, TRUE);

    /* Lo que pasa si se deja vacío, dicho en el mismo campo y en la ayuda
     * emergente, para que nadie tenga que adivinarlo. */
    gchar *tip;
    if (d->remote_name) {
        gtk_entry_set_placeholder_text (GTK_ENTRY (entry), d->remote_name);
        tip = g_strdup_printf (
            _("Leave it empty to go back to the device's own name (%s)."),
            d->remote_name);
    } else {
        tip = g_strdup (_("Leave it empty to go back to the device's own name."));
    }
    gtk_widget_set_tooltip_text (entry, tip);
    g_free (tip);

    g_object_set_data (G_OBJECT (entry), "row-data", rd);
    g_object_set_data_full (G_OBJECT (entry), "current-alias",
                            g_strdup (d->name), g_free);
    g_signal_connect (entry, "activate", G_CALLBACK (on_alias_save), NULL);
    gtk_box_pack_start (GTK_BOX (hbox), entry, TRUE, TRUE, 0);

    GtkWidget *save = gtk_button_new_with_label (_("Rename"));
    g_signal_connect_swapped (save, "clicked",
                              G_CALLBACK (on_alias_save), entry);
    gtk_box_pack_start (GTK_BOX (hbox), save, FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (rd->details_box), hbox, FALSE, FALSE, 0);
}

/* ---------- perfiles puntuales ----------
 *
 * Solo los que una persona reconoce y que tiene sentido levantar a mano.
 * Los UUID son los del aparato REMOTO: unos auriculares anuncian "receptor
 * de audio" (0x110B), y eso es lo que se le pide conectar.
 *
 * Lo que esto NO resuelve, y por eso va dicho en la ventana: elegir entre
 * sonido de alta calidad y manos libres (con micrófono) lo decide el
 * servidor de sonido, no BlueZ. Conectar el perfil de audio levanta el
 * enlace; cuál se usa para sonar se elige en la configuración de sonido. */
static const struct {
    guint16      id;
    gboolean     audio;
    const gchar *label;
} known_profiles[] = {
    { 0x110B, TRUE,  N_("High-quality audio (A2DP)") },
    { 0x110A, TRUE,  N_("Audio from the device (A2DP)") },
    { 0x111E, TRUE,  N_("Hands-free (HFP)") },
    { 0x1108, TRUE,  N_("Headset (HSP)") },
    { 0x1124, FALSE, N_("Keyboard / mouse (HID)") },
};

static void
on_profile_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    RowData *rd = rd_ptr;
    BtPopup *popup = rd->popup;

    if (op_running_for (popup, rd->device_path))
        return;

    const gchar *uuid  = g_object_get_data (G_OBJECT (btn), "profile-uuid");
    gboolean     conn  = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn),
                                                             "profile-connect"));
    GSList      *extras = collect_extras_with_primary (rd, btn);

    op_start_profile (popup,
                      conn ? OP_CONNECT_PROFILE : OP_DISCONNECT_PROFILE,
                      rd->device_path, rd->adapter_path, rd->name, uuid,
                      rd->expand_box, btn, gtk_button_get_label (GTK_BUTTON (btn)),
                      extras);
}

static GtkWidget *
profile_button (RowData *rd, const gchar *uuid, gboolean connect)
{
    GtkWidget *b = gtk_button_new_with_label (connect ? _("Connect")
                                                      : _("Disconnect"));
    g_object_set_data_full (G_OBJECT (b), "profile-uuid", g_strdup (uuid),
                            g_free);
    g_object_set_data (G_OBJECT (b), "profile-connect",
                       GINT_TO_POINTER (connect));
    g_signal_connect (b, "clicked", G_CALLBACK (on_profile_clicked), rd);
    return b;
}

static void
details_add_profiles (RowData *rd, const BtDevice *d)
{
    if (!d->uuids)
        return;

    gboolean any = FALSE, any_audio = FALSE;

    for (guint p = 0; p < G_N_ELEMENTS (known_profiles); p++) {
        const gchar *uuid = NULL;
        for (gchar **u = d->uuids; *u && !uuid; u++)
            if (bt_uuid16 (*u) == known_profiles[p].id)
                uuid = *u;
        if (!uuid)
            continue;

        if (!any) {
            GtkWidget *head = gtk_label_new (_("Connect a single profile"));
            gtk_label_set_xalign (GTK_LABEL (head), 0.0);
            gtk_style_context_add_class (gtk_widget_get_style_context (head),
                                         "dim-label");
            gtk_widget_set_margin_top (head, 6);
            gtk_box_pack_start (GTK_BOX (rd->details_box), head,
                                FALSE, FALSE, 0);
            any = TRUE;
        }
        any_audio |= known_profiles[p].audio;

        GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *l = gtk_label_new (_(known_profiles[p].label));
        gtk_label_set_xalign (GTK_LABEL (l), 0.0);
        gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start (GTK_BOX (hbox), l, TRUE, TRUE, 0);

        /* BlueZ no informa qué perfil está conectado y cuál no, así que no
         * se puede mostrar un interruptor honesto. Con el aparato conectado
         * se ofrecen las dos acciones; desconectado, solo conectar. */
        if (d->connected)
            gtk_box_pack_end (GTK_BOX (hbox), profile_button (rd, uuid, FALSE),
                              FALSE, FALSE, 0);
        gtk_box_pack_end (GTK_BOX (hbox), profile_button (rd, uuid, TRUE),
                          FALSE, FALSE, 0);

        gtk_box_pack_start (GTK_BOX (rd->details_box), hbox, FALSE, FALSE, 0);
    }

    if (any_audio) {
        GtkWidget *hint = gtk_label_new (
            _("To choose between high-quality sound and hands-free with "
              "microphone, use the sound settings: that choice belongs to "
              "the sound server, not to Bluetooth."));
        gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
        gtk_label_set_line_wrap (GTK_LABEL (hint), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (hint), 34);
        gtk_style_context_add_class (gtk_widget_get_style_context (hint),
                                     "dim-label");
        PangoAttrList *attrs = pango_attr_list_new ();
        pango_attr_list_insert (attrs, pango_attr_scale_new (PANGO_SCALE_SMALL));
        gtk_label_set_attributes (GTK_LABEL (hint), attrs);
        pango_attr_list_unref (attrs);
        gtk_box_pack_start (GTK_BOX (rd->details_box), hint, FALSE, FALSE, 0);
    }
}

static void
on_details_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData *rd = rd_ptr;

    if (gtk_widget_get_visible (rd->details_box)) {
        gtk_widget_hide (rd->details_box);
        if (rd->details_arrow)
            gtk_image_set_from_icon_name (GTK_IMAGE (rd->details_arrow),
                                          "pan-down-symbolic",
                                          GTK_ICON_SIZE_MENU);
        return;
    }

    /* Si hay una operación en curso, sus botones pueden vivir acá adentro
     * (los de perfil): rellenar ahora los destruiría con la operación
     * todavía apuntándolos. Se muestra lo que ya había, sin rehacerlo. */
    GList   *had  = gtk_container_get_children (GTK_CONTAINER (rd->details_box));
    gboolean full = (had != NULL);
    g_list_free (had);
    if (full && op_running_for (rd->popup, rd->device_path)) {
        box_show_deep (rd->details_box);
        if (rd->details_arrow)
            gtk_image_set_from_icon_name (GTK_IMAGE (rd->details_arrow),
                                          "pan-up-symbolic", GTK_ICON_SIZE_MENU);
        return;
    }

    /* Rellenar con datos frescos cada vez que se abre. */
    GList *old = gtk_container_get_children (GTK_CONTAINER (rd->details_box));
    g_list_free_full (old, (GDestroyNotify) gtk_widget_destroy);

    BtDevice *d = bt_get_device (rd->popup->conn, rd->device_path);
    if (d) {
        /* Renombrar solo tiene sentido en los emparejados: el alias de un
         * aparato de paso se pierde en cuanto BlueZ lo descarta. */
        if (d->paired) {
            details_add_alias_editor (rd, d);
            /* Si el nombre visible es un alias, mostrar también el de
             * fábrica, para que se sepa qué aparato es en realidad. */
            if (d->remote_name && g_strcmp0 (d->name, d->remote_name) != 0)
                details_add_row (rd->details_box, _("Device name:"),
                                 d->remote_name);
        }
        details_add_row (rd->details_box, _("Address:"), d->address);

        gchar *profiles = bt_profiles_string (d->uuids);
        details_add_row (rd->details_box, _("Profiles:"), profiles);
        g_free (profiles);

        if (d->battery >= 0) {
            gchar *bat = g_strdup_printf ("%d%%", d->battery);
            details_add_row (rd->details_box, _("Battery:"), bat);
            g_free (bat);
        }
        if (d->has_rssi) {
            gchar *sig = g_strdup_printf (_("%d%% (%d dBm)"),
                                          bt_rssi_to_quality (d->rssi),
                                          (gint) d->rssi);
            details_add_row (rd->details_box, _("Signal:"), sig);
            g_free (sig);
        }
        details_add_row (rd->details_box, _("Paired:"),
                         d->paired ? _("Yes") : _("No"));
        if (d->legacy_pairing)
            details_add_row (rd->details_box, _("Pairing:"), _("Legacy (PIN)"));

        if (d->paired && !d->blocked)
            details_add_profiles (rd, d);

        bt_device_free (d);
    }

    box_show_deep (rd->details_box);
    if (rd->details_arrow)
        gtk_image_set_from_icon_name (GTK_IMAGE (rd->details_arrow),
                                      "pan-up-symbolic", GTK_ICON_SIZE_MENU);
}

/* ---------- texto de estado de la fila ---------- */

static gchar *
subtitle_for (const BtDevice *d)
{
    if (d->blocked)
        return g_strdup (_("Blocked"));

    if (d->connected) {
        if (d->battery >= 0)
            return g_strdup_printf (_("Connected · Battery %d%%"), d->battery);
        return g_strdup (_("Connected"));
    }

    if (d->paired) {
        if (!d->has_rssi)
            return g_strdup (_("Paired · Out of range"));
        return g_strdup_printf (_("Paired · Signal %d%%"),
                                bt_rssi_to_quality (d->rssi));
    }

    if (d->has_rssi)
        return g_strdup_printf (_("Available · Signal %d%%"),
                                bt_rssi_to_quality (d->rssi));
    return g_strdup (_("Available"));
}

/* ---------- huella de la fila ----------
 *
 * Condensa todo lo que cambia el CONTENIDO de la fila (qué botones lleva el
 * expand, qué dice el título). Deliberadamente NO incluye RSSI ni batería:
 * eso se actualiza en el lugar, sin destruir nada. Tampoco incluye el error
 * de la última operación, para que un fallo no haga parpadear la fila justo
 * cuando el usuario está mirando el mensaje. */
gchar *
row_fingerprint (BtPopup *popup, BtDevice *dev)
{
    (void) popup;
    return g_strdup_printf ("%d|%d|%d|%d|%s|%s",
                            dev->connected, dev->paired, dev->trusted,
                            dev->blocked,
                            bt_device_icon_name (dev),
                            dev->name ? dev->name : "");
}

/* ---------- actualización en el lugar ---------- */

void
row_update_in_place (GtkWidget *event_box, BtDevice *dev)
{
    gint old_rssi = GPOINTER_TO_INT (
        g_object_get_data (G_OBJECT (event_box), "row-rssi"));
    gint old_bat  = GPOINTER_TO_INT (
        g_object_get_data (G_OBJECT (event_box), "row-battery"));

    gint new_rssi = dev->has_rssi ? (gint) dev->rssi : 0;
    if (old_rssi == new_rssi && old_bat == dev->battery)
        return;

    g_object_set_data (G_OBJECT (event_box), "row-rssi",
                       GINT_TO_POINTER (new_rssi));
    g_object_set_data (G_OBJECT (event_box), "row-battery",
                       GINT_TO_POINTER (dev->battery));

    GtkWidget *sub = g_object_get_data (G_OBJECT (event_box), "subtitle-label");
    if (sub && GTK_IS_LABEL (sub)) {
        gchar *txt = subtitle_for (dev);
        gtk_label_set_text (GTK_LABEL (sub), txt);
        g_free (txt);
    }
}

/* ---------- construcción de la fila ---------- */

GtkWidget *
make_device_row (BtDevice *dev, BtPopup *popup)
{
    GtkWidget *outer, *event_box, *row, *name_label, *icon;
    GtkWidget *expand_box;

    outer     = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    event_box = gtk_event_box_new ();
    gtk_event_box_set_above_child (GTK_EVENT_BOX (event_box), FALSE);
    gtk_widget_add_events (event_box, GDK_BUTTON_PRESS_MASK);
    gtk_widget_set_can_focus (event_box, TRUE);
    gtk_style_context_add_class (gtk_widget_get_style_context (event_box),
                                 "bt-row");
    gtk_container_add (GTK_CONTAINER (event_box), outer);

    /* ---- línea principal ---- */
    row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start  (row, 10);
    gtk_widget_set_margin_end    (row, 10);
    gtk_widget_set_margin_top    (row, 6);
    gtk_widget_set_margin_bottom (row, 6);
    gtk_box_pack_start (GTK_BOX (outer), row, FALSE, FALSE, 0);

    icon = make_device_icon (bt_device_icon_name (dev), ROW_ICON_SIZE);
    gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
    gtk_box_pack_start (GTK_BOX (row), icon, FALSE, FALSE, 0);

    GtkWidget *text_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign (text_box, GTK_ALIGN_CENTER);
    gtk_box_pack_start (GTK_BOX (row), text_box, TRUE, TRUE, 0);

    name_label = gtk_label_new (dev->name);
    gtk_label_set_xalign     (GTK_LABEL (name_label), 0.0);
    gtk_label_set_ellipsize  (GTK_LABEL (name_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (name_label), 26);
    gtk_box_pack_start (GTK_BOX (text_box), name_label, FALSE, FALSE, 0);

    GtkWidget *subtitle = gtk_label_new (NULL);
    {
        gchar *txt = subtitle_for (dev);
        gtk_label_set_text (GTK_LABEL (subtitle), txt);
        g_free (txt);
    }
    gtk_label_set_xalign (GTK_LABEL (subtitle), 0.0);
    gtk_style_context_add_class (gtk_widget_get_style_context (subtitle),
                                 "dim-label");
    {
        PangoAttrList *attrs = pango_attr_list_new ();
        pango_attr_list_insert (attrs, pango_attr_scale_new (PANGO_SCALE_SMALL));
        gtk_label_set_attributes (GTK_LABEL (subtitle), attrs);
        pango_attr_list_unref (attrs);
    }
    gtk_box_pack_start (GTK_BOX (text_box), subtitle, FALSE, FALSE, 0);

    /* Marca visual de conectado, a la derecha. */
    if (dev->connected) {
        GtkWidget *check = gtk_image_new_from_icon_name (
                               "object-select-symbolic", GTK_ICON_SIZE_MENU);
        gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
        gtk_box_pack_end (GTK_BOX (row), check, FALSE, FALSE, 0);
    }

    /* ---- panel expandible ---- */
    expand_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start  (expand_box, 40);
    gtk_widget_set_margin_end    (expand_box, 10);
    gtk_widget_set_margin_bottom (expand_box, 8);
    gtk_widget_set_no_show_all (expand_box, TRUE);
    gtk_widget_hide (expand_box);
    gtk_box_pack_start (GTK_BOX (outer), expand_box, FALSE, FALSE, 0);

    RowData *rd = g_new0 (RowData, 1);
    rd->popup        = popup;
    rd->expand_box   = expand_box;
    rd->device_path  = g_strdup (dev->object_path);
    rd->adapter_path = g_strdup (dev->adapter_path);
    rd->address      = g_strdup (dev->address);
    rd->name         = g_strdup (dev->name);
    rd->paired       = dev->paired;
    rd->connected    = dev->connected;
    rd->trusted      = dev->trusted;
    rd->blocked      = dev->blocked;
    rd->row_box      = event_box;

    /* Error de la última operación, si lo hubo. Se crea de entrada para que
     * al reabrir la fila el usuario vea por qué falló. */
    const gchar *prev_err = popup_get_error (popup, dev->object_path);
    if (prev_err) {
        GtkWidget *err = gtk_label_new (prev_err);
        gtk_style_context_add_class (gtk_widget_get_style_context (err), "error");
        gtk_label_set_xalign    (GTK_LABEL (err), 0.0);
        gtk_label_set_line_wrap (GTK_LABEL (err), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (err), 34);
        gtk_box_pack_start (GTK_BOX (expand_box), err, FALSE, FALSE, 0);
        g_object_set_data (G_OBJECT (expand_box), "error-label", err);
    }

    /* ---- fila de botones ---- */
    rd->action_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start (GTK_BOX (expand_box), rd->action_row, FALSE, FALSE, 0);

    const gchar *primary_label;
    if (dev->blocked)        primary_label = _("Unblock");
    else if (dev->connected) primary_label = _("Disconnect");
    else if (dev->paired)    primary_label = _("Connect");
    else                     primary_label = _("Pair");

    rd->primary_btn = gtk_button_new_with_label (primary_label);
    gtk_style_context_add_class (
        gtk_widget_get_style_context (rd->primary_btn), "suggested-action");
    g_signal_connect (rd->primary_btn, "clicked",
                      G_CALLBACK (on_primary_clicked), rd);
    gtk_box_pack_start (GTK_BOX (rd->action_row), rd->primary_btn,
                        FALSE, FALSE, 0);

    /* Reconectar: el remedio de un clic para el auricular conectado pero
     * mudo. Solo tiene sentido si hay algo conectado que reconectar. */
    if (dev->connected && !dev->blocked) {
        rd->reconnect_btn = gtk_button_new_with_label (_("Reconnect"));
        gtk_widget_set_tooltip_text (rd->reconnect_btn,
            _("Disconnect and connect again. Often fixes a device that is "
              "connected but does not work."));
        g_signal_connect (rd->reconnect_btn, "clicked",
                          G_CALLBACK (on_reconnect_clicked), rd);
        gtk_box_pack_start (GTK_BOX (rd->action_row), rd->reconnect_btn,
                            FALSE, FALSE, 0);
    }

    if (dev->paired) {
        rd->forget_btn = gtk_button_new_with_label (_("Forget"));
        g_signal_connect (rd->forget_btn, "clicked",
                          G_CALLBACK (on_forget_clicked), rd);
        gtk_box_pack_start (GTK_BOX (rd->action_row), rd->forget_btn,
                            FALSE, FALSE, 0);
    }

    /* ---- confirmación de "olvidar" ---- */
    rd->confirm_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_no_show_all (rd->confirm_box, TRUE);
    gtk_widget_hide (rd->confirm_box);
    gtk_box_pack_start (GTK_BOX (expand_box), rd->confirm_box, FALSE, FALSE, 0);
    {
        GtkWidget *q = gtk_label_new (_("Forget this device?"));
        gtk_label_set_xalign    (GTK_LABEL (q), 0.0);
        gtk_label_set_line_wrap (GTK_LABEL (q), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (q), 34);
        gtk_box_pack_start (GTK_BOX (rd->confirm_box), q, FALSE, FALSE, 0);

        GtkWidget *hint = gtk_label_new (
            _("You will have to pair it again to use it."));
        gtk_label_set_xalign    (GTK_LABEL (hint), 0.0);
        gtk_label_set_line_wrap (GTK_LABEL (hint), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (hint), 34);
        gtk_style_context_add_class (gtk_widget_get_style_context (hint),
                                     "dim-label");
        gtk_box_pack_start (GTK_BOX (rd->confirm_box), hint, FALSE, FALSE, 0);

        GtkWidget *btns = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_pack_start (GTK_BOX (rd->confirm_box), btns, FALSE, FALSE, 0);

        GtkWidget *cancel = gtk_button_new_with_label (_("Cancel"));
        g_signal_connect (cancel, "clicked",
                          G_CALLBACK (on_forget_cancel_clicked), rd);
        gtk_box_pack_start (GTK_BOX (btns), cancel, FALSE, FALSE, 0);

        GtkWidget *confirm = gtk_button_new_with_label (_("Forget"));
        gtk_style_context_add_class (gtk_widget_get_style_context (confirm),
                                     "destructive-action");
        g_signal_connect (confirm, "clicked",
                          G_CALLBACK (on_forget_confirm_clicked), rd);
        gtk_box_pack_start (GTK_BOX (btns), confirm, FALSE, FALSE, 0);
    }

    /* ---- "enviar archivo" ----
     *
     * Solo para aparatos que anuncian que saben recibir archivos. Unos
     * auriculares o un mouse no lo anuncian, y mostrarles la acción sería
     * ofrecer algo que siempre falla.
     *
     * Contra conocida: varios teléfonos publican ese perfil recién después
     * de emparejarse, así que la acción puede aparecer recién ahí. */
    if (bt_obex_device_can_receive (dev->uuids)) {
        rd->send_btn = gtk_button_new_with_label (_("Send file…"));
        gtk_widget_set_halign (rd->send_btn, GTK_ALIGN_START);
        g_signal_connect (rd->send_btn, "clicked",
                          G_CALLBACK (on_send_file_clicked), rd);
        gtk_box_pack_start (GTK_BOX (expand_box), rd->send_btn,
                            FALSE, FALSE, 0);
    }

    /* ---- "conectar automáticamente" (Trusted) ---- */
    if (dev->paired) {
        rd->trust_check = gtk_check_button_new_with_label (
                              _("Connect automatically"));
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (rd->trust_check),
                                      dev->trusted);
        rd->trust_handler = g_signal_connect (rd->trust_check, "toggled",
                                              G_CALLBACK (on_trust_toggled), rd);
        gtk_box_pack_start (GTK_BOX (expand_box), rd->trust_check,
                            FALSE, FALSE, 0);
    }

    /* ---- botón Detalles ---- */
    rd->details_btn = gtk_button_new ();
    gtk_button_set_relief (GTK_BUTTON (rd->details_btn), GTK_RELIEF_NONE);
    {
        GtkWidget *dbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *dlbl = gtk_label_new (_("Details"));
        rd->details_arrow = gtk_image_new_from_icon_name ("pan-down-symbolic",
                                                          GTK_ICON_SIZE_MENU);
        gtk_box_pack_start (GTK_BOX (dbox), dlbl, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (dbox), rd->details_arrow, FALSE, FALSE, 0);
        gtk_container_add (GTK_CONTAINER (rd->details_btn), dbox);
    }
    gtk_widget_set_halign (rd->details_btn, GTK_ALIGN_START);
    g_signal_connect (rd->details_btn, "clicked",
                      G_CALLBACK (on_details_clicked), rd);
    gtk_box_pack_start (GTK_BOX (expand_box), rd->details_btn, FALSE, FALSE, 0);

    rd->details_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_no_show_all (rd->details_box, TRUE);
    gtk_widget_hide (rd->details_box);
    gtk_box_pack_start (GTK_BOX (expand_box), rd->details_box, FALSE, FALSE, 0);

    /* ---- datos y señales de la fila ---- */
    g_signal_connect (event_box, "button-press-event",
                      G_CALLBACK (on_row_clicked), rd);
    g_signal_connect (event_box, "key-press-event",
                      G_CALLBACK (on_row_key_press), rd);

    g_object_set_data_full (G_OBJECT (event_box), "row-data", rd,
                            (GDestroyNotify) row_data_free);
    g_object_set_data_full (G_OBJECT (event_box), "address",
                            g_strdup (dev->address), g_free);
    g_object_set_data_full (G_OBJECT (event_box), "device-path",
                            g_strdup (dev->object_path), g_free);
    g_object_set_data_full (G_OBJECT (event_box), "row-fp",
                            row_fingerprint (popup, dev), g_free);
    g_object_set_data (G_OBJECT (event_box), "subtitle-label", subtitle);
    g_object_set_data (G_OBJECT (event_box), "row-rssi",
                       GINT_TO_POINTER (dev->has_rssi ? (gint) dev->rssi : 0));
    g_object_set_data (G_OBJECT (event_box), "row-battery",
                       GINT_TO_POINTER (dev->battery));

    /* Si había una operación en curso para este aparato (la fila se recreó a
     * mitad de camino), dejar el botón coherente con esa realidad. */
    if (op_running_for (popup, dev->object_path)) {
        gtk_widget_set_sensitive (rd->primary_btn, FALSE);
        gtk_button_set_label (GTK_BUTTON (rd->primary_btn), _("Working…"));
    }

    return event_box;
}
