/* popup.c — núcleo del popup: ventana (crear/mostrar/ocultar/destruir),
 * posicionamiento y grab, operaciones en curso, helpers compartidos y
 * manejadores de la ventana. La fila de dispositivo vive en popup-rows.c;
 * las secciones y el refresco, en popup-sections.c; lo compartido se declara
 * en popup-private.h. */

#include "popup-private.h"

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif

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

/* Regla "imán", a lo largo del panel. La comparten X11 y Wayland:
 *
 *   1. Centrar la ventana sobre el botón.
 *   2. Si no entra en el área, correrla hasta que entre.
 *   3. Si contra un borde del área queda un hueco menor que media ventana,
 *      pegarla a ese borde (al más cercano, si pasa en los dos).
 *
 * Resultado con una pantalla de 1920 y una ventana de 400: si el botón está
 * a menos de unos 400 píxeles de un extremo, la ventana se pega a ese
 * extremo; en el medio, queda centrada sobre el botón. Así no quedan esos
 * huecos chicos contra el borde que parecen un error de cálculo.
 *
 * Todo en una sola dimensión: area_* es el tramo disponible (pantalla útil
 * en X11, el panel en Wayland), plugin_* el botón y popup_len el largo de la
 * ventana en esa dirección. Devuelve dónde empieza la ventana. */
static gint
place_along_panel (gint area_start, gint area_len,
                   gint plugin_start, gint plugin_len, gint popup_len)
{
    gint area_end = area_start + area_len;
    gint half     = popup_len / 2;
    gint pos      = plugin_start + plugin_len / 2 - half;

    if (pos + popup_len > area_end)
        pos = area_end - popup_len;
    if (pos < area_start)
        pos = area_start;

    gint gap_start = pos - area_start;
    gint gap_end   = area_end - (pos + popup_len);

    if (gap_start < half && gap_start <= gap_end)
        pos = area_start;
    else if (gap_end < half)
        pos = area_end - popup_len;

    /* Ventana más larga que el área: que al menos se vea el principio. */
    return MAX (pos, area_start);
}

/* Estilo "flotante": con esquinas redondeadas, la ventana queda despegada
 * esta distancia del panel y de los bordes de la pantalla, la misma en todas
 * las direcciones (un hueco en una sola dirección parecería un error de
 * cálculo). Sin redondeo, pegada como siempre. */
#define POPUP_FLOAT_GAP 6

/* La decide popup_update_rounding al abrir, según el estilo elegido en
 * Propiedades y el radio del tema. */
static gint
popup_float_gap (BtPopup *popup)
{
    return popup->floating ? POPUP_FLOAT_GAP : 0;
}

static gboolean
panel_is_vertical (XfcePanelPlugin *plugin)
{
    GtkArrowType arrow = xfce_panel_plugin_arrow_type (plugin);
    return arrow == GTK_ARROW_LEFT || arrow == GTK_ARROW_RIGHT;
}

static void
position_popup (BtPopup *popup, XfcePanelPlugin *plugin, GtkWidget *button)
{
    GdkWindow      *gdk_win;
    GdkMonitor     *monitor;
    GdkRectangle    wa;
    GtkRequisition  preferred;
    gint bx, by, bw, bh, pw, ph, x, y;

    gdk_win = gtk_widget_get_window (button);
    if (!gdk_win)
        return;
    gdk_window_get_origin (gdk_win, &bx, &by);
    /* El origen es el de la ventana del botón; si el botón no tiene ventana
     * propia, falta sumarle su lugar dentro de ella. */
    if (!gtk_widget_get_has_window (button)) {
        GtkAllocation al;
        gtk_widget_get_allocation (button, &al);
        bx += al.x;
        by += al.y;
    }
    bw = gtk_widget_get_allocated_width  (button);
    bh = gtk_widget_get_allocated_height (button);

    gtk_widget_get_preferred_size (popup->window, NULL, &preferred);
    pw = preferred.width;
    ph = preferred.height;
    popup->placed_w = pw;
    popup->placed_h = ph;

    monitor = gdk_display_get_monitor_at_point (
                  gtk_widget_get_display (button), bx + bw / 2, by + bh / 2);
    if (!monitor)
        return;
    gdk_monitor_get_workarea (monitor, &wa);

    gint g = popup_float_gap (popup);   /* separación del estilo flotante */

    if (!panel_is_vertical (plugin)) {
        x = place_along_panel (wa.x + g, wa.width - 2 * g, bx, bw, pw);
        /* Del lado del panel que da a la pantalla; si no entra, del otro. */
        if (xfce_panel_plugin_arrow_type (plugin) == GTK_ARROW_UP) {
            y = by - ph - g;                           /* panel abajo */
            if (y < wa.y)
                y = by + bh + g;
        } else {
            y = by + bh + g;                           /* panel arriba */
            if (y + ph > wa.y + wa.height)
                y = by - ph - g;
        }
        y = CLAMP (y, wa.y, MAX (wa.y, wa.y + wa.height - ph));
    } else {
        y = place_along_panel (wa.y + g, wa.height - 2 * g, by, bh, ph);
        if (xfce_panel_plugin_arrow_type (plugin) == GTK_ARROW_LEFT) {
            x = bx - pw - g;                           /* panel a la derecha */
            if (x < wa.x)
                x = bx + bw + g;
        } else {
            x = bx + bw + g;                           /* panel a la izquierda */
            if (x + pw > wa.x + wa.width)
                x = bx - pw - g;
        }
        x = CLAMP (x, wa.x, MAX (wa.x, wa.x + wa.width - pw));
    }

    gtk_window_move (GTK_WINDOW (popup->window), x, y);
}

