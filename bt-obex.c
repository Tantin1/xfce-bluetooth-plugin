/* bt-obex.c — detección del servicio de transferencia de archivos.
 *
 * Estrategia de detección, en este orden y por este motivo:
 *
 *   1. ¿Alguien tiene el nombre org.bluez.obex tomado en el bus de sesión?
 *      Si sí, el demonio ya está corriendo. Listo.
 *
 *   2. ¿El nombre figura entre los activables del bus? Si sí, el demonio
 *      está instalado y el bus lo va a arrancar solo la primera vez que le
 *      hablemos. También listo, y sin que el usuario habilite nada.
 *
 *   3. Si no figura, buscamos el ejecutable en el disco. Si está, el
 *      problema es que falta habilitar el servicio de usuario.
 *
 *   4. Si tampoco está, falta instalar el paquete.
 *
 * Los pasos 1 y 2 le hablan al demonio del bus de sesión, no a org.bluez.obex,
 * así que NO activan nada. Es importante: preguntar no debe encender un
 * servicio que el usuario tal vez nunca quiera usar.
 */

/* Meson compila con -std=c11 a secas, que apaga las extensiones de POSIX
 * por default: sin esto, glibc no declara lstat() (la usa g_lstat() de
 * <glib/gstdio.h>) y la compilación se cae con "declaración implícita".
 * Tiene que ir ANTES de cualquier #include, incluido bt-obex.h: las macros
 * de features de glibc se fijan con el primer encabezado del sistema que
 * entra, no importa desde qué archivo. */
#define _DEFAULT_SOURCE

#include "bt-obex.h"

#include <gio/gio.h>
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

/* Archivo que declara el servicio como activable por el bus. Su presencia
 * es señal fuerte de que el paquete está instalado. */
#define OBEX_DBUS_SERVICE_FILE \
    "/usr/share/dbus-1/services/org.bluez.obex.service"

/* Rutas típicas del ejecutable. Debian y Fedora lo ponen en libexec;
 * Arch en lib. Las derivadas con multiarch se cubren aparte. */
static const gchar *const obexd_paths[] = {
    "/usr/libexec/bluetooth/obexd",
    "/usr/lib/bluetooth/obexd",
    "/usr/lib64/bluetooth/obexd",
    "/usr/local/libexec/bluetooth/obexd",
    "/usr/local/lib/bluetooth/obexd",
    NULL
};

/* ============================================================
 * Búsqueda del ejecutable
 * ============================================================ */

/* Busca el ejecutable y devuelve su ruta (recién reservada). NULL si no
 * apareció en ningún lado. La usa tanto obexd_binary_present() como el
 * instalador de la sustitución de systemd, que necesita la ruta real y no
 * solo un sí/no. */
static gchar *
obexd_binary_find (void)
{
    guint i;

    for (i = 0; obexd_paths[i] != NULL; i++) {
        if (g_file_test (obexd_paths[i], G_FILE_TEST_IS_EXECUTABLE))
            return g_strdup (obexd_paths[i]);
    }

    /* Multiarch de Debian y derivadas: /usr/lib/<arquitectura>/bluetooth/obexd
     * (por ejemplo /usr/lib/x86_64-linux-gnu/bluetooth/obexd). No se puede
     * clavar la ruta porque el nombre de la arquitectura cambia. */
    GDir *dir = g_dir_open ("/usr/lib", 0, NULL);
    if (dir != NULL) {
        const gchar *entry;
        gchar       *found = NULL;

        while (found == NULL && (entry = g_dir_read_name (dir)) != NULL) {
            gchar *cand = g_build_filename ("/usr/lib", entry,
                                            "bluetooth", "obexd", NULL);
            if (g_file_test (cand, G_FILE_TEST_IS_EXECUTABLE))
                found = cand;
            else
                g_free (cand);
        }
        g_dir_close (dir);
        if (found != NULL)
            return found;
    }

    return NULL;
}

static gboolean
obexd_binary_present (void)
{
    gchar    *path    = obexd_binary_find ();
    gboolean  present = (path != NULL);

    g_free (path);
    return present;
}

/* ============================================================
 * Consultas al demonio del bus de sesión
 * ============================================================ */

