/* popup-sections.c — secciones por adaptador, funciones de actualización,
 * refresco por diferencia, refresco coalescido y reconstrucción completa.
 * La maquinaria es la misma del plugin de red; lo que cambió es qué se
 * dibuja adentro. */

#include "popup-private.h"

/* ---------- orden de las filas ---------- */

/* Conectados primero, después emparejados, después el resto por señal.
 * Dentro de cada grupo, alfabético para que la lista no baile sola. */
static gint
device_compare (gconstpointer a, gconstpointer b)
{
    const BtDevice *da = a;
    const BtDevice *db = b;

    if (da->connected != db->connected)
        return da->connected ? -1 : 1;
    if (da->paired != db->paired)
        return da->paired ? -1 : 1;
    if (da->has_rssi != db->has_rssi)
        return da->has_rssi ? -1 : 1;
    if (da->has_rssi && da->rssi != db->rssi)
        return db->rssi - da->rssi;

    return g_utf8_collate (da->name ? da->name : "", db->name ? db->name : "");
}

/* Qué dispositivos merecen una fila.
 *
 * BlueZ guarda objetos de aparatos que vio alguna vez y todavía no descartó.
 * Si mostráramos todo, la lista se llenaría de fantasmas sin señal. Regla:
 * los emparejados siempre se ven (aunque estén apagados y fuera de alcance);
 * los demás, solo si los estamos viendo ahora mismo (tienen RSSI) o si están
 * conectados. */
static gboolean
device_is_listable (BtPopup *popup, const BtDevice *d)
{
    if (d->paired || d->connected)
        return TRUE;
    if (!d->has_rssi)
        return FALSE;

    /* Un aparato recién detectado aparece primero sin nombre: el advertising
     * inicial trae la dirección y el RSSI, y el nombre llega uno o dos
     * segundos después. Ocultarlo mientras tanto es justo lo peor, porque es
     * la ventana en la que el usuario está mirando la lista esperando que
     * aparezca. Por eso ahora se muestran igual, con la MAC de título hasta
     * que BlueZ resuelva el nombre; quien tenga demasiado ruido BLE alrededor
     * puede desmarcar la opción en Propiedades. */
    if (!popup->show_unnamed && !d->named)
        return FALSE;
    return TRUE;
}

/* ---------- textos de relleno ---------- */

static GtkWidget *
make_placeholder (const gchar *text)
{
    GtkWidget *l = gtk_label_new (text);
    gtk_label_set_xalign (GTK_LABEL (l), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (l), TRUE);
    gtk_widget_set_margin_start  (l, 14);
    gtk_widget_set_margin_end    (l, 14);
    gtk_widget_set_margin_top    (l, 8);
    gtk_widget_set_margin_bottom (l, 10);
    gtk_style_context_add_class (gtk_widget_get_style_context (l), "dim-label");
    g_object_set_data (G_OBJECT (l), "placeholder", GINT_TO_POINTER (1));
    return l;
}

static void
section_add_placeholder (BtPopup *popup, GtkWidget *section, gboolean powered)
{
    const gchar *txt = powered
        ? (popup->scanning ? _("Searching for devices…")
                           : _("No devices found. Press Search."))
        : _("This adapter is switched off.");
    GtkWidget *l = make_placeholder (txt);
    gtk_box_pack_start (GTK_BOX (section), l, FALSE, FALSE, 0);
    gtk_widget_show (l);
}

static void
section_clear_placeholders (GtkWidget *section)
{
    GList *kids = gtk_container_get_children (GTK_CONTAINER (section));
    for (GList *k = kids; k; k = k->next) {
        if (g_object_get_data (G_OBJECT (k->data), "placeholder"))
            gtk_widget_destroy (GTK_WIDGET (k->data));
    }
    g_list_free (kids);
}

/* ---------- sección por adaptador ---------- */

