/* bt-dbus.h — capa de acceso a BlueZ por D-Bus.
 *
 * Misma filosofía que nm-dbus.h del plugin de red: structs planos, listas
 * GSList, lecturas sincrónicas baratas (resueltas desde una instantánea) y
 * acciones asíncronas que no bloquean el hilo principal.
 *
 * Diferencia importante con NetworkManager: BlueZ SÍ contesta si una acción
 * falló. Connect/Pair/Disconnect devuelven éxito o un error tipado, así que
 * las acciones de este módulo llevan callback de resultado en vez de
 * "mandar y adivinar después mirando el estado del dispositivo".
 */
#ifndef BT_DBUS_H
#define BT_DBUS_H

#include <gio/gio.h>

#define BT_BUS_NAME         "org.bluez"
#define BT_ROOT_PATH        "/"
#define BT_ADAPTER_IFACE    "org.bluez.Adapter1"
#define BT_DEVICE_IFACE     "org.bluez.Device1"
#define BT_BATTERY_IFACE    "org.bluez.Battery1"
#define BT_AGENT_MGR_IFACE  "org.bluez.AgentManager1"
#define BT_AGENT_MGR_PATH   "/org/bluez"
#define BT_AGENT_IFACE      "org.bluez.Agent1"

/* ============================================================
 * Tipos
 * ============================================================ */

typedef struct {
    gchar    *object_path;   /* /org/bluez/hci0 */
    gchar    *address;       /* MAC del adaptador */
    gchar    *name;          /* Alias (editable) o Name */
    gboolean  powered;
    gboolean  discovering;
    gboolean  discoverable;
    gboolean  pairable;
} BtAdapter;

typedef struct {
    gchar    *object_path;   /* /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF */
    gchar    *adapter_path;
    gchar    *address;       /* AA:BB:CC:DD:EE:FF — clave estable de la fila */
    gchar    *name;          /* Alias, o Name, o la dirección si no hay nombre */
    gboolean  named;         /* FALSE si tuvimos que caer a la dirección */
    gchar    *remote_name;   /* Name: el nombre de fábrica que anuncia el
                              * aparato, sin el alias. NULL si no anuncia. */
    gchar    *icon;          /* propiedad Icon: "audio-headset", "input-mouse"… */
    guint32   klass;         /* Class of Device */
    gint16    rssi;
    gboolean  has_rssi;      /* RSSI solo existe si el aparato se vio recién */
    gboolean  paired;
    gboolean  trusted;
    gboolean  blocked;
    gboolean  connected;
    gboolean  legacy_pairing;
    gint      battery;       /* 0-100, o -1 si el aparato no lo informa */
    gchar   **uuids;         /* perfiles anunciados, terminado en NULL */
} BtDevice;

/* Resumen para el ícono y el tooltip del panel: una sola lectura. */
typedef struct {
    gboolean  available;     /* hay al menos un adaptador Bluetooth */
    gboolean  powered;       /* al menos un adaptador encendido */
    gboolean  discovering;
    gint      n_connected;
    gchar    *first_name;    /* nombre del primer dispositivo conectado */
    gchar    *first_icon;    /* su ícono */
} BtStatus;

/* ============================================================
 * Conexión e instantánea
 * ============================================================ */

GDBusConnection *bt_dbus_connect (void);

/* Instantánea (ObjectManager): bt_cache_begin trae TODOS los objetos de
 * BlueZ en una sola llamada al bus; mientras está activa, las consultas de
 * este módulo resuelven desde memoria. bt_cache_end la libera. Anidable.
 * Usar solo alrededor de bloques cortos de lectura (armar/refrescar la UI).
 *
 * OJO: BlueZ expone el ObjectManager en "/" (NetworkManager lo hacía en
 * "/org/freedesktop"). Es la única diferencia de fondo con el plugin de red. */
void bt_cache_begin (GDBusConnection *conn);
void bt_cache_end   (void);

/* TRUE si el servicio org.bluez está presente en el bus del sistema. */
gboolean bt_service_available (GDBusConnection *conn);

/* ============================================================
 * Adaptadores
 * ============================================================ */

GSList *bt_get_adapters      (GDBusConnection *conn);
void    bt_adapter_free      (BtAdapter *a);
void    bt_adapter_list_free (GSList *list);