/* TRUE si alguien tiene ese nombre tomado ahora mismo. */
static gboolean
bus_name_has_owner (GDBusConnection *bus, const gchar *name)
{
    GVariant *ret = g_dbus_connection_call_sync (
        bus,
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameHasOwner",
        g_variant_new ("(s)", name), G_VARIANT_TYPE ("(b)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL, NULL);

    if (ret == NULL)
        return FALSE;

    gboolean has_owner = FALSE;
    g_variant_get (ret, "(b)", &has_owner);
    g_variant_unref (ret);
    return has_owner;
}

/* TRUE si el bus sabe arrancar ese servicio por su cuenta cuando alguien
 * le hable. Esto es lo que hace que en la mayoría de los sistemas no haga
 * falta habilitar nada a mano. */
static gboolean
bus_name_activatable (GDBusConnection *bus, const gchar *name)
{
    GVariant *ret = g_dbus_connection_call_sync (
        bus,
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "ListActivatableNames",
        NULL, G_VARIANT_TYPE ("(as)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL, NULL);

    if (ret == NULL)
        return FALSE;

    GVariantIter *iter = NULL;
    const gchar  *item;
    gboolean      found = FALSE;

    g_variant_get (ret, "(as)", &iter);
    while (!found && g_variant_iter_next (iter, "&s", &item))
        found = (g_strcmp0 (item, name) == 0);

    g_variant_iter_free (iter);
    g_variant_unref (ret);
    return found;
}

/* ============================================================
 * Nombre del paquete según la distribución
 * ============================================================ */

/* Lee un campo de /etc/os-release, en minúsculas y sin comillas.
 * Devuelve NULL si no está. Liberar con g_free(). */
static gchar *
os_release_field (const gchar *content, const gchar *key)
{
    gchar **lines = g_strsplit (content, "\n", -1);
    gchar  *value = NULL;
    guint   i;

    for (i = 0; lines[i] != NULL && value == NULL; i++) {
        gchar *line = g_strstrip (lines[i]);

        if (!g_str_has_prefix (line, key))
            continue;
        /* Sin esto, buscar "ID" se comería también "ID_LIKE". */
        if (line[strlen (key)] != '=')
            continue;

        gchar *raw = g_strdup (line + strlen (key) + 1);
        g_strstrip (raw);

        gsize len = strlen (raw);
        if (len >= 2 && (raw[0] == '"' || raw[0] == '\'') && raw[len - 1] == raw[0]) {
            memmove (raw, raw + 1, len - 2);
            raw[len - 2] = '\0';
        }

        value = g_ascii_strdown (raw, -1);
        g_free (raw);
    }

    g_strfreev (lines);
    return value;
}

const gchar *
bt_obex_package_name (void)
{
    /* Se calcula una sola vez y queda vivo hasta que muere el proceso.
     * No se libera a propósito: es del tamaño de una palabra. */
    static const gchar *cached = NULL;

    if (cached != NULL)
        return cached;

    gboolean  arch_like = FALSE;
    gchar    *content   = NULL;

    if (g_file_get_contents ("/etc/os-release", &content, NULL, NULL)) {
        gchar *id      = os_release_field (content, "ID");
        gchar *id_like = os_release_field (content, "ID_LIKE");

        if ((id != NULL      && strstr (id, "arch") != NULL) ||
            (id_like != NULL && strstr (id_like, "arch") != NULL))
            arch_like = TRUE;

        g_free (id);
        g_free (id_like);
        g_free (content);
    }

    cached = arch_like ? "bluez-obex" : "bluez-obexd";
    return cached;
}

/* ============================================================
 * Interfaz pública
 * ============================================================ */

BtObexState
bt_obex_check (void)
{
    GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);

    if (bus == NULL)
        return BT_OBEX_NO_SESSION_BUS;

    BtObexState state;

    if (bus_name_has_owner (bus, BT_OBEX_BUS_NAME) ||
        bus_name_activatable (bus, BT_OBEX_BUS_NAME))
        state = BT_OBEX_READY;
    else if (obexd_binary_present () ||
             g_file_test (OBEX_DBUS_SERVICE_FILE, G_FILE_TEST_EXISTS))
        state = BT_OBEX_NOT_ENABLED;
    else
        state = BT_OBEX_NOT_INSTALLED;

    g_object_unref (bus);
    return state;
}

const gchar *
bt_obex_state_message (BtObexState state)
{
    switch (state) {
    case BT_OBEX_READY:
        return _("File transfer: ready");
    case BT_OBEX_NOT_ENABLED:
        return _("File transfer: service installed but not enabled");
    case BT_OBEX_NOT_INSTALLED:
        return _("File transfer: service not installed");
    case BT_OBEX_NO_SESSION_BUS:
    default:
        return _("File transfer: session bus unavailable");
    }
}

gchar *
bt_obex_state_hint (BtObexState state)
{
    switch (state) {
    case BT_OBEX_READY:
        return NULL;

    case BT_OBEX_NOT_ENABLED:
        return g_strdup (
            _("Enable it with:\nsystemctl --user enable --now obex.service"));

    case BT_OBEX_NOT_INSTALLED:
        return g_strdup_printf (
            _("Install the package \"%s\", then run:\n"
              "systemctl --user enable --now obex.service"),
            bt_obex_package_name ());

    case BT_OBEX_NO_SESSION_BUS:
    default:
        return g_strdup (
            _("The plugin could not reach the D-Bus session bus."));
    }
}

/* ============================================================
 * RECEPCIÓN — enlaces simbólicos en la ruta de destino
 *
 * obexd se niega a escribir si algún tramo de la ruta de destino es un
 * enlace simbólico (medida de seguridad de upstream, no un capricho). El
 * plugin no puede evitar esa restricción, pero sí puede: detectarla ANTES
 * de que la transferencia falle en silencio, y aplicar la única solución
 * que existe (la opción --symlinks del demonio) sin que el usuario tenga
 * que copiar comandos a mano. Como obex.service es un servicio de USUARIO,
 * todo esto se hace sin pedir contraseña ni privilegios de root.
 * ============================================================ */

/* Rutas donde cada distribución instala el unit ORIGINAL del paquete (no el
 * nuestro). Se leen solo para sacar su ExecStart tal cual viene, y así
 * escribir la sustitución con la ruta real del binario en vez de adivinarla
 * de nuevo. */
static const gchar *const obex_unit_paths[] = {
    "/usr/lib/systemd/user/obex.service",
    "/lib/systemd/user/obex.service",
    "/usr/local/lib/systemd/user/obex.service",
    NULL
};

static gchar *
find_original_execstart (void)
{
    guint u;

    for (u = 0; obex_unit_paths[u] != NULL; u++) {
        gchar *content = NULL;

        if (!g_file_get_contents (obex_unit_paths[u], &content, NULL, NULL))
            continue;

        gchar **lines = g_strsplit (content, "\n", -1);
        gchar  *found = NULL;

        for (guint i = 0; lines[i] != NULL && found == NULL; i++) {
            gchar *line = g_strstrip (lines[i]);
            if (g_str_has_prefix (line, "ExecStart="))
                found = g_strdup (line + strlen ("ExecStart="));
        }

        g_strfreev (lines);
        g_free (content);

        if (found != NULL)
            return found;
    }

    return NULL;
}

gchar *
bt_obex_find_symlink_in_path (const gchar *folder)
{
    g_return_val_if_fail (folder != NULL, NULL);

    /* Recorre desde la raíz, tramo por tramo, SIN usar realpath(): eso
     * seguiría los enlaces en vez de encontrarlos. lstat mira cada tramo
     * tal cual es, sin seguirlo. */
    gchar   **parts = g_strsplit (folder, G_DIR_SEPARATOR_S, -1);
    GString  *acc   = g_string_new ("");
    gchar    *found = NULL;

    for (guint i = 0; parts[i] != NULL && found == NULL; i++) {
        if (*parts[i] == '\0')
            continue; /* separador inicial o repetido */

        g_string_append_c (acc, G_DIR_SEPARATOR);
        g_string_append (acc, parts[i]);

        GStatBuf st;
        if (g_lstat (acc->str, &st) != 0)
            continue; /* tramo que todavía no existe: nada que revisar acá */

        if (S_ISLNK (st.st_mode))
            found = g_strdup (acc->str);
    }

    g_strfreev (parts);
    g_string_free (acc, TRUE);
    return found;
}

gboolean
bt_obex_symlinks_override_installed (void)
{
    gchar    *path    = g_build_filename (g_get_user_config_dir (),
                                          "systemd", "user", "obex.service.d",
                                          "10-symlinks.conf", NULL);
    gchar    *content = NULL;
    gboolean  found   = FALSE;

    if (g_file_get_contents (path, &content, NULL, NULL))
        found = (strstr (content, "--symlinks") != NULL);

    g_free (content);
    g_free (path);
    return found;
}

/* Corre "systemctl --user <argv...>" sin pasar por ninguna shell (así da lo
 * mismo si el usuario tiene fish, bash o lo que sea: no se interpreta nada,
 * solo se ejecuta el binario con esos argumentos). */
static gboolean
run_systemctl_user (const gchar *const *argv, GError **error)
{
    GSubprocess *proc = g_subprocess_newv (argv,
                                           G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                           error);
    if (proc == NULL)
        return FALSE;

    gboolean ok = g_subprocess_wait_check (proc, NULL, error);
    g_object_unref (proc);
    return ok;
}

gboolean
bt_obex_install_symlinks_override (GError **error)
{
    gchar *dir = g_build_filename (g_get_user_config_dir (),
                                   "systemd", "user", "obex.service.d", NULL);

    if (g_mkdir_with_parents (dir, 0755) != 0) {
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                    _("Could not create %s: %s"), dir, g_strerror (errno));
        g_free (dir);
        return FALSE;
    }

    gchar *conf_path = g_build_filename (dir, "10-symlinks.conf", NULL);
    g_free (dir);

    /* Preferimos la ruta que el propio paquete declara en su unit original;
     * si no la pudimos leer, caemos a la búsqueda de siempre, y solo como
     * último recurso a la ruta más común (Arch y derivadas). Si esa
     * suposición estuviera mal en algún sistema, el reinicio del servicio
     * de más abajo va a fallar, y el error queda a la vista en vez de una
     * escritura silenciosa que "parece" haber andado. */
    gchar *bin = find_original_execstart ();
    if (bin == NULL)
        bin = obexd_binary_find ();
    if (bin == NULL)
        bin = g_strdup ("/usr/lib/bluetooth/obexd");

    gchar *content = g_strdup_printf (
        "[Service]\nExecStart=\nExecStart=%s --symlinks\n", bin);
    g_free (bin);

    gboolean wrote = g_file_set_contents (conf_path, content, -1, error);
    g_free (content);
    g_free (conf_path);

    if (!wrote)
        return FALSE;

    const gchar *reload_argv[] = { "systemctl", "--user", "daemon-reload", NULL };
    if (!run_systemctl_user (reload_argv, error))
        return FALSE;

    const gchar *restart_argv[] = {
        "systemctl", "--user", "restart", "obex.service", NULL
    };
    return run_systemctl_user (restart_argv, error);
}

/* ============================================================
 * ENVÍO DE ARCHIVOS
 *
 * Secuencia completa, y por qué son tres pasos y no uno:
 *
 *   1. CreateSession con el destino y el tipo "opp". Esto conecta con el
 *      aparato y devuelve la ruta de una sesión. Puede tardar: si el aparato
 *      está apagado o lejos, acá es donde se nota.
 *
 *   2. SendFile sobre esa sesión. Devuelve la ruta de un objeto de
 *      transferencia y sus propiedades iniciales. OJO: que esto vuelva bien
 *      NO quiere decir que el archivo se esté mandando; quiere decir que se
 *      ofreció. El otro extremo todavía tiene que aceptar.
 *
 *   3. Escuchar los cambios de propiedades de ese objeto de transferencia.
 *      Ahí llegan el estado ("queued", "active", "complete", "error") y los
 *      bytes que van pasando.
 *
 * Al terminar, en cualquiera de las tres etapas, se cierra la sesión y se
 * avisa una sola vez.
 * ============================================================ */

#define OBEX_PUSH_IFACE     "org.bluez.obex.ObjectPush1"
#define OBEX_TRANSFER_IFACE "org.bluez.obex.Transfer1"
#define OBEX_OPP_UUID       "00001105-0000-1000-8000-00805f9b34fb"

struct _BtObexSend {
    GDBusConnection *bus;
    gchar           *filename;
    gchar           *session_path;
    gchar           *transfer_path;
    guint            watch_id;

    guint64          size;
    guint64          transferred;

    gboolean         finished;

    BtObexProgressCb progress_cb;
    BtObexDoneCb     done_cb;
    gpointer         user_data;

    /* Cuentas de vida. Cada llamada asíncrona en vuelo se lleva una, y la
     * devuelve al volver. Sin esto, terminar el envío mientras hay una
     * llamada en el aire liberaría memoria que esa llamada todavía va a
     * tocar cuando el bus le conteste. */
    gint             refs;
};

static void send_finish (BtObexSend *s, gboolean success, const gchar *message);

/* ---------- vida del objeto ---------- */

static BtObexSend *
send_ref (BtObexSend *s)
{
    s->refs++;
    return s;
}

static void
send_unref (BtObexSend *s)
{
    if (--s->refs > 0)
        return;

    if (s->bus != NULL)
        g_object_unref (s->bus);
    g_free (s->filename);
    g_free (s->session_path);
    g_free (s->transfer_path);
    g_free (s);
}

/* ---------- traducción de errores ---------- */

/* Convierte un error del bus en algo que un humano pueda leer. Liberar con
 * g_free(). Los dos casos que de verdad le pasan a la gente son que falte el
 * demonio y que el aparato no sepa recibir archivos; el resto se pasa tal
 * cual, ya despojado del prefijo del bus. */
static gchar *
obex_error_message (GError *error)
{
    if (error == NULL)
        return g_strdup (_("Unknown error."));

    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return g_strdup (_("Cancelled."));

    gchar *remote = g_dbus_error_get_remote_error (error);

    if (remote != NULL) {
        if (g_strcmp0 (remote, "org.freedesktop.DBus.Error.ServiceUnknown") == 0) {
            g_free (remote);
            return g_strdup_printf (
                _("The Bluetooth file transfer service is not available. "
                  "Install the package \"%s\"."),
                bt_obex_package_name ());
        }
        g_free (remote);
    }

    GError *copy = g_error_copy (error);
    g_dbus_error_strip_remote_error (copy);

    /* obexd contesta esto cuando el aparato no publica el perfil de
     * recepción, que es el caso más común después de la falta del demonio. */
    if (copy->message != NULL &&
        (strstr (copy->message, "service record") != NULL ||
         strstr (copy->message, "Service record") != NULL)) {
        g_error_free (copy);
        return g_strdup (_("This device does not accept files."));
    }

    gchar *msg = g_strdup (copy->message != NULL ? copy->message
                                                 : _("Unknown error."));
    g_error_free (copy);
    return msg;
}

/* ---------- cierre ---------- */

static void
send_finish (BtObexSend *s, gboolean success, const gchar *message)
{
    if (s->finished)
        return;
    s->finished = TRUE;

    if (s->watch_id != 0) {
        g_dbus_connection_signal_unsubscribe (s->bus, s->watch_id);
        s->watch_id = 0;
    }

    /* Cerrar la sesión con el aparato. Sin respuesta a propósito: si esto
     * falla no hay nada que el usuario pueda hacer, y el envío ya terminó. */
    if (s->session_path != NULL) {
        g_dbus_connection_call (s->bus, BT_OBEX_BUS_NAME, BT_OBEX_ROOT_PATH,
                                BT_OBEX_CLIENT_IFACE, "RemoveSession",
                                g_variant_new ("(o)", s->session_path),
                                NULL, G_DBUS_CALL_FLAGS_NONE, 10000,
                                NULL, NULL, NULL);
    }

    if (s->done_cb != NULL)
        s->done_cb (success, message, s->user_data);

    send_unref (s);   /* la cuenta del dueño */
}

void
bt_obex_send_cancel (BtObexSend *s)
{
    if (s == NULL || s->finished)
        return;

    /* Avisarle a BlueZ que abandone, sin esperar respuesta: el aviso de fin
     * lo damos nosotros acá abajo, no dependemos de que conteste. */
    if (s->transfer_path != NULL) {
        g_dbus_connection_call (s->bus, BT_OBEX_BUS_NAME, s->transfer_path,
                                OBEX_TRANSFER_IFACE, "Cancel",
                                NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 10000,
                                NULL, NULL, NULL);
    }

    send_finish (s, FALSE, _("Cancelled."));
}

/* ---------- lectura de las propiedades de la transferencia ---------- */

/* Mira un diccionario de propiedades y actualiza lo que encuentre. Devuelve
 * TRUE si el envío terminó (bien o mal) y deja el resultado en *success. */
static gboolean
apply_transfer_props (BtObexSend *s, GVariant *props,
                      gboolean *success, gchar **message)
{
    GVariant *v;
    gboolean  ended = FALSE;

    v = g_variant_lookup_value (props, "Size", G_VARIANT_TYPE_UINT64);
    if (v != NULL) {
        s->size = g_variant_get_uint64 (v);
        g_variant_unref (v);
    }

    v = g_variant_lookup_value (props, "Transferred", G_VARIANT_TYPE_UINT64);
    if (v != NULL) {
        s->transferred = g_variant_get_uint64 (v);
        g_variant_unref (v);
    }

    v = g_variant_lookup_value (props, "Status", G_VARIANT_TYPE_STRING);
    if (v != NULL) {
        const gchar *status = g_variant_get_string (v, NULL);

        if (g_strcmp0 (status, "complete") == 0) {
            ended    = TRUE;
            *success = TRUE;
            *message = g_strdup (_("File sent."));
        } else if (g_strcmp0 (status, "error") == 0) {
            ended    = TRUE;
            *success = FALSE;
            *message = g_strdup (_("The device rejected the file or the "
                                   "transfer was interrupted."));
        }
        g_variant_unref (v);
    }

    return ended;
}

static void
on_transfer_props (GDBusConnection *conn, const gchar *sender,
                   const gchar *path, const gchar *iface,
                   const gchar *signal_name, GVariant *params,
                   gpointer user_data)
{
    (void) conn; (void) sender; (void) path; (void) iface; (void) signal_name;
    BtObexSend *s = user_data;

    if (s->finished)
        return;

    const gchar *changed_iface = NULL;
    GVariant    *props         = NULL;

    g_variant_get (params, "(&s@a{sv}@as)", &changed_iface, &props, NULL);

    if (g_strcmp0 (changed_iface, OBEX_TRANSFER_IFACE) != 0) {
        g_variant_unref (props);
        return;
    }

    gboolean  success = FALSE;
    gchar    *message = NULL;
    gboolean  ended   = apply_transfer_props (s, props, &success, &message);

    g_variant_unref (props);

    if (!ended) {
        if (s->progress_cb != NULL)
            s->progress_cb (s->transferred, s->size, s->user_data);
        return;
    }

    send_finish (s, success, message);
    g_free (message);
}

/* Foto del estado justo después de suscribirnos.
 *
 * Cubre una carrera chica pero real: entre que SendFile contesta y que
 * alcanzamos a suscribirnos, la transferencia podría haber cambiado de
 * estado y nos perderíamos el aviso. Si el objeto ya no existe, damos por
 * terminado el envío juzgando por los bytes que habían pasado. */
static void
on_transfer_snapshot (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) source;
    BtObexSend *s   = user_data;
    GError     *err = NULL;

    GVariant *ret = g_dbus_connection_call_finish (s->bus, res, &err);

    if (s->finished) {
        if (ret != NULL) g_variant_unref (ret);
        g_clear_error (&err);
        send_unref (s);
        return;
    }

    if (ret == NULL) {
        gboolean done_ok = (s->size > 0 && s->transferred >= s->size);
        g_clear_error (&err);
        send_finish (s, done_ok,
                     done_ok ? _("File sent.")
                             : _("The transfer ended unexpectedly."));
        send_unref (s);
        return;
    }

    GVariant *props = NULL;
    g_variant_get (ret, "(@a{sv})", &props);

    gboolean  success = FALSE;
    gchar    *message = NULL;
    gboolean  ended   = apply_transfer_props (s, props, &success, &message);

    g_variant_unref (props);
    g_variant_unref (ret);

    if (ended) {
        send_finish (s, success, message);
        g_free (message);
    } else if (s->progress_cb != NULL) {
        s->progress_cb (s->transferred, s->size, s->user_data);
    }

    send_unref (s);
}

/* ---------- paso 2: ofrecer el archivo ---------- */

static void
on_send_started (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) source;
    BtObexSend *s   = user_data;
    GError     *err = NULL;

    GVariant *ret = g_dbus_connection_call_finish (s->bus, res, &err);

    if (s->finished) {
        if (ret != NULL) g_variant_unref (ret);
        g_clear_error (&err);
        send_unref (s);
        return;
    }

    if (ret == NULL) {
        gchar *msg = obex_error_message (err);
        g_clear_error (&err);
        send_finish (s, FALSE, msg);
        g_free (msg);
        send_unref (s);
        return;
    }

    const gchar *transfer_path = NULL;
    GVariant    *props         = NULL;
    g_variant_get (ret, "(&o@a{sv})", &transfer_path, &props);
    s->transfer_path = g_strdup (transfer_path);

    /* Suscribirse ANTES de mirar las propiedades iniciales: si el estado
     * cambia en el medio, el aviso ya nos encuentra escuchando. */
    s->watch_id = g_dbus_connection_signal_subscribe (
        s->bus, BT_OBEX_BUS_NAME,
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        s->transfer_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_transfer_props, s, NULL);

    gboolean  success = FALSE;
    gchar    *message = NULL;
    gboolean  ended   = apply_transfer_props (s, props, &success, &message);

    g_variant_unref (props);
    g_variant_unref (ret);

    if (ended) {
        send_finish (s, success, message);
        g_free (message);
        send_unref (s);
        return;
    }

    if (s->progress_cb != NULL)
        s->progress_cb (s->transferred, s->size, s->user_data);

    g_dbus_connection_call (s->bus, BT_OBEX_BUS_NAME, s->transfer_path,
                            "org.freedesktop.DBus.Properties", "GetAll",
                            g_variant_new ("(s)", OBEX_TRANSFER_IFACE),
                            G_VARIANT_TYPE ("(a{sv})"),
                            G_DBUS_CALL_FLAGS_NONE, 10000, NULL,
                            on_transfer_snapshot, send_ref (s));

    send_unref (s);
}

