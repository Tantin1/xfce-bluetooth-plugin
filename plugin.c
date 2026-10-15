/* plugin.c — punto de entrada del plugin del panel Xfce.
 *
 * Casi todo acá es chasis heredado del plugin de red: configuración en
 * GKeyFile, diálogo de propiedades, botón con stack ícono/spinner y ciclo de
 * vida del popup. Lo específico de Bluetooth es el ícono del panel y, sobre
 * todo, el agente de pareo: se registra ACÁ y no en el popup, porque un
 * aparato puede pedir vinculación con el popup cerrado. */

#include <libxfce4panel/libxfce4panel.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#ifdef HAVE_LIBNOTIFY
#include <libnotify/notify.h>
#endif
#include "popup.h"
#include "bt-dbus.h"
#include "bt-agent.h"
#include "bt-obex.h"
#include "bt-obex-agent.h"

#define DEFAULT_SQUARE_BUTTON       FALSE
#define DEFAULT_PLUGIN_SIZE         24
#define DEFAULT_USE_PANEL_ICON      FALSE
#define DEFAULT_ICON_SIZE           16
#define DEFAULT_POPUP_WIDTH         400
#define DEFAULT_POPUP_HEIGHT        340
#define DEFAULT_SHOW_SEPARATORS     TRUE
#define DEFAULT_SHOW_NOTIFICATIONS  FALSE
#define DEFAULT_SHOW_UNNAMED        TRUE
#define DEFAULT_HIDE_WHEN_OFF       FALSE

/* Recibir archivos arranca APAGADO a propósito: dejarlo prendido anuncia al
 * mundo que este equipo acepta archivos y mantiene un servicio más vivo.
 * Eso se elige, no se impone. */
#define DEFAULT_RECEIVE_FILES       FALSE

/* Detección de instancia duplicada.
 *
 * Intento anterior (una bandera estática a nivel de módulo) no funcionaba:
 * el .desktop.in tiene X-XFCE-Internal=false, lo que significa que Xfce NO
 * carga este .so dentro del proceso xfce4-panel, sino que lanza un proceso
 * "wrapper" (xfce4-panel-wrapper) POR CADA instancia del plugin. Cada
 * wrapper tiene su propia memoria, así que una variable estática de proceso
 * nunca la ve la segunda instancia — viven en programas distintos.
 *
 * Lo que sí comparten todas las instancias es el bus de sesión D-Bus. La
 * técnica estándar de "instancia única" es pedir un nombre bien conocido
 * con g_bus_own_name() y la bandera DO_NOT_QUEUE: el bus solo se lo da a
 * quien lo pide primero. El resultado llega por callback, asíncrono —
 * por eso bp->is_duplicate arranca en FALSE (default de g_new0) y recién
 * se define cuando el bus contesta. */
#define BT_PLUGIN_SINGLETON_NAME "com.tantin.XfceBluetoothPluginInstance"

typedef struct {
    XfcePanelPlugin *plugin;
    GtkWidget       *button;
    GtkWidget       *icon_box;
    GtkWidget       *stack;
    GtkWidget       *spinner;
    BtPopup         *popup;
    GDBusConnection *conn;
    BtAgent         *agent;

    gboolean  square_button;
    gint      plugin_size;
    gboolean  use_panel_icon;
    gint      icon_size;
    gint      popup_width;
    gint      popup_height;
    gboolean  show_separators;
    gboolean  show_notifications;
    gboolean  show_unnamed;
    gboolean  hide_when_off;

    /* Recepción de archivos por Bluetooth. */
    gboolean     receive_files;
    gchar       *receive_folder;
    BtObexAgent *obex_agent;

    GtkCssProvider *button_css;
    guint          *signal_ids;

    gboolean  busy;
    gboolean  notif_ready;
    gboolean  is_duplicate;   /* TRUE si otra instancia ya tenía el nombre D-Bus */
    guint     name_owner_id;  /* id de g_bus_own_name(), para soltarlo al salir */

    /* Último estado conocido, para redibujar sin volver a consultar el bus
     * y para decidir qué notificación corresponde. */
    gboolean  last_available;
    gboolean  last_powered;
    gint      last_connected;
    gchar    *last_name;
} BtPlugin;

static void on_configure_plugin (XfcePanelPlugin *plugin, BtPlugin *bp);
static gboolean on_bt_changed_cb (gpointer user_data);
static void on_bt_name_acquired (GDBusConnection *conn, const gchar *name,
                                 gpointer user_data);
static void on_bt_name_lost     (GDBusConnection *conn, const gchar *name,
                                 gpointer user_data);

/* ---------- ícono del botón del panel ---------- */

static void
update_panel_icon (BtPlugin *bp, gboolean available, gboolean powered,
                   gint n_connected)
{
    gint icon_px = bp->use_panel_icon
                   ? xfce_panel_plugin_get_icon_size (bp->plugin)
                   : bp->icon_size;

    bp->last_available = available;
    bp->last_powered   = powered;
    bp->last_connected = n_connected;

    GList *kids = gtk_container_get_children (GTK_CONTAINER (bp->icon_box));
    g_list_free_full (kids, (GDestroyNotify) gtk_widget_destroy);

    const gchar *const *names = bt_status_icon_names (available, powered,
                                                       n_connected);
    GIcon     *gicon = g_themed_icon_new_from_names ((gchar **) names, -1);
    GtkWidget *img   = gtk_image_new_from_gicon (gicon, GTK_ICON_SIZE_BUTTON);
    g_object_unref (gicon);
    gtk_image_set_pixel_size (GTK_IMAGE (img), icon_px);

    gtk_box_pack_start (GTK_BOX (bp->icon_box), img, TRUE, TRUE, 0);
    gtk_widget_show_all (bp->icon_box);

    /* Opción "ocultar cuando está apagado": el plugin se achica a nada pero
     * no se saca del panel, así el usuario puede volver a mostrarlo desde
     * las propiedades sin tener que agregarlo de cero. */
    if (!available)
        /* Sin adaptador Bluetooth en el equipo: no hay nada que mostrar,
         * independiente de la opción "ocultar cuando está apagado". */
        gtk_widget_hide (bp->button);
    else if (bp->hide_when_off && !powered)
        gtk_widget_hide (bp->button);
    else
        gtk_widget_show (bp->button);
}

/* ---------- aplicar configuración visual ---------- */

