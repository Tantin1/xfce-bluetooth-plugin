/* bt-agent.c — implementación del agente de pareo.
 *
 * Cada método de org.bluez.Agent1 que necesita respuesta del usuario retiene
 * el GDBusMethodInvocation mientras el diálogo está en pantalla y contesta
 * recién en el "response". Si BlueZ llama a Cancel mientras tanto (el otro
 * aparato abortó), cerramos el diálogo y contestamos org.bluez.Error.Canceled.
 */

#include "bt-agent.h"
#include "bt-dbus.h"

#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <string.h>
#include <unistd.h>

typedef enum {
    REQ_NONE,
    REQ_CONFIRMATION,   /* comparación numérica: responde vacío o Rejected */
    REQ_AUTHORIZATION,  /* aceptar el pareo a secas */
    REQ_SERVICE,        /* autorizar un servicio concreto */
    REQ_PINCODE,        /* devuelve 's' */
    REQ_PASSKEY         /* devuelve 'u' */
} ReqKind;

struct _BtAgent {
    GDBusConnection       *conn;
    GDBusNodeInfo         *node;
    gchar                 *path;
    guint                  reg_id;
    gboolean               is_default;

    /* Pedido en curso (solo puede haber uno: BlueZ serializa el pareo). */
    GDBusMethodInvocation *pending;
    ReqKind                pending_kind;
    GtkWidget             *dialog;
    GtkWidget             *entry;

    /* Diálogo puramente informativo (DisplayPasskey / DisplayPinCode). */
    GtkWidget             *display_dialog;
    GtkWidget             *display_label;
};

static const gchar agent_introspection_xml[] =
    "<node>"
    "  <interface name='org.bluez.Agent1'>"
    "    <method name='Release'/>"
    "    <method name='RequestPinCode'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='s' name='pincode' direction='out'/>"
    "    </method>"
    "    <method name='DisplayPinCode'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='s' name='pincode' direction='in'/>"
    "    </method>"
    "    <method name='RequestPasskey'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='u' name='passkey' direction='out'/>"
    "    </method>"
    "    <method name='DisplayPasskey'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='u' name='passkey' direction='in'/>"
    "      <arg type='q' name='entered' direction='in'/>"
    "    </method>"
    "    <method name='RequestConfirmation'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='u' name='passkey' direction='in'/>"
    "    </method>"
    "    <method name='RequestAuthorization'>"
    "      <arg type='o' name='device' direction='in'/>"
    "    </method>"
    "    <method name='AuthorizeService'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='s' name='uuid' direction='in'/>"
    "    </method>"
    "    <method name='Cancel'/>"
    "  </interface>"
    "</node>";

/* ---------- helpers ---------- */

/* El popup toma el asa (seat grab) mientras está abierto. Un diálogo que
 * aparece por debajo de ese asa queda sordo al teclado y al ratón, así que
 * la soltamos antes de mostrar cualquier cosa del agente. El popup se cierra
 * solo al perder el foco, que es exactamente lo que queremos acá. */
static void
release_seat_grab (void)
{
    GdkDisplay *display = gdk_display_get_default ();
    if (!display)
        return;
    GdkSeat *seat = gdk_display_get_default_seat (display);
    if (seat)
        gdk_seat_ungrab (seat);
}

/* Nombre legible del dispositivo a partir de su ruta de objeto.
 *
 * La lectura va dentro de una instantánea a propósito. Sin ella, armar el
 * dispositivo costaba dos idas y vueltas sincrónicas al bus (sus propiedades
 * y, aparte, la batería, que casi ningún aparato implementa y que por eso
 * terminaba en un error con su espera). Con instantánea es una sola lectura,
 * y de la interfaz de batería ausente ya se sabe la respuesta sin preguntar.
 * Importa porque esto corre justo antes de mostrar el cartel de pareo, con
 * el panel esperando. */