static GtkWidget *
make_adapter_section (BtPopup *popup, BtAdapter *ad, gboolean show_header)
{
    GtkWidget *section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    /* Etiqueta para que el refresco por diferencia pueda aparear cada
     * sección existente con su adaptador. */
    g_object_set_data_full (G_OBJECT (section), "adapter-path",
                            g_strdup (ad->object_path), g_free);

    if (popup->show_separators) {
        GtkWidget *mod_sep = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request (mod_sep, -1, 2);
        gtk_style_context_add_class (gtk_widget_get_style_context (mod_sep),
                                     "module-sep");
        gtk_box_pack_start (GTK_BOX (section), mod_sep, FALSE, FALSE, 0);
    }

    if (show_header) {
        GtkWidget *header_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_margin_start  (header_row, 12);
        gtk_widget_set_margin_end    (header_row, 12);
        gtk_widget_set_margin_top    (header_row, 8);
        gtk_widget_set_margin_bottom (header_row, 4);

        GtkWidget *header = gtk_label_new (NULL);
        gchar *markup = g_markup_printf_escaped ("<b><small>%s</small></b>",
                                                 ad->name);
        gtk_label_set_markup (GTK_LABEL (header), markup);
        g_free (markup);
        gtk_label_set_xalign (GTK_LABEL (header), 0.0);
        gtk_box_pack_start (GTK_BOX (header_row), header, TRUE, TRUE, 0);

        GtkWidget *ad_switch = gtk_switch_new ();
        gtk_switch_set_active (GTK_SWITCH (ad_switch), ad->powered);
        gtk_widget_set_valign (ad_switch, GTK_ALIGN_CENTER);

        AdapterSwitchData *d = g_new0 (AdapterSwitchData, 1);
        d->conn         = popup->conn;
        d->adapter_path = g_strdup (ad->object_path);
        g_object_set_data_full (G_OBJECT (ad_switch), "switch-data", d,
                                (GDestroyNotify) adapter_switch_data_free);
        g_signal_connect (ad_switch, "state-set",
                          G_CALLBACK (on_adapter_switch_toggled), d);

        gtk_box_pack_end (GTK_BOX (header_row), ad_switch, FALSE, FALSE, 0);
        popup->adapter_switches = g_slist_append (popup->adapter_switches,
                                                  ad_switch);

        gtk_box_pack_start (GTK_BOX (section), header_row, FALSE, FALSE, 0);
    }

    gint n_rows = 0;
    if (ad->powered) {
        GSList *devs = bt_get_devices (popup->conn, ad->object_path);
        devs = g_slist_sort (devs, device_compare);

        for (GSList *l = devs; l; l = l->next) {
            BtDevice *dev = l->data;
            if (!device_is_listable (popup, dev))
                continue;

            if (popup->show_separators) {
                GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
                gtk_box_pack_start (GTK_BOX (section), sep, FALSE, FALSE, 0);
            }
            GtkWidget *r = make_device_row (dev, popup);
            gtk_box_pack_start (GTK_BOX (section), r, FALSE, FALSE, 0);
            n_rows++;
        }
        bt_device_list_free (devs);
    }

    if (n_rows == 0)
        section_add_placeholder (popup, section, ad->powered);

    return section;
}

/* ---------- estado superior ---------- */

