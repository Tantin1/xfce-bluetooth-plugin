# xfce-bluetooth-plugin

*[English version](README.en.md)*

Complemento liviano para manejar el Bluetooth desde el panel de Xfce. Cubre
lo de todos los días, a la par de lo que traen los escritorios Cinnamon o
GNOME, sin abrir ningún programa aparte.

## Qué hace

- Conectar, desconectar, emparejar, olvidar, bloquear y marcar como de
  confianza (para que se conecte solo).
- **Reconectar** en un clic, para el auricular que quedó conectado pero mudo.
- **Conectar un perfil puntual** de un aparato: sonido, manos libres, teclado
  o mouse.
- **Renombrar** un aparato emparejado. Si dejás el nombre vacío, vuelve al de
  fábrica.
- Pedir y confirmar códigos de emparejamiento.
- Hacer visible el equipo para otros aparatos. La visibilidad **se apaga sola
  a los 3 minutos**.
- Mostrar la batería del aparato, cuando el aparato la informa.
- Avisar con una notificación al conectar y desconectar (opcional).
- **Enviar archivos** a un teléfono u otro equipo, con barra de progreso.
- **Recibir archivos**, con un cartel para aceptar o rechazar, barra de
  progreso y botón «Mostrar en la carpeta».
- Manejar varios adaptadores, cada uno con su interruptor.

La lista de aparatos se mantiene quieta: arriba los conectados, después los
emparejados en orden alfabético y al final los de paso, agrupados por
cercanía.

## Qué no hace, a propósito

- **Compartir internet del teléfono por Bluetooth.** Si usás NetworkManager,
  su propio ícono del panel ya lo hace, y bien.
- **Elegir entre sonido de alta calidad y manos libres.** Eso lo decide el
  servidor de sonido (PipeWire o PulseAudio), desde su control de volumen.
- **Las funciones avanzadas de Blueman**, como la ventana de servicios
  locales o el asistente paso a paso. Si las necesitás, Blueman sigue siendo
  la herramienta indicada.

## Con qué funciona

- Xfce 4.18 o posterior, sobre X11. También en Wayland, con el panel 4.20 o
  posterior (probado sobre labwc).
- Probado en CachyOS. Pensado también para Debian 12 y sus derivadas
  (Loc OS, CuerdOS).
- Versiones mínimas: GTK 3.24, GLib 2.56, panel de Xfce 4.18 y, si se usan
  las notificaciones, libnotify 0.7.

## Instalar

### 1. Lo necesario para compilar

En Debian y derivadas:

```bash
sudo apt install build-essential meson ninja-build pkg-config gettext \
                 libgtk-3-dev libxfce4panel-2.0-dev libglib2.0-dev libnotify-dev
```

En Arch y derivadas:

```bash
sudo pacman -S --needed base-devel meson ninja gettext gtk3 xfce4-panel libnotify
```

`libnotify` es opcional: sin ella el complemento compila igual, solo que sin
notificaciones.

### 2. Compilar e instalar

```bash
meson setup build --prefix=/usr
ninja -C build
sudo ninja -C build install
xfce4-panel -r
```

En Arch y derivadas, si la instalación protesta, usá
`sudo meson install -C build --no-rebuild`.

Después: clic derecho en el panel → Panel → Añadir elementos nuevos →
«Complemento de Bluetooth».

Si el complemento no aparece en esa lista, el archivo compilado quedó en una
carpeta que tu panel no revisa. Indicale la correcta al configurar (el
ejemplo es la de Debian):

```bash
meson setup build --prefix=/usr -Dplugin_dir=/usr/lib/x86_64-linux-gnu/xfce4/panel/plugins
```

También se puede compilar con CMake; está explicado en
[DESARROLLO.md](DESARROLLO.md).

### 3. Lo necesario para usarlo

- **`bluez`**, con su servicio funcionando. Es obligatorio. Para comprobarlo:

  ```bash
  systemctl status bluetooth
  ```

- **El servicio de transferencia de archivos**, solo si querés enviar o
  recibir archivos. Viene en un paquete aparte: `bluez-obex` en Arch,
  `bluez-obexd` en Debian. En algunas distribuciones, además, hay que
  encenderlo:

  ```bash
  systemctl --user enable --now obex.service
  ```

  No hace falta adivinar: en Propiedades → «Transferencia de archivos» el
  complemento dice qué falta y te da la orden exacta.

## Desinstalar

Desde la misma carpeta donde lo compilaste:

```bash
sudo ninja -C build uninstall
xfce4-panel -r
```

## Recibir archivos

Viene **apagado**, porque encenderlo anuncia que tu equipo acepta archivos.
Se activa en Propiedades → «Transferencia de archivos» → «Recibir archivos de
otros dispositivos», donde también elegís la carpeta (por omisión,
Descargas).

**Si el teléfono dice «transferencia prohibida»**, casi siempre la culpa es
del servicio de transferencia: algunas versiones se niegan a escribir cuando
la carpeta pasa por un enlace simbólico, y otras rechazan todo por una falla
propia. En esa misma ventana aparece el botón **«Arreglar
automáticamente»**, que lo resuelve sin pedir contraseña.

**Solo un programa puede recibir archivos a la vez.** Si Blueman está
abierto, el complemento te lo avisa y vuelve a intentar solo cada minuto:
alcanza con cerrar Blueman.

Para investigar un fallo, el registro que sirve es el del servicio de
transferencia:

```bash
journalctl --user -u obex --since "5 minutes ago" --no-pager
```

## Traducciones

Está en español e inglés. Para agregar un idioma, mirá
[DESARROLLO.md](DESARROLLO.md).

## Licencia

GPL-2.0-or-later.
