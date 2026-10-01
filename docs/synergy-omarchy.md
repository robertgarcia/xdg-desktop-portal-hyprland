# Synergy 3 en Omarchy (Hyprland) con este fork

Guía para dejar funcionando **Synergy 3.7.2 oficial** como cliente en un equipo
con Omarchy, partiendo de lo que ya se resolvió en la laptop `rog-zephyrus`.
Incluye mouse, teclado y portapapeles en ambos sentidos.

## Por qué hace falta este fork

Synergy 3 en Wayland no inyecta la entrada por sí mismo. Usa el portal
`org.freedesktop.portal.RemoteDesktop` con libei (`ConnectToEIS`) y, para el
portapapeles, `org.freedesktop.portal.Clipboard`. El
`xdg-desktop-portal-hyprland` de Arch no implementa ninguno de los dos. Sin
ellos, el core de Synergy muere en bucle con:

```
No such interface "org.freedesktop.portal.RemoteDesktop"
```

La rama `poc-synergy-remotedesktop` de este fork parte del PR upstream
hyprwm/xdg-desktop-portal-hyprland#402 y añade lo que faltaba:

- correcciones del PR: fugas, sesiones que no se liberaban, regiones por
  monitor, modificadores, scroll;
- un backend de Clipboard sobre `ext-data-control-v1`;
- un diálogo de consentimiento con "Always allow" persistente;
- solo acepta llamadas del frontend `xdg-desktop-portal`;
- el puntero se ajusta solo al conectar o mover monitores.

waynergy ya no sirve con un servidor Synergy 3.7.2 que exige equipos
verificados: el servidor pide certificado de cliente y corta la conexión. El
cliente oficial sí hace el emparejamiento.

## Lo que necesitas antes

- El Mac como servidor con **Synergy 3.7.2** o posterior.
- El instalador de Linux de Symless:
  `synergy-3.7.2-linux-resolute-x86_64.pkg.tar.zst` (en la laptop está en
  `~/Downloads`).
- Acceso físico al teclado y touchpad del equipo Linux: la primera aprobación
  se hace ahí, porque el Mac aún no controla nada.

## 1. Paquetes

```bash
sudo pacman -S --needed git cmake ninja gcc pkgconf \
  hyprwayland-scanner hyprutils hyprlang hyprland-protocols \
  sdbus-cpp wayland-protocols libei libxkbcommon pipewire libdrm mesa \
  qt6-base qt6-wayland hyprland-guiutils wl-clipboard
```

- `libei` trae también `libeis`.
- `hyprland-guiutils` da `hyprland-dialog`, el diálogo de consentimiento.
- `wayland-protocols` tiene que ser ≥ 1.39, por `ext-data-control-v1`.

## 2. Synergy 3.7.2 y apagar waynergy

```bash
# waynergy y Synergy no deben correr a la vez
systemctl --user disable --now waynergy.service

sudo pacman -U ~/Downloads/synergy-3.7.2-linux-resolute-x86_64.pkg.tar.zst
```

El paquete instala:

- `/opt/Synergy`;
- el servicio de usuario `synergy-3.service`;
- el autostart `/etc/xdg/autostart/synergy-3.desktop`.

Todavía no lo configures: sin el portal, el core se reinicia en bucle.

## 3. Clonar y compilar

```bash
mkdir -p ~/Projects/devcodex && cd ~/Projects/devcodex
git clone --recurse-submodules -b poc-synergy-remotedesktop \
  https://github.com/robertgarcia/xdg-desktop-portal-hyprland.git
cd xdg-desktop-portal-hyprland

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
```

No hace falta `install`: el binario se usa desde `build/`. También compila con
Meson (`meson setup build-meson && ninja -C build-meson`), pero los scripts
esperan el de CMake.

## 4. Activar el portal del fork

```bash
docs/poc/poc-on.sh
```

Debe terminar con `POC activa: org.freedesktop.portal.RemoteDesktop
disponible`. El script:

- copia los `.portal` del sistema más el `hyprland.portal` del repo a
  `~/.local/state/xdph-poc/portals`;
