/* bt-obex-agent.c — implementación del agente de recepción de archivos.
 *
 * Ciclo de vida de un archivo entrante:
 *
 *   1. El aparato empuja el archivo. obexd nos llama a AuthorizePush con la
 *      ruta de un objeto de transferencia.
 *   2. Le leemos a ese objeto el nombre propuesto y el tamaño, y a su sesión
 *      la dirección del que manda. Con la dirección buscamos el nombre
 *      legible del aparato en el bus del sistema.
 *   3. Mostramos el cartel de aceptar o rechazar, RETENIENDO la llamada sin
 *      contestar. Igual que el agente de pareo.
 *   4. Si el usuario acepta, contestamos con la ruta completa de destino, ya
 *      resuelta para no pisar ningún archivo existente. Si rechaza,
 *      contestamos con un error.
 *   5. Abrimos una ventana de progreso independiente y nos quedamos
 *      escuchando esa transferencia: el avance va a la barra, y el resultado
 *      (recibido o fallado) queda escrito en la misma ventana. Cerrarla
 *      mientras recibe cancela la transferencia. Si salió bien, la ventana
 *      ofrece mostrar el archivo en su carpeta.
 */

#include "bt-obex-agent.h"
#include "bt-obex.h"
#include "bt-dbus.h"
#include "popup.h"

#include <gtk/gtk.h>
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>
#include <string.h>

#ifdef HAVE_LIBNOTIFY
#include <libnotify/notify.h>
#endif

#define OBEX_AGENT_MGR_IFACE "org.bluez.obex.AgentManager1"
#define OBEX_AGENT_IFACE     "org.bluez.obex.Agent1"
#define OBEX_TRANSFER_IFACE  "org.bluez.obex.Transfer1"
#define OBEX_SESSION_IFACE   "org.bluez.obex.Session1"
#define OBEX_AGENT_PATH      "/com/tantin/XfceBluetoothPlugin/ObexAgent"

/* Cada cuánto se reintenta el registro cuando el puesto está tomado o el
 * servicio no está. Un minuto: barato (una llamada al bus) y suficiente para
 * que el usuario cierre el otro programa y esto arranque solo. */
#define RECV_RETRY_SECONDS 60

/* Comprobación temprana tras aceptar: si obexd falla al crear el archivo en
 * disco, destruye el objeto de transferencia en milisegundos sin emitir el
 * cambio de estado a "error". Y si el otro extremo desaparece en mitad de la
 * recepción sin cerrar sesión, el plazo de inactividad limpia el rastro. */
#define RECV_EARLY_CHECK_MS 800
#define RECV_IDLE_SECONDS   30

/* Una transferencia entrante ya aceptada. Tiene dos partes con vidas
 * distintas, y conviene no mezclarlas:
 *
 *   - el SEGUIMIENTO (la señal de cambios, los plazos, la lectura temprana):
 *     vive mientras la transferencia está en curso y se apaga en
 *     recv_finish ("finalizar recepción");
 *   - la VENTANA de progreso: vive hasta que el usuario la cierra, que puede
 *     ser bastante después de terminar, para leer el resultado.
 *
 * La estructura se libera cuando terminaron LAS DOS. Mientras el seguimiento
 * sigue, figura en agent->transfers; al terminar sale de ahí y rt->agent
 * queda en NULL, porque el agente puede destruirse (el usuario apagó la
 * recepción) con la ventana de resultado todavía abierta. */
typedef struct {
    BtObexAgent  *agent;          /* NULL una vez terminada */
    gchar        *transfer_path;
    gchar        *dest_path;
    gchar        *device_name;
    guint         watch_id;
    guint         check_id;
    guint         idle_id;
    GCancellable *cancellable;

    guint64       size;           /* 0 si el aparato no lo informó */
    guint64       transferred;
    gboolean      finished;

    GtkWidget    *window;         /* NULL una vez que la ventana se cerró */
    GtkWidget    *bar;
    GtkWidget    *status;
    GtkWidget    *button;
    GtkWidget    *show_btn;       /* «Mostrar en la carpeta», solo si salió bien */
} RecvTransfer;

struct _BtObexAgent {
    GDBusConnection *bus;          /* bus de SESIÓN, con referencia propia */
    GDBusConnection *system_conn;  /* bus del sistema, PRESTADA */
    GDBusNodeInfo   *node;
    guint            reg_id;       /* objeto exportado */
    guint            name_watch_id;
    guint            retry_id;

    /* TRUE mientras hay un RegisterAgent esperando respuesta. Sin esto
     * mandamos dos: uno desde bt_obex_agent_new y otro desde el vigía del
     * nombre, que dispara "apareció" apenas se instala si obexd ya está
     * (o apenas la primera llamada lo activa). El segundo vuelve con
     * AlreadyExists y nos hacía creer que el puesto lo tiene otro. */
    gboolean         reg_in_flight;

    BtObexRecvStatus status;
    gchar           *folder;
    gboolean         show_notifications;

    /* Pedido en pantalla. Solo uno a la vez: si llega un segundo mientras el
     * cartel está abierto, se rechaza. Dos aparatos empujando al mismo tiempo
     * es rarísimo, y encadenar carteles sería peor que rechazar el segundo. */
    GDBusMethodInvocation *pending;
    GtkWidget             *dialog;
    gchar                 *pending_transfer;
    gchar                 *pending_name;
    guint64                pending_size;
    GCancellable          *pending_cancellable;

    /* Red de seguridad: si por lo que sea nunca contestamos ni nos llega el
     * Cancel de obexd, "pending" quedaría ocupado para siempre y TODA
     * transferencia posterior se rechazaría al toque. */
    guint                  pending_timeout_id;

    GSList *transfers;             /* RecvTransfer* en curso */
};

static void try_register (BtObexAgent *agent);

/* ============================================================
 * Ayudas
 * ============================================================ */

/* La ventana emergente acapara el puntero mientras está abierta. Un cartel
 * que aparece por debajo de ese acaparamiento queda sordo al teclado y al
 * ratón, así que hay que soltarlo antes de mostrar nada. Mismo criterio que
 * en el agente de pareo, incluido el cierre de la ventana en Wayland. */
static void
release_seat_grab (void)
{
    popup_close_for_dialog ();

    GdkDisplay *display = gdk_display_get_default ();
    if (display == NULL)
        return;
    GdkSeat *seat = gdk_display_get_default_seat (display);
    if (seat != NULL)
        gdk_seat_ungrab (seat);
}

