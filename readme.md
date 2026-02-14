+# Pico 2W PIO Square Wave Generator
+
+Este proyecto implementa un generador de ondas cuadradas de alta precisión utilizando el **PIO (Programmable I/O)** y **DMA (Direct Memory Access)** del microcontrolador RP2350 (Raspberry Pi Pico 2W).
+
+El objetivo principal es generar patrones de ondas cuadradas con tiempos configurables sin ocupar tiempo de procesamiento de la CPU principal, permitiendo que ésta se dedique a otras tareas o entre en modo bajo consumo.
+
+## Características
+
+*   **Descarga de CPU (Zero-CPU overhead):** Una vez iniciada la generación de ondas, ésta es gestionada enteramente por el controlador DMA y las máquinas de estado PIO. La CPU puede dormir o realizar otras tareas.
+*   **Tres Modos de Operación:**
+    *   **DMA Stream:** Lectura continua de tiempos desde DMA para patrones dinámicos.
+    *   **Infinite Mode:** Generación de onda cuadrada con periodo constante.
+    *   **Burst Mode:** Generación de N pulsos exactos con tiempos configurables.
+*   **Alta Precisión:** La generación de pulsos se basa en ciclos de reloj del sistema (`clk_sys`), permitiendo resoluciones muy precisas.
+*   **Compatible con RP2350:** Optimizado para el Pico 2W con soporte para múltiples máquinas de estado PIO.
+
+## Estructura del Proyecto
+
+*   **`stepgen.pio`**: Código ensamblador del PIO. Define tres programas de máquina de estado:
+    *   `dma_stream`: Lectura continua de pares (alto, bajo) desde el FIFO.
+    *   `infinite`: Onda cuadrada con período constante.
+    *   `burst`: Generación de N pulsos exactos.
+*   **`stepgen.c`**: Lógica principal en C.
+    *   Inicialización del PIO y configuración de máquinas de estado.
+    *   Configuración de canales DMA para alimentar datos al PIO.
+    *   Cálculo de tiempos de pulso y patrones.
+    *   Manejo de interrupciones (opcional).
+
+## Configuración de Hardware
+
+Por defecto, el proyecto está configurado para usar el **GPIO 16** como salida de la onda cuadrada.
+
+Para cambiar el pin, puedes definir `STEPGEN_PIN` durante la compilación con CMake:
+
+```bash
+cmake -DSTEPGEN_PIN=<número> ..
+```
+
+O modificar la línea en `stepgen.c`:
+
+```c
+#if !defined(STEPGEN_PIN)
+#define STEPGEN_PIN 16
+#endif
+```
+
+## Compilación
+
+Este proyecto utiliza el **Pico SDK** y **CMake**.
+
+1.  Asegúrate de tener la cadena de herramientas ARM Cortex-M y el Pico SDK instalados.
+2.  Navega a la carpeta del proyecto y crea la carpeta de construcción:
+    ```bash
+    mkdir build
+    cd build
+    ```
+3.  Ejecuta CMake y Ninja (o Make):
+    ```bash
+    cmake ..
+    ninja
+    ```
+    O con Make:
+    ```bash
+    make
+    ```
+4.  El binario se genera en `build/pico2w_pio_square.uf2`.
+
+## Uso de la API
+
+### Inicialización
+
+El sistema se inicializa automáticamente en `main()`, configurando el PIO y reservando canales DMA:
+
+```c
+stepgen_init();  // Inicializa PIO y DMA
+```
+
+### Modo Infinite (Onda Cuadrada Continua)
+
+Para generar una onda cuadrada de período constante:
+
+```c
+// Tiempo alto: 100 ciclos, Tiempo bajo: 100 ciclos
+stepgen_infinite_wave(100u, 100u);
+```
+
+### Modo DMA Stream (Patrón Dinámico)
+
+Para enviar patrones dinámicos a través de DMA:
+
+```c
+uint32_t pattern[4] = {100, 100, 50, 150};  // [alto0, bajo0, alto1, bajo1]
+stepgen_dma_stream(pattern, 2);  // 2 ciclos
+```
+
+### Modo Burst (N Pulsos Exactos)
+
+Para generar una cantidad exacta de pulsos:
+
+```c
+stepgen_burst_pulses(10, 100u, 100u);  // 10 pulsos, 100 ciclos alto y bajo
+```
+
+## Funcionamiento Técnico (PIO + DMA)
+
+1.  **Cálculo:** La CPU pre-calcula los tiempos (alto y bajo) para generar los patrones deseados y los guarda en buffers en RAM.
+2.  **DMA:** El controlador DMA transfiere automáticamente estos tiempos al FIFO del PIO, liberando a la CPU.
+3.  **PIO:** Las máquinas de estado PIO leen los tiempos del FIFO, alternan el pin de salida, esperan los ciclos indicados, y repiten el proceso.
+4.  **Modos:**
+    *   En **Infinite Mode**, la máquina de estado repite indefinidamente con período constante.
+    *   En **DMA Stream**, el DMA alimenta continuamente pares (alto, bajo) para formar patrones complejos.
+    *   En **Burst Mode**, se genera un número exacto de pulsos y se detiene.
+
+---
+Autor: Alan Velazquez
