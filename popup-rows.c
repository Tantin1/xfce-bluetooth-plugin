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
    /* GTK mete un GtkViewport entre la lista y su contenido. Las
     * coordenadas respecto del viewport son las de la parte VISIBLE (ya
     * descuentan el desplazamiento): hay que medir contra el contenido.
     * Medido contra el viewport, con la lista corrida hacia abajo, abrir
     * una fila la llevaba hacia arriba y el panel quedaba tapado. */
    if (GTK_IS_VIEWPORT (child))
        child = gtk_bin_get_child (GTK_BIN (child));
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

/* Corrección de la lista con una referencia al expand: si la fila se
 * destruye antes de que llegue el momento libre, no queda un puntero suelto. */
static void
scroll_to_expand_later (GtkWidget *expand)
{
    g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, scroll_to_expand_idle,
                     g_object_ref (expand), g_object_unref);
}

/* La caja donde va el contenido del expand (dentro del revelador). Para el
 * código que agrega cosas al expand ya armado, como el aviso de error. */
GtkWidget *
expand_content_box (GtkWidget *expand)
{
    GtkWidget *content = expand
        ? g_object_get_data (G_OBJECT (expand), "bt-content") : NULL;
    return content ? content : expand;
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

/* ---------- despliegue animado ---------- */

#define EXPAND_ANIM_MS 150

/* La lista acompaña el despliegue. Calcularlo una sola vez no sirve: en el
 * primer cuadro el expand mide casi 0. Se corrige en cada cuadro mientras
 * dura (size-allocate de la caja envolvente) y una última vez al terminar. */
static void
expand_follow_on_alloc (GtkWidget *wrap, GdkRectangle *alloc, gpointer user_data)
{
    BtPopup *popup = user_data;
    (void) alloc;
    if (popup->current_expand_box != wrap) {   /* se cerró a mitad */
        g_signal_handlers_disconnect_by_func (wrap, expand_follow_on_alloc,
                                              user_data);
        return;
    }
    scroll_to_expand_later (wrap);              /* corre lo mínimo necesario */
}

static void
on_expand_revealed (GObject *revealer, GParamSpec *pspec, gpointer user_data)
{
    (void) pspec;
    if (!gtk_revealer_get_child_revealed (GTK_REVEALER (revealer)))
        return;
    GtkWidget *wrap = gtk_widget_get_parent (GTK_WIDGET (revealer));
    g_signal_handlers_disconnect_by_func (revealer, on_expand_revealed, user_data);
    g_signal_handlers_disconnect_by_func (wrap, expand_follow_on_alloc, user_data);
    scroll_to_expand_later (wrap);
}

/* ---------- pliegue animado ---------- */

/* El expand que se está plegando, si hay uno. Puntero débil: si un refresco
 * destruye la fila a mitad del pliegue, vuelve solo a NULL. */
static GtkWidget *collapsing_wrap = NULL;

static void on_expand_collapsed (GObject *revealer, GParamSpec *pspec,
                                 gpointer user_data);
static void on_collapsing_unmap (GtkWidget *revealer, gpointer user_data);

/* Termina de golpe el pliegue en curso: oculta la caja y deja el revelador
 * como siempre (abierto y sin transición). */
static void
expand_collapse_finish (void)
{
    GtkWidget *wrap = collapsing_wrap;
    if (!wrap)
        return;
    g_object_remove_weak_pointer (G_OBJECT (wrap), (gpointer *) &collapsing_wrap);
    collapsing_wrap = NULL;

    GtkRevealer *rev = g_object_get_data (G_OBJECT (wrap), "bt-revealer");
    if (rev) {
        g_signal_handlers_disconnect_by_func (rev, on_expand_collapsed, NULL);
        g_signal_handlers_disconnect_by_func (rev, on_collapsing_unmap, NULL);
    }
    gtk_widget_hide (wrap);
    if (rev) {
        gtk_revealer_set_transition_type (rev, GTK_REVEALER_TRANSITION_TYPE_NONE);
        gtk_revealer_set_reveal_child (rev, TRUE);
    }
}

static void
on_expand_collapsed (GObject *revealer, GParamSpec *pspec, gpointer user_data)
{
    (void) pspec; (void) user_data;
    if (gtk_revealer_get_child_revealed (GTK_REVEALER (revealer)))
        return;                                  /* todavía no terminó */
    expand_collapse_finish ();
}

/* El menú se cerró a mitad del pliegue: sin pantalla la animación no avanza. */
static void
on_collapsing_unmap (GtkWidget *revealer, gpointer user_data)
{
    (void) revealer; (void) user_data;
    expand_collapse_finish ();
}

/* Pliega el expand. Solo cuando el usuario toca la misma fila abierta (clic,
 * Enter o Espacio). Si abre otra fila, la anterior se va de golpe: nunca dos
 * animaciones a la vez, porque la de arriba correría a la de abajo mientras
 * crece y se vería un temblor. */
static void
expand_hide_animated (RowData *rd)
{
    GtkWidget   *wrap = rd->expand_box;
    GtkRevealer *rev  = g_object_get_data (G_OBJECT (wrap), "bt-revealer");

    if (!rev || !gtk_widget_get_mapped (wrap) || !popup_anim_enabled (rd->popup)) {
        gtk_widget_hide (wrap);
        return;
    }
    expand_collapse_finish ();                   /* otro que se estuviera plegando */

    /* Si se estaba desplegando, ya no hay que seguirlo. */
    g_signal_handlers_disconnect_by_func (wrap, expand_follow_on_alloc, rd->popup);
    g_signal_handlers_disconnect_by_func (rev, on_expand_revealed, rd->popup);

    collapsing_wrap = wrap;
    g_object_add_weak_pointer (G_OBJECT (wrap), (gpointer *) &collapsing_wrap);
    g_signal_connect (rev, "notify::child-revealed",
                      G_CALLBACK (on_expand_collapsed), NULL);
    g_signal_connect (rev, "unmap", G_CALLBACK (on_collapsing_unmap), NULL);
    gtk_revealer_set_transition_type (rev, GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_transition_duration (rev, EXPAND_ANIM_MS);
    gtk_revealer_set_reveal_child (rev, FALSE);
}

/* ---------- secciones dentro del expand («Detalles») ----------
 *
 * Cada sección va dentro de su propio revelador, normalmente abierto y sin
 * transición. La visibilidad sigue siendo la del widget de la sección: los
 * chequeos de siempre no cambian, salvo el del botón que la abre y cierra,
 * que usa sub_is_open. Datos en el revelador: "bt-wrap" (la caja envolvente
 * del expand, para que la lista acompañe) y "bt-closing" (se está plegando). */

static GtkWidget *
sub_wrap (GtkWidget *child, GtkWidget *expand_wrap)
{
    GtkWidget *rev = gtk_revealer_new ();
    gtk_revealer_set_transition_type (GTK_REVEALER (rev),
                                      GTK_REVEALER_TRANSITION_TYPE_NONE);
    gtk_revealer_set_reveal_child (GTK_REVEALER (rev), TRUE);
    gtk_container_add (GTK_CONTAINER (rev), child);
    gtk_widget_show (rev);
    g_object_set_data (G_OBJECT (child), "bt-sub-revealer", rev);
    g_object_set_data (G_OBJECT (rev), "bt-wrap", expand_wrap);
    return rev;
}

static gboolean
sub_is_open (GtkWidget *child)
{
    GtkWidget *rev = g_object_get_data (G_OBJECT (child), "bt-sub-revealer");
    return gtk_widget_get_visible (child) &&
           !(rev && g_object_get_data (G_OBJECT (rev), "bt-closing"));
}

static void on_sub_revealed (GObject *rev, GParamSpec *pspec, gpointer user_data);
static void on_sub_unmap    (GtkWidget *rev, gpointer user_data);

/* Cancela un despliegue que todavía espera su primer cuadro. */
static void
sub_cancel_start (GtkRevealer *rev)
{
    guint id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (rev), "bt-start-tick"));
    if (id) {
        gtk_widget_remove_tick_callback (GTK_WIDGET (rev), id);
        g_object_set_data (G_OBJECT (rev), "bt-start-tick", NULL);
    }
}

