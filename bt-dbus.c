/* bt-dbus.c — implementación de la capa BlueZ.
 *
 * Estructura calcada del nm-dbus.c del plugin de red:
 *   1. caché de instantánea vía ObjectManager,
 *   2. helpers get_property / get_all_properties que la consultan primero,
 *   3. constructores de structs planos a partir de los diccionarios,
 *   4. acciones asíncronas,
 *   5. suscripción a señales.
 */

#include "bt-dbus.h"
#include <glib/gi18n-lib.h>
#include <string.h>

/* ================================================================
 * Caché de instantánea (ObjectManager)
 * ================================================================ */

static GVariant *om_snapshot = NULL;   /* tipo a{oa{sa{sv}}} */
static gint      om_depth    = 0;

GDBusConnection *
bt_dbus_connect (void)
{
    GError          *err  = NULL;
    GDBusConnection *conn = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &err);
    if (!conn) {
        g_warning ("bt-dbus: no se pudo conectar al bus del sistema: %s",
                   err->message);
        g_error_free (err);
    }
    return conn;
}

/* Trae el árbol completo de objetos de BlueZ. Devuelve referencia nueva
 * (liberar con g_variant_unref) o NULL si BlueZ no está disponible. */
static GVariant *
om_fetch (GDBusConnection *conn)
{
    if (!conn)
        return NULL;

    GError   *err = NULL;
    GVariant *result = g_dbus_connection_call_sync (
        conn, BT_BUS_NAME, BT_ROOT_PATH,
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
        NULL, G_VARIANT_TYPE ("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);

    if (!result) {
        /* Sin BlueZ corriendo esto falla en cada refresco; no llenamos el log. */
        if (err) g_error_free (err);
        return NULL;
    }

    GVariant *objs = g_variant_get_child_value (result, 0);
    g_variant_unref (result);
    return objs;
}

void
bt_cache_begin (GDBusConnection *conn)
{
    om_depth++;
    if (om_depth > 1)
        return;
    om_snapshot = conn ? om_fetch (conn) : NULL;
}

void
bt_cache_end (void)
{
    if (om_depth <= 0) {
        g_warning ("bt-dbus: bt_cache_end sin bt_cache_begin");
        return;
    }
    om_depth--;
    if (om_depth == 0 && om_snapshot) {
        g_variant_unref (om_snapshot);
        om_snapshot = NULL;
    }
}

/* Devuelve el árbol de objetos: la instantánea si hay una activa (gratis),
 * o una lectura fresca si no. Referencia nueva en ambos casos. */
static GVariant *
om_get (GDBusConnection *conn)
{
    if (om_snapshot)
        return g_variant_ref (om_snapshot);
    return om_fetch (conn);
}

/* TRUE si hay una instantánea activa. Cuando la hay, una interfaz ausente
 * significa que el objeto realmente no la implementa: no hay que caer a una
 * llamada al bus para confirmarlo. */
static gboolean
snapshot_active (void)
{
    return (om_snapshot != NULL);
}

/* Diccionario de propiedades de una interfaz de un objeto, desde la
 * instantánea. Referencia nueva, o NULL. */
static GVariant *
snapshot_get_props (const gchar *object_path, const gchar *iface)
{
    if (!om_snapshot)
        return NULL;

    GVariant *ifaces = g_variant_lookup_value (om_snapshot, object_path,
                                               G_VARIANT_TYPE ("a{sa{sv}}"));
    if (!ifaces)
        return NULL;

    GVariant *props = g_variant_lookup_value (ifaces, iface,
                                              G_VARIANT_TYPE ("a{sv}"));
    g_variant_unref (ifaces);
    return props;
}

static GVariant *
get_property (GDBusConnection *conn,
              const gchar     *object_path,
              const gchar     *iface,
              const gchar     *prop)
{
    GVariant *value = NULL;

    GVariant *props = snapshot_get_props (object_path, iface);
    if (props) {
        value = g_variant_lookup_value (props, prop, NULL);
        g_variant_unref (props);
        if (value)
            return value;
    }

    GVariant *result = g_dbus_connection_call_sync (
        conn, BT_BUS_NAME, object_path,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new ("(ss)", iface, prop),
        G_VARIANT_TYPE ("(v)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);

    if (result) {
        g_variant_get (result, "(v)", &value);
        g_variant_unref (result);
    }
    return value;
}

static GVariant *
get_all_properties (GDBusConnection *conn,
                    const gchar     *object_path,
                    const gchar     *iface)
{
    GVariant *props = snapshot_get_props (object_path, iface);
    if (props)
        return props;

    GVariant *result = g_dbus_connection_call_sync (
        conn, BT_BUS_NAME, object_path,
        "org.freedesktop.DBus.Properties", "GetAll",
        g_variant_new ("(s)", iface),
        G_VARIANT_TYPE ("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);

    if (!result)
        return NULL;

    GVariant *dict = g_variant_get_child_value (result, 0);
    g_variant_unref (result);
    return dict;
}

static void
set_property_async (GDBusConnection *conn,
                    const gchar     *object_path,
                    const gchar     *iface,
                    const gchar     *prop,
                    GVariant        *value)
{
    g_dbus_connection_call (
        conn, BT_BUS_NAME, object_path,
        "org.freedesktop.DBus.Properties", "Set",
        g_variant_new ("(ssv)", iface, prop, value),
        NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
}

/* ---------- lectura tipada desde un diccionario a{sv} ---------- */

static gboolean
dict_bool (GVariant *props, const gchar *key, gboolean fallback)
{
    if (!props)
        return fallback;

    GVariant *v = g_variant_lookup_value (props, key, G_VARIANT_TYPE_BOOLEAN);
    if (!v)
        return fallback;

    gboolean out = g_variant_get_boolean (v);
    g_variant_unref (v);
    return out;
}

/* Lee una propiedad textual del diccionario.
 *
 * OJO con el tipo: g_variant_lookup() con el formato "s" es ESTRICTO y falla
 * en silencio si el valor no es exactamente un string. Varias propiedades de
 * BlueZ son object paths ('o'), no strings — en particular Device1.Adapter.
 * Con la versión estricta, d->adapter_path quedaba en NULL para todos los
 * dispositivos y el filtro por adaptador de bt_get_devices() los descartaba
 * a todos: la lista salía siempre vacía mientras el estado de arriba, que
 * consulta con adapter_path == NULL, sí los contaba. Aceptamos los tres
 * tipos que g_variant_dup_string() sabe manejar. */
static gchar *
dict_str (GVariant *props, const gchar *key)
{
    if (!props)
        return NULL;

    GVariant *v = g_variant_lookup_value (props, key, NULL);
    if (!v)
        return NULL;

    gchar *out = NULL;
    if (g_variant_is_of_type (v, G_VARIANT_TYPE_STRING)      ||
        g_variant_is_of_type (v, G_VARIANT_TYPE_OBJECT_PATH) ||
        g_variant_is_of_type (v, G_VARIANT_TYPE_SIGNATURE))
        out = g_variant_dup_string (v, NULL);

    g_variant_unref (v);
    return out;
}

static guint32
dict_u32 (GVariant *props, const gchar *key, guint32 fallback)
{
    if (!props)
        return fallback;

    GVariant *v = g_variant_lookup_value (props, key, NULL);
    if (!v)
        return fallback;

    guint32 out = fallback;
    if (g_variant_is_of_type (v, G_VARIANT_TYPE_UINT32))
        out = g_variant_get_uint32 (v);
    else if (g_variant_is_of_type (v, G_VARIANT_TYPE_UINT16))
        out = g_variant_get_uint16 (v);
    else if (g_variant_is_of_type (v, G_VARIANT_TYPE_BYTE))
        out = g_variant_get_byte (v);

    g_variant_unref (v);
    return out;
}

gboolean
bt_service_available (GDBusConnection *conn)
{
    if (!conn) return FALSE;
    GVariant *objs = om_get (conn);
    if (!objs) return FALSE;
    g_variant_unref (objs);
    return TRUE;
}

/* ================================================================
 * Adaptadores
 * ================================================================ */

static BtAdapter *
adapter_from_props (const gchar *path, GVariant *props)
{
    BtAdapter *a = g_new0 (BtAdapter, 1);
    a->object_path  = g_strdup (path);
    a->address      = dict_str (props, "Address");
    a->name         = dict_str (props, "Alias");
    if (!a->name || !*a->name) {
        g_free (a->name);
        a->name = dict_str (props, "Name");
    }
    if (!a->name || !*a->name) {
        g_free (a->name);
        /* Último recurso: el nombre del nodo (hci0). */
        const gchar *slash = strrchr (path, '/');
        a->name = g_strdup (slash ? slash + 1 : path);
    }
    a->powered      = dict_bool (props, "Powered",      FALSE);
    a->discovering  = dict_bool (props, "Discovering",  FALSE);
    a->discoverable = dict_bool (props, "Discoverable", FALSE);
    a->pairable     = dict_bool (props, "Pairable",     FALSE);
    return a;
}

GSList *
bt_get_adapters (GDBusConnection *conn)
{
    GSList   *list = NULL;
    GVariant *objs = om_get (conn);
    if (!objs)
        return NULL;

    GVariantIter  iter;
    const gchar  *path;
    GVariant     *ifaces;

    g_variant_iter_init (&iter, objs);
    while (g_variant_iter_next (&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *props = g_variant_lookup_value (ifaces, BT_ADAPTER_IFACE,
                                                  G_VARIANT_TYPE ("a{sv}"));
        if (props) {
            list = g_slist_append (list, adapter_from_props (path, props));
            g_variant_unref (props);
        }
        g_variant_unref (ifaces);
    }
    g_variant_unref (objs);
    return list;
}

void
bt_adapter_free (BtAdapter *a)
{
    if (!a) return;
    g_free (a->object_path);
    g_free (a->address);
    g_free (a->name);
    g_free (a);
}

void
bt_adapter_list_free (GSList *list)
{
    g_slist_free_full (list, (GDestroyNotify) bt_adapter_free);
}

gboolean
bt_any_adapter_powered (GDBusConnection *conn)
{
    gboolean found = FALSE;
    GSList  *as = bt_get_adapters (conn);
    for (GSList *l = as; l && !found; l = l->next)
        found = ((BtAdapter *) l->data)->powered;
    bt_adapter_list_free (as);
    return found;
}

gboolean
bt_any_adapter_discovering (GDBusConnection *conn)
{
    gboolean found = FALSE;
    GSList  *as = bt_get_adapters (conn);
    for (GSList *l = as; l && !found; l = l->next)
        found = ((BtAdapter *) l->data)->discovering;
    bt_adapter_list_free (as);
    return found;
}

gboolean
bt_any_adapter_discoverable (GDBusConnection *conn)
{
    gboolean found = FALSE;
    GSList  *as = bt_get_adapters (conn);
    for (GSList *l = as; l && !found; l = l->next)
        found = ((BtAdapter *) l->data)->discoverable;
    bt_adapter_list_free (as);
    return found;
}

void
bt_adapter_set_powered_async (GDBusConnection *conn, const gchar *adapter_path,
                              gboolean powered)
{
    set_property_async (conn, adapter_path, BT_ADAPTER_IFACE, "Powered",
                        g_variant_new_boolean (powered));
}

void
bt_adapter_set_discoverable_async (GDBusConnection *conn,
                                   const gchar *adapter_path,
                                   gboolean discoverable)
{
    /* Con plazo, a propósito (ver BT_DISCOVERABLE_TIMEOUT_S en el .h). El
     * miedo de la versión anterior, que el interruptor y el estado real se
     * desincronizaran al vencer el plazo, ya no aplica: la ventana escucha
     * los cambios del adaptador y redibuja sola, y al reabrirla lee el
     * estado fresco.
     *
     * El orden de las tres llamadas lo garantiza D-Bus (mismo cliente, mismo
     * destino): primero emparejable, después el plazo, al final la
     * visibilidad. Fijar el plazo antes evita que BlueZ arranque con el que
     * tuviera guardado. */
    if (discoverable) {
        bt_adapter_set_pairable (conn, adapter_path);
        set_property_async (conn, adapter_path, BT_ADAPTER_IFACE,
                            "DiscoverableTimeout",
                            g_variant_new_uint32 (BT_DISCOVERABLE_TIMEOUT_S));
    }
    set_property_async (conn, adapter_path, BT_ADAPTER_IFACE, "Discoverable",
                        g_variant_new_boolean (discoverable));
}

void
bt_set_all_powered_async (GDBusConnection *conn, gboolean powered)
{
    GSList *as = bt_get_adapters (conn);
    for (GSList *l = as; l; l = l->next)
        bt_adapter_set_powered_async (conn,
                                      ((BtAdapter *) l->data)->object_path,
                                      powered);
    bt_adapter_list_free (as);
}

void
bt_set_all_discoverable_async (GDBusConnection *conn, gboolean disc)
{
    GSList *as = bt_get_adapters (conn);
    for (GSList *l = as; l; l = l->next) {
        BtAdapter *a = l->data;
        if (a->powered)
            bt_adapter_set_discoverable_async (conn, a->object_path, disc);
    }
    bt_adapter_list_free (as);
}

/* Callback genérico para llamadas async sin resultado que nos importe. */
static void
on_fire_and_forget_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
    const gchar *what = user_data;
    GError      *err  = NULL;
    GVariant    *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src),
                                                    res, &err);
    if (r) {
        g_variant_unref (r);
    } else if (err) {
        /* "No discovery started" y similares son ruido esperable. */
        g_debug ("bt-dbus: %s: %s", what ? what : "(?)", err->message);
        g_error_free (err);
    }
}

/* A diferencia del fire-and-forget genérico, acá sí queremos enterarnos: un
 * StartDiscovery que falla deja la lista vacía y el usuario no tiene forma de
 * saber por qué. "InProgress" es la excepción esperable (ya estaba corriendo,
 * nuestro o de otro cliente) y no se reporta. */
static void
on_discovery_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
    const gchar *what = user_data;
    GError      *err  = NULL;
    GVariant    *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src),
                                                    res, &err);
    if (r) {
        g_variant_unref (r);
        return;
    }
    if (err) {
        gchar *remote = g_dbus_error_get_remote_error (err);
        /* "No discovery started" llega como un Failed genérico: es lo que
         * contesta BlueZ cuando pedimos parar en un adaptador donde nosotros
         * no estábamos buscando. Desde que la parada se pide siempre (ver
         * bt_stop_discovery_all), es el caso normal y no un error. */
        gboolean benign = remote &&
            (g_str_has_suffix (remote, ".InProgress") ||
             g_str_has_suffix (remote, ".NoDiscovery") ||
             g_str_has_suffix (remote, ".NotReady") ||
             (g_str_has_suffix (remote, ".Failed") && err->message &&
              strstr (err->message, "No discovery started") != NULL));
        if (!benign)
            g_warning ("bt-dbus: %s falló: %s", what ? what : "(?)",
                       err->message);
        g_free (remote);
        g_error_free (err);
    }
}

/* Filtro de descubrimiento. Hay que ponerlo ANTES de StartDiscovery.
 * Transport "auto" cubre BR/EDR y BLE, que es lo que necesitan los joysticks
 * y periféricos modernos: muchos anuncian solo por BLE. DuplicateData en
 * FALSE evita que BlueZ nos inunde con un evento por cada paquete de
 * advertising repetido. */
void
bt_set_discovery_filter (GDBusConnection *conn, const gchar *adapter_path)
{
    GVariantBuilder b;
    g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&b, "{sv}", "Transport",
                           g_variant_new_string ("auto"));
    g_variant_builder_add (&b, "{sv}", "DuplicateData",
                           g_variant_new_boolean (FALSE));

    g_dbus_connection_call (conn, BT_BUS_NAME, adapter_path, BT_ADAPTER_IFACE,
                            "SetDiscoveryFilter",
                            g_variant_new ("(a{sv})", &b), NULL,
                            G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                            on_discovery_done, (gpointer) "SetDiscoveryFilter");
}

void
bt_start_discovery (GDBusConnection *conn, const gchar *adapter_path)
{
    /* El orden importa, y D-Bus lo garantiza: los mensajes de un mismo
     * cliente al mismo destino se procesan en el orden en que se enviaron. */
    bt_set_discovery_filter (conn, adapter_path);

    g_dbus_connection_call (conn, BT_BUS_NAME, adapter_path, BT_ADAPTER_IFACE,
                            "StartDiscovery", NULL, NULL,
                            G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                            on_discovery_done, (gpointer) "StartDiscovery");
}

/* Deja el adaptador aceptando pedidos de vinculación. Sin esto, un aparato
 * que inicia el pareo desde su lado se topa con la puerta cerrada.
 *
 * Se llama SOLO al encender la visibilidad. Antes se forzaba cada vez que se
 * abría la ventana, lo que pisaba sin aviso lo que el usuario o el sistema
 * hubieran decidido. Emparejar desde este equipo (botón Emparejar) no lo
 * necesita: la puerta que abre es la de entrada, no la de salida. */
void
bt_adapter_set_pairable (GDBusConnection *conn, const gchar *adapter_path)
{
    set_property_async (conn, adapter_path, BT_ADAPTER_IFACE, "Pairable",
                        g_variant_new_boolean (TRUE));
}

void
bt_stop_discovery (GDBusConnection *conn, const gchar *adapter_path)
{
    g_dbus_connection_call (conn, BT_BUS_NAME, adapter_path, BT_ADAPTER_IFACE,
                            "StopDiscovery", NULL, NULL,
                            G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                            on_discovery_done, (gpointer) "StopDiscovery");
}

void
bt_start_discovery_all (GDBusConnection *conn)
{
    GSList *as = bt_get_adapters (conn);
    for (GSList *l = as; l; l = l->next) {
        BtAdapter *a = l->data;
        if (!a->powered)
            continue;

        /* OJO: acá había un `&& !a->discovering` que causaba el peor bug del
         * plugin. La propiedad Discovering se lee de una instantánea que
         * puede estar vieja unos milisegundos, y sobre todo NO refleja si
         * *nosotros* somos los que pedimos el descubrimiento: BlueZ lleva la
         * cuenta por cliente. Si otro programa lo tenía activo, nosotros
         * nunca lo pedíamos y quedábamos sin referencia propia; y en el
         * reinicio del botón Buscar, el StopDiscovery asíncrono todavía no se
         * había procesado, así que Discovering seguía en TRUE y el arranque
         * se salteaba: el botón terminaba APAGANDO la búsqueda.
         * Pedirlo siempre es lo correcto: si ya estaba, BlueZ contesta
         * InProgress y on_discovery_done lo ignora. */
        bt_start_discovery (conn, a->object_path);
    }
    bt_adapter_list_free (as);
}

void
bt_stop_discovery_all (GDBusConnection *conn)
{
    GSList *as = bt_get_adapters (conn);
    for (GSList *l = as; l; l = l->next) {
        BtAdapter *a = l->data;

        /* OJO: acá había un `if (a->discovering)`, el mismo error que ya se
         * había corregido en el arranque, pero del lado de la parada. Si se
         * abría y cerraba la ventana rápido, BlueZ todavía no había marcado
         * la búsqueda como activa, la parada se salteaba y NUESTRA búsqueda
         * quedaba corriendo para siempre: BlueZ la mantiene viva mientras el
         * cliente que la pidió siga conectado al bus, y el complemento no se
         * desconecta nunca. Resultado: batería gastada y audio entrecortado
         * sin ninguna pista de por qué.
         * Pedir la parada siempre es lo correcto: si no había búsqueda
         * nuestra, BlueZ contesta "No discovery started" y on_discovery_done
         * lo ignora; si el adaptador está apagado, contesta NotReady, que
         * también se ignora. Y la de otros programas no se toca, porque BlueZ
         * lleva la cuenta por cliente. */
        bt_stop_discovery (conn, a->object_path);
    }
    bt_adapter_list_free (as);
}

/* ================================================================
 * Dispositivos
 * ================================================================ */

static gint
battery_of (GDBusConnection *conn, const gchar *device_path)
{
    /* La mayoría de los aparatos NO implementan org.bluez.Battery1. Sin esta
     * guarda, cada uno de ellos se comía una llamada sincrónica al bus (con
     * su timeout) en cada refresco, y el refresco ocurre con cada RSSI que
     * llega durante el descubrimiento. Con instantánea activa, que la
     * interfaz no figure ya es respuesta suficiente. */
    if (snapshot_active ()) {
        GVariant *props = snapshot_get_props (device_path, BT_BATTERY_IFACE);
        if (!props)
            return -1;
        gint pct = -1;
        GVariant *pv = g_variant_lookup_value (props, "Percentage",
                                               G_VARIANT_TYPE_BYTE);
        if (pv) {
            pct = g_variant_get_byte (pv);
            g_variant_unref (pv);
        }
        g_variant_unref (props);
        return pct;
    }

    GVariant *v = get_property (conn, device_path, BT_BATTERY_IFACE,
                                "Percentage");
    if (!v)
        return -1;
    gint pct = -1;
    if (g_variant_is_of_type (v, G_VARIANT_TYPE_BYTE))
        pct = g_variant_get_byte (v);
    g_variant_unref (v);
    return pct;
}

static BtDevice *
device_from_props (GDBusConnection *conn, const gchar *path, GVariant *props)
{
    BtDevice *d = g_new0 (BtDevice, 1);
    d->object_path  = g_strdup (path);
    d->adapter_path = dict_str (props, "Adapter");
    d->address      = dict_str (props, "Address");

    d->remote_name = dict_str (props, "Name");
    if (d->remote_name && !*d->remote_name) {
        g_free (d->remote_name);
        d->remote_name = NULL;
    }

    d->name = dict_str (props, "Alias");
    if (!d->name || !*d->name) {
        g_free (d->name);
        d->name = g_strdup (d->remote_name);
    }
    /* BlueZ pone Alias = dirección con guiones bajos cuando no hay nombre.
     * En ese caso lo tratamos como "sin nombre" para poder filtrarlo. */
    if (d->name && d->address) {
        gchar *dashed = g_strdup (d->address);
        for (gchar *p = dashed; *p; p++)
            if (*p == ':') *p = '-';
        d->named = (g_ascii_strcasecmp (d->name, dashed) != 0);
        g_free (dashed);
    } else {
        d->named = (d->name != NULL && *d->name != '\0');
    }
    if (!d->name || !*d->name) {
        g_free (d->name);
        d->name  = g_strdup (d->address ? d->address : "?");
        d->named = FALSE;
    }

    d->icon           = dict_str  (props, "Icon");
    d->klass          = dict_u32  (props, "Class", 0);
    d->paired         = dict_bool (props, "Paired",        FALSE);
    d->trusted        = dict_bool (props, "Trusted",       FALSE);
    d->blocked        = dict_bool (props, "Blocked",       FALSE);
    d->connected      = dict_bool (props, "Connected",     FALSE);
    d->legacy_pairing = dict_bool (props, "LegacyPairing", FALSE);

    GVariant *rssi_v = props ? g_variant_lookup_value (props, "RSSI",
                                   G_VARIANT_TYPE_INT16) : NULL;
    if (rssi_v) {
        d->rssi     = g_variant_get_int16 (rssi_v);
        d->has_rssi = TRUE;
        g_variant_unref (rssi_v);
    }

    GVariant *uuids_v = props ? g_variant_lookup_value (props, "UUIDs",
                                    G_VARIANT_TYPE_STRING_ARRAY) : NULL;
    if (uuids_v) {
        d->uuids = g_variant_dup_strv (uuids_v, NULL);
        g_variant_unref (uuids_v);
    }

    d->battery = battery_of (conn, path);
    return d;
}

GSList *
bt_get_devices (GDBusConnection *conn, const gchar *adapter_path)
{
    GSList   *list = NULL;
    GVariant *objs = om_get (conn);
    if (!objs)
        return NULL;

    GVariantIter  iter;
    const gchar  *path;
    GVariant     *ifaces;

    g_variant_iter_init (&iter, objs);
    while (g_variant_iter_next (&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *props = g_variant_lookup_value (ifaces, BT_DEVICE_IFACE,
                                                  G_VARIANT_TYPE ("a{sv}"));
        if (props) {
            BtDevice *d = device_from_props (conn, path, props);
            if (!adapter_path || g_strcmp0 (d->adapter_path, adapter_path) == 0)
                list = g_slist_append (list, d);
            else
                bt_device_free (d);
            g_variant_unref (props);
        }
        g_variant_unref (ifaces);
    }
    g_variant_unref (objs);
    return list;
}

BtDevice *
bt_get_device (GDBusConnection *conn, const gchar *device_path)
{
    GVariant *props = get_all_properties (conn, device_path, BT_DEVICE_IFACE);
    if (!props)
        return NULL;
    BtDevice *d = device_from_props (conn, device_path, props);
    g_variant_unref (props);
    return d;
}

void
bt_device_free (BtDevice *d)
{
    if (!d) return;
    g_free (d->object_path);
    g_free (d->adapter_path);
    g_free (d->address);
    g_free (d->name);
    g_free (d->remote_name);
    g_free (d->icon);
    g_strfreev (d->uuids);
    g_free (d);
}

void
bt_device_list_free (GSList *list)
{
    g_slist_free_full (list, (GDestroyNotify) bt_device_free);
}

gint
bt_rssi_to_quality (gint16 rssi)
{
    /* -100 dBm o menos = 0 %, -40 dBm o más = 100 %. */
    gint q = (gint) ((rssi + 100) * 100 / 60);
    return CLAMP (q, 0, 100);
}

/* Los UUIDs de 16 bits viven embebidos en la base
 * 0000XXXX-0000-1000-8000-00805f9b34fb. */
static guint16
uuid16_of (const gchar *uuid)
{
    if (!uuid || strlen (uuid) != 36)
        return 0;
    if (g_ascii_strncasecmp (uuid + 8, "-0000-1000-8000-00805f9b34fb", 28) != 0)
        return 0;
    if (g_ascii_strncasecmp (uuid, "0000", 4) != 0)
        return 0;
    return (guint16) g_ascii_strtoull (uuid + 4, NULL, 16);
}

guint16
bt_uuid16 (const gchar *uuid)
{
    return uuid16_of (uuid);
}

gchar *
bt_profiles_string (gchar **uuids)
{
    if (!uuids)
        return NULL;

    GPtrArray *names = g_ptr_array_new ();
    gboolean audio = FALSE, hfp = FALSE, hid = FALSE, net = FALSE;
    gboolean xfer = FALSE, phone = FALSE, remote = FALSE;

    for (gchar **u = uuids; *u; u++) {
        switch (uuid16_of (*u)) {
            case 0x110A: case 0x110B: case 0x110D: audio  = TRUE; break;
            case 0x1108: case 0x111E: case 0x111F: hfp    = TRUE; break;
            case 0x110C: case 0x110E: case 0x110F: remote = TRUE; break;
            case 0x1124:                           hid    = TRUE; break;
            case 0x1115: case 0x1116: case 0x1117: net    = TRUE; break;
            case 0x1105: case 0x1106:              xfer   = TRUE; break;
            case 0x112F: case 0x1132: case 0x1130: phone  = TRUE; break;
            default: break;
        }
    }

    if (audio)  g_ptr_array_add (names, (gpointer) _("Audio"));
    if (hfp)    g_ptr_array_add (names, (gpointer) _("Hands-free"));
    if (remote) g_ptr_array_add (names, (gpointer) _("Remote control"));
    if (hid)    g_ptr_array_add (names, (gpointer) _("Keyboard/Mouse"));
    if (net)    g_ptr_array_add (names, (gpointer) _("Network"));
    if (xfer)   g_ptr_array_add (names, (gpointer) _("File transfer"));
    if (phone)  g_ptr_array_add (names, (gpointer) _("Contacts/Messages"));

    gchar *out = NULL;
    if (names->len > 0) {
        g_ptr_array_add (names, NULL);
        out = g_strjoinv (", ", (gchar **) names->pdata);
    }
    g_ptr_array_free (names, TRUE);
    return out;
}

/* Ícono fino para la clase "audio y video", según la subcategoría (bits
 * 2 a 7 de la clase). BlueZ solo distingue auriculares y cámaras: todo lo
 * demás (parlantes, micrófonos, equipos de música, audio de auto, pantallas)
 * lo manda a "audio-card", que varios temas, Papirus entre ellos, dibujan
 * como una placa de sonido interna. Los números son los de la norma de
 * Bluetooth (Assigned Numbers, clase mayor 0x04). Si el tema no tiene el
 * nombre fino, make_device_icon cae en "audio-card" igual que antes. */
static const gchar *
av_icon_from_class (guint32 klass)
{
    switch ((klass >> 2) & 0x3F) {
        case 0x01:              /* auriculares con micrófono */
        case 0x02:              /* manos libres */
            return "audio-headset";
        case 0x04:              /* micrófono */
            return "audio-input-microphone";
        case 0x05:              /* parlante */
        case 0x07:              /* audio portátil */
        case 0x08:              /* audio de auto */
        case 0x0A:              /* equipo de música */
            return "audio-speakers";
        case 0x06:              /* auriculares */
            return "audio-headphones";
        case 0x0B:              /* videograbadora */
        case 0x0C:              /* cámara de video */
        case 0x0D:              /* cámara con grabadora */
            return "camera-video";
        case 0x0E:              /* monitor de video */
        case 0x0F:              /* pantalla con parlante (televisor) */
            return "video-display";
        default:
            return "audio-card";
    }
}

const gchar *
bt_device_icon_name (const BtDevice *d)
{
    /* "audio-card" es el comodín de BlueZ para audio y video: se afina con
     * la subcategoría, que el aparato sí informa (ver av_icon_from_class). */
    if (d && d->icon && strcmp (d->icon, "audio-card") == 0 &&
        ((d->klass >> 8) & 0x1F) == 0x04)
        return av_icon_from_class (d->klass);

    if (d && d->icon && *d->icon)
        return d->icon;

    /* Respaldo por Class of Device: bits 8-12 son la clase mayor. */
    if (d) {
        guint major = (d->klass >> 8) & 0x1F;
        switch (major) {
            case 0x01: return "computer";
            case 0x02: return "phone";
            case 0x03: return "network-wireless";
            case 0x04: return av_icon_from_class (d->klass);
            case 0x05: return "input-mouse";
            case 0x06: return "camera-photo";
            case 0x07: return "video-display";
            default:   break;
        }
    }
    return "bluetooth";
}

const gchar *const *
bt_status_icon_names (gboolean available, gboolean powered, gint n_connected)
{
    /* Tres estados reales, cada uno con su propia cadena honesta de
     * respaldo. El error que tenían plugin.c y popup-sections.c antes de
     * este cambio era compartir una sola lista con "bluetooth-active-symbolic"
     * fijo como relleno para los tres casos: si el tema no tenía el ícono
     * específico del estado pedido, cualquier estado terminaba mostrando el
     * de "prendido", así que en la práctica el ícono parecía no cambiar
     * nunca. Además "conectado" pedía el mismo nombre que "prendido sin
     * nada conectado" (bluetooth-active-symbolic): varios temas (Papirus,
     * Zorin, Adwaita) tienen un ícono aparte para eso, bluetooth-paired. */
    static const gchar *disabled_names[] = {
        "bluetooth-disabled-symbolic", "bluetooth-disabled",
        "bluetooth-hardware-disabled-symbolic",
        "bluetooth-symbolic", "bluetooth", NULL
    };
    static const gchar *connected_names[] = {
        "bluetooth-paired-symbolic", "bluetooth-paired",
        "bluetooth-active-symbolic", "bluetooth-active",
        "bluetooth-symbolic", "bluetooth", NULL
    };
    static const gchar *active_names[] = {
        "bluetooth-active-symbolic", "bluetooth-active",
        "bluetooth-symbolic", "bluetooth", NULL
    };

    if (!available || !powered)
        return disabled_names;
    if (n_connected > 0)
        return connected_names;
    return active_names;
}

BtStatus *
bt_get_status (GDBusConnection *conn)
{
    BtStatus *s = g_new0 (BtStatus, 1);

    GSList *as = bt_get_adapters (conn);
    s->available = (as != NULL);
    for (GSList *l = as; l; l = l->next) {
        BtAdapter *a = l->data;
        if (a->powered)     s->powered     = TRUE;
        if (a->discovering) s->discovering = TRUE;
    }
    bt_adapter_list_free (as);

    if (!s->powered)
        return s;

    GSList *ds = bt_get_devices (conn, NULL);
    for (GSList *l = ds; l; l = l->next) {
        BtDevice *d = l->data;
        if (!d->connected)
            continue;
        s->n_connected++;
        if (!s->first_name) {
            s->first_name = g_strdup (d->name);
            s->first_icon = g_strdup (bt_device_icon_name (d));
        }
    }
    bt_device_list_free (ds);
    return s;
}

void
bt_status_free (BtStatus *s)
{
    if (!s) return;
    g_free (s->first_name);
    g_free (s->first_icon);
    g_free (s);
}

/* ================================================================
 * Acciones asíncronas con resultado
 * ================================================================ */

typedef struct {
    BtOpCallback cb;
    gpointer     user_data;
} CallData;

static void
on_op_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
    CallData *cd  = user_data;
    GError   *err = NULL;
    GVariant *r   = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src),
                                                   res, &err);
    if (r) {
        g_variant_unref (r);
        cd->cb (TRUE, NULL, NULL, cd->user_data);
    } else {
        gchar *remote = g_dbus_error_get_remote_error (err);
        g_dbus_error_strip_remote_error (err);
        cd->cb (FALSE, remote, err ? err->message : NULL, cd->user_data);
        g_free (remote);
        if (err) g_error_free (err);
    }
    g_free (cd);
}