static void
apply_config (BtPlugin *bp)
{
    XfcePanelPlugin *plugin = bp->plugin;

    {
        gchar *css;
        gint   ps = xfce_panel_plugin_get_size (plugin);

        if (bp->square_button)
            css = g_strdup_printf (
                "button{padding:0;margin:0;min-width:%dpx;min-height:%dpx;border:none;}",
                ps, ps);
        else {
            GtkOrientation orient = xfce_panel_plugin_get_orientation (plugin);
            if (orient == GTK_ORIENTATION_HORIZONTAL)
                css = g_strdup_printf (
                    "button{padding:0;margin:0;min-width:%dpx;min-height:0;border:none;}",
                    bp->plugin_size);
            else
                css = g_strdup_printf (
                    "button{padding:0;margin:0;min-width:0;min-height:%dpx;border:none;}",
                    bp->plugin_size);
        }

        if (!bp->button_css) {
            bp->button_css = gtk_css_provider_new ();
            gtk_style_context_add_provider (
                gtk_widget_get_style_context (bp->button),
                GTK_STYLE_PROVIDER (bp->button_css),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
        gtk_css_provider_load_from_data (bp->button_css, css, -1, NULL);
        g_free (css);
    }

    update_panel_icon (bp, bp->last_available, bp->last_powered,
                       bp->last_connected);
}

/* ---------- leer / guardar configuración ---------- */

#define CONFIG_PATH "xfce4/panel/xfce-bluetooth-plugin.ini"

static gchar *
get_config_path (void)
{
    return g_build_filename (g_get_user_config_dir (), CONFIG_PATH, NULL);
}

static void
load_config (BtPlugin *bp)
{
    GKeyFile *kf   = g_key_file_new ();
    gchar    *path = get_config_path ();
    GError   *err  = NULL;

    bp->square_button      = DEFAULT_SQUARE_BUTTON;
    bp->plugin_size        = DEFAULT_PLUGIN_SIZE;
    bp->use_panel_icon     = DEFAULT_USE_PANEL_ICON;
    bp->icon_size          = DEFAULT_ICON_SIZE;
    bp->popup_width        = DEFAULT_POPUP_WIDTH;
    bp->popup_height       = DEFAULT_POPUP_HEIGHT;
    bp->show_separators    = DEFAULT_SHOW_SEPARATORS;
    bp->show_notifications = DEFAULT_SHOW_NOTIFICATIONS;
    bp->show_unnamed       = DEFAULT_SHOW_UNNAMED;
    bp->hide_when_off      = DEFAULT_HIDE_WHEN_OFF;
    bp->receive_files      = DEFAULT_RECEIVE_FILES;

    /* Por omisión, la carpeta de Descargas del usuario. Si el sistema no
     * define esa carpeta especial, la personal. */
    g_free (bp->receive_folder);
    {
        const gchar *dl = g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD);
        bp->receive_folder = g_strdup (dl != NULL ? dl : g_get_home_dir ());
    }

    if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, &err)) {
        g_clear_error (&err);
        goto out;
    }

    bp->square_button  = g_key_file_get_boolean (kf, "appearance", "square_button",  NULL);
    bp->plugin_size    = g_key_file_get_integer (kf, "appearance", "plugin_size",    NULL);
    bp->use_panel_icon = g_key_file_get_boolean (kf, "appearance", "use_panel_icon", NULL);
    bp->icon_size      = g_key_file_get_integer (kf, "appearance", "icon_size",      NULL);
    bp->popup_width    = g_key_file_get_integer (kf, "appearance", "popup_width",    NULL);
    bp->popup_height   = g_key_file_get_integer (kf, "appearance", "popup_height",   NULL);

    /* Las banderas con valor por omisión TRUE necesitan has_key: sin la
     * clave, get_boolean devuelve FALSE y pisaría el valor correcto. */
    bp->show_separators = g_key_file_get_boolean (kf, "appearance", "show_separators", NULL);
    if (!g_key_file_has_key (kf, "appearance", "show_separators", NULL))
        bp->show_separators = DEFAULT_SHOW_SEPARATORS;

    bp->show_notifications = g_key_file_get_boolean (kf, "appearance", "show_notifications", NULL);
    if (!g_key_file_has_key (kf, "appearance", "show_notifications", NULL))
        bp->show_notifications = DEFAULT_SHOW_NOTIFICATIONS;

    bp->show_unnamed = g_key_file_get_boolean (kf, "appearance", "show_unnamed", NULL);
    if (!g_key_file_has_key (kf, "appearance", "show_unnamed", NULL))
        bp->show_unnamed = DEFAULT_SHOW_UNNAMED;

    bp->hide_when_off = g_key_file_get_boolean (kf, "appearance", "hide_when_off", NULL);
    if (!g_key_file_has_key (kf, "appearance", "hide_when_off", NULL))
        bp->hide_when_off = DEFAULT_HIDE_WHEN_OFF;

    bp->receive_files = g_key_file_get_boolean (kf, "transfer", "receive_files", NULL);
    if (!g_key_file_has_key (kf, "transfer", "receive_files", NULL))
        bp->receive_files = DEFAULT_RECEIVE_FILES;

    {
        gchar *folder = g_key_file_get_string (kf, "transfer", "receive_folder", NULL);
        if (folder != NULL && *folder) {
            g_free (bp->receive_folder);
            bp->receive_folder = folder;
        } else {
            g_free (folder);
        }
    }

    if (bp->plugin_size  == 0) bp->plugin_size  = DEFAULT_PLUGIN_SIZE;
    if (bp->icon_size    == 0) bp->icon_size    = DEFAULT_ICON_SIZE;
    if (bp->popup_width  == 0) bp->popup_width  = DEFAULT_POPUP_WIDTH;
    if (bp->popup_height == 0) bp->popup_height = DEFAULT_POPUP_HEIGHT;

out:
    g_key_file_free (kf);
    g_free (path);
}

