/* bt-obex-agent.h — agente de recepción de archivos (org.bluez.obex.Agent1).
 *
 * Mismo espíritu que bt-agent.h, y por el mismo motivo: exportamos un objeto
 * y ES EL SERVICIO QUIEN NOS LLAMA cuando un aparato quiere mandarnos algo.
 * Por eso vive en el plugin y no en la ventana emergente: un teléfono manda
 * una foto cuando se le canta, con el panel cerrado.
 *
 * Diferencia con el agente de pareo: aquel se registra en org.bluez, en el bus
 * del SISTEMA. Este se registra en org.bluez.obex, en el bus de SESIÓN.
 *
 * EL CONFLICTO, que es la parte importante:
 *
 * En todo el sistema puede haber UN SOLO agente de recepción registrado a la
 * vez. Si Blueman (o cualquier otro) ya se quedó con el puesto, nuestro
 * registro falla y no hay forma de pelearlo. Lo que sí se puede es:
 *
 *   - detectarlo con nombre y apellido, en vez de dejar al usuario
 *     preguntándose por qué no le llega nada;
 *   - reintentar solo, cada tanto, para que el día que el usuario cierre el
 *     otro programa esto empiece a andar sin que tenga que tocar nada.
 *
 * Eso es exactamente lo que hace este módulo. El estado se puede consultar
 * en cualquier momento para mostrarlo en la ventana de propiedades.
 */
#ifndef BT_OBEX_AGENT_H
#define BT_OBEX_AGENT_H

#include <gio/gio.h>
#include <glib.h>

typedef struct _BtObexAgent BtObexAgent;

typedef enum {
    /* Somos el receptor: los archivos que manden van a llegar. */
    BT_OBEX_RECV_REGISTERED,

    /* Otro programa se quedó con el puesto. Reintentamos solos. */
    BT_OBEX_RECV_TAKEN,

    /* El servicio de transferencia no está disponible todavía.
     * También se reintenta. */
    BT_OBEX_RECV_UNAVAILABLE
} BtObexRecvStatus;

/* Crea el agente y arranca el registro.
 *
 * system_conn es la conexión al bus del sistema del plugin, PRESTADA (no se
 * libera acá). Se usa solo para traducir la dirección del aparato que manda
 * en un nombre legible; si es NULL, se muestra la dirección cruda.
 *
 * folder es la carpeta de destino. show_notifications enciende el aviso de
 * escritorio al terminar cada archivo.
 *
 * Devuelve NULL solo si no se pudo exportar el objeto en el bus de sesión.
 * Que el registro falle NO es motivo de NULL: el objeto queda vivo,
 * reintentando, y el estado se consulta con bt_obex_agent_status(). */
BtObexAgent *bt_obex_agent_new (GDBusConnection *system_conn,
                                const gchar     *folder,
                                gboolean         show_notifications);

/* Se da de baja como receptor y libera todo. Las transferencias que estén a
 * mitad de camino se dejan de seguir, pero no se cortan. */
void bt_obex_agent_free (BtObexAgent *agent);

/* Cambios en caliente desde la ventana de propiedades. */
void bt_obex_agent_set_folder        (BtObexAgent *agent, const gchar *folder);
void bt_obex_agent_set_notifications (BtObexAgent *agent, gboolean show);

/* Estado actual del registro. */
BtObexRecvStatus bt_obex_agent_status (BtObexAgent *agent);

/* Frase corta y ya traducida para mostrar el estado. Acepta NULL como
 * "la recepción está apagada". Cadena estática; no liberar. */
const gchar *bt_obex_recv_status_message (BtObexAgent *agent);

#endif /* BT_OBEX_AGENT_H */