/* Emparejar puede tardar lo que tarde el usuario en tipear un PIN en el otro
 * aparato, y Connect a veces se cuelga esperando el enlace: el timeout por
 * defecto de D-Bus (25 s) es demasiado corto. */
#define BT_CALL_TIMEOUT_MS 120000

static void
call_device_method (GDBusConnection *conn, const gchar *device_path,
                    const gchar *method, GCancellable *cancellable,
                    BtOpCallback cb, gpointer user_data)
{
    CallData *cd = g_new0 (CallData, 1);
    cd->cb        = cb;
    cd->user_data = user_data;

    g_dbus_connection_call (conn, BT_BUS_NAME, device_path, BT_DEVICE_IFACE,
                            method, NULL, NULL,
                            G_DBUS_CALL_FLAGS_NONE, BT_CALL_TIMEOUT_MS,
                            cancellable, on_op_done, cd);
}

void
bt_device_connect_async (GDBusConnection *conn, const gchar *device_path,
                         GCancellable *cancellable,
                         BtOpCallback cb, gpointer user_data)
{
    call_device_method (conn, device_path, "Connect", cancellable, cb, user_data);
}

void
bt_device_disconnect_async (GDBusConnection *conn, const gchar *device_path,
                            GCancellable *cancellable,
                            BtOpCallback cb, gpointer user_data)
{
    call_device_method (conn, device_path, "Disconnect", cancellable, cb, user_data);
}

