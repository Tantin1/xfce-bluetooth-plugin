# Notas para desarrolladores

Lo que no le hace falta a quien solo instala el complemento: cómo está
armado, por qué algunas cosas son como son, y cómo compilar con CMake o
traducir. Para instalar y usar, ver [README.es.md](README.es.md).

## Alcance

El complemento cubre lo cotidiano, a la par de los complementos de Bluetooth
de Cinnamon o GNOME. **No busca reemplazar a Blueman.** Quedan fuera a
propósito: compartir internet por Bluetooth (lo hace NetworkManager), la
ventana de servicios locales y el asistente paso a paso de Blueman, y un
buscador dentro de la ventana emergente (la lista es corta y se ordena sola).
Antes de proponer una función nueva, conviene preguntarse si un escritorio
como Cinnamon la trae.

## Estructura

Todos los archivos fuente están en la raíz, sin subcarpeta.

| Archivo | Qué hace |
|---|---|
| `plugin.c` | Punto de entrada del panel: ícono, ayuda emergente, configuración, ventana de propiedades, registro de los dos agentes, instancia única. |
| `popup.c` | Ventana emergente: crear, mostrar, ocultar, posicionar (X11 y Wayland); operaciones en curso; flujo de envío de archivos. |
| `popup-rows.c` | Una fila por aparato, con su panel desplegable: acciones, renombrar, perfiles puntuales, detalles. |
| `popup-sections.c` | Secciones por adaptador, orden de la lista, refresco por diferencia y reconstrucción. |
| `popup-private.h` | Declaraciones compartidas entre los tres módulos de la ventana emergente. |
| `bt-dbus.c/h` | Capa de BlueZ (bus del sistema): adaptadores, aparatos, acciones, reconexión, señales, instantánea. |
| `bt-agent.c/h` | Agente de emparejamiento (`org.bluez.Agent1`). |
| `bt-obex.c/h` | Transferencia de archivos (bus de sesión): detección del servicio, motor de envío, arreglo de enlaces simbólicos, nombres de destino que no pisan archivos. |
| `bt-obex-agent.c/h` | Agente de recepción (`org.bluez.obex.Agent1`): cartel de autorización, ventana de progreso, reintento si otro programa tiene el puesto. |
| `po/` | Traducciones (gettext). |

## Notas de diseño

**Los agentes viven en el complemento, no en la ventana emergente.** Un
aparato puede pedir vinculación, o mandar un archivo, con la ventana cerrada.

**BlueZ contesta.** Conectar, emparejar y desconectar devuelven éxito o un
error con nombre, así que no hace falta deducir el resultado mirando el
estado del aparato. El plazo de 90 segundos que queda es solo una red de
seguridad.

**La búsqueda se detiene al cerrar, siempre.** No es un disparo único: queda
corriendo hasta que se la pare, gasta batería y entrecorta el audio. La
parada se pide en todos los adaptadores, sin fiarse de si figuran buscando.

**La visibilidad no es permanente.** Se apaga sola a los 3 minutos, para que
un olvido no deje el equipo anunciándose a cualquiera.

**La lista no se ordena por la señal cruda.** La señal cambia varias veces
por segundo mientras se busca; ordenar por ella hacía que las filas cambiaran
de lugar solas. Los emparejados van en orden alfabético y los de paso se
agrupan en tres niveles de cercanía (umbrales en −60 y −80 dBm), alfabéticos
dentro de cada nivel.

**Las filas se congelan mientras hay un panel desplegado.** Destruir una fila
debajo del cursor es peor que mostrar una lista un poco desactualizada.

**Fantasmas.** BlueZ conserva objetos de aparatos que vio alguna vez. Los
emparejados siempre se muestran; los demás, solo si se los está viendo ahora
o están conectados.

**La transferencia vive en el bus de sesión**, no en el del sistema como el
resto, porque el servicio corre como el usuario. Preguntar si está disponible
no lo enciende; solo se enciende cuando el usuario pide transferir.

**Sin systemd, la recepción anda igual.** Al demonio de transferencia lo
arranca el bus de sesión con la línea `Exec=` de su archivo de activación;
systemd solo interviene si existe (`/run/systemd/system`, la misma prueba
que usa la biblioteca de systemd). Lo que cambia es el arreglo de enlaces
simbólicos: con systemd es una sustitución de `obex.service`; sin systemd, un
archivo de activación del usuario en `~/.local/share/dbus-1/services/`, que
el bus prefiere al del sistema, con `--symlinks` agregado. Después se detiene
el demonio viejo (comprobando que sea obexd) y se arranca el nuevo por el
bus. Los mensajes de ayuda también cambian según haya o no systemd.

