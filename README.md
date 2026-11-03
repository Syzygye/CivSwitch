# CivSwitch para Flipper Zero

Port nativo en formato FAP de la macro HID de CivSwitch. Este paquete contiene solo el código de la app Flipper y los archivos mínimos que necesita su proceso de compilación. No incluye el firmware Pico completo, el instalador de guardado, imágenes de parche, dependencias descargadas ni binarios compilados.

## Compilar

Requisitos: Node.js/npm y uFBT configurado para el firmware de Flipper que vayas a usar. Desde PowerShell, en la carpeta `FlipperApp`:

```powershell
.\build.ps1
```

El script instala las dependencias bloqueadas, genera el payload desde `core.js`, lo integra en la FAP y ejecuta `ufbt build`. La salida queda en `FlipperApp/dist/CivSwitch.fap`.

## Fuentes y atribuciones

- **CivSwitch**, de Gezine: [repositorio original](https://github.com/Gezine/CivSwitch). De allí proceden el payload compartido y la cadena de compresión/transporte. Se incluye el aviso MIT original en `LICENSE`.
- **Netflix-N-Hack** y la versión de **ufm42**: referencias enlazadas desde `core.js`. El repositorio de Netflix-N-Hack indica GPL-3.0; revisa los avisos y licencias de los proyectos antes de redistribuir código derivado de ellos.
- **Pico-Switch-Controller**, de Element2424: referencia para descriptores e informes USB adaptados por el port. [Repositorio](https://github.com/Element2424/Pico-Switch-Controller). Al revisar su página el 3 de noviembre de 2026 no se veía un archivo de licencia. La atribución no concede permiso de redistribución; confirma las condiciones con el titular antes de publicar partes derivadas.
- **Firmware de Flipper Zero**, de Flipper Devices: la app usa las API de Furi/USB y se compila como FAP externo. [Código fuente](https://github.com/flipperdevices/flipperzero-firmware). No es un producto oficial ni está afiliado a Flipper Devices.

El aviso MIT conservado corresponde al CivSwitch original y no concede derechos sobre marcas ni sobre material de terceros con condiciones propias. Mantén los avisos aplicables y comprueba los permisos de redistribución de cada componente.

## Descargo de responsabilidad

El software se proporciona «tal cual», sin garantías de funcionamiento, seguridad, compatibilidad o idoneidad, sujeto a los términos de `LICENSE` y a la legislación aplicable. Este texto no es asesoramiento jurídico ni garantiza que un uso determinado esté permitido.

Úsalo solo en dispositivos, juegos y redes tuyos o para los que tengas autorización expresa. Tú eres responsable de cumplir la legislación, los derechos de terceros y las condiciones de los servicios utilizados. La FAP emite entradas USB HID y escribe en la aplicación o campo que esté enfocado; confirma el foco antes de iniciar la macro. No se garantiza que su uso evite consecuencias previstas por las políticas de terceros.

Nintendo Switch, Civilization, Flipper Zero y sus marcas pertenecen a sus respectivos titulares. El proyecto no está patrocinado ni aprobado por Nintendo, 2K, Firaxis Games o Flipper Devices. Este descargo no limita derechos que no puedan excluirse conforme a la ley aplicable.