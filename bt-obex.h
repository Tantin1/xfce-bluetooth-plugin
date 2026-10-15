/* bt-obex.h — capa de transferencia de archivos por OBEX.
 *
 * OJO, DIFERENCIA DE FONDO CON EL RESTO DEL PLUGIN: todo lo demás (bt-dbus.c,
 * bt-agent.c) habla con org.bluez en el bus del SISTEMA. El servicio de
 * transferencia, org.bluez.obex, vive en el bus de SESIÓN y corre como el
 * usuario, no como root. Por eso este módulo pide su propia conexión al bus
 * en vez de recibir la que ya tiene el plugin: pasarle la del sistema no
 * funcionaría.
 *
 * Etapa 1 (esta): solo detección. Saber si el sistema puede transferir
 * archivos, y si no puede, poder decir exactamente por qué y qué hacer.
 * Sobre esto se apoyan después el envío y la recepción.
 *
 * Motivo de que la detección sea un módulo aparte y no una línea suelta:
 * el demonio obexd viene en un paquete separado del de BlueZ (desde 2024
 * también en Arch), y aun instalado puede tener su servicio de usuario sin
 * habilitar. Son dos fallas distintas con dos soluciones distintas, y el
 * usuario no tiene forma de adivinar cuál le tocó si solo le decimos
 * "no se pudo".
 */
#ifndef BT_OBEX_H
#define BT_OBEX_H

#include <glib.h>

#define BT_OBEX_BUS_NAME    "org.bluez.obex"
#define BT_OBEX_ROOT_PATH   "/org/bluez/obex"
#define BT_OBEX_CLIENT_IFACE "org.bluez.obex.Client1"

typedef enum {
    /* El servicio está corriendo, o no está corriendo pero el bus lo puede
     * arrancar solo en cuanto le hablemos. Para el usuario, listo. */
    BT_OBEX_READY,

    /* El demonio está instalado en el disco pero el bus de sesión no lo
     * puede activar: falta habilitar su servicio de usuario. */
    BT_OBEX_NOT_ENABLED,

    /* No hay demonio: falta instalar el paquete. */
    BT_OBEX_NOT_INSTALLED,

    /* No se pudo hablar con el bus de sesión. Raro; sesión sin D-Bus. */
    BT_OBEX_NO_SESSION_BUS
} BtObexState;

/* Averigua el estado actual. Barata: son dos consultas al demonio del bus
 * de sesión (que siempre está ahí, no hay que activar nada) más, a lo sumo,
 * unos g_file_test. NO arranca el servicio ni lo activa. Se puede llamar
 * cada vez que haga falta; no cachea a propósito, así detecta al toque si
 * el usuario instaló el paquete con el panel abierto. */
BtObexState bt_obex_check (void);

/* Frase corta para mostrar como estado. Cadena estática ya traducida;
 * no liberar. */
const gchar *bt_obex_state_message (BtObexState state);

/* Qué tiene que hacer el usuario para arreglarlo, con el nombre de paquete
 * que corresponde a su distribución. Devuelve NULL si no hay nada que
 * arreglar. Liberar con g_free(). */
gchar *bt_obex_state_hint (BtObexState state);

/* Nombre del paquete del demonio de transferencia según la distribución
 * detectada en /etc/os-release ("bluez-obex" en Arch y derivadas,
 * "bluez-obexd" en el resto). Cadena estática; no liberar. */
const gchar *bt_obex_package_name (void);

/* ============================================================
 * Recepción — enlaces simbólicos en la ruta de destino
 *
 * obexd se niega a escribir si algún tramo de la carpeta de destino es un
 * enlace simbólico. Es una limitación de obexd, no del complemento, pero el
 * complemento sí puede detectarla de antemano y ofrecer el arreglo con un
 * clic en vez de dejar que la transferencia falle en silencio.
 * ============================================================ */

/* Recorre folder tramo por tramo (con lstat, sin seguir enlaces) buscando
 * un enlace simbólico en el camino. Alcanza con uno solo en cualquier lugar
 * de la ruta para que obexd rechace escribir ahí.
 *
 * Devuelve la ruta del primer tramo que resultó ser un enlace, para
 * mostrarla en el aviso. NULL si no hay ninguno, o si algún tramo todavía
 * no existe (nada que revisar ahí todavía). Liberar con g_free(). */