void
update_top_status (BtPopup *popup)
{
    if (!popup->ui_built)
        return;

    BtStatus *st = bt_get_status (popup->conn);

    /* --- switch global --- */
    if (popup->bt_switch) {
        g_signal_handler_block (popup->bt_switch, popup->bt_switch_handler);
        gtk_switch_set_active (GTK_SWITCH (popup->bt_switch), st->powered);
        gtk_widget_set_sensitive (popup->bt_switch, st->available);
        g_signal_handler_unblock (popup->bt_switch, popup->bt_switch_handler);
    }

    /* --- switch de visibilidad --- */
    if (popup->discoverable_switch) {
        gboolean disc = st->powered && bt_any_adapter_discoverable (popup->conn);
        g_signal_handler_block (popup->discoverable_switch,
                                popup->discoverable_handler);
        gtk_switch_set_active (GTK_SWITCH (popup->discoverable_switch), disc);
        gtk_widget_set_sensitive (popup->discoverable_switch, st->powered);
        g_signal_handler_unblock (popup->discoverable_switch,
                                  popup->discoverable_handler);
    }

    /* --- texto de estado --- */
    gchar *text;
    if (!st->available)
        text = g_strdup (_("No Bluetooth adapter"));
    else if (!st->powered)
        text = g_strdup (_("Bluetooth is off"));
    else if (st->n_connected == 0)
        text = g_strdup (_("No devices connected"));
    else if (st->n_connected == 1)
        text = g_strdup_printf (_("Connected to %s"), st->first_name);
    else
        text = g_strdup_printf (_("%d devices connected"), st->n_connected);

    if (popup->status_label)
        gtk_label_set_text (GTK_LABEL (popup->status_label), text);
    g_free (text);

    /* --- ícono --- */
    if (popup->status_icon) {
        const gchar *const *names =
            bt_status_icon_names (st->available, st->powered, st->n_connected);
        GIcon *gicon = g_themed_icon_new_from_names ((gchar **) names, -1);
        gtk_image_set_from_gicon (GTK_IMAGE (popup->status_icon), gicon,
                                  GTK_ICON_SIZE_MENU);
        g_object_unref (gicon);
    }

    /* --- botón Buscar --- */
    if (popup->refresh_button)
        gtk_widget_set_sensitive (popup->refresh_button,
                                  st->powered && !popup->scanning);

    bt_status_free (st);
}

/* ---------- refresco por diferencia ----------
 *
 * En vez de destruir y recrear TODAS las filas en cada señal de BlueZ (y son
 * muchas: cada RSSI nuevo dispara una), se compara la lista nueva contra las
 * filas existentes usando la dirección MAC como clave:
 *   - misma huella  → se reutiliza la fila (solo se actualiza señal/batería);
 *   - huella distinta → se recrea solo esa fila;
 *   - aparato que desapareció → se elimina su fila; nuevo → se agrega. */

/* Si hay una operación en curso cuyos widgets viven en una fila que está por
 * destruirse, descartar la entrada para no dejar punteros colgando. */
static void
drop_op_for_row (BtPopup *popup, const gchar *device_path)
{
    if (device_path)
        g_hash_table_remove (popup->ops_in_progress, device_path);
}

static void
forget_expand_if_inside (BtPopup *popup, GtkWidget *row)
{
    if (popup->current_expand_box &&
        gtk_widget_is_ancestor (popup->current_expand_box, row))
        popup->current_expand_box = NULL;
}