/* Deja el revelador como siempre: abierto, sin transición, sin avisos. */
static void
sub_reset (GtkRevealer *rev, BtPopup *popup)
{
    GtkWidget *wrap = g_object_get_data (G_OBJECT (rev), "bt-wrap");
    sub_cancel_start (rev);
    g_signal_handlers_disconnect_by_func (rev, on_sub_revealed, popup);
    g_signal_handlers_disconnect_by_func (rev, on_sub_unmap, popup);
    if (wrap)
        g_signal_handlers_disconnect_by_func (wrap, expand_follow_on_alloc, popup);
    g_object_set_data (G_OBJECT (rev), "bt-closing", NULL);
    gtk_revealer_set_transition_type (rev, GTK_REVEALER_TRANSITION_TYPE_NONE);
    gtk_revealer_set_reveal_child (rev, TRUE);
}

/* Oculta de golpe: cierres automáticos, o una sección que reemplaza a otra. */
static void
sub_hide_now (BtPopup *popup, GtkWidget *child)
{
    GtkRevealer *rev = g_object_get_data (G_OBJECT (child), "bt-sub-revealer");
    gtk_widget_hide (child);
    if (rev)
        sub_reset (rev, popup);
}

/* Fin de una transición. La marca dice qué se esperaba: si coincide con el
 * estado del revelador, todavía no terminó. */