static gchar *
device_label (BtAgent *agent, const gchar *device_path)
{
    bt_cache_begin (agent->conn);
    BtDevice *d = bt_get_device (agent->conn, device_path);
    bt_cache_end ();
    gchar    *out;
    if (d && d->name)
        out = g_strdup (d->name);
    else {
        /* dev_AA_BB_CC_DD_EE_FF → AA:BB:CC:DD:EE:FF */
        const gchar *tail = strrchr (device_path, '/');
        tail = tail ? tail + 1 : device_path;
        if (g_str_has_prefix (tail, "dev_")) {
            gchar *mac = g_strdup (tail + 4);
            for (gchar *p = mac; *p; p++)
                if (*p == '_') *p = ':';
            out = mac;
        } else {
            out = g_strdup (tail);
        }
    }
    bt_device_free (d);
    return out;
}

static void
clear_pending (BtAgent *agent)
{
    agent->pending      = NULL;
    agent->pending_kind = REQ_NONE;
    agent->entry        = NULL;
    if (agent->dialog) {
        GtkWidget *d = agent->dialog;
        agent->dialog = NULL;
        gtk_widget_destroy (d);
    }
}

static void
close_display_dialog (BtAgent *agent)
{
    if (agent->display_dialog) {
        GtkWidget *d = agent->display_dialog;
        agent->display_dialog = NULL;
        agent->display_label  = NULL;
        gtk_widget_destroy (d);
    }
}

/* Contesta al pedido en curso con un error y limpia el estado. */
static void
reject_pending (BtAgent *agent, const gchar *dbus_error, const gchar *msg)
{
    if (!agent->pending)
        return;
    GDBusMethodInvocation *inv = agent->pending;
    agent->pending = NULL;
    g_dbus_method_invocation_return_dbus_error (inv, dbus_error, msg);
}

/* ---------- respuesta de los diálogos ---------- */

static void
on_dialog_response (GtkDialog *dialog, gint response, gpointer user_data)
{
    BtAgent *agent = user_data;

    /* El diálogo ya no nos pertenece: evitamos que clear_pending lo destruya
     * dos veces. */
    if (agent->dialog == GTK_WIDGET (dialog))
        agent->dialog = NULL;

    GDBusMethodInvocation *inv  = agent->pending;
    ReqKind                kind = agent->pending_kind;
    GtkWidget             *entry = agent->entry;

    agent->pending      = NULL;
    agent->pending_kind = REQ_NONE;
    agent->entry        = NULL;

    if (inv) {
        if (response == GTK_RESPONSE_ACCEPT) {
            switch (kind) {
                case REQ_PINCODE: {
                    const gchar *txt = entry && GTK_IS_ENTRY (entry)
                                       ? gtk_entry_get_text (GTK_ENTRY (entry))
                                       : "";
                    g_dbus_method_invocation_return_value (
                        inv, g_variant_new ("(s)", txt));
                    break;
                }
                case REQ_PASSKEY: {
                    const gchar *txt = entry && GTK_IS_ENTRY (entry)
                                       ? gtk_entry_get_text (GTK_ENTRY (entry))
                                       : "0";
                    guint32 passkey = (guint32) g_ascii_strtoull (txt, NULL, 10);
                    g_dbus_method_invocation_return_value (
                        inv, g_variant_new ("(u)", passkey));
                    break;
                }
                default:
                    /* Confirmación / autorización: respuesta vacía = aceptado. */
                    g_dbus_method_invocation_return_value (inv, NULL);
                    break;
            }
        } else {
            g_dbus_method_invocation_return_dbus_error (
                inv, "org.bluez.Error.Rejected",
                "Rejected by the user");
        }
    }

    gtk_widget_destroy (GTK_WIDGET (dialog));
}

/* ---------- construcción de diálogos ---------- */

static GtkWidget *
make_dialog (const gchar *title, const gchar *icon_name)
{
    GtkWidget *dialog = gtk_dialog_new ();
    gtk_window_set_title      (GTK_WINDOW (dialog), title);
    gtk_window_set_position   (GTK_WINDOW (dialog), GTK_WIN_POS_CENTER);
    gtk_window_set_resizable  (GTK_WINDOW (dialog), FALSE);
    gtk_window_set_keep_above (GTK_WINDOW (dialog), TRUE);
    gtk_window_set_urgency_hint (GTK_WINDOW (dialog), TRUE);
    if (icon_name)
        gtk_window_set_icon_name (GTK_WINDOW (dialog), icon_name);
    return dialog;
}