**El nombre del archivo entrante se poda a su última parte.** Lo elige el
aparato que manda; sin la poda, uno hostil podría escribir fuera de la
carpeta elegida. No sacar.

**El ícono de los aparatos de audio no se toma tal cual de BlueZ.** Para
parlantes, micrófonos, equipos de música o televisores BlueZ manda siempre
`audio-card` («placa de sonido»). El complemento lo afina con la subcategoría
que el aparato informa en su clase, y deja `audio-card` de respaldo por si el
tema no tiene el nombre fino.

**Elegir entre sonido de alta calidad y manos libres no es tarea de este
complemento.** Lo decide el servidor de sonido. Lo que el complemento sí hace
es reconectar y levantar un perfil puntual.

## Wayland

**X11 no se toca.** Todo lo de Wayland va detrás de una comprobación hecha al
correr (`popup_is_wayland`, "¿es Wayland?"), así que un mismo paquete anda en
las dos sesiones. En el código no hay ninguna llamada exclusiva de X11.

**En Wayland todos los complementos viven en el proceso del panel.** Tres
consecuencias:

- Si el complemento se cae, se cae el panel entero.
- El idioma: se usa `<glib/gi18n-lib.h>`, que nombra el dominio propio en
  cada texto, y **no** se llama a `textdomain()`, que cambiaría el idioma por
  omisión de todos los complementos.
- La instancia duplicada: dos instancias comparten la conexión al bus, y
  pedir el mismo nombre dos veces desde una conexión no da «ocupado». Por eso
  hay además una marca de proceso (`bt_real_instance`, "instancia real").