/* ================================================================
 * Wayland
 *
 * Todo lo de X11 de arriba queda igual. En Wayland una ventana común no
 * elige su posición (gtk_window_move se ignora) y el acaparamiento funciona
 * distinto, así que la ventana emergente sigue otro camino:
 *
 *   1. Se hace "hija" del panel (transient_for) y se ubica junto al botón
 *      con gdk_window_move_to_rect: el compositor la pone ahí.
 *   2. El acaparamiento se pide ANTES de mostrarla y la muestra la función
 *      "preparar" (como hace GtkMenu). Si se pide después, no hay
 *      acaparamiento de verdad y la ventana no recibe el teclado (el campo
 *      de renombrar quedaría sordo).
 *   3. El compositor la cierra solo al hacer clic en OTRO programa. El panel
 *      es el mismo programa, así que el clic en el panel o en el botón se
 *      detecta a mano: durante el acaparamiento llega como si fuera en la
 *      ventana emergente, con coordenadas fuera de su área.
 *   4. Cuando el compositor la cierra, GTK la sigue creyendo visible: hay
 *      que ocultarla del todo para que el resto (búsqueda, señales, botón)
 *      se entere.
 * ================================================================ */

/* La ventana emergente abierta en Wayland, si hay una (ver
 * popup_close_for_dialog). En X11 queda siempre en NULL. */
static BtPopup *wl_open_popup = NULL;

static gboolean
popup_is_wayland (GtkWidget *w)
{
#ifdef GDK_WINDOWING_WAYLAND
    return GDK_IS_WAYLAND_DISPLAY (gtk_widget_get_display (w));
#else
    (void) w;
    return FALSE;
#endif
}

/* Clic durante el acaparamiento: ¿adentro o afuera? La ventana del evento no
 * alcanza (los clics en el panel también llegan a nombre nuestro), así que se
 * llevan las coordenadas hasta la ventana propia y se miran. */
static gboolean
wl_on_button_press (GtkWidget *widget, GdkEventButton *ev, BtPopup *popup)
{
    (void) widget;
    GdkWindow *own    = gtk_widget_get_window (popup->window);
    GdkWindow *win    = ev->window;
    gdouble    x      = ev->x, y = ev->y;
    gboolean   inside = FALSE;

    while (win != NULL && win != own) {
        gdk_window_coords_to_parent (win, x, y, &x, &y);
        win = gdk_window_get_effective_parent (win);
    }
    if (win == own)
        inside = x >= 0 && y >= 0 &&
                 x < gtk_widget_get_allocated_width  (popup->window) &&
                 y < gtk_widget_get_allocated_height (popup->window);

    WL_DEBUG ("wayland: clic %s (%.0f, %.0f) en ventana de %dx%d",
              inside ? "adentro" : "afuera", x, y,
              gtk_widget_get_allocated_width  (popup->window),
              gtk_widget_get_allocated_height (popup->window));

    if (inside)
        return GDK_EVENT_PROPAGATE;

    popup_hide (popup);
    /* Consumirlo: si el clic fue en el botón del panel, que no la vuelva a
     * abrir enseguida. */
    return GDK_EVENT_STOP;
}

/* El compositor la cerró (clic en otro programa) o rompió el acaparamiento.
 * Conectado una sola vez, al crearla; en X11 no hace nada.
 *
 * Probado con un compositor real: al cerrarla él, GTK 3 NO manda ni
 * "unmap-event" ni "grab-broken-event"; lo único que llega es el cambio de
 * estado de la ventana ("window-state-event") con la marca de retirada. Se
 * escuchan los tres por si otro compositor se porta distinto.
 *
 * Solo cuenta si la ventana de verdad ya no está a la vista: el aviso de
 * "dejó de mostrarse" de un cierre anterior puede llegar tarde, con la
 * ventana ya reabierta, y la cerraría de nuevo sin motivo. */
static gboolean
wl_on_popup_gone (GtkWidget *widget, GdkEvent *ev, BtPopup *popup)
{
    if (popup_is_wayland (widget) && gtk_widget_get_visible (widget) &&
        !gdk_window_is_visible (gtk_widget_get_window (widget))) {
        WL_DEBUG ("wayland: el compositor cerró la ventana (%s)",
                  ev->type == GDK_GRAB_BROKEN  ? "acaparamiento roto" :
                  ev->type == GDK_WINDOW_STATE ? "estado: retirada"
                                               : "dejó de mostrarse");
        popup_hide (popup);
    }
    return GDK_EVENT_PROPAGATE;
}

static void
wl_prepare_show (GdkSeat *seat, GdkWindow *window, gpointer data)
{
    (void) seat; (void) window;
    gtk_widget_show_all (GTK_WIDGET (data));
}