/* Extrae una propiedad de texto o ruta de objeto ('s' u 'o') desde un
 * diccionario a{sv} ya traído de forma asíncrona. Liberar con g_free().
 *
 * OJO con el tipo: en org.bluez.obex.Transfer1 la propiedad Session está
 * declarada como 'o' (ruta de objeto), no como 's'. */
static gchar *
dict_get_string (GVariant *props, const gchar *key)
{
    if (props == NULL)
        return NULL;

    GVariant *v = g_variant_lookup_value (props, key, NULL);
    if (v == NULL)
        return NULL;

    gchar *out = NULL;
    if (g_variant_is_of_type (v, G_VARIANT_TYPE_STRING) ||
        g_variant_is_of_type (v, G_VARIANT_TYPE_OBJECT_PATH))
        out = g_variant_dup_string (v, NULL);

    g_variant_unref (v);
    return out;
}

/* Nombre legible del aparato a partir de su dirección, buscándolo entre los
 * que conoce BlueZ. Si no aparece, devuelve la dirección misma. */
static gchar *
device_name_for_address (BtObexAgent *agent, const gchar *address)
{
    if (address == NULL)
        return g_strdup (_("Unknown device"));

    if (agent->system_conn == NULL)
        return g_strdup (address);

    /* Instantánea: una sola lectura del bus para todos los aparatos. Sin
     * ella, cada aparato conocido sumaba una consulta sincrónica más (la de
     * su batería), y con veinte aparatos fantasma eran veinte idas y vueltas
     * bloqueando el panel justo antes de mostrar el cartel. */
    gchar  *out  = NULL;
    bt_cache_begin (agent->system_conn);
    GSList *devs = bt_get_devices (agent->system_conn, NULL);
    bt_cache_end ();

    for (GSList *l = devs; l != NULL && out == NULL; l = l->next) {
        BtDevice *d = l->data;
        if (d->address != NULL && d->name != NULL && d->named &&
            g_ascii_strcasecmp (d->address, address) == 0)
            out = g_strdup (d->name);
    }
    bt_device_list_free (devs);

    return out != NULL ? out : g_strdup (address);
}

/* ============================================================
 * Seguimiento de la transferencia aceptada, con su ventana
 *
 * Mismas tres decisiones que la ventana de envío de popup.c, y por los
 * mismos motivos:
 *
 *   1. La ventana es INDEPENDIENTE de la emergente del panel: si viviera
 *      adentro, el primer clic afuera se llevaría puesta la transferencia.
 *   2. Un solo botón. Mientras recibe dice «Cancelar», cuando termina dice
 *      «Cerrar», y las dos veces hace lo mismo: destruir la ventana.
 *   3. Destruir la ventana con la transferencia en curso la cancela. El
 *      archivo a medias lo borra el propio demonio (lo hace con todo envío
 *      entrante abortado), así que acá no hay que limpiar nada en el disco.
 *
 * El resultado (recibido o fallado) se muestra DENTRO de la ventana. Eso
 * reemplaza el cartel de error suelto que había antes: no tiene sentido
 * avisar dos veces lo mismo en dos ventanas distintas.
 * ============================================================ */

/* Corta todo lo que sigue a la transferencia: la señal, los plazos y la
 * lectura temprana en vuelo. Toca rt->agent, así que solo se llama mientras
 * la transferencia está en curso (y por lo tanto el agente existe). */
static void
recv_stop_tracking (RecvTransfer *rt)
{
    if (rt->watch_id != 0) {
        g_dbus_connection_signal_unsubscribe (rt->agent->bus, rt->watch_id);
        rt->watch_id = 0;
    }
    if (rt->check_id != 0) {
        g_source_remove (rt->check_id);
        rt->check_id = 0;
    }
    if (rt->idle_id != 0) {
        g_source_remove (rt->idle_id);
        rt->idle_id = 0;
    }
    if (rt->cancellable != NULL) {
        g_cancellable_cancel (rt->cancellable);
        g_clear_object (&rt->cancellable);
    }
}

static void
recv_transfer_free (RecvTransfer *rt)
{
    if (rt == NULL)
        return;
    g_free (rt->transfer_path);
    g_free (rt->dest_path);
    g_free (rt->device_name);
    g_free (rt);
}

static void
recv_notify (BtObexAgent *agent, gboolean success,
             const gchar *dest_path, const gchar *device_name)
{
    if (!agent->show_notifications)
        return;

#ifdef HAVE_LIBNOTIFY
    gchar *base = dest_path != NULL ? g_path_get_basename (dest_path)
                                    : g_strdup (_("file"));
    gchar *body;

    if (success)
        body = g_strdup_printf (_("%s received from %s"), base,
                                device_name != NULL ? device_name
                                                    : _("a device"));
    else
        body = g_strdup_printf (_("Could not receive %s"), base);

    NotifyNotification *n = notify_notification_new (
        _("Bluetooth"), body,
        success ? "document-save-symbolic" : "dialog-error-symbolic");
    notify_notification_show (n, NULL);
    g_object_unref (n);

    g_free (body);
    g_free (base);
#else
    (void) success; (void) dest_path; (void) device_name;
#endif
}

/* Pone en la ventana lo que se sabe del avance. Sin tamaño informado no hay
 * porcentaje posible: la barra va y viene y se muestra lo recibido hasta
 * ahora, que igual le dice al usuario que algo está pasando. */
static void
recv_ui_update_progress (RecvTransfer *rt)
{
    if (rt->window == NULL)
        return;

    if (rt->size > 0) {
        gdouble fraction = (gdouble) rt->transferred / (gdouble) rt->size;
        gchar *done  = g_format_size (rt->transferred);
        gchar *total = g_format_size (rt->size);
        gchar *txt   = g_strdup_printf (_("%s of %s"), done, total);

        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (rt->bar),
                                       CLAMP (fraction, 0.0, 1.0));
        gtk_progress_bar_set_text (GTK_PROGRESS_BAR (rt->bar), txt);
        gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (rt->bar), TRUE);

        g_free (done);
        g_free (total);
        g_free (txt);
    } else {
        gtk_progress_bar_pulse (GTK_PROGRESS_BAR (rt->bar));
        if (rt->transferred > 0) {
            gchar *done = g_format_size (rt->transferred);
            gtk_progress_bar_set_text (GTK_PROGRESS_BAR (rt->bar), done);
            gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (rt->bar), TRUE);
            g_free (done);
        }
    }

    if (rt->transferred > 0)
        gtk_label_set_text (GTK_LABEL (rt->status), _("Receiving…"));
}