static void
save_config (BtPlugin *bp)
{
    GKeyFile *kf   = g_key_file_new ();
    gchar    *path = get_config_path ();
    gchar    *dir  = g_path_get_dirname (path);
    GError   *err  = NULL;

    g_mkdir_with_parents (dir, 0755);

    g_key_file_set_boolean (kf, "appearance", "square_button",      bp->square_button);
    g_key_file_set_integer (kf, "appearance", "plugin_size",        bp->plugin_size);
    g_key_file_set_boolean (kf, "appearance", "use_panel_icon",     bp->use_panel_icon);
    g_key_file_set_integer (kf, "appearance", "icon_size",          bp->icon_size);
    g_key_file_set_integer (kf, "appearance", "popup_width",        bp->popup_width);
    g_key_file_set_integer (kf, "appearance", "popup_height",       bp->popup_height);
    g_key_file_set_boolean (kf, "appearance", "show_separators",    bp->show_separators);
    g_key_file_set_boolean (kf, "appearance", "show_notifications", bp->show_notifications);
    g_key_file_set_boolean (kf, "appearance", "show_unnamed",       bp->show_unnamed);
    g_key_file_set_boolean (kf, "appearance", "hide_when_off",      bp->hide_when_off);

    g_key_file_set_boolean (kf, "transfer", "receive_files", bp->receive_files);
    if (bp->receive_folder != NULL)
        g_key_file_set_string (kf, "transfer", "receive_folder", bp->receive_folder);

    if (!g_key_file_save_to_file (kf, path, &err)) {
        g_warning ("xfce-bluetooth-plugin: no se pudo guardar la configuración: %s",
                   err->message);
        g_error_free (err);
    }

    g_key_file_free (kf);
    g_free (dir);
    g_free (path);
}

/* ---------- recepción de archivos ----------
 *
 * El agente se crea cuando la opción está prendida y se destruye cuando se
 * apaga. Mientras existe, se registra y se REGISTRA SOLO DE NUEVO si hace
 * falta: si otro programa tenía el puesto tomado, reintenta cada minuto, así
 * que cerrar ese otro programa alcanza para que esto arranque sin tocar nada.
 *
 * Igual que el agente de pareo, vive a nivel del plugin y no de la ventana
 * emergente: un aparato manda un archivo cuando quiere, con el panel
 * cerrado. */
static void
apply_receive_config (BtPlugin *bp)
{
    /* Una instancia duplicada no debe competir por el puesto de receptor. */
    if (bp->is_duplicate)
        return;

    if (bp->receive_files && bp->obex_agent == NULL) {
        bp->obex_agent = bt_obex_agent_new (bp->conn, bp->receive_folder,
                                            bp->show_notifications);
    } else if (!bp->receive_files && bp->obex_agent != NULL) {
        bt_obex_agent_free (bp->obex_agent);
        bp->obex_agent = NULL;
    } else if (bp->obex_agent != NULL) {
        bt_obex_agent_set_folder (bp->obex_agent, bp->receive_folder);
        bt_obex_agent_set_notifications (bp->obex_agent, bp->show_notifications);
    }
}

/* ---------- ventana de propiedades ---------- */

typedef struct {
    BtPlugin  *bp;
    GtkWidget *square_check;
    GtkWidget *plugin_spin;
    GtkWidget *panel_icon_check;
    GtkWidget *icon_spin;
    GtkWidget *popup_width_spin;
    GtkWidget *popup_height_spin;
    GtkWidget *separators_check;
    GtkWidget *notifications_check;
    GtkWidget *unnamed_check;
    GtkWidget *hide_check;

    /* Zona de transferencia de archivos. */
    GtkWidget *recv_check;
    GtkWidget *recv_folder_btn;
    GtkWidget *recv_status;
    guint      recv_status_id;   /* refresco periódico del estado */

    /* Aviso de enlace simbólico en la carpeta de recepción, con su botón
     * de arreglo automático. symlink_box empieza oculto y solo se muestra
     * si update_symlink_warning() encuentra algo. */
    GtkWidget *symlink_box;
    GtkWidget *symlink_label;
    GtkWidget *symlink_fix_btn;
} PropsDialog;

static void
on_square_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    gboolean active = gtk_toggle_button_get_active (btn);
    gtk_widget_set_sensitive (pd->plugin_spin, !active);
    pd->bp->square_button = active;
    save_config (pd->bp);
    apply_config (pd->bp);
}

static void
on_plugin_size_changed (GtkSpinButton *spin, PropsDialog *pd)
{
    pd->bp->plugin_size = gtk_spin_button_get_value_as_int (spin);
    save_config (pd->bp);
    apply_config (pd->bp);
}

static void
on_panel_icon_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    gboolean active = gtk_toggle_button_get_active (btn);
    gtk_widget_set_sensitive (pd->icon_spin, !active);
    pd->bp->use_panel_icon = active;
    save_config (pd->bp);
    apply_config (pd->bp);
}

static void
on_icon_size_changed (GtkSpinButton *spin, PropsDialog *pd)
{
    pd->bp->icon_size = gtk_spin_button_get_value_as_int (spin);
    save_config (pd->bp);
    apply_config (pd->bp);
}

static void
on_popup_width_changed (GtkSpinButton *spin, PropsDialog *pd)
{
    pd->bp->popup_width = gtk_spin_button_get_value_as_int (spin);
    save_config (pd->bp);
}

static void
on_popup_height_changed (GtkSpinButton *spin, PropsDialog *pd)
{
    pd->bp->popup_height = gtk_spin_button_get_value_as_int (spin);
    save_config (pd->bp);
}

static void
on_separators_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->bp->show_separators = gtk_toggle_button_get_active (btn);
    save_config (pd->bp);
    pd->bp->popup->show_separators = pd->bp->show_separators;
    pd->bp->popup->ui_built = FALSE;   /* forzar reconstrucción al reabrir */
}

#ifdef HAVE_LIBNOTIFY
static void
on_notifications_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->bp->show_notifications = gtk_toggle_button_get_active (btn);
    save_config (pd->bp);
    apply_receive_config (pd->bp);
}
#endif

static void
on_unnamed_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->bp->show_unnamed = gtk_toggle_button_get_active (btn);
    save_config (pd->bp);
    pd->bp->popup->show_unnamed = pd->bp->show_unnamed;
    pd->bp->popup->ui_built = FALSE;
}

static void
on_hide_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->bp->hide_when_off = gtk_toggle_button_get_active (btn);
    save_config (pd->bp);
    apply_config (pd->bp);
}

/* Refresca el texto de estado. Corre cada dos segundos mientras la ventana
 * de propiedades está abierta, para que el usuario vea el cambio en vivo
 * cuando cierra el programa que le tenía tomado el puesto. */