static void
diff_refresh_one_section (BtPopup *popup, GtkWidget *section, BtAdapter *ad)
{
    section_clear_placeholders (section);

    GSList *devs = NULL;
    if (ad->powered) {
        devs = bt_get_devices (popup->conn, ad->object_path);
        devs = g_slist_sort (devs, device_compare);
    }

    /* Inventario de filas existentes, clave = dirección MAC. El separador
     * propio de cada fila (el GtkSeparator inmediatamente anterior) se anota
     * en la fila. También se cuenta el prefijo de widgets fijos (separador de
     * módulo, encabezado) que precede a la zona de filas. */
    GHashTable *existing = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, NULL);
    GList     *kids = gtk_container_get_children (GTK_CONTAINER (section));
    gint       prefix = 0;
    gboolean   in_rows = FALSE;
    GtkWidget *pending_sep = NULL;

    for (GList *k = kids; k; k = k->next) {
        GtkWidget *w = k->data;
        if (GTK_IS_SEPARATOR (w)) {
            in_rows = TRUE;
            pending_sep = w;
            continue;
        }
        if (GTK_IS_EVENT_BOX (w)) {
            in_rows = TRUE;
            const gchar *addr = g_object_get_data (G_OBJECT (w), "address");
            if (addr) {
                g_object_set_data (G_OBJECT (w), "own-separator", pending_sep);
                g_hash_table_replace (existing, g_strdup (addr), w);
            }
            pending_sep = NULL;
            continue;
        }
        if (!in_rows)
            prefix++;
    }
    g_list_free (kids);

    /* Recorrer la lista nueva en orden, reutilizando o recreando. */
    gint pos = prefix;
    gint n_rows = 0;

    for (GSList *l = devs; l; l = l->next) {
        BtDevice *dev = l->data;
        if (!device_is_listable (popup, dev))
            continue;
        if (!dev->address)
            continue;

        GtkWidget *old_row = g_hash_table_lookup (existing, dev->address);
        gchar     *fp_new  = row_fingerprint (popup, dev);
        GtkWidget *row_w   = NULL;
        GtkWidget *sep_w   = NULL;

        if (old_row) {
            const gchar *fp_old = g_object_get_data (G_OBJECT (old_row), "row-fp");
            sep_w = g_object_get_data (G_OBJECT (old_row), "own-separator");
            if (fp_old && g_strcmp0 (fp_old, fp_new) == 0) {
                row_update_in_place (old_row, dev);
                row_w = old_row;
            } else {
                drop_op_for_row (popup, dev->object_path);
                forget_expand_if_inside (popup, old_row);
                gtk_widget_destroy (old_row);
                row_w = make_device_row (dev, popup);
                gtk_box_pack_start (GTK_BOX (section), row_w, FALSE, FALSE, 0);
                gtk_widget_show_all (row_w);
            }
            g_hash_table_remove (existing, dev->address);   /* usada */
        } else {
            row_w = make_device_row (dev, popup);
            gtk_box_pack_start (GTK_BOX (section), row_w, FALSE, FALSE, 0);
            gtk_widget_show_all (row_w);
        }

        if (popup->show_separators && !sep_w) {
            sep_w = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
            gtk_box_pack_start (GTK_BOX (section), sep_w, FALSE, FALSE, 0);
            gtk_widget_show (sep_w);
        }
        if (sep_w) {
            g_object_set_data (G_OBJECT (row_w), "own-separator", sep_w);
            gtk_box_reorder_child (GTK_BOX (section), sep_w, pos++);
        }
        gtk_box_reorder_child (GTK_BOX (section), row_w, pos++);

        n_rows++;
        g_free (fp_new);
    }

    /* Filas sobrantes: aparatos que ya no están. */
    GHashTableIter it;
    gpointer       k2, v2;
    g_hash_table_iter_init (&it, existing);
    while (g_hash_table_iter_next (&it, &k2, &v2)) {
        GtkWidget   *row = v2;
        const gchar *dp  = g_object_get_data (G_OBJECT (row), "device-path");
        drop_op_for_row (popup, dp);
        forget_expand_if_inside (popup, row);
        GtkWidget *sep = g_object_get_data (G_OBJECT (row), "own-separator");
        if (sep)
            gtk_widget_destroy (sep);
        gtk_widget_destroy (row);
    }

    g_hash_table_destroy (existing);
    bt_device_list_free (devs);

    if (n_rows == 0)
        section_add_placeholder (popup, section, ad->powered);
}

/* Intenta el refresco por diferencia de todas las secciones. Devuelve FALSE
 * si el conjunto (u orden) de adaptadores cambió: en ese caso el llamador
 * hace la reconstrucción completa. */
static gboolean
diff_refresh_sections (BtPopup *popup)
{
    GSList *adapters = bt_get_adapters (popup->conn);
    GList  *sections = gtk_container_get_children (
                           GTK_CONTAINER (popup->content_box));

    gboolean match = (g_list_length (sections) == g_slist_length (adapters));
    if (match) {
        GList  *s = sections;
        GSList *a = adapters;
        for (; s && a; s = s->next, a = a->next) {
            const gchar *sec_ad = g_object_get_data (G_OBJECT (s->data),
                                                     "adapter-path");
            if (!sec_ad ||
                g_strcmp0 (sec_ad, ((BtAdapter *) a->data)->object_path) != 0) {
                match = FALSE;
                break;
            }
        }
    }

    if (match) {
        GList  *s = sections;
        GSList *a = adapters;
        for (; s && a; s = s->next, a = a->next)
            diff_refresh_one_section (popup, GTK_WIDGET (s->data),
                                      (BtAdapter *) a->data);
    }

    g_list_free (sections);
    bt_adapter_list_free (adapters);
    return match;
}