/* Fin de la transferencia, por cualquier motivo. Se ejecuta una sola vez.
 *
 * message es el texto a mostrar en la ventana. NULL significa "cancelada
 * por el usuario": en ese caso la ventana ya no está y tampoco se manda la
 * notificación de fallo, porque no fue un fallo sino una decisión.
 *
 * Si la ventana ya no está, rt se libera acá adentro: quien llama no debe
 * volver a tocarlo. */
static void
recv_finish (RecvTransfer *rt, gboolean success, const gchar *message)
{
    if (rt->finished)
        return;
    rt->finished = TRUE;

    BtObexAgent *agent = rt->agent;

    recv_stop_tracking (rt);
    agent->transfers = g_slist_remove (agent->transfers, rt);

    if (message != NULL)
        recv_notify (agent, success, rt->dest_path, rt->device_name);

    /* De acá en adelante la estructura puede sobrevivir al agente (si el
     * usuario apaga la recepción con la ventana de resultado abierta), así
     * que se corta el vínculo para que nadie lo use por error. */
    rt->agent = NULL;

    if (rt->window == NULL) {
        recv_transfer_free (rt);
        return;
    }

    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (rt->bar),
                                   success ? 1.0 : 0.0);
    gtk_progress_bar_set_text (GTK_PROGRESS_BAR (rt->bar), NULL);
    gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (rt->bar), FALSE);

    gtk_label_set_text (GTK_LABEL (rt->status),
                        message != NULL ? message : "");
    if (!success) {
        gtk_style_context_add_class (
            gtk_widget_get_style_context (rt->status), "error");
        /* Un fallo tiene que verse aunque la ventana haya quedado tapada. */
        gtk_window_present (GTK_WINDOW (rt->window));
    } else if (rt->show_btn != NULL) {
        /* Recién ahora hay algo que mostrar en la carpeta. */
        gtk_widget_show (rt->show_btn);
        gtk_widget_grab_focus (rt->show_btn);
    }

    gtk_button_set_label (GTK_BUTTON (rt->button), _("Close"));
}

/* Mensajes de fallo, juntos para que todos los caminos digan lo mismo. */
static const gchar *
recv_error_message (void)
{
    return _("The transfer was interrupted or the file transfer service "
             "could not write to the destination folder.");
}

static gchar *
recv_success_message (RecvTransfer *rt)
{
    gchar *dir = rt->dest_path != NULL ? g_path_get_dirname (rt->dest_path)
                                       : g_strdup ("");
    gchar *msg = g_strdup_printf (_("File received. Saved in:\n%s"), dir);
    g_free (dir);
    return msg;
}

static void
recv_finish_ok (RecvTransfer *rt)
{
    gchar *msg = recv_success_message (rt);
    recv_finish (rt, TRUE, msg);
    g_free (msg);
}

static gboolean
on_recv_idle_timeout (gpointer user_data)
{
    RecvTransfer *rt = user_data;

    rt->idle_id = 0;
    g_warning ("obex: transferencia entrante «%s» sin actividad durante %d s; "
               "se da por fallida",
               rt->transfer_path != NULL ? rt->transfer_path : "?",
               RECV_IDLE_SECONDS);

    gchar *msg = g_strdup_printf (
        _("Nothing arrived from the other device for %d seconds, so the "
          "transfer was given up."), RECV_IDLE_SECONDS);
    recv_finish (rt, FALSE, msg);
    g_free (msg);
    return G_SOURCE_REMOVE;
}

static void
recv_reset_idle_timer (RecvTransfer *rt)
{
    if (rt->idle_id != 0)
        g_source_remove (rt->idle_id);
    rt->idle_id = g_timeout_add_seconds (RECV_IDLE_SECONDS,
                                         on_recv_idle_timeout, rt);
}

/* Aplica un diccionario de propiedades de la transferencia. Devuelve TRUE
 * si la transferencia terminó; en ese caso rt puede haber sido liberado y
 * no hay que volver a tocarlo. */
static gboolean
recv_apply_props (RecvTransfer *rt, GVariant *props)
{
    GVariant *v;

    v = g_variant_lookup_value (props, "Size", G_VARIANT_TYPE_UINT64);
    if (v != NULL) {
        guint64 size = g_variant_get_uint64 (v);
        if (size > 0)
            rt->size = size;
        g_variant_unref (v);
    }

    v = g_variant_lookup_value (props, "Transferred", G_VARIANT_TYPE_UINT64);
    if (v != NULL) {
        rt->transferred = g_variant_get_uint64 (v);
        g_variant_unref (v);
    }

    v = g_variant_lookup_value (props, "Status", G_VARIANT_TYPE_STRING);
    if (v != NULL) {
        gchar *status = g_variant_dup_string (v, NULL);
        g_variant_unref (v);

        if (g_strcmp0 (status, "complete") == 0) {
            g_free (status);
            recv_finish_ok (rt);
            return TRUE;
        }
        if (g_strcmp0 (status, "error") == 0) {
            g_free (status);
            recv_finish (rt, FALSE, recv_error_message ());
            return TRUE;
        }
        g_free (status);
    }

    recv_ui_update_progress (rt);

    /* Mientras llegan actualizaciones (por ejemplo Transferred), el plazo de
     * inactividad se renueva para no cortar archivos grandes. */
    recv_reset_idle_timer (rt);
    return FALSE;
}

static void
on_recv_props (GDBusConnection *conn, const gchar *sender, const gchar *path,
               const gchar *iface, const gchar *signal_name,
               GVariant *params, gpointer user_data)
{
    (void) conn; (void) sender; (void) path; (void) iface; (void) signal_name;
    RecvTransfer *rt = user_data;

    const gchar *changed_iface = NULL;
    GVariant    *props         = NULL;
    g_variant_get (params, "(&s@a{sv}@as)", &changed_iface, &props, NULL);

    if (g_strcmp0 (changed_iface, OBEX_TRANSFER_IFACE) == 0)
        recv_apply_props (rt, props);

    g_variant_unref (props);
}

static void
on_recv_early_snapshot (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GDBusConnection *bus = G_DBUS_CONNECTION (source);
    RecvTransfer    *rt  = user_data;
    GError          *err = NULL;

    GVariant *ret = g_dbus_connection_call_finish (bus, res, &err);

    if (ret == NULL) {
        /* Cancelada: el seguimiento ya terminó y rt puede no existir más. */
        if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            /* El objeto desapareció sin avisar. Si el archivo quedó en el
             * disco con el tamaño anunciado, salió bien y fue muy rápido;
             * si no está (o está incompleto), el demonio no pudo escribir. */
            GStatBuf st;
            gboolean exists = (rt->dest_path != NULL &&
                               g_stat (rt->dest_path, &st) == 0);
            gboolean whole  = exists &&
                              (rt->size == 0 || (guint64) st.st_size >= rt->size);

            g_warning ("obex: el objeto de transferencia «%s» desapareció sin "
                       "avisar (archivo en disco: %s)",
                       rt->transfer_path != NULL ? rt->transfer_path : "?",
                       whole ? "completo" : exists ? "incompleto" : "no");

            if (whole)
                recv_finish_ok (rt);
            else
                recv_finish (rt, FALSE, recv_error_message ());
        }
        g_clear_error (&err);
        return;
    }

    GVariant *props = NULL;
    g_variant_get (ret, "(@a{sv})", &props);
    recv_apply_props (rt, props);
    g_variant_unref (props);
    g_variant_unref (ret);
}