static void
update_recv_status (PropsDialog *pd)
{
    if (pd->recv_status == NULL)
        return;

    const gchar *txt = pd->bp->receive_files
        ? bt_obex_recv_status_message (pd->bp->obex_agent)
        : bt_obex_recv_status_message (NULL);

    gtk_label_set_text (GTK_LABEL (pd->recv_status), txt);
}

static gboolean
on_recv_status_tick (gpointer user_data)
{
    update_recv_status ((PropsDialog *) user_data);
    return G_SOURCE_CONTINUE;
}

/* Muestra u oculta el aviso del arreglo de obexd. A diferencia de la
 * primera versión, YA NO se queda callado cuando no encuentra un enlace
 * simbólico: la sesión 0020-en-adelante confirmó con strace que algunas
 * versiones de obexd tienen un bug en su propia revisión de enlaces (leen
 * "no es un enlace" como si fuera un error) y rechazan CUALQUIER carpeta,
 * tenga o no un enlace de verdad. Por eso ahora hay tres estados:
 *
 *   1. Recepción apagada → oculto, no corresponde ofrecer nada.
 *   2. Arreglo ya instalado → oculto, no hay nada más para avisar acá.
 *   3. Arreglo sin instalar → visible siempre, con dos redacciones:
 *      si encontramos un enlace real, lo nombramos; si no, explicamos el
 *      bug conocido en términos generales. En los dos casos el botón hace
 *      lo mismo: instalar el arreglo y reiniciar el servicio. */
static void
update_symlink_warning (PropsDialog *pd)
{
    if (pd->symlink_box == NULL)
        return;

    if (!pd->bp->receive_files || bt_obex_symlinks_override_installed ()) {
        gtk_widget_hide (pd->symlink_box);
        return;
    }

    gchar *bad = bt_obex_find_symlink_in_path (pd->bp->receive_folder);
    gchar *msg;

    if (bad != NULL) {
        msg = g_strdup_printf (
            _("\"%s\" is a symlink. The file transfer service refuses to "
              "write through symlinks, so incoming files will fail here."),
            bad);
    } else {
        msg = g_strdup (
            _("Some versions of the file transfer service reject every "
              "incoming file due to a bug in their own safety check — even "
              "without any symlink involved. Phones show this as "
              "\"transfer forbidden\". If reception keeps failing, try "
              "this fix; it does not require a symlink to be present."));
    }

    gtk_label_set_text (GTK_LABEL (pd->symlink_label), msg);
    g_free (msg);
    g_free (bad);

    gtk_widget_set_sensitive (pd->symlink_fix_btn, TRUE);
    gtk_widget_show (pd->symlink_fix_btn);
    gtk_widget_show (pd->symlink_box);
}

static void
on_symlink_fix_clicked (GtkWidget *btn, PropsDialog *pd)
{
    GError *error = NULL;

    gtk_widget_set_sensitive (pd->symlink_fix_btn, FALSE);

    if (bt_obex_install_symlinks_override (&error)) {
        update_symlink_warning (pd);
        update_recv_status (pd);
        return;
    }

    GtkWidget *dlg = gtk_message_dialog_new (
        GTK_WINDOW (gtk_widget_get_toplevel (btn)),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
        "%s", _("Could not apply the fix"));
    gtk_message_dialog_format_secondary_text (
        GTK_MESSAGE_DIALOG (dlg), "%s",
        (error != NULL && error->message != NULL)
            ? error->message : _("Unknown error"));
    g_signal_connect (dlg, "response", G_CALLBACK (gtk_widget_destroy), NULL);
    gtk_widget_show (dlg);
    g_clear_error (&error);

    gtk_widget_set_sensitive (pd->symlink_fix_btn, TRUE);
}

static void
on_recv_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->bp->receive_files = gtk_toggle_button_get_active (btn);
    save_config (pd->bp);
    apply_receive_config (pd->bp);

    if (pd->recv_folder_btn != NULL)
        gtk_widget_set_sensitive (pd->recv_folder_btn, pd->bp->receive_files);
    update_recv_status (pd);
    update_symlink_warning (pd);
}

static void
on_recv_folder_set (GtkFileChooserButton *btn, PropsDialog *pd)
{
    gchar *folder = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (btn));
    if (folder == NULL)
        return;

    g_free (pd->bp->receive_folder);
    pd->bp->receive_folder = folder;
    save_config (pd->bp);
    apply_receive_config (pd->bp);
    update_symlink_warning (pd);
}

static void
on_props_dialog_destroy (GtkWidget *win, PropsDialog *pd)
{
    (void) win;
    if (pd->recv_status_id != 0)
        g_source_remove (pd->recv_status_id);
    g_free (pd);
}