void
update_adapters_section (BtPopup *popup)
{
    if (!popup->ui_built)
        return;

    /* Regla de oro heredada del plugin de red: si hay un expand abierto NO
     * reconstruimos la lista de filas. El usuario está interactuando con
     * ella, y una fila que se destruye debajo del cursor es peor que una
     * lista un poco desactualizada. */
    if (popup->current_expand_box)
        return;

    if (diff_refresh_sections (popup))
        return;

    /* ---- Reconstrucción completa ----
     * Solo cuando cambió el conjunto de adaptadores (o es la primera vez). */
    GList *kids = gtk_container_get_children (GTK_CONTAINER (popup->content_box));
    g_list_free_full (kids, (GDestroyNotify) gtk_widget_destroy);

    g_slist_free (popup->adapter_switches);
    popup->adapter_switches = NULL;

    GSList *adapters = bt_get_adapters (popup->conn);
    gint    n = g_slist_length (adapters);

    if (n == 0) {
        GtkWidget *l = make_placeholder (
            _("No Bluetooth adapter was found on this computer."));
        gtk_box_pack_start (GTK_BOX (popup->content_box), l, FALSE, FALSE, 0);
    }

    for (GSList *a = adapters; a; a = a->next) {
        GtkWidget *section = make_adapter_section (popup, a->data, n > 1);
        gtk_box_pack_start (GTK_BOX (popup->content_box), section,
                            FALSE, FALSE, 0);
    }
    bt_adapter_list_free (adapters);

    gtk_widget_show_all (popup->content_box);

    /* Tras reconstruir, los expand vuelven a no_show_all + hidden, así que
     * no queda ninguno abierto. */
    popup->current_expand_box = NULL;

    /* Las operaciones en curso ya no tienen sus widgets: se descartan. */
    g_hash_table_remove_all (popup->ops_in_progress);
}

/* ---------- refresco coalescido ---------- */

/* BlueZ es hablador: durante un descubrimiento manda un PropertiesChanged por
 * cada actualización de RSSI de cada aparato. Sin coalescer, cada una de esas
 * señales dispararía un refresco completo. */
static gboolean
refresh_ui_cb (gpointer user_data)
{
    BtPopup *popup = user_data;
    g_object_set_data (G_OBJECT (popup->window), "refresh-pending", NULL);

    if (!popup->ui_built) return G_SOURCE_REMOVE;
    if (!gtk_widget_get_visible (popup->window)) return G_SOURCE_REMOVE;

    /* Instantánea: todo el refresco lee de una sola foto del bus. */
    bt_cache_begin (popup->conn);
    update_top_status       (popup);
    update_adapters_section (popup);
    bt_cache_end ();

    return G_SOURCE_REMOVE;
}

void
schedule_refresh_ui (BtPopup *popup)
{
    if (!popup || !popup->window) return;
    if (!gtk_widget_get_visible (popup->window)) return;
    if (g_object_get_data (G_OBJECT (popup->window), "refresh-pending"))
        return;
    g_object_set_data (G_OBJECT (popup->window), "refresh-pending",
                       GINT_TO_POINTER (1));
    g_idle_add (refresh_ui_cb, popup);
}

void
on_bt_signal_popup (gpointer user_data)
{
    schedule_refresh_ui ((BtPopup *) user_data);
}

/* ---------- construcción inicial de la UI ---------- */