void
bt_device_pair_async (GDBusConnection *conn, const gchar *device_path,
                      GCancellable *cancellable,
                      BtOpCallback cb, gpointer user_data)
{
    call_device_method (conn, device_path, "Pair", cancellable, cb, user_data);
}

void
bt_device_remove_async (GDBusConnection *conn, const gchar *adapter_path,
                        const gchar *device_path, GCancellable *cancellable,
                        BtOpCallback cb, gpointer user_data)
{
    CallData *cd = g_new0 (CallData, 1);
    cd->cb        = cb;
    cd->user_data = user_data;

    g_dbus_connection_call (conn, BT_BUS_NAME, adapter_path, BT_ADAPTER_IFACE,
                            "RemoveDevice",
                            g_variant_new ("(o)", device_path),
                            NULL, G_DBUS_CALL_FLAGS_NONE, 10000,
                            cancellable, on_op_done, cd);
}

/* ---------- perfiles puntuales ---------- */

static void
call_profile_method (GDBusConnection *conn, const gchar *device_path,
                     const gchar *method, const gchar *uuid,
                     GCancellable *cancellable,
                     BtOpCallback cb, gpointer user_data)
{
    CallData *cd = g_new0 (CallData, 1);
    cd->cb        = cb;
    cd->user_data = user_data;

    g_dbus_connection_call (conn, BT_BUS_NAME, device_path, BT_DEVICE_IFACE,
                            method, g_variant_new ("(s)", uuid ? uuid : ""),
                            NULL, G_DBUS_CALL_FLAGS_NONE, BT_CALL_TIMEOUT_MS,
                            cancellable, on_op_done, cd);
}