static void
on_configure_plugin (XfcePanelPlugin *plugin, BtPlugin *bp)
{
    PropsDialog *pd = g_new0 (PropsDialog, 1);
    pd->bp = bp;

    GtkWidget *dialog = gtk_dialog_new_with_buttons (
        _("Properties — Bluetooth"), NULL,
        GTK_DIALOG_DESTROY_WITH_PARENT,
        _("Close"), GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_resizable (GTK_WINDOW (dialog), FALSE);

    GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_container_set_border_width (GTK_CONTAINER (content), 12);

    GtkWidget *frame = gtk_frame_new (_("Appearance"));
    GtkWidget *grid  = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 6);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    gtk_container_set_border_width (GTK_CONTAINER (grid), 8);
    gtk_container_add (GTK_CONTAINER (frame), grid);

    gint row = 0;

    pd->square_check = gtk_check_button_new_with_label (_("Square button"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->square_check),
                                  bp->square_button);
    gtk_grid_attach (GTK_GRID (grid), pd->square_check, 0, row++, 2, 1);

    GtkWidget *plugin_size_label = gtk_label_new (_("Plugin size:"));
    gtk_label_set_xalign (GTK_LABEL (plugin_size_label), 0.0);
    gtk_widget_set_margin_start (plugin_size_label, 12);
    pd->plugin_spin = gtk_spin_button_new_with_range (16, 200, 1);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (pd->plugin_spin), bp->plugin_size);
    gtk_widget_set_sensitive (pd->plugin_spin, !bp->square_button);
    gtk_grid_attach (GTK_GRID (grid), plugin_size_label, 0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), pd->plugin_spin,   1, row++, 1, 1);

    GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top (sep, 4);
    gtk_widget_set_margin_bottom (sep, 4);
    gtk_grid_attach (GTK_GRID (grid), sep, 0, row++, 2, 1);

    pd->panel_icon_check = gtk_check_button_new_with_label (_("Use panel size"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->panel_icon_check),
                                  bp->use_panel_icon);
    gtk_grid_attach (GTK_GRID (grid), pd->panel_icon_check, 0, row++, 2, 1);

    GtkWidget *icon_size_label = gtk_label_new (_("Icon size:"));
    gtk_label_set_xalign (GTK_LABEL (icon_size_label), 0.0);
    gtk_widget_set_margin_start (icon_size_label, 12);
    pd->icon_spin = gtk_spin_button_new_with_range (8, 64, 1);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (pd->icon_spin), bp->icon_size);
    gtk_widget_set_sensitive (pd->icon_spin, !bp->use_panel_icon);
    gtk_grid_attach (GTK_GRID (grid), icon_size_label, 0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), pd->icon_spin,   1, row++, 1, 1);

    GtkWidget *sep2 = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top (sep2, 4);
    gtk_widget_set_margin_bottom (sep2, 4);
    gtk_grid_attach (GTK_GRID (grid), sep2, 0, row++, 2, 1);

    GtkWidget *pw_label = gtk_label_new (_("Popup width:"));
    gtk_label_set_xalign (GTK_LABEL (pw_label), 0.0);
    gtk_widget_set_margin_start (pw_label, 12);
    pd->popup_width_spin = gtk_spin_button_new_with_range (300, 800, 10);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (pd->popup_width_spin), bp->popup_width);
    gtk_grid_attach (GTK_GRID (grid), pw_label,             0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), pd->popup_width_spin, 1, row++, 1, 1);

    GtkWidget *ph_label = gtk_label_new (_("Popup height:"));
    gtk_label_set_xalign (GTK_LABEL (ph_label), 0.0);
    gtk_widget_set_margin_start (ph_label, 12);
    pd->popup_height_spin = gtk_spin_button_new_with_range (200, 600, 10);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (pd->popup_height_spin), bp->popup_height);
    gtk_grid_attach (GTK_GRID (grid), ph_label,              0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), pd->popup_height_spin, 1, row++, 1, 1);

    GtkWidget *sep3 = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top (sep3, 4);
    gtk_widget_set_margin_bottom (sep3, 4);
    gtk_grid_attach (GTK_GRID (grid), sep3, 0, row++, 2, 1);

    pd->separators_check = gtk_check_button_new_with_label (_("Show separators"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->separators_check),
                                  bp->show_separators);
    gtk_grid_attach (GTK_GRID (grid), pd->separators_check, 0, row++, 2, 1);

    pd->unnamed_check = gtk_check_button_new_with_label (
        _("Show devices without a name"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->unnamed_check),
                                  bp->show_unnamed);
    gtk_grid_attach (GTK_GRID (grid), pd->unnamed_check, 0, row++, 2, 1);

    pd->hide_check = gtk_check_button_new_with_label (
        _("Hide the icon when Bluetooth is off"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->hide_check),
                                  bp->hide_when_off);
    gtk_grid_attach (GTK_GRID (grid), pd->hide_check, 0, row++, 2, 1);

#ifdef HAVE_LIBNOTIFY
    pd->notifications_check = gtk_check_button_new_with_label (
        _("Show notifications"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->notifications_check),
                                  bp->show_notifications);
    gtk_grid_attach (GTK_GRID (grid), pd->notifications_check, 0, row++, 2, 1);