gchar *bt_obex_find_symlink_in_path (const gchar *folder);

/* TRUE si ya se instaló el archivo de sustitución que agrega --symlinks al
 * arranque del servicio. Es una lectura barata del archivo: no confirma que
 * el servicio ya esté CORRIENDO con esa opción, solo que el arreglo ya se
 * aplicó (para eso está bt_obex_install_symlinks_override, que además
 * reinicia el servicio). Sirve para no repetir el aviso una vez resuelto. */
gboolean bt_obex_symlinks_override_installed (void);

/* Instala el archivo de sustitución de systemd --user que agrega
 * --symlinks al arranque de obexd, y reinicia el servicio para que tome
 * efecto ya mismo. No hace falta contraseña ni privilegios: obex.service es
 * un servicio de USUARIO, no del sistema.
 *
 * Devuelve FALSE y llena error si algún paso falla (no se pudo escribir el
 * archivo, o systemctl devolvió error). Si falla en el reinicio, el archivo
 * ya quedó bien escrito, así que un segundo intento no lo vuelve a tocar,
 * solo reintenta la parte de systemctl. */
gboolean bt_obex_install_symlinks_override (GError **error);

/* ============================================================
 * Envío de archivos
 * ============================================================ */

/* TRUE si el aparato anuncia el perfil de recepción de archivos (Object
 * Push, UUID 1105). Un mouse o unos auriculares no lo anuncian, así que la
 * acción de enviar no debe ni aparecer para ellos.
 *
 * Limitación conocida: varios teléfonos publican este perfil recién después
 * de emparejarse, así que la acción puede tardar en aparecer. Es preferible
 * a mostrarla siempre y que falle. */
gboolean bt_obex_device_can_receive (gchar **uuids);

/* Un envío en curso. Opaco: la interfaz solo guarda el puntero para poder
 * cancelar, y lo tiene que olvidar en cuanto le llega el aviso de fin. */
typedef struct _BtObexSend BtObexSend;

/* Progreso. size vale 0 mientras el otro extremo no informó el tamaño. */
typedef void (*BtObexProgressCb) (guint64  transferred,
                                  guint64  size,
                                  gpointer user_data);

/* Fin del envío. Se llama SIEMPRE y exactamente una vez, tanto si salió bien
 * como si falló o se canceló. Cuando vuelve, el BtObexSend ya no existe:
 * quien lo tenía guardado debe poner su puntero en NULL acá adentro. */
typedef void (*BtObexDoneCb) (gboolean     success,
                              const gchar *message,
                              gpointer     user_data);

/* Arranca el envío. Devuelve NULL solo si no se pudo ni hablar con el bus de
 * sesión (en ese caso el aviso de fin NO se llama, porque nunca empezó).
 *
 * La secuencia real es de dos pasos encadenados: primero se abre una sesión
 * con el aparato, después se le ofrece el archivo. Recién ahí el otro extremo
 * decide si acepta. Por eso una transferencia puede quedarse un buen rato en
 * "esperando": el usuario del otro aparato todavía no tocó "aceptar". */
BtObexSend *bt_obex_send_file (const gchar     *address,
                               const gchar     *filename,
                               BtObexProgressCb progress_cb,
                               BtObexDoneCb     done_cb,
                               gpointer         user_data);

/* Cancela. El aviso de fin llega igual (con success = FALSE), así que quien
 * llama no tiene que limpiar nada por su cuenta. Es seguro llamarla aunque el
 * envío ya haya terminado si el puntero todavía no se puso en NULL. */
void bt_obex_send_cancel (BtObexSend *send);

/* ============================================================
 * Utilidad compartida
 * ============================================================ */

/* Arma una ruta de destino que no pise nada. Si "foto.jpg" ya existe en la
 * carpeta, devuelve "foto (1).jpg", y así.
 *
 * SEGURIDAD: el nombre se reduce siempre a su última parte antes de usarlo.
 * El nombre lo elige el aparato que manda, y sin esa poda un aparato hostil
 * podría mandar algo como "../../.bashrc" y escribir fuera de la carpeta
 * elegida. No sacar esa poda.
 *
 * Liberar con g_free(). */
gchar *bt_obex_unique_path (const gchar *folder, const gchar *name);

#endif /* BT_OBEX_H */