void
bt_device_connect_profile_async (GDBusConnection *conn,
                                 const gchar *device_path, const gchar *uuid,
                                 GCancellable *cancellable,
                                 BtOpCallback cb, gpointer user_data)
{
    call_profile_method (conn, device_path, "ConnectProfile", uuid,
                         cancellable, cb, user_data);
}

void
bt_device_disconnect_profile_async (GDBusConnection *conn,
                                    const gchar *device_path,
                                    const gchar *uuid,
                                    GCancellable *cancellable,
                                    BtOpCallback cb, gpointer user_data)
{
    call_profile_method (conn, device_path, "DisconnectProfile", uuid,
                         cancellable, cb, user_data);
}

/* ---------- reconectar ---------- */

typedef struct {
    GDBusConnection *conn;        /* referencia propia: la secuencia puede
                                   * sobrevivir a la ventana que la pidió */
    gchar           *device_path;
    BtOpCallback     cb;
    gpointer         user_data;
} ReconnectData;

static void
reconnect_data_free (ReconnectData *rd)
{
    g_object_unref (rd->conn);
    g_free (rd->device_path);
    g_free (rd);
}

static void
on_reconnect_connected (gboolean success, const gchar *error_name,
                        const gchar *error_message, gpointer user_data)
{
    ReconnectData *rd = user_data;
    rd->cb (success, error_name, error_message, rd->user_data);
    reconnect_data_free (rd);
}