#endif

    gtk_box_pack_start (GTK_BOX (content), frame, FALSE, FALSE, 0);

    /* ---- Estado de la transferencia de archivos (OBEX) ----
     *
     * Por ahora es solo informativo: enviar y recibir todavía no están
     * implementados. Está acá primero a propósito, porque el demonio de
     * transferencia viene en un paquete aparte del de BlueZ y puede además
     * tener su servicio de usuario sin habilitar. Que el usuario pueda ver
     * de antemano si su sistema está listo evita el clásico "aprieto enviar
     * y no pasa nada" sin explicación.
     *
     * Se consulta cada vez que se abre este diálogo, no una sola vez al
     * arrancar: así, si el usuario instala el paquete con el panel abierto,
     * al reabrir Propiedades ya lo ve reflejado. */
    {
        BtObexState  obex_state = bt_obex_check ();
        GtkWidget   *obex_frame = gtk_frame_new (_("File transfer"));
        GtkWidget   *obex_box   = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);

        gtk_container_set_border_width (GTK_CONTAINER (obex_box), 8);
        gtk_container_add (GTK_CONTAINER (obex_frame), obex_box);

        GtkWidget *obex_label = gtk_label_new (bt_obex_state_message (obex_state));
        gtk_label_set_xalign (GTK_LABEL (obex_label), 0.0);
        gtk_box_pack_start (GTK_BOX (obex_box), obex_label, FALSE, FALSE, 0);

        gchar *obex_hint = bt_obex_state_hint (obex_state);
        if (obex_hint != NULL) {
            GtkWidget *hint_label = gtk_label_new (obex_hint);
            gtk_label_set_xalign (GTK_LABEL (hint_label), 0.0);
            gtk_label_set_line_wrap (GTK_LABEL (hint_label), TRUE);
            gtk_label_set_max_width_chars (GTK_LABEL (hint_label), 48);
            /* Seleccionable para que la orden se pueda copiar con el mouse. */
            gtk_label_set_selectable (GTK_LABEL (hint_label), TRUE);
            gtk_style_context_add_class (
                gtk_widget_get_style_context (hint_label), "dim-label");
            gtk_box_pack_start (GTK_BOX (obex_box), hint_label, FALSE, FALSE, 0);
            g_free (obex_hint);
        }

        /* ---- recibir archivos ---- */
        GtkWidget *sep_recv = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
        gtk_widget_set_margin_top (sep_recv, 4);
        gtk_widget_set_margin_bottom (sep_recv, 4);
        gtk_box_pack_start (GTK_BOX (obex_box), sep_recv, FALSE, FALSE, 0);

        pd->recv_check = gtk_check_button_new_with_label (
            _("Receive files from other devices"));
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->recv_check),
                                      bp->receive_files);
        gtk_box_pack_start (GTK_BOX (obex_box), pd->recv_check, FALSE, FALSE, 0);

        GtkWidget *folder_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_pack_start (GTK_BOX (obex_box), folder_row, FALSE, FALSE, 0);

        GtkWidget *folder_lbl = gtk_label_new (_("Save to:"));
        gtk_label_set_xalign (GTK_LABEL (folder_lbl), 0.0);
        gtk_box_pack_start (GTK_BOX (folder_row), folder_lbl, FALSE, FALSE, 0);

        pd->recv_folder_btn = gtk_file_chooser_button_new (
            _("Destination folder"), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
        if (bp->receive_folder != NULL)
            gtk_file_chooser_set_filename (
                GTK_FILE_CHOOSER (pd->recv_folder_btn), bp->receive_folder);
        gtk_widget_set_sensitive (pd->recv_folder_btn, bp->receive_files);
        gtk_box_pack_start (GTK_BOX (folder_row), pd->recv_folder_btn,
                            TRUE, TRUE, 0);

        pd->recv_status = gtk_label_new (NULL);
        gtk_label_set_xalign (GTK_LABEL (pd->recv_status), 0.0);
        gtk_label_set_line_wrap (GTK_LABEL (pd->recv_status), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (pd->recv_status), 48);
        gtk_style_context_add_class (
            gtk_widget_get_style_context (pd->recv_status), "dim-label");
        gtk_box_pack_start (GTK_BOX (obex_box), pd->recv_status,
                            FALSE, FALSE, 0);

        update_recv_status (pd);

        /* En vivo: si el usuario cierra el otro programa que tenía el puesto
         * tomado, el texto cambia solo, sin cerrar y reabrir esta ventana. */
        pd->recv_status_id = g_timeout_add_seconds (2, on_recv_status_tick, pd);

        /* ---- aviso de enlace simbólico en la carpeta de recepción ----
         *
         * Empieza oculto. gtk_widget_set_no_show_all() es necesario porque
         * más abajo se llama gtk_widget_show_all() sobre todo el diálogo:
         * sin esa marca, show_all volvería a mostrar el recuadro aunque acá
         * lo hayamos ocultado. */
        pd->symlink_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_no_show_all (pd->symlink_box, TRUE);
        gtk_widget_set_margin_top (pd->symlink_box, 4);
        gtk_box_pack_start (GTK_BOX (obex_box), pd->symlink_box, FALSE, FALSE, 0);

        pd->symlink_label = gtk_label_new (NULL);
        gtk_label_set_xalign (GTK_LABEL (pd->symlink_label), 0.0);
        gtk_label_set_line_wrap (GTK_LABEL (pd->symlink_label), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (pd->symlink_label), 48);
        gtk_style_context_add_class (
            gtk_widget_get_style_context (pd->symlink_label), "warning");
        gtk_box_pack_start (GTK_BOX (pd->symlink_box), pd->symlink_label,
                            FALSE, FALSE, 0);

        pd->symlink_fix_btn = gtk_button_new_with_label (_("Fix automatically"));
        gtk_widget_set_halign (pd->symlink_fix_btn, GTK_ALIGN_START);
        gtk_box_pack_start (GTK_BOX (pd->symlink_box), pd->symlink_fix_btn,
                            FALSE, FALSE, 0);

        update_symlink_warning (pd);

        gtk_widget_set_margin_top (obex_frame, 8);
        gtk_box_pack_start (GTK_BOX (content), obex_frame, FALSE, FALSE, 0);
    }

    g_signal_connect (pd->square_check,        "toggled",       G_CALLBACK (on_square_toggled),      pd);
    g_signal_connect (pd->plugin_spin,         "value-changed", G_CALLBACK (on_plugin_size_changed), pd);
    g_signal_connect (pd->panel_icon_check,    "toggled",       G_CALLBACK (on_panel_icon_toggled),  pd);
    g_signal_connect (pd->icon_spin,           "value-changed", G_CALLBACK (on_icon_size_changed),   pd);
    g_signal_connect (pd->popup_width_spin,    "value-changed", G_CALLBACK (on_popup_width_changed), pd);
    g_signal_connect (pd->popup_height_spin,   "value-changed", G_CALLBACK (on_popup_height_changed),pd);
    g_signal_connect (pd->separators_check,    "toggled",       G_CALLBACK (on_separators_toggled),  pd);
    g_signal_connect (pd->unnamed_check,       "toggled",       G_CALLBACK (on_unnamed_toggled),     pd);
    g_signal_connect (pd->hide_check,          "toggled",       G_CALLBACK (on_hide_toggled),        pd);
#ifdef HAVE_LIBNOTIFY
    g_signal_connect (pd->notifications_check, "toggled",       G_CALLBACK (on_notifications_toggled), pd);
#endif
    g_signal_connect (pd->recv_check,      "toggled",  G_CALLBACK (on_recv_toggled),    pd);
    g_signal_connect (pd->recv_folder_btn, "file-set", G_CALLBACK (on_recv_folder_set), pd);
    g_signal_connect (pd->symlink_fix_btn, "clicked",  G_CALLBACK (on_symlink_fix_clicked), pd);
    g_signal_connect (dialog, "destroy", G_CALLBACK (on_props_dialog_destroy), pd);

    xfce_panel_plugin_block_menu (plugin);
    g_signal_connect_swapped (dialog, "response",
                              G_CALLBACK (gtk_widget_destroy), dialog);
    g_signal_connect_swapped (dialog, "destroy",
                              G_CALLBACK (xfce_panel_plugin_unblock_menu), plugin);

    gtk_widget_show_all (dialog);
}

/* ---------- spinner del panel ---------- */

void
bt_plugin_set_busy (gpointer bp_ptr, gboolean busy)
{
    BtPlugin *bp = bp_ptr;
    if (!bp || bp->busy == busy)
        return;
    bp->busy = busy;
    if (busy) {
        gtk_spinner_start (GTK_SPINNER (bp->spinner));
        gtk_stack_set_visible_child_name (GTK_STACK (bp->stack), "spinner");
    } else {
        gtk_spinner_stop (GTK_SPINNER (bp->spinner));
        gtk_stack_set_visible_child_name (GTK_STACK (bp->stack), "icon");
    }
}

/* ---------- refresco por señales DBus ---------- */