/* ---------- paso 1: abrir la sesión ---------- */

static void
on_session_created (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) source;
    BtObexSend *s   = user_data;
    GError     *err = NULL;

    GVariant *ret = g_dbus_connection_call_finish (s->bus, res, &err);

    if (s->finished) {
        /* Cancelado mientras se abría la sesión. Si igual llegó a abrirse,
         * hay que cerrarla o queda colgada ocupando el aparato. */
        if (ret != NULL) {
            const gchar *path = NULL;
            g_variant_get (ret, "(&o)", &path);
            g_dbus_connection_call (s->bus, BT_OBEX_BUS_NAME, BT_OBEX_ROOT_PATH,
                                    BT_OBEX_CLIENT_IFACE, "RemoveSession",
                                    g_variant_new ("(o)", path),
                                    NULL, G_DBUS_CALL_FLAGS_NONE, 10000,
                                    NULL, NULL, NULL);
            g_variant_unref (ret);
        }
        g_clear_error (&err);
        send_unref (s);
        return;
    }

    if (ret == NULL) {
        gchar *msg = obex_error_message (err);
        g_clear_error (&err);
        send_finish (s, FALSE, msg);
        g_free (msg);
        send_unref (s);
        return;
    }

    const gchar *session_path = NULL;
    g_variant_get (ret, "(&o)", &session_path);
    s->session_path = g_strdup (session_path);
    g_variant_unref (ret);

    g_dbus_connection_call (s->bus, BT_OBEX_BUS_NAME, s->session_path,
                            OBEX_PUSH_IFACE, "SendFile",
                            g_variant_new ("(s)", s->filename),
                            G_VARIANT_TYPE ("(oa{sv})"),
                            G_DBUS_CALL_FLAGS_NONE, 60000, NULL,
                            on_send_started, send_ref (s));

    send_unref (s);
}