static gboolean
on_reconnect_pause_done (gpointer user_data)
{
    ReconnectData *rd = user_data;
    call_device_method (rd->conn, rd->device_path, "Connect", NULL,
                        on_reconnect_connected, rd);
    return G_SOURCE_REMOVE;
}

static void
on_reconnect_disconnected (gboolean success, const gchar *error_name,
                           const gchar *error_message, gpointer user_data)
{
    ReconnectData *rd = user_data;

    /* "No estaba conectado" no impide el paso siguiente: el usuario quiere
     * terminar conectado, y ya está a mitad de camino. */
    gboolean not_connected = error_name &&
                             g_str_has_suffix (error_name, ".NotConnected");

    if (!success && !not_connected) {
        rd->cb (FALSE, error_name, error_message, rd->user_data);
        reconnect_data_free (rd);
        return;
    }

    /* La pausa no es decorativa: muchos auriculares necesitan un instante
     * para soltar el enlace del todo; reconectar en el acto suele fallar
     * con "en curso" o dejar el mismo problema. */
    g_timeout_add (BT_RECONNECT_PAUSE_MS, on_reconnect_pause_done, rd);
}

void
bt_device_reconnect_async (GDBusConnection *conn, const gchar *device_path,
                           BtOpCallback cb, gpointer user_data)
{
    ReconnectData *rd = g_new0 (ReconnectData, 1);
    rd->conn        = g_object_ref (conn);
    rd->device_path = g_strdup (device_path);
    rd->cb          = cb;
    rd->user_data   = user_data;

    call_device_method (conn, device_path, "Disconnect", NULL,
                        on_reconnect_disconnected, rd);
}