static gboolean
on_bt_changed_cb (gpointer user_data)
{
    BtPlugin *bp = user_data;
    if (!bp->conn)
        return G_SOURCE_REMOVE;
    g_object_set_data (G_OBJECT (bp->button), "bt-refresh-pending", NULL);

    /* Una sola lectura: ícono, tooltip y notificaciones la comparten. */
    bt_cache_begin (bp->conn);
    BtStatus *st = bt_get_status (bp->conn);
    bt_cache_end ();

    gboolean prev_available = bp->last_available;
#ifdef HAVE_LIBNOTIFY
    gint     prev_connected = bp->last_connected;
#endif
    gchar   *prev_name      = bp->last_name;

    /* bluetoothd volvió al bus tras un reinicio: se le olvidaron todos los
     * agentes registrados, así que hay que volver a presentarse. */
    if (st->available && !prev_available && bp->agent)
        bt_agent_reregister (bp->agent);

    update_panel_icon (bp, st->available, st->powered, st->n_connected);

#ifdef HAVE_LIBNOTIFY
    if (bp->notif_ready && bp->show_notifications) {
        NotifyNotification *notif = NULL;
        if (st->n_connected > prev_connected && st->first_name) {
            gchar *body = g_strdup_printf (_("%s connected"), st->first_name);
            notif = notify_notification_new (_("Bluetooth"), body,
                                             "bluetooth-active-symbolic");
            g_free (body);
        } else if (st->n_connected < prev_connected) {
            gchar *body = prev_name
                ? g_strdup_printf (_("%s disconnected"), prev_name)
                : g_strdup (_("Device disconnected"));
            notif = notify_notification_new (_("Bluetooth"), body,
                                             "bluetooth-disabled-symbolic");
            g_free (body);
        }
        if (notif) {
            notify_notification_show (notif, NULL);
            g_object_unref (notif);
        }
    }
    bp->notif_ready = TRUE;
#endif

    /* Tooltip */
    {
        gchar *tip;
        if (!st->available)
            tip = g_strdup (_("No Bluetooth adapter"));
        else if (!st->powered)
            tip = g_strdup (_("Bluetooth is off"));
        else if (st->n_connected == 0)
            tip = g_strdup (_("Bluetooth on — no devices connected"));
        else if (st->n_connected == 1)
            tip = g_strdup_printf (_("Connected to %s"), st->first_name);
        else
            tip = g_strdup_printf (_("%d devices connected"), st->n_connected);
        gtk_widget_set_tooltip_text (bp->button, tip);
        g_free (tip);
    }

    bp->last_name = g_strdup (st->first_name);
    g_free (prev_name);

    bt_status_free (st);

    /* El popup tiene su propia suscripción mientras está abierto: acá solo
     * nos ocupamos del ícono y el tooltip del botón del panel. */
    return G_SOURCE_REMOVE;
}

static void
on_bt_changed (gpointer user_data)
{
    BtPlugin *bp = user_data;
    /* Coalescer: BlueZ manda ráfagas de señales durante el descubrimiento. */
    if (g_object_get_data (G_OBJECT (bp->button), "bt-refresh-pending"))
        return;
    g_object_set_data (G_OBJECT (bp->button), "bt-refresh-pending",
                       GINT_TO_POINTER (1));
    g_timeout_add (150, on_bt_changed_cb, bp);
}

/* ---------- callbacks del panel ---------- */

static void
on_button_toggled (GtkToggleButton *btn, BtPlugin *bp)
{
    (void) btn;
    if (!bp->conn) {
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (bp->button), FALSE);
        return;
    }
    if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (bp->button))) {
        bp->popup->show_separators = bp->show_separators;
        bp->popup->show_unnamed    = bp->show_unnamed;
        popup_show (bp->popup, bp->plugin, bp->button, bp->conn,
                    bp->popup_width, bp->popup_height);
    } else {
        popup_hide (bp->popup);
    }
}

static void
on_popup_hidden (GtkWidget *win, BtPlugin *bp)
{
    (void) win;
    g_signal_handlers_block_by_func (bp->button, on_button_toggled, bp);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (bp->button), FALSE);
    g_signal_handlers_unblock_by_func (bp->button, on_button_toggled, bp);
}

static gboolean
on_size_changed (XfcePanelPlugin *plugin, gint size, BtPlugin *bp)
{
    (void) plugin; (void) size;
    apply_config (bp);
    return TRUE;
}

/* ---------- construcción ---------- */

/* ---------- instancia única (ver comentario junto a BT_PLUGIN_SINGLETON_NAME) ---------- */

static void
on_bt_name_acquired (GDBusConnection *conn, const gchar *name, gpointer user_data)
{
    (void) conn; (void) name; (void) user_data;
    /* Confirmado: esta es la instancia real. No hace falta hacer nada más,
     * ya se construyó todo de forma optimista en bt_plugin_new. */
}

static void
on_bt_name_lost (GDBusConnection *conn, const gchar *name, gpointer user_data)
{
    (void) conn; (void) name;
    BtPlugin *bp = user_data;

    if (bp->is_duplicate)
        return; /* ya procesado (no debería llamarse dos veces, pero por las dudas) */
    bp->is_duplicate = TRUE;

    /* Deshacer lo que se registró de forma optimista al construir: esta
     * instancia no debe competir con la real por el agente de pareo ni
     * duplicar notificaciones de conexión/desconexión. */
    if (bp->signal_ids) {
        bt_unsubscribe_signals (bp->conn, bp->signal_ids);
        bp->signal_ids = NULL;
    }
    if (bp->agent) {
        bt_agent_free (bp->agent);
        bp->agent = NULL;
    }
    if (bp->obex_agent) {
        bt_obex_agent_free (bp->obex_agent);
        bp->obex_agent = NULL;
    }

    gtk_widget_set_sensitive (bp->button, FALSE);
    gtk_widget_set_tooltip_text (bp->button,
        _("There is already another instance of this plugin on the panel."));

    GtkWidget *dlg = gtk_message_dialog_new (NULL, 0, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_OK, "%s",
        _("There is already another instance of this plugin on the panel."));
    gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dlg), "%s",
        _("This instance will remain inactive. Remove it from the panel to avoid confusion."));
    g_signal_connect (dlg, "response", G_CALLBACK (gtk_widget_destroy), NULL);
    gtk_widget_show (dlg);
}