/* Cuerpo común: ícono grande a la izquierda, texto a la derecha. */
static GtkWidget *
dialog_body (GtkWidget *dialog, const gchar *heading, const gchar *detail)
{
    GtkWidget *area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_container_set_border_width (GTK_CONTAINER (area), 12);
    gtk_box_set_spacing (GTK_BOX (area), 8);

    GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start (GTK_BOX (area), hbox, FALSE, FALSE, 0);

    GtkWidget *img = gtk_image_new_from_icon_name ("bluetooth",
                                                   GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_valign (img, GTK_ALIGN_START);
    gtk_box_pack_start (GTK_BOX (hbox), img, FALSE, FALSE, 0);

    GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start (GTK_BOX (hbox), vbox, TRUE, TRUE, 0);

    GtkWidget *head = gtk_label_new (NULL);
    gchar *markup = g_markup_printf_escaped ("<b>%s</b>", heading);
    gtk_label_set_markup  (GTK_LABEL (head), markup);
    g_free (markup);
    gtk_label_set_xalign     (GTK_LABEL (head), 0.0);
    gtk_label_set_line_wrap  (GTK_LABEL (head), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (head), 40);
    gtk_box_pack_start (GTK_BOX (vbox), head, FALSE, FALSE, 0);

    if (detail) {
        GtkWidget *det = gtk_label_new (detail);
        gtk_label_set_xalign    (GTK_LABEL (det), 0.0);
        gtk_label_set_line_wrap (GTK_LABEL (det), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (det), 40);
        gtk_style_context_add_class (gtk_widget_get_style_context (det),
                                     "dim-label");
        gtk_box_pack_start (GTK_BOX (vbox), det, FALSE, FALSE, 0);
    }

    return vbox;
}

/* Diálogo de confirmación (sí/no). */
static void
show_confirm_dialog (BtAgent *agent, GDBusMethodInvocation *inv, ReqKind kind,
                     const gchar *heading, const gchar *detail,
                     const gchar *big_code, const gchar *accept_label)
{
    release_seat_grab ();
    clear_pending (agent);

    GtkWidget *dialog = make_dialog (_("Bluetooth pairing"), "bluetooth");
    GtkWidget *vbox   = dialog_body (dialog, heading, detail);

    if (big_code) {
        GtkWidget *code = gtk_label_new (NULL);
        gchar *markup = g_markup_printf_escaped (
            "<span size='xx-large' weight='bold'>%s</span>", big_code);
        gtk_label_set_markup (GTK_LABEL (code), markup);
        g_free (markup);
        gtk_label_set_xalign (GTK_LABEL (code), 0.0);
        gtk_widget_set_margin_top    (code, 6);
        gtk_widget_set_margin_bottom (code, 6);
        gtk_box_pack_start (GTK_BOX (vbox), code, FALSE, FALSE, 0);
    }

    gtk_dialog_add_button (GTK_DIALOG (dialog), _("Cancel"), GTK_RESPONSE_REJECT);
    GtkWidget *ok = gtk_dialog_add_button (GTK_DIALOG (dialog),
                                           accept_label, GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class (gtk_widget_get_style_context (ok),
                                 "suggested-action");
    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);

    agent->pending      = inv;
    agent->pending_kind = kind;
    agent->dialog       = dialog;

    g_signal_connect (dialog, "response",
                      G_CALLBACK (on_dialog_response), agent);
    gtk_widget_show_all (dialog);
    gtk_window_present (GTK_WINDOW (dialog));
}

/* Diálogo con campo de texto (PIN o clave numérica). */
static void
show_entry_dialog (BtAgent *agent, GDBusMethodInvocation *inv, ReqKind kind,
                   const gchar *heading, const gchar *detail,
                   gboolean numeric_only)
{
    release_seat_grab ();
    clear_pending (agent);

    GtkWidget *dialog = make_dialog (_("Bluetooth pairing"), "bluetooth");
    GtkWidget *vbox   = dialog_body (dialog, heading, detail);

    GtkWidget *entry = gtk_entry_new ();
    gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
    gtk_entry_set_max_length (GTK_ENTRY (entry), numeric_only ? 6 : 16);
    if (numeric_only)
        gtk_entry_set_input_purpose (GTK_ENTRY (entry),
                                     GTK_INPUT_PURPOSE_DIGITS);
    gtk_widget_set_margin_top (entry, 6);
    gtk_box_pack_start (GTK_BOX (vbox), entry, FALSE, FALSE, 0);

    gtk_dialog_add_button (GTK_DIALOG (dialog), _("Cancel"), GTK_RESPONSE_REJECT);
    GtkWidget *ok = gtk_dialog_add_button (GTK_DIALOG (dialog),
                                           _("Pair"), GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class (gtk_widget_get_style_context (ok),
                                 "suggested-action");
    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);

    agent->pending      = inv;
    agent->pending_kind = kind;
    agent->dialog       = dialog;
    agent->entry        = entry;

    g_signal_connect (dialog, "response",
                      G_CALLBACK (on_dialog_response), agent);
    gtk_widget_show_all (dialog);
    gtk_window_present (GTK_WINDOW (dialog));
    gtk_widget_grab_focus (entry);
}

/* Diálogo informativo: "escribí este código en el otro aparato". No retiene
 * el invocation — a estos métodos hay que contestarles enseguida. */
static void
show_display_dialog (BtAgent *agent, const gchar *heading, const gchar *code)
{
    release_seat_grab ();

    if (agent->display_dialog && agent->display_label) {
        gchar *markup = g_markup_printf_escaped (
            "<span size='xx-large' weight='bold'>%s</span>", code);
        gtk_label_set_markup (GTK_LABEL (agent->display_label), markup);
        g_free (markup);
        return;
    }

    close_display_dialog (agent);

    GtkWidget *dialog = make_dialog (_("Bluetooth pairing"), "bluetooth");
    GtkWidget *vbox   = dialog_body (dialog, heading,
        _("Type this code on the other device and confirm."));

    GtkWidget *label = gtk_label_new (NULL);
    gchar *markup = g_markup_printf_escaped (
        "<span size='xx-large' weight='bold'>%s</span>", code);
    gtk_label_set_markup (GTK_LABEL (label), markup);
    g_free (markup);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_widget_set_margin_top    (label, 6);
    gtk_widget_set_margin_bottom (label, 6);
    gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 0);

    gtk_dialog_add_button (GTK_DIALOG (dialog), _("Close"), GTK_RESPONSE_CLOSE);
    g_signal_connect (dialog, "response",
                      G_CALLBACK (gtk_widget_destroy), NULL);
    g_signal_connect (dialog, "destroy",
                      G_CALLBACK (gtk_widget_destroyed),
                      &agent->display_dialog);

    agent->display_dialog = dialog;
    agent->display_label  = label;

    gtk_widget_show_all (dialog);
    gtk_window_present (GTK_WINDOW (dialog));
}