/* ---------- interfaz pública ---------- */

gboolean
bt_obex_device_can_receive (gchar **uuids)
{
    if (uuids == NULL)
        return FALSE;

    for (guint i = 0; uuids[i] != NULL; i++) {
        if (g_ascii_strcasecmp (uuids[i], OBEX_OPP_UUID) == 0)
            return TRUE;
        /* Algunos aparatos publican la forma corta. */
        if (g_ascii_strcasecmp (uuids[i], "1105") == 0 ||
            g_ascii_strcasecmp (uuids[i], "0x1105") == 0)
            return TRUE;
    }
    return FALSE;
}

BtObexSend *
bt_obex_send_file (const gchar     *address,
                   const gchar     *filename,
                   BtObexProgressCb progress_cb,
                   BtObexDoneCb     done_cb,
                   gpointer         user_data)
{
    g_return_val_if_fail (address != NULL, NULL);
    g_return_val_if_fail (filename != NULL, NULL);

    GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    if (bus == NULL)
        return NULL;

    BtObexSend *s = g_new0 (BtObexSend, 1);
    s->bus         = bus;
    s->filename    = g_strdup (filename);
    s->progress_cb = progress_cb;
    s->done_cb     = done_cb;
    s->user_data   = user_data;
    s->refs        = 1;             /* la cuenta del dueño, la suelta send_finish */

    GVariantBuilder args;
    g_variant_builder_init (&args, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&args, "{sv}", "Target",
                           g_variant_new_string ("opp"));

    /* Acá SÍ queremos que el bus encienda el demonio si hace falta: es el
     * momento en que el usuario pidió transferir de verdad. La detección de
     * bt_obex_check() es la que se cuida de no encender nada. */
    g_dbus_connection_call (s->bus, BT_OBEX_BUS_NAME, BT_OBEX_ROOT_PATH,
                            BT_OBEX_CLIENT_IFACE, "CreateSession",
                            g_variant_new ("(sa{sv})", address, &args),
                            G_VARIANT_TYPE ("(o)"),
                            G_DBUS_CALL_FLAGS_NONE, 120000, NULL,
                            on_session_created, send_ref (s));

    return s;
}