static BtPlugin *
bt_plugin_new (XfcePanelPlugin *plugin)
{
    BtPlugin *bp = g_new0 (BtPlugin, 1);
    bp->plugin = plugin;

    bp->is_duplicate = FALSE;
    bp->conn   = bt_dbus_connect ();

    load_config (bp);

    bp->button = gtk_toggle_button_new ();
    gtk_button_set_relief (GTK_BUTTON (bp->button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text (bp->button, _("Bluetooth"));
    xfce_panel_plugin_set_shrink (plugin, TRUE);
    xfce_panel_plugin_set_small  (plugin, TRUE);

    bp->icon_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    {
        const gchar *names[] = { "bluetooth-disabled-symbolic",
                                 "bluetooth-symbolic", "bluetooth", NULL };
        GIcon     *gicon = g_themed_icon_new_from_names ((gchar **) names, -1);
        GtkWidget *init  = gtk_image_new_from_gicon (gicon, GTK_ICON_SIZE_BUTTON);
        g_object_unref (gicon);
        gtk_image_set_pixel_size (GTK_IMAGE (init), bp->icon_size);
        gtk_box_pack_start (GTK_BOX (bp->icon_box), init, TRUE, TRUE, 0);
    }

    bp->spinner = gtk_spinner_new ();
    bp->stack   = gtk_stack_new ();
    gtk_stack_add_named (GTK_STACK (bp->stack), bp->icon_box, "icon");
    gtk_stack_add_named (GTK_STACK (bp->stack), bp->spinner,  "spinner");
    gtk_stack_set_visible_child_name (GTK_STACK (bp->stack), "icon");
    gtk_container_add (GTK_CONTAINER (bp->button), bp->stack);
    gtk_widget_show_all (bp->button);

    bp->popup = popup_create (plugin, bp->button);
    bp->popup->plugin_ref      = bp;
    bp->popup->show_separators = bp->show_separators;
    bp->popup->show_unnamed    = bp->show_unnamed;

    /* El agente de pareo: registrado a nivel de plugin, para que atienda los
     * pedidos aunque el popup esté cerrado. Se registra de forma optimista:
     * todavía no sabemos si esta instancia es duplicada (esa respuesta llega
     * después, asíncrona, por g_bus_own_name). Si resulta serlo, se
     * desregistra en on_bt_name_lost. */
    if (bp->conn)
        bp->agent = bt_agent_new (bp->conn);

    /* Receptor de archivos, si está prendido. Igual que el agente de pareo,
     * se crea de forma optimista: todavía no sabemos si esta instancia es
     * duplicada. Si resulta serlo, se destruye en on_bt_name_lost. */
    apply_receive_config (bp);

    g_signal_connect (plugin, "size-changed", G_CALLBACK (on_size_changed), bp);
    g_signal_connect (bp->button, "toggled",  G_CALLBACK (on_button_toggled), bp);

    if (bp->conn)
        bp->signal_ids = bt_subscribe_signals (bp->conn, on_bt_changed, bp);
    g_signal_connect (bp->popup->window, "hide",
                      G_CALLBACK (on_popup_hidden), bp);

    on_bt_changed_cb (bp);
    apply_config (bp);

    /* Pedido de instancia única: quien lo consiga primero es la instancia
     * real; a las demás el bus les avisa con "name lost" (ver
     * on_bt_name_lost) y ahí se desactivan. DO_NOT_QUEUE es la parte
     * importante: sin ella, el bus haría cola y nos daría el nombre más
     * tarde si la primera instancia se cierra, lo cual no es lo que
     * queremos detectar acá. */
    bp->name_owner_id = g_bus_own_name (
        G_BUS_TYPE_SESSION, BT_PLUGIN_SINGLETON_NAME,
        G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
        NULL, on_bt_name_acquired, on_bt_name_lost, bp, NULL);

    xfce_panel_plugin_add_action_widget (plugin, bp->button);
    gtk_container_add (GTK_CONTAINER (plugin), bp->button);

    return bp;
}

static void
bt_plugin_about (XfcePanelPlugin *plugin, BtPlugin *bp)
{
    (void) plugin; (void) bp;
    const gchar *authors[] = { "Tantin", NULL };
    GdkPixbuf *logo = gtk_icon_theme_load_icon (gtk_icon_theme_get_default (),
                                                "bluetooth", 48, 0, NULL);
    gtk_show_about_dialog (NULL,
        "program-name", "xfce-bluetooth-plugin",
        "version",      "0.1.0",
        "comments",     _("Bluetooth manager plugin for the XFCE panel"),
        "authors",      authors,
        "license-type", GTK_LICENSE_GPL_2_0,
        "website",      "https://github.com/Tantin1/xfce-bluetooth-plugin",
        "logo",         logo,
        NULL);
    if (logo)
        g_object_unref (logo);
}

static void
bt_plugin_free (XfcePanelPlugin *plugin, BtPlugin *bp)
{
    (void) plugin;
    if (bp->name_owner_id)
        g_bus_unown_name (bp->name_owner_id);
    if (bp->signal_ids)
        bt_unsubscribe_signals (bp->conn, bp->signal_ids);
    if (bp->agent)
        bt_agent_free (bp->agent);
    if (bp->obex_agent)
        bt_obex_agent_free (bp->obex_agent);
    popup_destroy (bp->popup);
    if (bp->conn)
        g_object_unref (bp->conn);
    if (bp->button_css)
        g_object_unref (bp->button_css);
    g_free (bp->last_name);
    g_free (bp->receive_folder);
    g_free (bp);
}

static void
bt_plugin_construct (XfcePanelPlugin *plugin)
{
    bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);
#ifdef HAVE_LIBNOTIFY
    notify_init ("xfce-bluetooth-plugin");
#endif

    BtPlugin *bp = bt_plugin_new (plugin);

    xfce_panel_plugin_menu_show_configure (plugin);
    xfce_panel_plugin_menu_show_about (plugin);
    g_signal_connect (plugin, "about",            G_CALLBACK (bt_plugin_about),     bp);
    g_signal_connect (plugin, "free-data",        G_CALLBACK (bt_plugin_free),      bp);
    g_signal_connect (plugin, "configure-plugin", G_CALLBACK (on_configure_plugin), bp);
    /* OJO: "size-changed" ya se conecta en bt_plugin_new; conectarla otra vez
     * acá hacía que apply_config corriera dos veces por cada cambio. */
}

XFCE_PANEL_PLUGIN_REGISTER (bt_plugin_construct);