- crea dos drop-ins `poc-synergy.conf` en `~/.config/systemd/user/`:
  - el frontend usa esa carpeta (`XDG_DESKTOP_PORTAL_DIR`);
  - el backend arranca el binario de `build/` con `--verbose`;
- reinicia ambos portales y Synergy.

No toca nada bajo `/usr`, y un `pacman -Syu` no lo deshace.

## 5. Configurar Synergy y aprobar

1. Abre Synergy desde el lanzador y configúralo como **cliente** del Mac.
2. En el Mac, acepta el equipo nuevo cuando Synergy lo pida (verificar huella).
3. En Linux aparece **"Allow remote control?"** con tres botones:
   - **Deny**: rechaza;
   - **Allow once**: solo esta vez, vuelve a preguntar al reiniciar;
   - **Always allow**: el permiso queda guardado y Synergy ya no pregunta.

   Pulsa **Always allow** con el touchpad del equipo Linux. El teclado no
   activa los botones; cerrar el diálogo equivale a denegar.
4. Para copiar imágenes grandes desde el Mac, sube en su Synergy el límite
   del portapapeles (`clipboardSharingSize`, por defecto 4096 KB).

## 6. Verificar

```bash
# portal y Synergy activos
systemctl --user is-active xdg-desktop-portal-hyprland synergy-3

# Synergy recuperó su permiso sin preguntar (tras un reinicio)
journalctl --user -u xdg-desktop-portal-hyprland -n 50 | grep -E "restored|Start"

# portapapeles: copia algo en el Mac, entra a Linux y
wl-paste
```

Prueba también copiar en Linux y pegar en el Mac.

## Uso diario

| Situación | Qué hacer |
|---|---|
| `pacman -Syu` actualizó hyprutils, sdbus-cpp, libei o hyprwayland-scanner | `cmake --build build` y `docs/poc/poc-on.sh` |
| Traer cambios del fork | `git pull` → `cmake --build build` → `docs/poc/poc-on.sh` |
| Se reinició el portal a mano | `systemctl --user restart synergy-3`: Synergy no reabre su conexión EIS solo |
| Ver qué pasa | `journalctl --user -u xdg-desktop-portal-hyprland -f` |

### Revocar el "Always allow"

El permiso vive en el PermissionStore, tabla `remote-desktop`. El token de
Synergy está en `~/.config/Synergy/synergy-core.conf`.

```bash
busctl --user call org.freedesktop.impl.portal.PermissionStore \
  /org/freedesktop/impl/portal/PermissionStore \
  org.freedesktop.impl.portal.PermissionStore Lookup ss remote-desktop <token>

busctl --user call org.freedesktop.impl.portal.PermissionStore \
  /org/freedesktop/impl/portal/PermissionStore \
  org.freedesktop.impl.portal.PermissionStore Delete ss remote-desktop <token>
```

## Revertir a waynergy

```bash
docs/poc/poc-off.sh
systemctl --user disable --now synergy-3.service
systemctl --user enable --now waynergy.service
```

Para que Synergy no arranque al iniciar sesión, desactiva también el
autostart: copia `/etc/xdg/autostart/synergy-3.desktop` a
`~/.config/autostart/` y añade `Hidden=true`.

## Problemas conocidos

| Síntoma | Causa | Solución |
|---|---|---|
| El core de Synergy se reinicia cada pocos segundos | No está activo el portal del fork | `docs/poc/poc-on.sh` |
| El Mac no mueve nada tras reiniciar el portal | Synergy no reabre EIS | `systemctl --user restart synergy-3` |
| No llega una imagen del Mac | Pasa del límite de 4 MB de Synergy | Subir `clipboardSharingSize` en el Mac |
| El diálogo aprobó algo sin querer | Apareció bajo el cursor y un clic lo eligió | Revocar el permiso (arriba) y volver a aprobar |
| Vaciar el portapapeles no se propaga | Hyprland no avisa del vaciado | Ninguna por ahora |
| La selección primaria (clic central) no se comparte | No implementado | Ninguna por ahora |