/* ============================================================
 * Utilidad compartida
 * ============================================================ */

gchar *
bt_obex_unique_path (const gchar *folder, const gchar *name)
{
    g_return_val_if_fail (folder != NULL, NULL);

    /* Poda de seguridad: quedarse SOLO con la última parte del nombre. Lo
     * elige el aparato que manda, no nosotros. Ver el comentario del .h. */
    gchar *base = g_path_get_basename (name != NULL && *name ? name : "file");

    /* g_path_get_basename devuelve "." o "/" para entradas degeneradas. */
    if (g_strcmp0 (base, ".") == 0 || g_strcmp0 (base, "..") == 0 ||
        g_strcmp0 (base, G_DIR_SEPARATOR_S) == 0) {
        g_free (base);
        base = g_strdup ("file");
    }

    gchar *candidate = g_build_filename (folder, base, NULL);
    if (!g_file_test (candidate, G_FILE_TEST_EXISTS)) {
        g_free (base);
        return candidate;
    }
    g_free (candidate);

    /* Partir en cuerpo y extensión para que el número quede antes del punto:
     * "foto (1).jpg" y no "foto.jpg (1)". */
    const gchar *dot  = strrchr (base, '.');
    gchar       *stem;
    gchar       *ext;

    if (dot != NULL && dot != base) {
        stem = g_strndup (base, (gsize) (dot - base));
        ext  = g_strdup (dot);
    } else {
        stem = g_strdup (base);
        ext  = g_strdup ("");
    }
    g_free (base);

    gchar *result = NULL;
    for (guint i = 1; i < 10000; i++) {
        gchar *try_name = g_strdup_printf ("%s (%u)%s", stem, i, ext);
        gchar *try_path = g_build_filename (folder, try_name, NULL);
        g_free (try_name);

        if (!g_file_test (try_path, G_FILE_TEST_EXISTS)) {
            result = try_path;
            break;
        }
        g_free (try_path);
    }

    /* Diez mil archivos con el mismo nombre: caso imposible, pero nunca
     * devolver NULL. */
    if (result == NULL) {
        gchar *fallback = g_strdup_printf ("%s (%" G_GINT64_FORMAT ")%s",
                                           stem, g_get_real_time (), ext);
        result = g_build_filename (folder, fallback, NULL);
        g_free (fallback);
    }

    g_free (stem);
    g_free (ext);
    return result;
}