void
bt_device_cancel_pairing (GDBusConnection *conn, const gchar *device_path)
{
    g_dbus_connection_call (conn, BT_BUS_NAME, device_path, BT_DEVICE_IFACE,
                            "CancelPairing", NULL, NULL,
                            G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                            on_fire_and_forget_done, (gpointer) "CancelPairing");
}

void
bt_device_set_trusted (GDBusConnection *conn, const gchar *device_path,
                       gboolean trusted)
{
    set_property_async (conn, device_path, BT_DEVICE_IFACE, "Trusted",
                        g_variant_new_boolean (trusted));
}

void
bt_device_set_blocked (GDBusConnection *conn, const gchar *device_path,
                       gboolean blocked)
{
    set_property_async (conn, device_path, BT_DEVICE_IFACE, "Blocked",
                        g_variant_new_boolean (blocked));
}

void
bt_device_set_alias (GDBusConnection *conn, const gchar *device_path,
                     const gchar *alias)
{
    /* BlueZ exige el tipo exacto 's': cualquier otro lo rechaza con
     * InvalidArguments. Vacío = volver al nombre de fábrica. */
    set_property_async (conn, device_path, BT_DEVICE_IFACE, "Alias",
                        g_variant_new_string (alias != NULL ? alias : ""));
}