static void
popup_show_wayland (BtPopup *popup, XfcePanelPlugin *plugin, GtkWidget *button)
{
    GtkWidget *win      = popup->window;
    GtkWidget *toplevel = gtk_widget_get_toplevel (button);   /* el panel */
    gint       gap      = popup_float_gap (popup);   /* estilo flotante */

    if (GTK_IS_WINDOW (toplevel))
        gtk_window_set_transient_for (GTK_WINDOW (win), GTK_WINDOW (toplevel));
    gtk_widget_realize (win);

    /* Rectángulo del botón, medido desde el panel. */
    GdkRectangle rect = { 0, 0, 1, 1 };
    if (GTK_IS_WINDOW (toplevel))
        gtk_widget_translate_coordinates (button, toplevel, 0, 0,
                                          &rect.x, &rect.y);
    rect.width  = MAX (gtk_widget_get_allocated_width  (button), 1);
    rect.height = MAX (gtk_widget_get_allocated_height (button), 1);

    WL_DEBUG ("wayland: panel=%s flecha=%d botón=%d,%d %dx%d",
              G_OBJECT_TYPE_NAME (toplevel),
              (gint) xfce_panel_plugin_arrow_type (plugin),
              rect.x, rect.y, rect.width, rect.height);

    /* Regla "imán" (place_along_panel). En Wayland el programa no sabe dónde
     * está el panel en la pantalla, así que el área es el propio panel: es
     * exacto si el panel ocupa todo el ancho (o alto) y aproximado si es
     * corto. Al compositor se le pasa un rectángulo de 1 píxel donde tiene
     * que empezar la ventana; el "correr" de los anclajes queda como red de
     * seguridad si igual no entrara. */
    if (GTK_IS_WINDOW (toplevel)) {
        GtkRequisition pref;
        gtk_widget_get_preferred_size (win, NULL, &pref);
        if (!panel_is_vertical (plugin)) {
            rect.x = place_along_panel (gap, gtk_widget_get_allocated_width (toplevel) - 2 * gap,
                                        rect.x, rect.width, pref.width);
            rect.width = 1;
        } else {
            rect.y = place_along_panel (gap, gtk_widget_get_allocated_height (toplevel) - 2 * gap,
                                        rect.y, rect.height, pref.height);
            rect.height = 1;
        }
        WL_DEBUG ("wayland: imán: ventana %dx%d, empieza en %d,%d",
                  pref.width, pref.height, rect.x, rect.y);
    }

    /* Anclajes según dónde está el panel. Dar vuelta solo en el eje que cruza
     * el panel y correr solo a lo largo de él: con los dos ejes a la vez, si
     * no entra, primero la espeja y queda un hueco contra el borde. */
    GdkGravity     rect_anchor, win_anchor;
    GdkAnchorHints hints;
    switch (xfce_panel_plugin_arrow_type (plugin)) {
        case GTK_ARROW_UP:      /* panel abajo */
            rect_anchor = GDK_GRAVITY_NORTH_WEST;
            win_anchor  = GDK_GRAVITY_SOUTH_WEST;
            hints       = GDK_ANCHOR_FLIP_Y | GDK_ANCHOR_SLIDE_X;
            break;
        case GTK_ARROW_RIGHT:   /* panel a la izquierda */
            rect_anchor = GDK_GRAVITY_NORTH_EAST;
            win_anchor  = GDK_GRAVITY_NORTH_WEST;
            hints       = GDK_ANCHOR_FLIP_X | GDK_ANCHOR_SLIDE_Y;
            break;
        case GTK_ARROW_LEFT:    /* panel a la derecha */
            rect_anchor = GDK_GRAVITY_NORTH_WEST;
            win_anchor  = GDK_GRAVITY_NORTH_EAST;
            hints       = GDK_ANCHOR_FLIP_X | GDK_ANCHOR_SLIDE_Y;
            break;
        case GTK_ARROW_DOWN:    /* panel arriba */
        default:
            rect_anchor = GDK_GRAVITY_SOUTH_WEST;
            win_anchor  = GDK_GRAVITY_NORTH_WEST;
            hints       = GDK_ANCHOR_FLIP_Y | GDK_ANCHOR_SLIDE_X;
            break;
    }
    /* Separación del panel (estilo flotante), hacia el lado de la pantalla.
     * Si el compositor da vuelta la ventana, da vuelta también esto. */
    gint dx = 0, dy = 0;
    switch (xfce_panel_plugin_arrow_type (plugin)) {
        case GTK_ARROW_UP:    dy = -gap; break;   /* panel abajo */
        case GTK_ARROW_RIGHT: dx =  gap; break;   /* panel a la izquierda */
        case GTK_ARROW_LEFT:  dx = -gap; break;   /* panel a la derecha */
        case GTK_ARROW_DOWN:                      /* panel arriba */
        default:              dy =  gap; break;
    }
    gdk_window_move_to_rect (gtk_widget_get_window (win), &rect,
                             rect_anchor, win_anchor, hints, dx, dy);

    /* Acaparamiento ANTES de mostrar: la muestra wl_prepare_show. */
    GdkSeat *seat = gdk_display_get_default_seat (gtk_widget_get_display (win));
    GdkGrabStatus st = gdk_seat_grab (seat, gtk_widget_get_window (win),
                                      GDK_SEAT_CAPABILITY_ALL, TRUE,
                                      NULL, NULL, wl_prepare_show, win);
    WL_DEBUG ("wayland: acaparamiento %s (estado %d)",
              st == GDK_GRAB_SUCCESS ? "conseguido" : "FALLÓ", (gint) st);
    if (!gtk_widget_get_visible (win))
        gtk_widget_show_all (win);

    if (st == GDK_GRAB_SUCCESS) {
        /* Mismo grupo de ventanas que el panel, para que el acaparamiento de
         * GTK abarque también sus clics. */
        if (GTK_IS_WINDOW (toplevel))
            gtk_window_group_add_window (
                gtk_window_get_group (GTK_WINDOW (toplevel)), GTK_WINDOW (win));
        gtk_grab_add (win);
        WL_DEBUG ("wayland: acaparamiento de GTK %s",
                  gtk_widget_has_grab (win) ? "activo" : "INACTIVO");
        popup->press_handler =
            g_signal_connect (win, "button-press-event",
                              G_CALLBACK (wl_on_button_press), popup);
    } else {
        /* Sin acaparamiento: cerrar al perder el foco, como reserva. */
        popup->press_handler =
            g_signal_connect (win, "focus-out-event",
                              G_CALLBACK (on_focus_out), popup);
    }
    wl_open_popup = popup;
}