static void
on_sub_revealed (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    GtkRevealer *rev      = GTK_REVEALER (obj);
    BtPopup     *popup    = user_data;
    gboolean     closing  = g_object_get_data (obj, "bt-closing") != NULL;
    gboolean     revealed = gtk_revealer_get_child_revealed (rev);
    (void) pspec;

    if (closing && !revealed) {
        sub_hide_now (popup, gtk_bin_get_child (GTK_BIN (rev)));
    } else if (!closing && revealed) {
        GtkWidget *wrap = g_object_get_data (obj, "bt-wrap");
        sub_reset (rev, popup);
        if (wrap)
            scroll_to_expand_later (wrap);   /* una última corrección */
    }
}

/* El menú se cerró a mitad del pliegue. */
static void
on_sub_unmap (GtkWidget *rev, gpointer user_data)
{
    if (g_object_get_data (G_OBJECT (rev), "bt-closing"))
        sub_hide_now (user_data, gtk_bin_get_child (GTK_BIN (rev)));
}

/* Arranca el despliegue en el primer cuadro después de armar la sección.
 * Rellenar «Detalles» lee del bus y espera la respuesta; si la transición
 * arrancara antes, esa espera se comería la primera mitad (con BlueZ lento,
 * por ejemplo buscando dispositivos, saltaba de 0 a casi el alto final). */
static gboolean
sub_start_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
    GtkRevealer *rev = GTK_REVEALER (widget);
    (void) clock; (void) user_data;
    g_object_set_data (G_OBJECT (rev), "bt-start-tick", NULL);
    gtk_revealer_set_transition_type (rev, GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration (rev, EXPAND_ANIM_MS);
    gtk_revealer_set_reveal_child (rev, TRUE);
    return G_SOURCE_REMOVE;
}

/* Despliega la sección: misma secuencia que el expand (cerrar sin
 * transición, mostrar, abrir con transición). Si se estaba plegando, se
 * invierte desde donde está. La lista acompaña mientras dura. */
static void
sub_open (RowData *rd, GtkWidget *child)
{
    BtPopup     *popup = rd->popup;
    GtkRevealer *rev   = g_object_get_data (G_OBJECT (child), "bt-sub-revealer");
    GtkWidget   *wrap  = rev ? g_object_get_data (G_OBJECT (rev), "bt-wrap") : NULL;

    if (!rev || !gtk_widget_get_mapped (gtk_widget_get_parent (GTK_WIDGET (rev)))
        || !popup_anim_enabled (popup)) {
        if (rev)
            sub_reset (rev, popup);
        box_show_deep (child);
        return;
    }

    if (g_object_get_data (G_OBJECT (rev), "bt-closing")) {
        g_object_set_data (G_OBJECT (rev), "bt-closing", NULL);
        g_signal_handlers_disconnect_by_func (rev, on_sub_unmap, popup);
    } else {
        sub_reset (rev, popup);
        gtk_revealer_set_reveal_child (rev, FALSE);
    }
    box_show_deep (child);

    g_signal_handlers_disconnect_by_func (rev, on_sub_revealed, popup);
    g_signal_connect (rev, "notify::child-revealed",
                      G_CALLBACK (on_sub_revealed), popup);
    if (wrap) {
        g_signal_handlers_disconnect_by_func (wrap, expand_follow_on_alloc, popup);
        g_signal_connect (wrap, "size-allocate",
                          G_CALLBACK (expand_follow_on_alloc), popup);
    }
    sub_cancel_start (rev);
    g_object_set_data (G_OBJECT (rev), "bt-start-tick", GUINT_TO_POINTER (
        gtk_widget_add_tick_callback (GTK_WIDGET (rev), sub_start_tick, NULL, NULL)));
}