static gboolean
on_recv_early_check (gpointer user_data)
{
    RecvTransfer *rt = user_data;

    rt->check_id = 0;
    g_dbus_connection_call (rt->agent->bus, BT_OBEX_BUS_NAME,
                            rt->transfer_path,
                            "org.freedesktop.DBus.Properties", "GetAll",
                            g_variant_new ("(s)", OBEX_TRANSFER_IFACE),
                            G_VARIANT_TYPE ("(a{sv})"),
                            G_DBUS_CALL_FLAGS_NONE, 5000, rt->cancellable,
                            on_recv_early_snapshot, rt);
    return G_SOURCE_REMOVE;
}

/* ---------- mostrar el archivo recibido en su carpeta ----------
 *
 * Dos caminos, en este orden:
 *
 *   1. Pedirle al gestor de archivos que abra la carpeta CON EL ARCHIVO YA
 *      SELECCIONADO. Es la interfaz estándar de escritorio para eso
 *      (org.freedesktop.FileManager1, método ShowItems, "mostrar
 *      elementos"); Thunar la implementa, igual que Nautilus, Nemo, Caja y
 *      Dolphin. El bus arranca el gestor solo si no estaba corriendo.
 *
 *   2. Si no hay ningún gestor que la entienda, abrir la carpeta con el
 *      programa predeterminado del usuario para carpetas.
 *
 * A propósito NO se ofrece «Abrir archivo»: el archivo lo eligió el aparato
 * que lo mandó, no el usuario, y abrirlo con el programa predeterminado de
 * su tipo puede ser peligroso con ciertos tipos (un lanzador .desktop, un
 * guion). Mostrarlo en la carpeta deja que el usuario vea qué es antes de
 * hacer doble clic.
 *
 * Nada de esto bloquea: la llamada al bus es asíncrona y el lanzamiento del
 * programa predeterminado vuelve enseguida. Tampoco depende de la ventana ni
 * del agente: lleva sus propias copias de las rutas. */

typedef struct {
    gchar *file_uri;
    gchar *folder_uri;
} RevealRequest;

static void
reveal_request_free (RevealRequest *req)
{
    g_free (req->file_uri);
    g_free (req->folder_uri);
    g_free (req);
}

static void
reveal_open_folder (RevealRequest *req)
{
    GError              *err = NULL;
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context (
                                   gdk_display_get_default ());

    if (!g_app_info_launch_default_for_uri (req->folder_uri,
                                            G_APP_LAUNCH_CONTEXT (ctx), &err)) {
        g_warning ("obex: no se pudo abrir la carpeta «%s»: %s",
                   req->folder_uri, err != NULL ? err->message : "?");
        g_clear_error (&err);
    }
    g_object_unref (ctx);
}

static void
on_show_items_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    RevealRequest *req = user_data;
    GError        *err = NULL;
    GVariant      *ret = g_dbus_connection_call_finish (
                             G_DBUS_CONNECTION (source), res, &err);

    if (ret != NULL) {
        g_variant_unref (ret);
    } else {
        /* Lo normal acá es que no haya gestor con esa interfaz: no es un
         * error del usuario, solo se cambia de camino. */
        g_message ("obex: el gestor de archivos no atendió ShowItems (%s); "
                   "abro la carpeta directamente",
                   err != NULL ? err->message : "?");
        g_clear_error (&err);
        reveal_open_folder (req);
    }

    reveal_request_free (req);
}

static void
recv_reveal_in_folder (const gchar *dest_path)
{
    if (dest_path == NULL)
        return;

    gchar         *dir = g_path_get_dirname (dest_path);
    RevealRequest *req = g_new0 (RevealRequest, 1);
    req->file_uri   = g_filename_to_uri (dest_path, NULL, NULL);
    req->folder_uri = g_filename_to_uri (dir, NULL, NULL);
    g_free (dir);

    if (req->folder_uri == NULL) {
        reveal_request_free (req);
        return;
    }

    GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    if (bus == NULL || req->file_uri == NULL) {
        if (bus != NULL)
            g_object_unref (bus);
        reveal_open_folder (req);
        reveal_request_free (req);
        return;
    }

    const gchar *uris[] = { req->file_uri, NULL };
    g_dbus_connection_call (bus, "org.freedesktop.FileManager1",
                            "/org/freedesktop/FileManager1",
                            "org.freedesktop.FileManager1", "ShowItems",
                            g_variant_new ("(^ass)", uris, ""),
                            NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL,
                            on_show_items_done, req);
    g_object_unref (bus);
}

/* El botón muestra el archivo y cierra la ventana: lo que el usuario quería
 * ver ya está en pantalla en el gestor de archivos. Se copia la ruta antes
 * de cerrar, porque cerrar libera rt. */
static void
on_show_in_folder_clicked (GtkWidget *btn, gpointer user_data)
{
    (void) btn;
    RecvTransfer *rt = user_data;

    recv_reveal_in_folder (rt->dest_path);
    if (rt->window != NULL)
        gtk_widget_destroy (rt->window);
}

/* ---------- la ventana ---------- */

/* La ventana se cerró, por el botón o por la cruz. Si la transferencia
 * sigue, se cancela; si ya había terminado, solo queda liberar. Los punteros
 * a la ventana se ponen en NULL ANTES de cancelar, para que recv_finish no
 * intente dibujar en una ventana que se está destruyendo. */
static void
on_recv_window_destroy (GtkWidget *win, gpointer user_data)
{
    (void) win;
    RecvTransfer *rt = user_data;

    rt->window = NULL;
    rt->bar    = NULL;
    rt->status = NULL;
    rt->button = NULL;
    rt->show_btn = NULL;

    if (rt->finished) {
        recv_transfer_free (rt);
        return;
    }

    /* Tiene que salir por la MISMA conexión con la que se registró el
     * agente: el demonio solo acepta cancelaciones de quien autorizó. */
    g_message ("obex: recepción de «%s» cancelada por el usuario",
               rt->transfer_path != NULL ? rt->transfer_path : "?");
    g_dbus_connection_call (rt->agent->bus, BT_OBEX_BUS_NAME,
                            rt->transfer_path, OBEX_TRANSFER_IFACE, "Cancel",
                            NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 5000,
                            NULL, NULL, NULL);

    recv_finish (rt, FALSE, NULL);   /* libera rt: la ventana ya no está */
}