void
rebuild_ui (BtPopup *popup)
{
    GDBusConnection *conn = popup->conn;

    bt_cache_begin (conn);

    GList *children = gtk_container_get_children (GTK_CONTAINER (popup->content_box));
    g_list_free_full (children, (GDestroyNotify) gtk_widget_destroy);

    GList *top_children = gtk_container_get_children (GTK_CONTAINER (popup->top_box));
    g_list_free_full (top_children, (GDestroyNotify) gtk_widget_destroy);

    g_slist_free (popup->adapter_switches);
    popup->adapter_switches = NULL;
    popup->current_expand_box = NULL;

    /* ---- Barra superior fija ---- */
    GtkWidget *top_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start  (top_row, 6);
    gtk_widget_set_margin_end    (top_row, 12);
    gtk_widget_set_margin_top    (top_row, 8);
    gtk_widget_set_margin_bottom (top_row, 8);

    {
        const gchar *names[] = { "bluetooth-disabled-symbolic",
                                 "bluetooth-symbolic", "bluetooth", NULL };
        GIcon *gicon = g_themed_icon_new_from_names ((gchar **) names, -1);
        popup->status_icon = gtk_image_new_from_gicon (gicon, GTK_ICON_SIZE_MENU);
        g_object_unref (gicon);
    }
    gtk_widget_set_valign (popup->status_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom (popup->status_icon, 22);
    gtk_box_pack_start (GTK_BOX (top_row), popup->status_icon, FALSE, FALSE, 0);

    GtkWidget *center_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start (GTK_BOX (top_row), center_box, TRUE, TRUE, 0);

    /* Botón Buscar */
    popup->refresh_button = gtk_button_new ();
    {
        GtkWidget *rbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *ricon = gtk_image_new_from_icon_name ("view-refresh-symbolic",
                                                         GTK_ICON_SIZE_MENU);
        gtk_box_pack_start (GTK_BOX (rbox), ricon, FALSE, FALSE, 0);
        popup->refresh_label = gtk_label_new (popup->scanning ? _("Searching…")
                                                              : _("Search"));
        gtk_box_pack_start (GTK_BOX (rbox), popup->refresh_label, FALSE, FALSE, 0);
        gtk_container_add (GTK_CONTAINER (popup->refresh_button), rbox);
    }
    gtk_button_set_relief (GTK_BUTTON (popup->refresh_button), GTK_RELIEF_NONE);
    gtk_widget_set_valign (popup->refresh_button, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom (popup->refresh_button, 16);
    g_signal_connect (popup->refresh_button, "clicked",
                      G_CALLBACK (on_refresh_clicked), popup);
    gtk_box_pack_end (GTK_BOX (top_row), popup->refresh_button, FALSE, FALSE, 0);

    /* Fila Bluetooth + switch */
    GtkWidget *bt_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start (GTK_BOX (center_box), bt_row, FALSE, FALSE, 0);

    GtkWidget *bt_label = gtk_label_new (_("Bluetooth"));
    gtk_label_set_xalign (GTK_LABEL (bt_label), 0.0);
    gtk_box_pack_start (GTK_BOX (bt_row), bt_label, FALSE, FALSE, 0);

    popup->bt_switch = gtk_switch_new ();
    gtk_widget_set_valign (popup->bt_switch, GTK_ALIGN_CENTER);
    popup->bt_switch_handler = g_signal_connect (
        popup->bt_switch, "state-set",
        G_CALLBACK (on_bt_switch_toggled), popup);
    gtk_box_pack_start (GTK_BOX (bt_row), popup->bt_switch, FALSE, FALSE, 0);

    popup->status_label = gtk_label_new (NULL);
    gtk_label_set_xalign    (GTK_LABEL (popup->status_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (popup->status_label),
                             PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (popup->status_label), 28);
    gtk_box_pack_start (GTK_BOX (center_box), popup->status_label,
                        FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (popup->top_box), top_row, FALSE, FALSE, 0);

    popup->ui_built = TRUE;

    update_top_status       (popup);
    update_adapters_section (popup);

    bt_cache_end ();
}