void
popup_close_for_dialog (void)
{
    if (wl_open_popup != NULL) {
        WL_DEBUG ("wayland: se cierra la ventana para mostrar un cartel");
        popup_hide (wl_open_popup);
    }
}

/* ================================================================
 * Aspecto del tema
 *
 * Todo el contenido va adentro de una caja que el tema ve como un popover
 * (BtPopupFrame): le pone el fondo, el borde, las esquinas y también los
 * colores de letra que usa en sus popovers. Así la ventana se ve como los
 * popovers de cada tema, sin un radio ni colores inventados.
 *
 * Para que las esquinas redondeadas se vean, la ventana necesita
 * transparencia, y eso solo funciona con compositor (siempre en Wayland; en
 * X11 cuando el de xfwm4 u otro está activo). Sin compositor la ventana
 * queda opaca y la caja con esquinas rectas: con transparencia las esquinas
 * saldrían negras.
 *
 * Por qué el contenido va ADENTRO y no se pinta solo el fondo detrás: un
 * tema puede pintar sus popovers de un color muy distinto al de sus ventanas
 * (Numix los pinta oscuros sobre ventanas claras). Con el contenido afuera,
 * la letra seguía siendo la de una ventana común: medido, contraste 1,3 en
 * Numix, ilegible. Adentro, cada cosa toma el color del popover. El costo:
 * las reglas del tema para "lo que está dentro de un popover" también valen
 * acá (Arc, por ejemplo, aclara los separadores), que es justamente el
 * aspecto que el tema quiere para sus popovers.
 *
 * La ventana mide lo mismo que antes (el borde y el relleno del tema se
 * descuentan de la lista) y está en el mismo lugar: no cambian la posición,
 * el acaparamiento ni el clic afuera. Sin sombra: quedaría cortada en el
 * borde de la ventana.
 * ================================================================ */

/* Caja con NOMBRE de componente "popover". Los temas escriben sus reglas para
 * el componente ("popover.background"), no para una clase: medido en
 * Adwaita, Greybird y Arc, una caja común con la clase .popover o .menu da
 * radio 0, y esta da el mismo radio, borde y fondo que un popover real. */
typedef GtkBox      BtPopupFrame;
typedef GtkBoxClass BtPopupFrameClass;
G_DEFINE_TYPE (BtPopupFrame, bt_popup_frame, GTK_TYPE_BOX)

static void
bt_popup_frame_class_init (BtPopupFrameClass *klass)
{
    gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "popover");
}

static void
bt_popup_frame_init (BtPopupFrame *self)
{
    (void) self;
}

/* Segundo marco, con NOMBRE de componente "menu", para la ventana pegada al
 * panel: así toma los colores de los menús del tema, igual que los plugins
 * nativos de Xfce (que son GtkMenu). En casi todos los temas coinciden con
 * los del popover, pero no en Greybird-dark: popover #676d6e (contraste 4,5
 * con el texto) contra menú #303133 (11,2). Medido en el plugin de
 * extraíbles. Sin la clase "background": en Greybird claro pisaría el
 * blanco del menú con el gris de las ventanas. */
typedef GtkBox      BtPopupMenuFrame;
typedef GtkBoxClass BtPopupMenuFrameClass;
G_DEFINE_TYPE (BtPopupMenuFrame, bt_popup_menu_frame, GTK_TYPE_BOX)

static void
bt_popup_menu_frame_class_init (BtPopupMenuFrameClass *klass)
{
    gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "menu");
}

static void
bt_popup_menu_frame_init (BtPopupMenuFrame *self)
{
    (void) self;
}