static void
recv_window_new (RecvTransfer *rt)
{
    rt->window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title (GTK_WINDOW (rt->window), _("Receiving file"));
    gtk_window_set_icon_name (GTK_WINDOW (rt->window), "bluetooth");
    gtk_window_set_resizable (GTK_WINDOW (rt->window), FALSE);
    gtk_window_set_position (GTK_WINDOW (rt->window), GTK_WIN_POS_CENTER);
    gtk_window_set_default_size (GTK_WINDOW (rt->window), 380, -1);
    gtk_container_set_border_width (GTK_CONTAINER (rt->window), 12);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add (GTK_CONTAINER (rt->window), box);

    gchar     *base  = rt->dest_path != NULL
                       ? g_path_get_basename (rt->dest_path)
                       : g_strdup (_("file"));
    GtkWidget *fname = gtk_label_new (base);
    g_free (base);
    gtk_label_set_xalign (GTK_LABEL (fname), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (fname), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars (GTK_LABEL (fname), 40);
    gtk_box_pack_start (GTK_BOX (box), fname, FALSE, FALSE, 0);

    gchar *from = g_strdup_printf (_("From: %s"),
                                   rt->device_name != NULL ? rt->device_name
                                                           : _("a device"));
    GtkWidget *src = gtk_label_new (from);
    g_free (from);
    gtk_label_set_xalign (GTK_LABEL (src), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (src), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (src), 40);
    gtk_style_context_add_class (gtk_widget_get_style_context (src),
                                 "dim-label");
    gtk_box_pack_start (GTK_BOX (box), src, FALSE, FALSE, 0);

    rt->bar = gtk_progress_bar_new ();
    gtk_box_pack_start (GTK_BOX (box), rt->bar, FALSE, FALSE, 0);

    rt->status = gtk_label_new (_("Waiting for data…"));
    gtk_label_set_xalign (GTK_LABEL (rt->status), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (rt->status), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (rt->status), 40);
    gtk_label_set_selectable (GTK_LABEL (rt->status), TRUE);
    gtk_box_pack_start (GTK_BOX (box), rt->status, FALSE, FALSE, 0);

    GtkWidget *btn_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start (GTK_BOX (box), btn_row, FALSE, FALSE, 0);

    /* Arranca oculto (y con no_show_all, para que el show_all de más abajo
     * no lo muestre): aparece recién cuando el archivo llegó entero. */
    rt->show_btn = gtk_button_new_with_label (_("Show in folder"));
    gtk_style_context_add_class (gtk_widget_get_style_context (rt->show_btn),
                                 "suggested-action");
    gtk_widget_set_no_show_all (rt->show_btn, TRUE);
    gtk_box_pack_start (GTK_BOX (btn_row), rt->show_btn, FALSE, FALSE, 0);
    g_signal_connect (rt->show_btn, "clicked",
                      G_CALLBACK (on_show_in_folder_clicked), rt);

    rt->button = gtk_button_new_with_label (_("Cancel"));
    gtk_box_pack_end (GTK_BOX (btn_row), rt->button, FALSE, FALSE, 0);
    g_signal_connect_swapped (rt->button, "clicked",
                              G_CALLBACK (gtk_widget_destroy), rt->window);

    g_signal_connect (rt->window, "destroy",
                      G_CALLBACK (on_recv_window_destroy), rt);

    gtk_widget_show_all (rt->window);

    /* Si el tamaño ya se conocía desde la autorización, arrancar mostrando
     * "0 de N" en vez de una barra vacía sin explicación. */
    recv_ui_update_progress (rt);
}

/* Abandono por cierre del agente (el usuario apagó la recepción o se quita
 * el complemento): se deja de seguir la transferencia y se cierra su
 * ventana SIN cancelar nada. Se desconecta antes el manejador de destrucción
 * para que cerrar la ventana no se interprete como «el usuario canceló». */
static void
recv_transfer_abandon (RecvTransfer *rt)
{
    recv_stop_tracking (rt);
    if (rt->window != NULL) {
        GtkWidget *w = rt->window;
        g_signal_handlers_disconnect_by_data (w, rt);
        rt->window = NULL;
        gtk_widget_destroy (w);
    }
    recv_transfer_free (rt);
}

/* ============================================================
 * El cartel de aceptar o rechazar
 * ============================================================ */

static void
clear_pending (BtObexAgent *agent)
{
    agent->pending = NULL;
    g_clear_pointer (&agent->pending_transfer, g_free);
    g_clear_pointer (&agent->pending_name, g_free);
    agent->pending_size = 0;
    if (agent->pending_cancellable != NULL) {
        g_cancellable_cancel (agent->pending_cancellable);
        g_clear_object (&agent->pending_cancellable);
    }
    if (agent->pending_timeout_id != 0) {
        g_source_remove (agent->pending_timeout_id);
        agent->pending_timeout_id = 0;
    }
    if (agent->dialog != NULL) {
        GtkWidget *d = agent->dialog;
        agent->dialog = NULL;
        gtk_widget_destroy (d);
    }
}

static void
reject_pending (BtObexAgent *agent, const gchar *reason)
{
    if (agent->pending == NULL)
        return;

    GDBusMethodInvocation *inv = agent->pending;
    agent->pending = NULL;

    g_dbus_method_invocation_return_dbus_error (
        inv, "org.bluez.obex.Error.Rejected",
        reason != NULL ? reason : "Rejected");

    clear_pending (agent);
}

/* obexd corta por su cuenta al minuto y nos manda Cancel; esto es solo por
 * si ese Cancel no llega. */
#define PENDING_GUARD_SECONDS 90

static gboolean
on_pending_timeout (gpointer user_data)
{
    BtObexAgent *agent = user_data;

    agent->pending_timeout_id = 0;
    if (agent->pending != NULL) {
        g_warning ("obex: nadie contestó la autorización en %d s; "
                   "la libero para no bloquear las próximas",
                   PENDING_GUARD_SECONDS);
        reject_pending (agent, "Timed out waiting for the user");
    }
    return G_SOURCE_REMOVE;
}

/* Los datos del cartel viven pegados al cartel mismo (g_object_set_data_full,
 * "guardar dato con liberación automática") y se liberan solos cuando el
 * cartel se destruye, venga de donde venga esa destrucción.
 *
 * Antes se liberaban a mano dentro de on_authorize_response, y eso solo
 * cubría el caso en que el usuario contesta. Cuando el cartel lo cerraba
 * otro (el plazo de seguridad, la cancelación del aparato que manda, o el
 * cierre del complemento, todos pasan por clear_pending), la respuesta nunca
 * llegaba y estos datos quedaban perdidos en memoria. */
typedef struct {
    BtObexAgent *agent;
    gchar       *device_name;
} DialogData;

static void
dialog_data_free (DialogData *dd)
{
    if (dd == NULL)
        return;
    g_free (dd->device_name);
    g_free (dd);
}

static void
on_authorize_response (GtkWidget *dialog, gint response, gpointer user_data)
{
    DialogData  *dd    = user_data;
    BtObexAgent *agent = dd->agent;

    /* El cartel se cierra siempre; lo que cambia es qué le contestamos.
     * OJO: dd se libera junto con el cartel, así que no se usa después del
     * gtk_widget_destroy de más abajo. */
    agent->dialog = NULL;

    GDBusMethodInvocation *inv = agent->pending;
    agent->pending = NULL;

    if (inv == NULL) {
        /* Ya se había resuelto por otro lado (Cancel del otro extremo). */
        gtk_widget_destroy (dialog);
        return;
    }

    if (response == GTK_RESPONSE_ACCEPT) {
        gchar *dest = bt_obex_unique_path (agent->folder, agent->pending_name);

        /* obexd parte esta ruta en carpeta y nombre, así que mandarla
         * completa es lo que fija dónde se guarda el archivo. */
        g_message ("obex: AuthorizePush aceptado, contesto «%s»", dest);
        g_dbus_method_invocation_return_value (inv, g_variant_new ("(s)", dest));

        /* Seguirle el rastro, con su ventana de progreso. */
        RecvTransfer *rt = g_new0 (RecvTransfer, 1);
        rt->agent         = agent;
        rt->transfer_path = g_strdup (
            g_object_get_data (G_OBJECT (dialog), "transfer-path"));
        rt->dest_path     = dest;   /* se lo queda rt */
        rt->device_name   = g_strdup (dd->device_name);
        rt->size          = agent->pending_size;
        rt->cancellable   = g_cancellable_new ();
        rt->watch_id = g_dbus_connection_signal_subscribe (
            agent->bus, BT_OBEX_BUS_NAME,
            "org.freedesktop.DBus.Properties", "PropertiesChanged",
            rt->transfer_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
            on_recv_props, rt, NULL);
        rt->check_id = g_timeout_add (RECV_EARLY_CHECK_MS,
                                      on_recv_early_check, rt);
        recv_reset_idle_timer (rt);

        agent->transfers = g_slist_prepend (agent->transfers, rt);

        recv_window_new (rt);
    } else {
        g_message ("obex: AuthorizePush rechazado por el usuario");
        g_dbus_method_invocation_return_dbus_error (
            inv, "org.bluez.obex.Error.Rejected", "Rejected by user");
    }

    clear_pending (agent);
    gtk_widget_destroy (dialog);
}

static void
show_authorize_dialog (BtObexAgent *agent, const gchar *transfer_path,
                       const gchar *device_name, const gchar *filename,
                       guint64 size)
{
    release_seat_grab ();

    gchar *title = g_strdup_printf (_("%s wants to send you a file"),
                                    device_name);

    GtkWidget *dlg = gtk_message_dialog_new (
        NULL, 0, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", title);
    g_free (title);

    gchar *detail;
    if (size > 0) {
        gchar *human = g_format_size (size);
        detail = g_strdup_printf (_("%s  (%s)\n\nIt will be saved in:\n%s"),
                                  filename, human, agent->folder);
        g_free (human);
    } else {
        detail = g_strdup_printf (_("%s\n\nIt will be saved in:\n%s"),
                                  filename, agent->folder);
    }
    gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dlg),
                                              "%s", detail);
    g_free (detail);

    gtk_dialog_add_button (GTK_DIALOG (dlg), _("Reject"), GTK_RESPONSE_REJECT);
    GtkWidget *ok = gtk_dialog_add_button (GTK_DIALOG (dlg), _("Accept"),
                                           GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class (gtk_widget_get_style_context (ok),
                                 "suggested-action");
    gtk_window_set_position (GTK_WINDOW (dlg), GTK_WIN_POS_CENTER);
    gtk_window_set_keep_above (GTK_WINDOW (dlg), TRUE);

    g_object_set_data_full (G_OBJECT (dlg), "transfer-path",
                            g_strdup (transfer_path), g_free);

    DialogData *dd = g_new0 (DialogData, 1);
    dd->agent       = agent;
    dd->device_name = g_strdup (device_name);
    g_object_set_data_full (G_OBJECT (dlg), "dialog-data", dd,
                            (GDestroyNotify) dialog_data_free);

    g_signal_connect (dlg, "response",
                      G_CALLBACK (on_authorize_response), dd);

    agent->dialog = dlg;
    gtk_widget_show_all (dlg);
}

/* ============================================================
 * Métodos de org.bluez.obex.Agent1
 * ============================================================ */

static const gchar agent_introspection_xml[] =
    "<node>"
    "  <interface name='org.bluez.obex.Agent1'>"
    "    <method name='Release'/>"
    "    <method name='AuthorizePush'>"
    "      <arg type='o' name='transfer' direction='in'/>"
    "      <arg type='s' name='filename' direction='out'/>"
    "    </method>"
    "    <method name='Cancel'/>"
    "  </interface>"
    "</node>";

static void
on_auth_session_dest (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GDBusConnection *bus   = G_DBUS_CONNECTION (source);
    BtObexAgent     *agent = user_data;
    GError          *err   = NULL;

    GVariant *ret = g_dbus_connection_call_finish (bus, res, &err);

    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
        agent->pending == NULL) {
        if (ret != NULL)
            g_variant_unref (ret);
        g_clear_error (&err);
        return;
    }
    g_clear_error (&err);

    gchar *address = NULL;
    if (ret != NULL) {
        GVariant *inner = NULL;
        g_variant_get (ret, "(v)", &inner);
        if (inner != NULL) {
            if (g_variant_is_of_type (inner, G_VARIANT_TYPE_STRING))
                address = g_variant_dup_string (inner, NULL);
            g_variant_unref (inner);
        }
        g_variant_unref (ret);
    }

    gchar *device_name = device_name_for_address (agent, address);

    g_message ("obex: nombre=%s tamaño=%" G_GUINT64_FORMAT " dirección=%s",
               agent->pending_name != NULL ? agent->pending_name : "(no informado)",
               agent->pending_size,
               address != NULL ? address : "(no informada)");

    show_authorize_dialog (agent, agent->pending_transfer, device_name,
                           agent->pending_name, agent->pending_size);

    g_free (device_name);
    g_free (address);
}