gboolean bt_any_adapter_powered      (GDBusConnection *conn);
gboolean bt_any_adapter_discovering  (GDBusConnection *conn);
gboolean bt_any_adapter_discoverable (GDBusConnection *conn);

void bt_adapter_set_powered_async      (GDBusConnection *conn,
                                        const gchar *adapter_path,
                                        gboolean powered);

/* Cuánto dura la visibilidad antes de apagarse sola. Antes era "para
 * siempre" (plazo 0): con un olvido, el equipo quedaba anunciándose a
 * cualquiera que pasara cerca. Tres minutos alcanzan de sobra para buscarlo
 * desde el teléfono y emparejar. Cuando el plazo vence, BlueZ avisa por
 * señal y el interruptor de la ventana se acomoda solo.
 *
 * Al encender la visibilidad también se deja el adaptador "emparejable":
 * el que se hace visible es justamente porque espera que el otro aparato
 * inicie el pareo. Al apagarla NO se toca: dejarlo emparejable es el
 * comportamiento normal de BlueZ, y forzarlo en falso rompería cosas que
 * hoy andan (aparatos ya conocidos que vuelven a pedir vinculación). */
#define BT_DISCOVERABLE_TIMEOUT_S 180

void bt_adapter_set_discoverable_async (GDBusConnection *conn,
                                        const gchar *adapter_path,
                                        gboolean discoverable);
void bt_set_all_powered_async          (GDBusConnection *conn, gboolean powered);
void bt_set_all_discoverable_async     (GDBusConnection *conn, gboolean disc);

/* Descubrimiento. A diferencia de RequestScan de NetworkManager esto NO es
 * un disparo único: queda corriendo hasta StopDiscovery. Arrancarlo al abrir
 * el popup y pararlo al cerrarlo (consume batería e interfiere con el audio).
 *
 * bt_stop_discovery_all pide la parada en TODOS los adaptadores, sin mirar
 * si figuran buscando: esa propiedad puede estar vieja y, si se la cree, la
 * búsqueda propia queda viva para siempre. */
void bt_set_discovery_filter (GDBusConnection *conn, const gchar *adapter_path);
void bt_adapter_set_pairable (GDBusConnection *conn, const gchar *adapter_path);
void bt_start_discovery (GDBusConnection *conn, const gchar *adapter_path);
void bt_stop_discovery  (GDBusConnection *conn, const gchar *adapter_path);
void bt_start_discovery_all (GDBusConnection *conn);
void bt_stop_discovery_all  (GDBusConnection *conn);

/* ============================================================
 * Dispositivos
 * ============================================================ */

/* adapter_path == NULL devuelve los dispositivos de todos los adaptadores. */
GSList    *bt_get_devices     (GDBusConnection *conn, const gchar *adapter_path);
BtDevice  *bt_get_device      (GDBusConnection *conn, const gchar *device_path);
void       bt_device_free     (BtDevice *d);
void       bt_device_list_free (GSList *list);

/* Cadena legible con los perfiles derivados de los UUIDs ("Audio, Manos
 * libres, Control remoto"). Liberar con g_free(). NULL si no hay ninguno. */
gchar *bt_profiles_string (gchar **uuids);

/* Número corto (16 bits) de un UUID de perfil estándar, por ejemplo 0x110B
 * para "0000110b-0000-1000-8000-00805f9b34fb". 0 si no es de la base
 * estándar de Bluetooth. */
guint16 bt_uuid16 (const gchar *uuid);

/* Nombre de ícono del tema para el dispositivo, con respaldo por clase. */
const gchar *bt_device_icon_name (const BtDevice *d);

/* Cadena de nombres de ícono (terminada en NULL) para el ícono de ESTADO
 * general de Bluetooth (apagado / prendido / con algo conectado), en orden
 * de preferencia. Compartida entre el ícono del panel y el del popup para
 * que los dos usen la misma lógica de 3 estados. No liberar: son literales
 * estáticos, válidos mientras dure el proceso. */
const gchar *const *bt_status_icon_names (gboolean available, gboolean powered,
                                          gint n_connected);

/* Calidad de señal 0-100 derivada del RSSI en dBm. */
gint bt_rssi_to_quality (gint16 rssi);

BtStatus *bt_get_status  (GDBusConnection *conn);
void      bt_status_free (BtStatus *s);