/* ---------- despacho de los métodos de org.bluez.Agent1 ---------- */

static void
agent_method_call (GDBusConnection       *conn,
                   const gchar           *sender,
                   const gchar           *object_path,
                   const gchar           *interface_name,
                   const gchar           *method_name,
                   GVariant              *params,
                   GDBusMethodInvocation *invocation,
                   gpointer               user_data)
{
    (void) conn; (void) sender; (void) object_path; (void) interface_name;
    BtAgent *agent = user_data;

    if (g_strcmp0 (method_name, "Release") == 0) {
        close_display_dialog (agent);
        reject_pending (agent, "org.bluez.Error.Canceled", "Agent released");
        clear_pending (agent);
        agent->is_default = FALSE;
        g_dbus_method_invocation_return_value (invocation, NULL);
        return;
    }

    if (g_strcmp0 (method_name, "Cancel") == 0) {
        close_display_dialog (agent);
        reject_pending (agent, "org.bluez.Error.Canceled",
                        "Cancelled by the remote device");
        clear_pending (agent);
        g_dbus_method_invocation_return_value (invocation, NULL);
        return;
    }

    if (g_strcmp0 (method_name, "RequestConfirmation") == 0) {
        const gchar *path = NULL;
        guint32      passkey = 0;
        g_variant_get (params, "(&ou)", &path, &passkey);

        gchar *name = device_label (agent, path);
        gchar *head = g_strdup_printf (_("Pair with “%s”?"), name);
        gchar *code = g_strdup_printf ("%06u", passkey);
        show_confirm_dialog (agent, invocation, REQ_CONFIRMATION, head,
            _("Check that the same code is shown on the other device."),
            code, _("Pair"));
        g_free (code); g_free (head); g_free (name);
        return;
    }

    if (g_strcmp0 (method_name, "RequestAuthorization") == 0) {
        const gchar *path = NULL;
        g_variant_get (params, "(&o)", &path);

        gchar *name = device_label (agent, path);
        gchar *head = g_strdup_printf (_("Pair with “%s”?"), name);
        show_confirm_dialog (agent, invocation, REQ_AUTHORIZATION, head,
            _("This device does not show a confirmation code. "
              "Only accept if you started the pairing yourself."),
            NULL, _("Pair"));
        g_free (head); g_free (name);
        return;
    }

    if (g_strcmp0 (method_name, "AuthorizeService") == 0) {
        const gchar *path = NULL, *uuid = NULL;
        g_variant_get (params, "(&o&s)", &path, &uuid);

        gchar  *name  = device_label (agent, path);
        gchar  *uuids[2] = { (gchar *) uuid, NULL };
        gchar  *profile = bt_profiles_string (uuids);
        gchar  *head  = g_strdup_printf (_("Allow “%s” to connect?"), name);
        gchar  *detail = profile
            ? g_strdup_printf (_("Requested service: %s"), profile)
            : g_strdup (_("The device is asking to use a service on this computer."));

        show_confirm_dialog (agent, invocation, REQ_SERVICE, head, detail,
                             NULL, _("Allow"));
        g_free (detail); g_free (head); g_free (profile); g_free (name);
        return;
    }

    if (g_strcmp0 (method_name, "RequestPinCode") == 0) {
        const gchar *path = NULL;
        g_variant_get (params, "(&o)", &path);

        gchar *name = device_label (agent, path);
        gchar *head = g_strdup_printf (_("Enter the PIN for “%s”"), name);
        show_entry_dialog (agent, invocation, REQ_PINCODE, head,
            _("Older devices usually use 0000 or 1234."), FALSE);
        g_free (head); g_free (name);
        return;
    }

    if (g_strcmp0 (method_name, "RequestPasskey") == 0) {
        const gchar *path = NULL;
        g_variant_get (params, "(&o)", &path);

        gchar *name = device_label (agent, path);
        gchar *head = g_strdup_printf (_("Enter the code for “%s”"), name);
        show_entry_dialog (agent, invocation, REQ_PASSKEY, head,
            _("Type the code shown on the other device."), TRUE);
        g_free (head); g_free (name);
        return;
    }

    if (g_strcmp0 (method_name, "DisplayPinCode") == 0) {
        const gchar *path = NULL, *pin = NULL;
        g_variant_get (params, "(&o&s)", &path, &pin);

        gchar *name = device_label (agent, path);
        gchar *head = g_strdup_printf (_("Pairing with “%s”"), name);
        show_display_dialog (agent, head, pin);
        g_free (head); g_free (name);

        g_dbus_method_invocation_return_value (invocation, NULL);
        return;
    }

    if (g_strcmp0 (method_name, "DisplayPasskey") == 0) {
        const gchar *path = NULL;
        guint32      passkey = 0;
        guint16      entered = 0;
        g_variant_get (params, "(&ouq)", &path, &passkey, &entered);

        gchar *name = device_label (agent, path);
        gchar *head = g_strdup_printf (_("Pairing with “%s”"), name);
        gchar *code = g_strdup_printf ("%06u", passkey);
        show_display_dialog (agent, head, code);
        g_free (code); g_free (head); g_free (name);

        g_dbus_method_invocation_return_value (invocation, NULL);
        return;
    }

    g_dbus_method_invocation_return_dbus_error (
        invocation, "org.bluez.Error.Rejected", "Unknown method");
}