static void
on_auth_transfer_props (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GDBusConnection *bus   = G_DBUS_CONNECTION (source);
    BtObexAgent     *agent = user_data;
    GError          *err   = NULL;

    GVariant *ret = g_dbus_connection_call_finish (bus, res, &err);

    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
        agent->pending == NULL) {
        if (ret != NULL)
            g_variant_unref (ret);
        g_clear_error (&err);
        return;
    }
    g_clear_error (&err);

    gchar   *name    = NULL;
    gchar   *session = NULL;
    guint64  size    = 0;

    if (ret != NULL) {
        GVariant *props = NULL;
        g_variant_get (ret, "(@a{sv})", &props);

        name    = dict_get_string (props, "Name");
        session = dict_get_string (props, "Session");

        GVariant *sv = g_variant_lookup_value (props, "Size",
                                               G_VARIANT_TYPE_UINT64);
        if (sv != NULL) {
            size = g_variant_get_uint64 (sv);
            g_variant_unref (sv);
        }

        g_variant_unref (props);
        g_variant_unref (ret);
    }

    g_free (agent->pending_name);
    agent->pending_name = g_strdup (name != NULL && *name ? name : "file");
    agent->pending_size = size;
    g_free (name);

    if (session != NULL) {
        g_dbus_connection_call (
            agent->bus, BT_OBEX_BUS_NAME, session,
            "org.freedesktop.DBus.Properties", "Get",
            g_variant_new ("(ss)", OBEX_SESSION_IFACE, "Destination"),
            G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE, 3000,
            agent->pending_cancellable, on_auth_session_dest, agent);
        g_free (session);
        return;
    }

    gchar *device_name = device_name_for_address (agent, NULL);
    show_authorize_dialog (agent, agent->pending_transfer, device_name,
                           agent->pending_name, agent->pending_size);
    g_free (device_name);
}