/* Lo que ocupan el borde y el relleno del tema alrededor del contenido. */
static void
popup_frame_extra (BtPopup *popup, gint *w, gint *h)
{
    GtkStyleContext *ctx = gtk_widget_get_style_context (popup->frame);
    GtkStateFlags    st  = gtk_style_context_get_state (ctx);
    GtkBorder        b, p;

    gtk_style_context_get_border  (ctx, st, &b);
    gtk_style_context_get_padding (ctx, st, &p);
    *w = b.left + b.right  + p.left + p.right;
    *h = b.top  + b.bottom + p.top  + p.bottom;
}

/* Pasa la ventana a un modo o al otro. Cambiar la transparencia exige que la
 * ventana no esté realizada; quien llama se ocupa de que esté oculta. */
static void
popup_apply_rounding (BtPopup *popup, gboolean rounded)
{
    GtkWidget       *win    = popup->window;
    GdkScreen       *screen = gtk_widget_get_screen (win);
    GtkStyleContext *wctx   = gtk_widget_get_style_context (win);
    GdkVisual       *visual = rounded ? gdk_screen_get_rgba_visual (screen)
                                      : NULL;

    if (gtk_widget_get_realized (win))
        gtk_widget_unrealize (win);
    gtk_widget_set_visual (win, visual != NULL
                                ? visual
                                : gdk_screen_get_system_visual (screen));

    popup->rounded = rounded;
    if (rounded)
        gtk_style_context_add_class    (wctx, "bt-rounded");
    else
        gtk_style_context_remove_class (wctx, "bt-rounded");
}

/* ¿Corresponde redondear ahora? Si cambió y la ventana está oculta, se
 * aplica; si está abierta, queda para la próxima apertura (popup_show
 * vuelve a llamar acá antes de mostrarla). */
/* Esquinas del marco de menú. Los menús casi nunca tienen radio, así que se
 * le pone el que el tema da a sus POPOVERS, solo en flotante con
 * transparencia (con la ventana opaca las esquinas se pintarían con el fondo
 * de la ventana). Un proveedor CSS por proceso: el tema es el mismo para
 * todos. */
