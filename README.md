# xfce-bluetooth-plugin

Complemento de gestión de Bluetooth para el panel de XFCE, con la misma
arquitectura que `xfce-net-plugin`: botón en el panel, ventana emergente con
construcción perezosa, refresco por diferencia y capa D-Bus propia.

Hace lo que hace Blueman en el uso corriente, sin salir del panel:

- Conectar, desconectar, emparejar, olvidar, bloquear y marcar como de
  confianza (conexión automática).
- **Reconectar** en un clic, para el auricular que quedó conectado pero mudo.
- **Conectar un perfil puntual** (audio de alta calidad, manos libres, teclado
  o mouse…).
- **Renombrar** un dispositivo emparejado; vacío vuelve al nombre de fábrica.
- Agente de emparejamiento propio: PIN, confirmación de código y autorización
  de servicios.
- Varios adaptadores, cada uno con su interruptor, y visibilidad para otros
  dispositivos que **se apaga sola a los 3 minutos**.
- Nivel de batería cuando el aparato lo informa, y notificaciones opcionales.
- **Enviar archivos** a un teléfono u otro equipo, con ventana de progreso.
- **Recibir archivos**, con cartel para aceptar o rechazar, ventana de
  progreso, botón «Mostrar en la carpeta» y arreglo automático del problema
  de enlaces simbólicos del servicio de transferencia.

## Dependencias

**Para compilar** (Debian / Ubuntu / Loc OS):

```bash
sudo apt install build-essential meson ninja-build pkg-config gettext \
                 libgtk-3-dev libxfce4panel-2.0-dev libglib2.0-dev libnotify-dev
```

En Arch y derivadas (CachyOS):

```bash
sudo pacman -S --needed base-devel meson ninja gettext gtk3 xfce4-panel libnotify
```

`libnotify` es opcional: sin ella el complemento compila igual, solo que sin
notificaciones de escritorio.

**En tiempo de ejecución**:

- `bluez`, con el servicio `bluetooth.service` corriendo. **Obligatorio.**

  ```bash
  systemctl status bluetooth
  ```

- El servicio de transferencia de archivos, que viene en un paquete aparte:
  `bluez-obex` en Arch y derivadas, `bluez-obexd` en Debian y derivadas.
  **Recomendado, no obligatorio**: sin él todo funciona menos enviar y recibir
  archivos. En algunas distribuciones además hay que habilitarlo:

  ```bash
  systemctl --user enable --now obex.service
  ```

  El complemento detecta cuál de los dos casos falta y lo dice, con la orden
  exacta, en Propiedades → «Transferencia de archivos».

## Compilar e instalar

Con Meson (recomendado):

```bash
meson setup build --prefix=/usr
ninja -C build
sudo ninja -C build install
xfce4-panel -r          # reiniciar el panel
```