static void
handle_authorize_push (BtObexAgent *agent, GDBusMethodInvocation *inv,
                       GVariant *params)
{
    const gchar *transfer_path = NULL;
    g_variant_get (params, "(&o)", &transfer_path);

    g_message ("obex: AuthorizePush para %s", transfer_path);

    /* Ya hay un pedido en curso (leyendo propiedades o con el cartel en
     * pantalla): rechazar el segundo. */
    if (agent->pending != NULL) {
        g_warning ("obex: llegó una segunda autorización («%s») con otra "
                   "todavía sin contestar; la rechazo", transfer_path);
        g_dbus_method_invocation_return_dbus_error (
            inv, "org.bluez.obex.Error.Rejected",
            "Another transfer is waiting for confirmation");
        return;
    }

    agent->pending             = inv;
    agent->pending_transfer    = g_strdup (transfer_path);
    agent->pending_name        = g_strdup ("file");
    agent->pending_size        = 0;
    agent->pending_cancellable = g_cancellable_new ();
    agent->pending_timeout_id  = g_timeout_add_seconds (PENDING_GUARD_SECONDS,
                                                        on_pending_timeout,
                                                        agent);

    /* Lectura asíncrona de todas las propiedades de la transferencia en una
     * sola llamada para no congelar el panel si obexd tarda en contestar. */
    g_dbus_connection_call (
        agent->bus, BT_OBEX_BUS_NAME, transfer_path,
        "org.freedesktop.DBus.Properties", "GetAll",
        g_variant_new ("(s)", OBEX_TRANSFER_IFACE),
        G_VARIANT_TYPE ("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 3000,
        agent->pending_cancellable, on_auth_transfer_props, agent);
}

static void
agent_method_call (GDBusConnection *conn, const gchar *sender,
                   const gchar *object_path, const gchar *iface,
                   const gchar *method, GVariant *params,
                   GDBusMethodInvocation *inv, gpointer user_data)
{
    (void) conn; (void) sender; (void) object_path; (void) iface;
    BtObexAgent *agent = user_data;

    if (g_strcmp0 (method, "AuthorizePush") == 0) {
        handle_authorize_push (agent, inv, params);
        return;
    }

    if (g_strcmp0 (method, "Cancel") == 0) {
        /* El otro extremo abandonó mientras el cartel estaba abierto. */
        reject_pending (agent, "Cancelled by the remote device");
        g_dbus_method_invocation_return_value (inv, NULL);
        return;
    }

    if (g_strcmp0 (method, "Release") == 0) {
        /* Nos dieron de baja. No reintentamos acá: si obexd se reinició, el
         * vigía del nombre se va a encargar. */
        agent->status = BT_OBEX_RECV_UNAVAILABLE;
        g_dbus_method_invocation_return_value (inv, NULL);
        return;
    }

    g_dbus_method_invocation_return_dbus_error (
        inv, "org.bluez.obex.Error.NotSupported", "Unknown method");
}

static const GDBusInterfaceVTable agent_vtable = {
    agent_method_call, NULL, NULL, { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

/* ============================================================
 * Registro, con reintento
 * ============================================================ */

static gboolean
on_retry_register (gpointer user_data)
{
    BtObexAgent *agent = user_data;

    if (agent->status == BT_OBEX_RECV_REGISTERED) {
        agent->retry_id = 0;
        return G_SOURCE_REMOVE;
    }

    try_register (agent);
    return G_SOURCE_CONTINUE;
}

static void
schedule_retry (BtObexAgent *agent)
{
    if (agent->retry_id != 0)
        return;
    agent->retry_id = g_timeout_add_seconds (RECV_RETRY_SECONDS,
                                             on_retry_register, agent);
}

static void
on_register_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) source;
    BtObexAgent *agent = user_data;
    GError      *err   = NULL;

    agent->reg_in_flight = FALSE;

    GVariant *ret = g_dbus_connection_call_finish (agent->bus, res, &err);

    if (ret != NULL) {
        g_variant_unref (ret);
        agent->status = BT_OBEX_RECV_REGISTERED;
        if (agent->retry_id != 0) {
            g_source_remove (agent->retry_id);
            agent->retry_id = 0;
        }
        return;
    }

    /* AlreadyExists es EL caso interesante: otro programa tiene el puesto.
     * PERO ojo: si ya estamos registrados, el que tiene el puesto somos
     * nosotros y esto es un registro repetido rebotando. Degradar el estado
     * ahí era la mentira que ponía "otro programa maneja los archivos
     * entrantes" con el plugin funcionando perfectamente. */
    gchar *remote = g_dbus_error_get_remote_error (err);
    if (remote != NULL &&
        g_str_has_suffix (remote, ".Error.AlreadyExists")) {
        if (agent->status != BT_OBEX_RECV_REGISTERED)
            agent->status = BT_OBEX_RECV_TAKEN;
    } else {
        agent->status = BT_OBEX_RECV_UNAVAILABLE;
    }

    g_free (remote);
    g_clear_error (&err);

    if (agent->status != BT_OBEX_RECV_REGISTERED)
        schedule_retry (agent);
}

static void
try_register (BtObexAgent *agent)
{
    if (agent->reg_in_flight)
        return;

    agent->reg_in_flight = TRUE;
    g_dbus_connection_call (agent->bus, BT_OBEX_BUS_NAME, "/org/bluez/obex",
                            OBEX_AGENT_MGR_IFACE, "RegisterAgent",
                            g_variant_new ("(o)", OBEX_AGENT_PATH),
                            NULL, G_DBUS_CALL_FLAGS_NONE, 20000, NULL,
                            on_register_done, agent);
}

/* obexd apareció en el bus. Si se reinició, se olvidó de todos los agentes,
 * así que hay que volver a presentarse. Es idempotente. */
static void
on_obex_appeared (GDBusConnection *conn, const gchar *name,
                  const gchar *owner, gpointer user_data)
{
    (void) conn; (void) name; (void) owner;
    BtObexAgent *agent = user_data;

    /* Si ya estábamos registrados esto es el vigía contándonos algo que ya
     * sabíamos (se instala con obexd en el bus). Cuando obexd se reinicia de
     * verdad pasa antes por on_obex_vanished, que sí baja el estado. */
    if (agent->status != BT_OBEX_RECV_REGISTERED)
        agent->status = BT_OBEX_RECV_UNAVAILABLE;

    try_register (agent);
}

static void
on_obex_vanished (GDBusConnection *conn, const gchar *name, gpointer user_data)
{
    (void) conn; (void) name;
    BtObexAgent *agent = user_data;
    agent->status = BT_OBEX_RECV_UNAVAILABLE;
    schedule_retry (agent);
}

/* ============================================================
 * Interfaz pública
 * ============================================================ */

BtObexAgent *
bt_obex_agent_new (GDBusConnection *system_conn, const gchar *folder,
                   gboolean show_notifications)
{
    GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    if (bus == NULL)
        return NULL;

    GError        *err  = NULL;
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml (
                              agent_introspection_xml, &err);
    if (node == NULL) {
        g_clear_error (&err);
        g_object_unref (bus);
        return NULL;
    }

    BtObexAgent *agent = g_new0 (BtObexAgent, 1);
    agent->bus                = bus;
    agent->system_conn        = system_conn;
    agent->node               = node;
    agent->status             = BT_OBEX_RECV_UNAVAILABLE;
    agent->show_notifications = show_notifications;
    agent->folder = g_strdup (folder != NULL && *folder
                                  ? folder
                                  : g_get_home_dir ());

    agent->reg_id = g_dbus_connection_register_object (
        bus, OBEX_AGENT_PATH, node->interfaces[0],
        &agent_vtable, agent, NULL, &err);

    if (agent->reg_id == 0) {
        g_clear_error (&err);
        g_dbus_node_info_unref (node);
        g_object_unref (bus);
        g_free (agent->folder);
        g_free (agent);
        return NULL;
    }

    /* El vigía NO enciende el servicio (esa bandera queda apagada a
     * propósito): solo avisa si aparece o desaparece. Quien lo enciende es
     * el intento de registro, que es cuando de verdad hace falta. */
    agent->name_watch_id = g_bus_watch_name_on_connection (
        bus, BT_OBEX_BUS_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
        on_obex_appeared, on_obex_vanished, agent, NULL);

    try_register (agent);
    return agent;
}

void
bt_obex_agent_free (BtObexAgent *agent)
{
    if (agent == NULL)
        return;

    reject_pending (agent, "The plugin is shutting down");

    if (agent->retry_id != 0)
        g_source_remove (agent->retry_id);
    if (agent->name_watch_id != 0)
        g_bus_unwatch_name (agent->name_watch_id);

    if (agent->status == BT_OBEX_RECV_REGISTERED) {
        g_dbus_connection_call (agent->bus, BT_OBEX_BUS_NAME, "/org/bluez/obex",
                                OBEX_AGENT_MGR_IFACE, "UnregisterAgent",
                                g_variant_new ("(o)", OBEX_AGENT_PATH),
                                NULL, G_DBUS_CALL_FLAGS_NONE, 5000,
                                NULL, NULL, NULL);
    }

    /* Solo quedan acá las que siguen en curso; las terminadas con su
     * ventana de resultado abierta ya no dependen del agente y se liberan
     * solas cuando el usuario cierra esa ventana. */
    for (GSList *l = agent->transfers; l != NULL; l = l->next)
        recv_transfer_abandon (l->data);
    g_slist_free (agent->transfers);
    agent->transfers = NULL;

    if (agent->reg_id != 0)
        g_dbus_connection_unregister_object (agent->bus, agent->reg_id);

    g_dbus_node_info_unref (agent->node);
    g_object_unref (agent->bus);
    g_free (agent->folder);
    g_free (agent->pending_name);
    g_free (agent);
}

void
bt_obex_agent_set_folder (BtObexAgent *agent, const gchar *folder)
{
    if (agent == NULL || folder == NULL || !*folder)
        return;
    g_free (agent->folder);
    agent->folder = g_strdup (folder);
}

void
bt_obex_agent_set_notifications (BtObexAgent *agent, gboolean show)
{
    if (agent == NULL)
        return;
    agent->show_notifications = show;
}

BtObexRecvStatus
bt_obex_agent_status (BtObexAgent *agent)
{
    if (agent == NULL)
        return BT_OBEX_RECV_UNAVAILABLE;
    return agent->status;
}

const gchar *
bt_obex_recv_status_message (BtObexAgent *agent)
{
    if (agent == NULL)
        return _("Receiving files is off.");

    switch (agent->status) {
    case BT_OBEX_RECV_REGISTERED:
        return _("Ready to receive files.");
    case BT_OBEX_RECV_TAKEN:
        return _("Another program is already handling incoming files "
                 "(Blueman, for example). Close it and this will take over "
                 "on its own within a minute.");
    case BT_OBEX_RECV_UNAVAILABLE:
    default:
        return _("Waiting for the file transfer service…");
    }
}