static const GDBusInterfaceVTable agent_vtable = {
    agent_method_call, NULL, NULL, { 0 }
};

/* ---------- registro en BlueZ ---------- */

static void
on_request_default_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
    BtAgent  *agent = user_data;
    GError   *err   = NULL;
    GVariant *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src),
                                                 res, &err);
    if (r) {
        agent->is_default = TRUE;
        g_variant_unref (r);
    } else {
        /* Típico: blueman o gnome-bluetooth ya tomaron el puesto. Seguimos
         * registrados como agente secundario, así que no es fatal. */
        agent->is_default = FALSE;
        g_message ("bt-agent: no somos el agente por omisión (%s). "
                   "El pareo lo va a atender el otro agente del escritorio.",
                   err ? err->message : "?");
        if (err) g_error_free (err);
    }
}

static void
on_register_agent_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
    BtAgent  *agent = user_data;
    GError   *err   = NULL;
    GVariant *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src),
                                                 res, &err);
    if (!r) {
        /* AlreadyExists si ya estábamos registrados: seguimos igual y
         * pedimos ser el agente por omisión. */
        gchar *remote = g_dbus_error_get_remote_error (err);
        gboolean already = (remote &&
                            g_str_has_suffix (remote, ".AlreadyExists"));
        if (!already)
            g_warning ("bt-agent: RegisterAgent falló: %s",
                       err ? err->message : "?");
        g_free (remote);
        if (err) g_error_free (err);
        if (!already)
            return;
    } else {
        g_variant_unref (r);
    }

    g_dbus_connection_call (agent->conn, BT_BUS_NAME, BT_AGENT_MGR_PATH,
                            BT_AGENT_MGR_IFACE, "RequestDefaultAgent",
                            g_variant_new ("(o)", agent->path),
                            NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                            on_request_default_done, agent);
}