static void
popup_set_round (BtPopup *popup, gint radius)
{
    static GtkCssProvider *provider = NULL;
    GtkStyleContext *fctx = gtk_widget_get_style_context (popup->frame);

    if (radius <= 0) {
        gtk_style_context_remove_class (fctx, "bt-round");
        return;
    }
    if (provider == NULL) {
        provider = gtk_css_provider_new ();
        gtk_style_context_add_provider_for_screen (
            gtk_widget_get_screen (popup->window), GTK_STYLE_PROVIDER (provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gchar *css = g_strdup_printf (
        "menu.bt-popup-frame.bt-round { border-radius: %dpx; }", radius);
    gtk_css_provider_load_from_data (provider, css, -1, NULL);
    g_free (css);
    gtk_style_context_add_class (fctx, "bt-round");
}

/* Modo automático: desde cuántos píxeles de radio el popover del tema se ve
 * redondeado. Casi ningún tema da 0 (Numix 2, Arc 3, Qogir 3, Greybird 5), y
 * a esos tamaños las esquinas se ven rectas; Adwaita da 9. Medido en el
 * plugin de red. */
#define POPUP_ROUND_MIN 6

static void
popup_update_rounding (BtPopup *popup)
{
    if (gtk_widget_get_visible (popup->window))
        return;

    GdkScreen       *screen = gtk_widget_get_screen (popup->window);
    /* Radio de los POPOVERS del tema, leído del popover que nunca se muestra
     * (el marco de menú casi siempre da 0 y Automático no flotaría nunca). */
    GtkStyleContext *fctx   = gtk_widget_get_style_context (popup->frame_popover);
    gint radius = 0;
    gtk_style_context_get (fctx, gtk_style_context_get_state (fctx),
                           "border-radius", &radius, NULL);

    gboolean composited = gdk_screen_is_composited (screen) &&
                          gdk_screen_get_rgba_visual (screen) != NULL;

    switch (popup->style) {
    case BT_POPUP_STYLE_FLOATING: popup->floating = TRUE;  break;
    case BT_POPUP_STYLE_ATTACHED: popup->floating = FALSE; break;
    case BT_POPUP_STYLE_AUTO:
    default:
        popup->floating = composited && radius >= POPUP_ROUND_MIN;
        break;
    }

    /* "Pegada" implica siempre sin transparencia y con esquinas rectas: con
     * un tema de 3 px no queda pegada con puntas apenas redondas. */
    gboolean want = popup->floating && composited;
    if (want != popup->rounded)
        popup_apply_rounding (popup, want);

    /* Los colores son siempre los del menú; el estilo solo decide la
     * separación y las esquinas: redondeadas con el radio de los popovers
     * del tema si flota con transparencia, rectas en cualquier otro caso. */
    popup_set_round (popup, want ? radius : 0);
}

/* GTK no actualiza el estilo de los widgets de una ventana OCULTA (lo hace
 * al mostrarla); los widgets NUEVOS sí nacen con el tema actual. Con la
 * ventana cerrada, se marca para rearmar al abrir: si no, la primera
 * apertura tras cambiar el tema se medía con el tamaño del tema anterior y
 * quedaba mal ubicada (visto en el plugin de red). Con la ventana abierta GTK
 * la actualiza en el momento. */
static void
on_theme_changed (GObject *settings, GParamSpec *pspec, BtPopup *popup)
{
    (void) settings; (void) pspec;
    if (!gtk_widget_get_visible (popup->window))
        popup->theme_dirty = TRUE;
}

static GtkWidget *
popup_frame_new (void)
{
    GtkWidget *frame = g_object_new (bt_popup_frame_get_type (),
                                     "orientation", GTK_ORIENTATION_VERTICAL,
                                     NULL);
    gtk_style_context_add_class (gtk_widget_get_style_context (frame),
                                 "background");
    gtk_style_context_add_class (gtk_widget_get_style_context (frame),
                                 "bt-popup-frame");
    return frame;
}

static GtkWidget *
popup_menu_frame_new (void)
{
    GtkWidget *frame = g_object_new (bt_popup_menu_frame_get_type (),
                                     "orientation", GTK_ORIENTATION_VERTICAL,
                                     NULL);
    gtk_style_context_add_class (gtk_widget_get_style_context (frame),
                                 "bt-popup-frame");
    return frame;
}

/* El marco visible (menú) y el popover que nunca se muestra. El de menú lo
 * mantiene vivo la ventana; el popover, su referencia propia. */
static void
popup_create_frames (BtPopup *popup)
{
    popup->frame_popover = g_object_ref_sink (popup_frame_new ());
    popup->frame         = popup_menu_frame_new ();
}

/* Tras un cambio de tema: marcos nuevos (el borde y el relleno del de menú se
 * usan para medir, y en los viejos quedarían los del tema anterior). El
 * contenido pasa al nuevo sin rearmarse acá. Las esquinas y la transparencia
 * las vuelve a poner popup_update_rounding, justo después. */
static void
popup_recreate_frame (BtPopup *popup)
{
    g_object_ref (popup->outer_box);
    gtk_container_remove (GTK_CONTAINER (popup->frame), popup->outer_box);
    gtk_container_remove (GTK_CONTAINER (popup->window), popup->frame);
    gtk_widget_destroy (popup->frame_popover);
    g_object_unref (popup->frame_popover);

    popup_create_frames (popup);
    gtk_container_add (GTK_CONTAINER (popup->window), popup->frame);
    gtk_container_add (GTK_CONTAINER (popup->frame), popup->outer_box);
    g_object_unref (popup->outer_box);
}

static void
on_composited_changed (GdkScreen *screen, BtPopup *popup)
{
    (void) screen;
    popup_update_rounding (popup);
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
    const gchar *names[5];
    gint n = 0;

    if (icon_name && *icon_name)
        names[n++] = icon_name;
    /* Respaldo para los nombres finos de audio (ver av_icon_from_class en
     * bt-dbus.c): si el tema no tiene, por ejemplo, "audio-speakers", queda
     * el genérico de siempre antes que el logo de Bluetooth. */
    if (icon_name && g_str_has_prefix (icon_name, "audio-") &&
        g_strcmp0 (icon_name, "audio-card") != 0)
        names[n++] = "audio-card";
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
    gint w, h, ox, oy;
    gtk_window_get_size (GTK_WINDOW (popup->window), &w, &h);

    /* Adentro o afuera con las coordenadas de PANTALLA del clic. En X11 el
     * botón del panel vive en otra ventanita del mismo proceso (la del
     * complemento, dentro del panel); con el puntero acaparado, GTK le pasa
     * a esta ventana el clic hecho sobre el botón, pero con las coordenadas
     * medidas desde la ventanita del botón (15,15, por ejemplo): caía
     * "adentro" y un segundo clic en el botón no la cerraba. Visto en un
     * panel real con xfwm4; fuera del panel no pasa. */
    gdk_window_get_origin (gtk_widget_get_window (popup->window), &ox, &oy);
    gdouble x = event->x_root - ox;
    gdouble y = event->y_root - oy;

    /* Click fuera de la ventana: cerrar (y consumirlo: si fue en el botón
     * del panel, que no la vuelva a abrir). */
    if (x < 0 || y < 0 || x > w || y > h) {
        popup_hide (popup);
        return TRUE;
    }
    return FALSE;
}

/* ---------- API pública ---------- */

/* X11: cuando la ventana aparece, GTK le da el tamaño real; si no coincide
 * con el usado para ubicarla (pasaba en la primera apertura tras cambiar el
 * tema), se la vuelve a ubicar. En el uso normal coinciden y no hace nada, y
 * no se repite: position_popup guarda la medida nueva, que ya es la real.
 * En Wayland no: mover una ventana emergente abierta puede cerrarla o
 * hacerle perder la captura (ahí lo cubre el rearmado de on_theme_changed). */
static void
on_popup_size_allocate (GtkWidget *win, GtkAllocation *alloc, BtPopup *popup)
{
    (void) alloc;
    if (!gtk_widget_get_visible (win) || popup->button == NULL ||
        popup->panel_plugin == NULL || popup_is_wayland (win))
        return;

    gint w, h;
    gtk_window_get_size (GTK_WINDOW (win), &w, &h);
    if (w != popup->placed_w || h != popup->placed_h)
        position_popup (popup, XFCE_PANEL_PLUGIN (popup->panel_plugin),
                        popup->button);
}

/* Zona fija de abajo. Se llama al crear la ventana y, tras un cambio de tema,
 * para que sus widgets nazcan con el estilo nuevo (ver on_theme_changed). El
 * estado del interruptor lo vuelve a poner update_top_status. */
static void
popup_build_bottom_zone (BtPopup *popup)
{
    GList *old = gtk_container_get_children (GTK_CONTAINER (popup->bottom_box));
    g_list_free_full (old, (GDestroyNotify) gtk_widget_destroy);
    popup->discoverable_switch  = NULL;
    popup->discoverable_handler = 0;

    GtkWidget *adv_btn = gtk_button_new_with_label (_("Advanced settings…"));
    gtk_button_set_relief (GTK_BUTTON (adv_btn), GTK_RELIEF_NONE);
    gtk_widget_set_margin_start  (adv_btn, 8);
    gtk_widget_set_margin_end    (adv_btn, 8);
    gtk_widget_set_margin_top    (adv_btn, 4);
    gtk_widget_set_margin_bottom (adv_btn, 4);
    g_signal_connect (adv_btn, "clicked",
                      G_CALLBACK (on_advanced_clicked), NULL);
    gtk_box_pack_end (GTK_BOX (popup->bottom_box), adv_btn, FALSE, FALSE, 0);

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
    gtk_box_pack_end (GTK_BOX (popup->bottom_box), disc_row, FALSE, FALSE, 0);

    /* Sin línea entre la lista y la zona fija, a propósito (0051): la línea
     * del último aparato ya la separa, y con las dos juntas se veía doble. */

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
}

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
                /* Resaltado de la fila: el color de la letra con poca
                 * opacidad, sobre el fondo que haya. Antes era una mezcla fija
                 * del fondo de VENTANA del tema: dentro del popover no
                 * coincidía con el fondo real (casi invisible en Adwaita, y en
                 * Numix, con popovers oscuros, dejaba la letra ilegible). La
                 * barra a la izquierda, con el color de selección del tema, es
                 * lo que marca de verdad el aparato abierto. */
                ".bt-row:focus { background-color: alpha(currentColor, 0.06); }"
                ".bt-row-open, .bt-row-open:focus { background-color: alpha(currentColor, 0.12);"
                " box-shadow: inset 3px 0 @theme_selected_bg_color; }"
                ".bt-row-open:dir(rtl), .bt-row-open:focus:dir(rtl) {"
                " box-shadow: inset -3px 0 @theme_selected_bg_color; }"
                /* Separadores con el color de la letra del propio popover:
                 * dentro de un popover muchos temas los pintan casi del color
                 * del fondo (Arc, blancos sobre blanco) y no se veían, sobre
                 * todo en temas oscuros. La línea gruesa usaba el fondo de
                 * VENTANA del tema, que dentro del popover no coincide. */
                ".module-sep { background-color: alpha(currentColor, 0.3); background-image: none; min-height: 2px; }"
                ".bt-header-sep { background-color: alpha(currentColor, 0.15); background-image: none; min-height: 1px; }"
                /* Aspecto del tema (ver BtPopupFrame). */
                "window.bt-popup-window.bt-rounded { background-color: transparent; background-image: none; }"
                /* Marco de menú (en los tres estilos): recto salvo con la
                 * clase bt-round (ver popup_set_round). Varios temas les dan
                 * margen a los separadores dentro de menús y popovers (3 o 4
                 * px): quedaba una franja sin resaltar entre la fila abierta
                 * y la línea siguiente. */
                "menu.bt-popup-frame { margin: 0; box-shadow: none; border-radius: 0; }"
                "menu.bt-popup-frame separator { margin: 0; min-height: 1px;"
                " background-color: alpha(currentColor, 0.15); background-image: none; }",
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

    gtk_style_context_add_class (gtk_widget_get_style_context (win),
                                 "bt-popup-window");

    /* Marco visible con los colores de los menús del tema, como los plugins
     * nativos de Xfce, en los tres estilos; y un popover que nunca se
     * muestra, solo para leer el radio del tema (ver popup_update_rounding). */
    popup_create_frames (popup);
    gtk_container_add (GTK_CONTAINER (win), popup->frame);

    outer_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class (gtk_widget_get_style_context (outer_box),
                                 "popup");
    gtk_container_add (GTK_CONTAINER (popup->frame), outer_box);
    popup->outer_box = outer_box;

    popup->top_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start (GTK_BOX (outer_box), popup->top_box, FALSE, FALSE, 0);

    scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start (GTK_BOX (outer_box), scroll, TRUE, TRUE, 0);
    popup->scroll = scroll;

    popup->content_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (scroll), popup->content_box);

    /* ---- Zona fija inferior (ver popup_build_bottom_zone) ---- */
    popup->bottom_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_end (GTK_BOX (outer_box), popup->bottom_box, FALSE, FALSE, 0);
    popup_build_bottom_zone (popup);

    g_signal_connect (win, "key-press-event", G_CALLBACK (on_key_press), popup);
    g_signal_connect (win, "size-allocate",
                      G_CALLBACK (on_popup_size_allocate), popup);

    /* Wayland: enterarse cuando el compositor la cierra solo. En X11 estos
     * manejadores no hacen nada (ver wl_on_popup_gone). */
    g_signal_connect (win, "unmap-event",
                      G_CALLBACK (wl_on_popup_gone), popup);
    g_signal_connect (win, "grab-broken-event",
                      G_CALLBACK (wl_on_popup_gone), popup);
    g_signal_connect (win, "window-state-event",
                      G_CALLBACK (wl_on_popup_gone), popup);

    popup->window = win;

    /* Modo inicial y aviso de cambios del compositor (en X11 se puede
     * prender o apagar con el panel andando). */
    popup_apply_rounding (popup,
        gdk_screen_is_composited (gtk_widget_get_screen (win)) &&
        gdk_screen_get_rgba_visual (gtk_widget_get_screen (win)) != NULL);
    popup->composited_handler = g_signal_connect (
        gtk_widget_get_screen (win), "composited-changed",
        G_CALLBACK (on_composited_changed), popup);

    popup->theme_settings = gtk_settings_get_for_screen (gtk_widget_get_screen (win));
    popup->theme_handler  = g_signal_connect (popup->theme_settings,
                                              "notify::gtk-theme-name",
                                              G_CALLBACK (on_theme_changed), popup);

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
    popup->conn         = conn;
    popup->popup_width  = popup_width;
    popup->popup_height = popup_height;
    popup->panel_plugin = plugin;

    /* El tema cambió con la ventana cerrada: caja "popover", zona de abajo y
     * contenido nuevos, ANTES de medir (ver on_theme_changed). */
    if (popup->theme_dirty) {
        popup->theme_dirty = FALSE;
        popup_recreate_frame (popup);
        popup_build_bottom_zone (popup);
        popup->ui_built = FALSE;
    }

    /* Por si el compositor o el tema cambiaron con la ventana cerrada, o
     * el compositor cambió con la ventana abierta la vez anterior. */
    popup_update_rounding (popup);

    /* El borde y el relleno del tema salen de la lista y no se suman por
     * fuera: la ventana mide lo configurado. */
    gint frame_w, frame_h;
    popup_frame_extra (popup, &frame_w, &frame_h);
    gtk_widget_set_size_request (popup->window, popup_width, -1);
    gtk_widget_set_size_request (popup->scroll, MAX (popup_width - frame_w, 1),
                                 MAX (popup_height - frame_h, 1));

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

    popup->button = button;

    /* Mostrar el contenido (no la ventana) antes de medirla: la primera vez
     * nada adentro es visible todavía, GTK la mide con alto 0 y la ventana
     * quedaba mal ubicada con el panel abajo o a un costado. */
    gtk_widget_show_all (popup->frame);

    if (popup_is_wayland (popup->window)) {
        popup_show_wayland (popup, plugin, button);
    } else {
        /* X11, como siempre. */
        gtk_widget_realize (popup->window);
        position_popup (popup, plugin, button);
        gtk_widget_show_all (popup->window);

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

    if (!gtk_widget_get_visible (popup->window) || popup->hiding)
        return;
    popup->hiding = TRUE;
    if (wl_open_popup == popup)
        wl_open_popup = NULL;

    display = gtk_widget_get_display (popup->window);
    seat    = gdk_display_get_default_seat (display);
    gdk_seat_ungrab (seat);
    if (gtk_widget_has_grab (popup->window))
        gtk_grab_remove (popup->window);   /* solo existe en Wayland */

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

    /* Wayland: durante el acaparamiento el botón del panel no se entera de
     * que el puntero se fue, y queda resaltado (y con el recuadro de foco)
     * hasta volver a pasar por encima. */
    if (popup->button != NULL && popup_is_wayland (popup->window)) {
        gtk_widget_unset_state_flags (popup->button,
                                      GTK_STATE_FLAG_PRELIGHT |
                                      GTK_STATE_FLAG_ACTIVE);
        GtkWidget *top = gtk_widget_get_toplevel (popup->button);
        if (GTK_IS_WINDOW (top))
            gtk_window_set_focus_visible (GTK_WINDOW (top), FALSE);
    }

    gtk_widget_hide (popup->window);
    popup->hiding = FALSE;
}

void
popup_destroy (BtPopup *popup)
{
    if (!popup) return;
    popup_hide (popup);
    if (popup->composited_handler)
        g_signal_handler_disconnect (gtk_widget_get_screen (popup->window),
                                     popup->composited_handler);
    /* La configuración de GTK sobrevive al popup: sin esto, cambiar el tema
     * después de quitar el plugin llamaría a un popup ya liberado. */
    if (popup->theme_handler)
        g_signal_handler_disconnect (popup->theme_settings, popup->theme_handler);
    popup->theme_handler = 0;
    if (popup->ops_in_progress)
        g_hash_table_destroy (popup->ops_in_progress);
    if (popup->failed_devices)
        g_hash_table_destroy (popup->failed_devices);
    g_slist_free (popup->adapter_switches);
    gtk_widget_destroy (popup->window);
    /* El marco de menú cayó con la ventana; el popover oculto tiene su propia
     * referencia. */
    gtk_widget_destroy (popup->frame_popover);
    g_object_unref (popup->frame_popover);
    g_free (popup);
}