**La ventana emergente en Wayland** (`popup_show_wayland`, "mostrar en
Wayland"): se hace hija del panel y se ubica junto al botón con
`gdk_window_move_to_rect`; se da vuelta solo en el eje que cruza el panel y
se corre solo a lo largo de él. El acaparamiento se pide **antes** de
mostrarla, o no recibe el teclado (el campo de renombrar quedaría sordo). El
clic en el panel o en el botón se detecta por coordenadas, porque el
compositor solo la cierra al hacer clic en otro programa.

**Cuando el compositor la cierra, GTK 3 no avisa con «unmap-event» ni con
«grab-broken-event».** Lo único que llega es «window-state-event» con la
marca de retirada. Se escuchan los tres, y cualquiera cuenta solo si la
ventana de verdad ya no está a la vista: un aviso viejo de un cierre anterior
puede llegar con la ventana ya reabierta.

**Los carteles que aparecen solos** (PIN, confirmación de código,
autorización de recepción) cierran antes la ventana emergente con
`popup_close_for_dialog` ("cerrar para un cartel"). En Wayland, mientras
exista, ella se queda con el teclado y el cartel no recibe lo que se escribe.
En X11 esa función no hace nada.

**Diagnóstico:** lanzar el panel con
`G_MESSAGES_DEBUG=xfce-bluetooth-plugin` muestra qué pasa con la ventana
(tipo de panel, acaparamiento, cada clic adentro o afuera, cierres del
compositor). Silencioso por omisión.

## Presentación de la ventana emergente

**Aspecto del tema.** Todo el contenido va adentro de una caja con nombre
de componente «popover» (`BtPopupFrame`): el tema le pone fondo, borde,
esquinas y colores de letra como a sus popovers (los temas escriben sus
reglas para el componente, no para una clase). Si hay compositor (siempre en
Wayland; en X11 cuando está activo el de xfwm4 u otro), la ventana tiene
transparencia y se ven las esquinas redondeadas; sin compositor, la caja
queda con esquinas rectas. Si el compositor se prende o se apaga con la
ventana abierta, el cambio se aplica en la próxima apertura. La ventana mide
lo configurado en los dos modos. No pintar solo el fondo del popover detrás
de un contenido común: con temas como Numix la letra queda ilegible.

**Estilo de la ventana** (Propiedades, clave `popup_style`): Automático
flota y redondea solo si hay compositor y el tema les pone radio a sus
popovers; Flotante siempre queda despegada 6 px; Pegada al panel no usa
transparencia, deja las esquinas rectas y pega la ventana al panel y al
borde. El diálogo de Propiedades se crea sin padre a propósito: en Wayland,
con el panel como padre, las listas desplegables salen fuera de lugar.

**Regla «imán».** A lo largo del panel: centrada sobre el botón; si no
entra, se corre; si contra un borde queda un hueco menor que media ventana,
se pega a ese borde. Vale para X11 y Wayland, con paneles horizontales y
verticales (`place_along_panel`). En Wayland el área es el propio panel,
porque el programa no sabe dónde está en la pantalla.

## Compilar con CMake

Alternativa a Meson:

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr
make -C build -j$(nproc)
sudo make -C build install
xfce4-panel -r
```

Para forzar la carpeta de complementos del panel:
`-DXFCE_PLUGIN_DIR=/usr/lib/x86_64-linux-gnu/xfce4/panel/plugins`.

CMake no trae orden para desinstalar; se borra lo que anotó al instalar:

```bash
sudo xargs rm -v < build/install_manifest.txt
```

## Traducciones

Los textos van en inglés dentro del código y se traducen en `po/`, nunca en
el código.

```bash
# Actualizar la plantilla cuando cambian los textos del código
xgettext --files-from=po/POTFILES.in -o po/xfce-bluetooth-plugin.pot \
         --keyword=_ --keyword=N_ --from-code=UTF-8

# Refrescar una traducción existente, sin adivinanzas automáticas
msgmerge --no-fuzzy-matching -U po/es.po po/xfce-bluetooth-plugin.pot

# Comprobar que no falte nada
msgfmt --check --statistics -o /dev/null po/es.po

# Agregar un idioma nuevo
msginit -i po/xfce-bluetooth-plugin.pot -l fr -o po/fr.po
echo fr >> po/LINGUAS      # Meson y CMake leen los dos esta lista
```

Si se agrega un archivo `.c` con textos, sumarlo también a `po/POTFILES.in`.

## Al empaquetar

El servicio de transferencia de archivos es **opcional**: «recomendado» en
Debian (`bluez-obexd`) y «opcional» en Arch (`bluez-obex`), nunca
obligatorio. libnotify es opcional **al compilar**, pero un paquete compilado
con ella queda enlazado a la biblioteca y la necesita para arrancar: en los
paquetes armados acá es obligatoria (es chica y viene en cualquier Xfce).

### Paquetes automáticos (GitHub Actions)

`.github/workflows/paquetes.yml` arma cuatro paquetes: Debian 12, Debian 13
y Ubuntu 24.04 (`.deb`, cada uno en su sistema, dentro de un contenedor) y
Arch (`.pkg.tar.zst`).

- **Al subir una etiqueta** `v<versión>`, los adjunta a una publicación de
  versión. La etiqueta tiene que coincidir con la `version` de
  `meson.build`; si no, el flujo se corta con un error que lo dice.
- **A mano** (Actions → Paquetes → Run workflow), los deja como archivos
  descargables de esa ejecución, sin publicar.

Para publicar una versión nueva: cambiar `version` en `meson.build` (y en
`plugin.c`, en la ventana «Acerca de»), subir el cambio, y después
`git tag v0.1.1 && git push origin v0.1.1`.

Los archivos de empaquetado:

- `debian/`: `control` (dependencias), `rules`, `copyright`. En
  `control` va `libxfce4ui-2-dev` aunque el código no la use: en Debian 13
  la biblioteca del panel la exige y su paquete de desarrollo no la trae. **`rules`
  fuerza Meson** (`--buildsystem=meson`): como el proyecto también trae
  `CMakeLists.txt`, la herramienta de Debian elegía CMake, que ignora
  `plugin_dir` y dejaba el complemento fuera de la carpeta multiarquitectura
  que mira el panel. `changelog` lo reescribe el flujo con la versión y el
  sistema (`0.1.0-1~deb12`, etc.).
- `packaging/arch/PKGBUILD`: baja el código de la etiqueta en GitHub; el
  flujo le deja al lado el comprimido con el mismo nombre y makepkg no
  descarga nada. Sirve tal cual como base para el AUR.

Para armar el `.deb` a mano en Debian o Ubuntu:

```bash
sudo apt install build-essential && sudo apt build-dep ./
dpkg-buildpackage -us -uc -b      # el paquete queda en la carpeta de arriba
```