const gchar *
bt_error_to_message (const gchar *error_name, const gchar *error_message)
{
    if (error_name) {
        const gchar *tail = strrchr (error_name, '.');
        tail = tail ? tail + 1 : error_name;

        if (g_strcmp0 (tail, "AuthenticationFailed") == 0)
            return _("Pairing failed: the code did not match.");
        if (g_strcmp0 (tail, "AuthenticationCanceled") == 0)
            return _("Pairing was cancelled.");
        if (g_strcmp0 (tail, "AuthenticationRejected") == 0)
            return _("The device rejected the pairing.");
        if (g_strcmp0 (tail, "AuthenticationTimeout") == 0)
            return _("The device did not answer in time.");
        if (g_strcmp0 (tail, "ConnectionAttemptFailed") == 0)
            return _("Could not connect. Make sure the device is on and nearby.");
        if (g_strcmp0 (tail, "AlreadyExists") == 0)
            return _("The device is already paired.");
        if (g_strcmp0 (tail, "AlreadyConnected") == 0)
            return _("The device is already connected.");
        if (g_strcmp0 (tail, "InProgress") == 0)
            return _("Another operation is already running on this device.");
        if (g_strcmp0 (tail, "NotReady") == 0)
            return _("The adapter is not ready.");
        if (g_strcmp0 (tail, "NotAvailable") == 0 ||
            g_strcmp0 (tail, "NotSupported") == 0)
            return _("The device does not support this operation.");
        if (g_strcmp0 (tail, "Failed") == 0)
            return _("The operation failed. Try again.");
    }

    if (error_message && *error_message)
        return error_message;
    return _("The operation failed.");
}