/* ============================================================
 * ACCIONES ASYNC
 *
 * El callback se invoca siempre, exactamente una vez. En caso de error,
 * error_name trae el nombre D-Bus remoto ("org.bluez.Error.AuthenticationFailed")
 * o NULL si el fallo fue local (por ejemplo, cancelado); error_message trae
 * el texto ya despojado del prefijo remoto.
 * ============================================================ */

typedef void (*BtOpCallback) (gboolean     success,
                              const gchar *error_name,
                              const gchar *error_message,
                              gpointer     user_data);

void bt_device_connect_async    (GDBusConnection *conn, const gchar *device_path,
                                 GCancellable *cancellable,
                                 BtOpCallback cb, gpointer user_data);
void bt_device_disconnect_async (GDBusConnection *conn, const gchar *device_path,
                                 GCancellable *cancellable,
                                 BtOpCallback cb, gpointer user_data);
void bt_device_pair_async       (GDBusConnection *conn, const gchar *device_path,
                                 GCancellable *cancellable,
                                 BtOpCallback cb, gpointer user_data);
void bt_device_remove_async     (GDBusConnection *conn, const gchar *adapter_path,
                                 const gchar *device_path,
                                 GCancellable *cancellable,
                                 BtOpCallback cb, gpointer user_data);

/* Conectar o desconectar UN perfil puntual (uuid = el del aparato remoto,
 * tal como figura en BtDevice.uuids). Ojo con lo que NO hace: no elige
 * entre audio de alta calidad y manos libres para el sonido; eso lo decide
 * el servidor de audio (PipeWire o PulseAudio). Esto solo levanta o baja el
 * enlace de ese perfil. */
void bt_device_connect_profile_async    (GDBusConnection *conn,
                                         const gchar *device_path,
                                         const gchar *uuid,
                                         GCancellable *cancellable,
                                         BtOpCallback cb, gpointer user_data);
void bt_device_disconnect_profile_async (GDBusConnection *conn,
                                         const gchar *device_path,
                                         const gchar *uuid,
                                         GCancellable *cancellable,
                                         BtOpCallback cb, gpointer user_data);

/* Reconectar: desconecta, espera un momento y vuelve a conectar. Es el
 * remedio clásico para el auricular que quedó conectado pero mudo.
 *
 * La secuencia entera vive acá adentro y NO depende de quien la pidió: si
 * la ventana se cierra en el medio, el aparato igual termina reconectado
 * (sin esto podría quedar desconectado justo en la pausa). Por eso no lleva
 * cancelable. El callback llega una sola vez, al final; si la desconexión
 * falla, llega ahí mismo con ese error y no se intenta conectar. */
#define BT_RECONNECT_PAUSE_MS 1500
void bt_device_reconnect_async          (GDBusConnection *conn,
                                         const gchar *device_path,
                                         BtOpCallback cb, gpointer user_data);

void bt_device_cancel_pairing (GDBusConnection *conn, const gchar *device_path);
void bt_device_set_trusted    (GDBusConnection *conn, const gchar *device_path,
                               gboolean trusted);
void bt_device_set_blocked    (GDBusConnection *conn, const gchar *device_path,
                               gboolean blocked);

/* Cambia el nombre con que se muestra el aparato (propiedad Alias, "nombre
 * alternativo"). Lo guarda BlueZ en el disco, así que vale para todos los
 * programas y sobrevive a reinicios. Con alias vacío ("") BlueZ borra el
 * alias y el aparato vuelve a mostrarse con su nombre de fábrica (Name). */
void bt_device_set_alias      (GDBusConnection *conn, const gchar *device_path,
                               const gchar *alias);

/* Traduce un error de BlueZ a un mensaje corto y entendible para la UI.
 * Devuelve una cadena estática ya traducida; no liberar. */
const gchar *bt_error_to_message (const gchar *error_name,
                                  const gchar *error_message);

/* ============================================================
 * Señales
 * ============================================================ */

typedef void (*BtSignalCallback) (gpointer user_data);

guint *bt_subscribe_signals   (GDBusConnection *conn,
                               BtSignalCallback callback,
                               gpointer         user_data);
void   bt_unsubscribe_signals (GDBusConnection *conn, guint *ids);

#endif /* BT_DBUS_H */
