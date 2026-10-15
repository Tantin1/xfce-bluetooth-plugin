/* bt-agent.h — agente de pareo (org.bluez.Agent1).
 *
 * Esto no tiene equivalente en el plugin de red. NetworkManager recibe la
 * contraseña dentro del pedido de conexión; BlueZ invierte el flujo: nosotros
 * exportamos un objeto D-Bus y ES BLUEZ QUIEN NOS LLAMA cuando hace falta
 * confirmar un código, pedir un PIN o autorizar un servicio.
 *
 * Consecuencia de diseño: el agente vive en el plugin, NO en el popup. Un
 * aparato puede iniciar el pareo con el popup cerrado (típico: apretás el
 * botón de sincronismo del teclado). Si lo registráramos en popup_show nos
 * perderíamos esos eventos.
 */
#ifndef BT_AGENT_H
#define BT_AGENT_H

#include <gio/gio.h>
#include <glib.h>

typedef struct _BtAgent BtAgent;

/* Registra el agente en BlueZ y pide ser el agente por omisión.
 * Devuelve NULL si no se pudo exportar el objeto. Que RequestDefaultAgent
 * falle NO es motivo de NULL: si blueman o gnome-bluetooth ya se quedaron
 * con el puesto, seguimos registrados como agente secundario. */
BtAgent *bt_agent_new  (GDBusConnection *conn);
void     bt_agent_free (BtAgent *agent);

/* Vuelve a registrarse en BlueZ. Llamar cuando bluetoothd reaparece en el
 * bus: al reiniciarse pierde la lista de agentes. Es idempotente. */
void bt_agent_reregister (BtAgent *agent);

/* TRUE si conseguimos ser el agente por omisión. */
gboolean bt_agent_is_default (BtAgent *agent);

#endif /* BT_AGENT_H */