Con CMake (alternativa):

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr
make -C build -j$(nproc)
sudo make -C build install
xfce4-panel -r
```

Si el `.so` termina en un directorio que tu panel no mira, forzá la ruta:

```bash
meson setup build --prefix=/usr -Dplugin_dir=/usr/lib/x86_64-linux-gnu/xfce4/panel/plugins
# o con CMake:
cmake -S . -B build -DXFCE_PLUGIN_DIR=/usr/lib/x86_64-linux-gnu/xfce4/panel/plugins
```

En CachyOS/Arch y derivadas, `sudo ninja -C build install` puede necesitar
`sudo meson install -C build --no-rebuild`.

Después: clic derecho en el panel → Panel → Añadir complementos → «Complemento
de Bluetooth».

## Recibir archivos

Viene **apagado** a propósito: encenderlo anuncia que este equipo acepta
archivos. Se activa en Propiedades → «Transferencia de archivos» →
«Recibir archivos de otros dispositivos», donde también se elige la carpeta
(por omisión, Descargas).

Si los archivos entrantes fallan y el teléfono dice «transferencia prohibida»,
la causa casi siempre es el servicio de transferencia, no el complemento:
algunas versiones rechazan escribir cuando la carpeta pasa por un enlace
simbólico, y otras rechazan todo por una falla en ese mismo control. En la
misma ventana de propiedades aparece el botón **«Arreglar automáticamente»**:
agrega al servicio de usuario la opción que permite enlaces simbólicos y lo
reinicia. No pide contraseña, porque ese servicio es del usuario.

Solo puede haber un programa recibiendo archivos a la vez. Si Blueman ya
tiene el puesto, el complemento lo avisa y lo vuelve a intentar solo cada
minuto: alcanza con cerrar Blueman.

Para investigar un fallo de recepción, el registro que sirve es el del
servicio de transferencia:

```bash
journalctl --user -u obex --since "5 minutes ago" --no-pager
```

## Estructura

| Archivo | Qué hace |
|---|---|
| `plugin.c` | Punto de entrada del panel: ícono, ayuda emergente, configuración, ventana de propiedades, registro de los dos agentes, instancia única. |
| `popup.c` | Ventana emergente: crear, mostrar, ocultar, posicionar; operaciones en curso; flujo de envío de archivos. |
| `popup-rows.c` | Una fila por dispositivo, con su panel desplegable: acciones, renombrar, perfiles puntuales, detalles. |
| `popup-sections.c` | Secciones por adaptador, refresco por diferencia y reconstrucción. |
| `popup-private.h` | Declaraciones compartidas entre los tres módulos de la ventana emergente. |
| `bt-dbus.c/h` | Capa BlueZ (bus del sistema): adaptadores, dispositivos, acciones, reconexión, señales, instantánea. |
| `bt-agent.c/h` | Agente de emparejamiento `org.bluez.Agent1`. |
| `bt-obex.c/h` | Transferencia de archivos (bus de sesión): detección del servicio, motor de envío, arreglo de enlaces simbólicos, nombres de destino sin pisar archivos. |
| `bt-obex-agent.c/h` | Agente de recepción `org.bluez.obex.Agent1`: cartel de autorización, ventana de progreso, reintento si otro programa tiene el puesto. |
| `po/` | Traducciones (gettext). |

## Notas de diseño

**Los agentes viven en el complemento, no en la ventana emergente.** Un
aparato puede pedir vinculación, o mandar un archivo, con la ventana cerrada.

**BlueZ contesta.** `Connect()`, `Pair()` y `Disconnect()` devuelven éxito o
un error con nombre, así que no hace falta deducir el resultado mirando el
estado del dispositivo. El plazo de 90 s que queda es solo red de seguridad.

**El descubrimiento se para al cerrar, siempre.** No es un disparo único:
queda corriendo hasta que se lo pare, gasta batería y entrecorta el audio.
La parada se pide en todos los adaptadores sin fiarse de si figuran buscando.

**La visibilidad no es permanente.** Se apaga sola a los 3 minutos, para que
un olvido no deje el equipo anunciándose a cualquiera.

**Las filas se congelan mientras hay un panel desplegado.** Destruir una fila
debajo del cursor es peor que mostrar una lista un poco desactualizada.

**Fantasmas.** BlueZ conserva objetos de aparatos que vio alguna vez. Los
emparejados siempre se muestran; los demás, solo si se los está viendo ahora
o están conectados.

**La transferencia vive en el bus de sesión**, no en el del sistema como el
resto: el servicio corre como el usuario. Preguntar si está disponible no lo
enciende; solo se enciende cuando el usuario pide transferir.

**El nombre del archivo entrante se poda a su última parte.** Lo elige el
aparato que manda; sin la poda, uno hostil podría escribir fuera de la
carpeta elegida.

**Elegir entre sonido de alta calidad y manos libres no es tarea de este
complemento.** Lo decide el servidor de sonido (PipeWire o PulseAudio). Lo
que el complemento sí hace es reconectar y levantar un perfil puntual.

## Traducciones

El código lleva los textos en inglés; la traducción al español está completa
en `po/es.po`.

```bash
# Actualizar la plantilla cuando cambian los textos del código
xgettext --files-from=po/POTFILES.in -o po/xfce-bluetooth-plugin.pot \
         --keyword=_ --keyword=N_ --from-code=UTF-8

# Refrescar una traducción existente
msgmerge -U po/es.po po/xfce-bluetooth-plugin.pot

# Agregar un idioma nuevo
msginit -i po/xfce-bluetooth-plugin.pot -l fr -o po/fr.po
echo fr >> po/LINGUAS      # y agregarlo a LANGS en CMakeLists.txt
```

Si se agrega un archivo `.c` con textos, sumarlo también a `po/POTFILES.in`.

## Licencia

GPL-2.0-or-later.