void
bt_agent_reregister (BtAgent *agent)
{
    if (!agent || !agent->conn)
        return;

    /* "KeyboardDisplay" es la capacidad más completa: habilita comparación
     * numérica, entrada de PIN y visualización. Con "NoInputNoOutput" BlueZ
     * acepta todo sin preguntar (Just Works), lo que es cómodo pero deja al
     * usuario sin la confirmación de código que hace segura la vinculación. */
    g_dbus_connection_call (agent->conn, BT_BUS_NAME, BT_AGENT_MGR_PATH,
                            BT_AGENT_MGR_IFACE, "RegisterAgent",
                            g_variant_new ("(os)", agent->path,
                                           "KeyboardDisplay"),
                            NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                            on_register_agent_done, agent);
}

BtAgent *
bt_agent_new (GDBusConnection *conn)
{
    if (!conn)
        return NULL;

    GError        *err  = NULL;
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml (agent_introspection_xml,
                                                        &err);
    if (!node) {
        g_warning ("bt-agent: introspección inválida: %s",
                   err ? err->message : "?");
        if (err) g_error_free (err);
        return NULL;
    }

    BtAgent *agent = g_new0 (BtAgent, 1);
    agent->conn = conn;
    agent->node = node;

    /* Ruta única por instancia: puede haber más de un panel, y dos objetos
     * en la misma ruta no pueden convivir en la misma conexión. */
    static guint counter = 0;
    agent->path = g_strdup_printf ("/org/xfce/BluetoothPlugin/agent_%d_%u",
                                   (int) getpid (), counter++);

    agent->reg_id = g_dbus_connection_register_object (
        conn, agent->path, node->interfaces[0],
        &agent_vtable, agent, NULL, &err);

    if (agent->reg_id == 0) {
        g_warning ("bt-agent: no se pudo exportar el objeto en %s: %s",
                   agent->path, err ? err->message : "?");
        if (err) g_error_free (err);
        g_dbus_node_info_unref (node);
        g_free (agent->path);
        g_free (agent);
        return NULL;
    }

    bt_agent_reregister (agent);
    return agent;
}

gboolean
bt_agent_is_default (BtAgent *agent)
{
    return agent ? agent->is_default : FALSE;
}

void
bt_agent_free (BtAgent *agent)
{
    if (!agent)
        return;

    close_display_dialog (agent);
    reject_pending (agent, "org.bluez.Error.Canceled", "Plugin shutting down");
    clear_pending (agent);

    if (agent->conn) {
        g_dbus_connection_call (agent->conn, BT_BUS_NAME, BT_AGENT_MGR_PATH,
                                BT_AGENT_MGR_IFACE, "UnregisterAgent",
                                g_variant_new ("(o)", agent->path),
                                NULL, G_DBUS_CALL_FLAGS_NONE, 2000,
                                NULL, NULL, NULL);
        if (agent->reg_id)
            g_dbus_connection_unregister_object (agent->conn, agent->reg_id);
    }

    if (agent->node)
        g_dbus_node_info_unref (agent->node);
    g_free (agent->path);
    g_free (agent);
}