/* Pliega la sección y, al terminar, la oculta. */
static void
sub_close (RowData *rd, GtkWidget *child)
{
    BtPopup     *popup = rd->popup;
    GtkRevealer *rev   = g_object_get_data (G_OBJECT (child), "bt-sub-revealer");

    if (!rev || !gtk_widget_get_mapped (child) || !popup_anim_enabled (popup)) {
        sub_hide_now (popup, child);
        return;
    }
    GtkWidget *wrap = g_object_get_data (G_OBJECT (rev), "bt-wrap");
    sub_cancel_start (rev);
    /* Cerrado antes del primer cuadro del despliegue: el revelador sigue
     * cerrado y GTK no avisaría ningún fin. Se oculta de golpe. */
    if (!gtk_revealer_get_reveal_child (rev)) {
        sub_hide_now (popup, child);
        return;
    }
    if (wrap)   /* si se estaba desplegando, ya no hay que seguirlo */
        g_signal_handlers_disconnect_by_func (wrap, expand_follow_on_alloc, popup);

    g_object_set_data (G_OBJECT (rev), "bt-closing", GINT_TO_POINTER (1));
    g_signal_handlers_disconnect_by_func (rev, on_sub_revealed, popup);
    g_signal_handlers_disconnect_by_func (rev, on_sub_unmap, popup);
    g_signal_connect (rev, "notify::child-revealed",
                      G_CALLBACK (on_sub_revealed), popup);
    g_signal_connect (rev, "unmap", G_CALLBACK (on_sub_unmap), popup);
    gtk_revealer_set_transition_type (rev, GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_transition_duration (rev, EXPAND_ANIM_MS);
    gtk_revealer_set_reveal_child (rev, FALSE);
}

/* Muestra el expand deslizándolo hacia abajo. Solo lo usa el clic, Enter o
 * Espacio sobre la fila: todo lo demás que muestra un expand es instantáneo.
 *
 * Orden obligatorio: un GtkRevealer que no está mapeado salta al final sin
 * animar. Por eso primero se cierra SIN transición, después se muestra la
 * caja (con el menú a la vista queda mapeada en el acto) y recién ahí se
 * abre CON transición. Con las animaciones del sistema apagadas, GTK abre
 * de golpe y avisa en el acto: el aviso de fin deja todo consistente. */
static void
expand_show_animated (RowData *rd)
{
    GtkWidget   *wrap = rd->expand_box;
    GtkRevealer *rev  = g_object_get_data (G_OBJECT (wrap), "bt-revealer");
    BtPopup     *popup = rd->popup;

    if (!rev || !popup_anim_enabled (popup)) {
        expand_collapse_finish ();
        box_show_deep (wrap);
        g_signal_connect (wrap, "size-allocate",
                          G_CALLBACK (scroll_to_expand_on_alloc), NULL);
        return;
    }

    if (collapsing_wrap == wrap) {
        /* Se reabre la misma fila mientras se pliega: no cerrar el
         * revelador. Se sueltan los avisos del pliegue y se abre con
         * transición; GTK invierte la animación desde donde está. */
        g_object_remove_weak_pointer (G_OBJECT (wrap), (gpointer *) &collapsing_wrap);
        collapsing_wrap = NULL;
        g_signal_handlers_disconnect_by_func (rev, on_expand_collapsed, NULL);
        g_signal_handlers_disconnect_by_func (rev, on_collapsing_unmap, NULL);
    } else {
        /* Otra fila se estaba plegando: termina de golpe. */
        expand_collapse_finish ();
        gtk_revealer_set_transition_type (rev, GTK_REVEALER_TRANSITION_TYPE_NONE);
        gtk_revealer_set_reveal_child (rev, FALSE);
    }
    box_show_deep (wrap);

    g_signal_handlers_disconnect_by_func (wrap, expand_follow_on_alloc, popup);
    g_signal_handlers_disconnect_by_func (rev, on_expand_revealed, popup);
    g_signal_connect (wrap, "size-allocate",
                      G_CALLBACK (expand_follow_on_alloc), popup);
    g_signal_connect (rev, "notify::child-revealed",
                      G_CALLBACK (on_expand_revealed), popup);

    gtk_revealer_set_transition_type (rev, GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration (rev, EXPAND_ANIM_MS);
    gtk_revealer_set_reveal_child (rev, TRUE);
}

static void
row_toggle_expand (RowData *rd)
{
    BtPopup *popup = rd->popup;

    /* Ya estaba abierto: lo cierra el usuario, se pliega. */
    if (popup->current_expand_box == rd->expand_box) {
        popup->current_expand_box = NULL;
        expand_hide_animated (rd);
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
        sub_hide_now (popup, rd->details_box);
    if (rd->details_arrow)
        gtk_image_set_from_icon_name (GTK_IMAGE (rd->details_arrow),
                                      "pan-down-symbolic", GTK_ICON_SIZE_MENU);

    /* Primero marcarlo como el abierto: el seguimiento de la lista lo
     * compara en cada cuadro. */
    popup->current_expand_box = rd->expand_box;
    expand_show_animated (rd);

    if (rd->row_box)
        gtk_style_context_add_class (gtk_widget_get_style_context (rd->row_box),
                                     "bt-row-open");
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

    if (sub_is_open (rd->details_box)) {
        sub_close (rd, rd->details_box);
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
        sub_open (rd, rd->details_box);
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

    sub_open (rd, rd->details_box);
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

    /* ---- panel expandible ----
     *
     * fila → outer → [caja envolvente]    ← rd->expand_box ("el expand")
     *                   └ GtkRevealer      ← dato "bt-revealer"
     *                       └ expand_box   ← dato "bt-content" (el contenido)
     *
     * Todo el código sigue tratando a la caja envolvente como "el expand":
     * la muestra, la oculta, la compara con current_expand_box. El
     * revelador está normalmente abierto y sin transición, así que mostrar
     * u ocultar la caja sigue siendo instantáneo desde cualquier lugar; solo
     * expand_show_animated lo toca para desplegar. no_show_all va en la
     * caja envolvente (no en el contenido): así box_show_deep, al hacer
     * show_all del revelador, muestra el contenido entero. */
    expand_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start  (expand_box, 40);
    gtk_widget_set_margin_end    (expand_box, 10);
    gtk_widget_set_margin_bottom (expand_box, 8);

    GtkWidget *expand_rev  = gtk_revealer_new ();
    GtkWidget *expand_wrap = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_revealer_set_transition_type (GTK_REVEALER (expand_rev),
                                      GTK_REVEALER_TRANSITION_TYPE_NONE);
    gtk_revealer_set_reveal_child (GTK_REVEALER (expand_rev), TRUE);
    gtk_container_add (GTK_CONTAINER (expand_rev), expand_box);
    gtk_box_pack_start (GTK_BOX (expand_wrap), expand_rev, FALSE, FALSE, 0);
    gtk_widget_show (expand_box);
    gtk_widget_show (expand_rev);
    g_object_set_data (G_OBJECT (expand_wrap), "bt-revealer", expand_rev);
    g_object_set_data (G_OBJECT (expand_wrap), "bt-content",  expand_box);

    gtk_widget_set_no_show_all (expand_wrap, TRUE);
    gtk_widget_hide (expand_wrap);
    gtk_box_pack_start (GTK_BOX (outer), expand_wrap, FALSE, FALSE, 0);

    RowData *rd = g_new0 (RowData, 1);
    rd->popup        = popup;
    rd->expand_box   = expand_wrap;
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
        /* En la caja envolvente: es donde lo busca popup.c. */
        g_object_set_data (G_OBJECT (expand_wrap), "error-label", err);
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
    /* En su revelador (sub_wrap), que es lo que se empaqueta. */
    gtk_box_pack_start (GTK_BOX (expand_box), sub_wrap (rd->details_box, expand_wrap),
                        FALSE, FALSE, 0);

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