/* ================================================================
 * Suscripción a señales
 * ================================================================ */

typedef struct {
    BtSignalCallback callback;
    gpointer         user_data;
} SignalData;

static void
on_bt_signal (GDBusConnection *conn,
              const gchar     *sender,
              const gchar     *object_path,
              const gchar     *iface,
              const gchar     *signal_name,
              GVariant        *params,
              gpointer         user_data)
{
    (void) conn; (void) sender; (void) object_path;
    (void) iface; (void) signal_name; (void) params;

    SignalData *sd = user_data;
    sd->callback (sd->user_data);
}

guint *
bt_subscribe_signals (GDBusConnection *conn,
                      BtSignalCallback callback,
                      gpointer         user_data)
{
    guint      *ids = g_new0 (guint, 16);
    gint        n   = 0;

    SignalData *sd  = g_new0 (SignalData, 1);
    sd->callback  = callback;
    sd->user_data = user_data;

    /* 1. Altas de objetos: un dispositivo nuevo apareció en el descubrimiento,
     *    o se enchufó un adaptador USB. */
    ids[n++] = g_dbus_connection_signal_subscribe (
        conn, BT_BUS_NAME, "org.freedesktop.DBus.ObjectManager",
        "InterfacesAdded", BT_ROOT_PATH, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_bt_signal, sd, NULL);

    /* 2. Bajas: BlueZ descarta los dispositivos "temporales" que dejó de ver. */
    ids[n++] = g_dbus_connection_signal_subscribe (
        conn, BT_BUS_NAME, "org.freedesktop.DBus.ObjectManager",
        "InterfacesRemoved", BT_ROOT_PATH, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_bt_signal, sd, NULL);

    /* 3. Cambios en cualquier dispositivo (Connected, Paired, RSSI…).
     *    Sin filtrar por object_path, igual que hacía el plugin de red. */
    ids[n++] = g_dbus_connection_signal_subscribe (
        conn, BT_BUS_NAME, "org.freedesktop.DBus.Properties",
        "PropertiesChanged", NULL, BT_DEVICE_IFACE,
        G_DBUS_SIGNAL_FLAGS_NONE, on_bt_signal, sd, NULL);

    /* 4. Cambios en cualquier adaptador (Powered, Discovering, Discoverable). */
    ids[n++] = g_dbus_connection_signal_subscribe (
        conn, BT_BUS_NAME, "org.freedesktop.DBus.Properties",
        "PropertiesChanged", NULL, BT_ADAPTER_IFACE,
        G_DBUS_SIGNAL_FLAGS_NONE, on_bt_signal, sd, NULL);

    /* 5. Nivel de batería del dispositivo. */
    ids[n++] = g_dbus_connection_signal_subscribe (
        conn, BT_BUS_NAME, "org.freedesktop.DBus.Properties",
        "PropertiesChanged", NULL, BT_BATTERY_IFACE,
        G_DBUS_SIGNAL_FLAGS_NONE, on_bt_signal, sd, NULL);

    /* 6. Arranque/parada del servicio: si bluetoothd se reinicia queremos
     *    enterarnos para volver a dibujar (y para re-registrar el agente). */
    ids[n++] = g_dbus_connection_signal_subscribe (
        conn, "org.freedesktop.DBus", "org.freedesktop.DBus",
        "NameOwnerChanged", "/org/freedesktop/DBus", BT_BUS_NAME,
        G_DBUS_SIGNAL_FLAGS_NONE, on_bt_signal, sd, NULL);

    /* Guardamos sd indexado por el puntero al array para que dos llamadores
     * (el plugin y el popup) no se pisen. */
    gchar *key = g_strdup_printf ("bt-signal-data-%p", (void *) ids);
    g_object_set_data_full (G_OBJECT (conn), key, sd, g_free);
    g_free (key);

    return ids;
}

void
bt_unsubscribe_signals (GDBusConnection *conn, guint *ids)
{
    if (!ids) return;
    for (gint i = 0; ids[i] != 0; i++)
        g_dbus_connection_signal_unsubscribe (conn, ids[i]);

    gchar *key = g_strdup_printf ("bt-signal-data-%p", (void *) ids);
    g_object_set_data (G_OBJECT (conn), key, NULL);
    g_free (key);

    g_free (ids);
}
