--- /dev/null
+# Pico PIO Step Generator (S-Curve DMA)
+
+Este proyecto implementa un generador de pulsos de alta precisión para el control de motores paso a paso (steppers) utilizando el **PIO (Programmable I/O)** y **DMA (Direct Memory Access)** del microcontrolador RP2040 (Raspberry Pi Pico).
+
+El objetivo principal es generar perfiles de movimiento suaves (aceleración y desaceleración en **Curva S**) sin ocupar tiempo de procesamiento de la CPU principal.
+
+## Características
+
+*   **Descarga de CPU (Zero-CPU overhead):** Una vez configurada la rampa, el movimiento es gestionado enteramente por el controlador DMA y las máquinas de estado PIO. La CPU puede dormir o realizar otras tareas.
+*   **Perfil de Movimiento en Curva S:** Implementa la función `smoothstep` para calcular rampas de aceleración suaves, reduciendo la vibración y el estrés mecánico en el motor.
+*   **Alta Precisión:** La generación de pulsos se basa en ciclos de reloj del sistema (`clk_sys`), permitiendo resoluciones de nanosegundos.
+*   **Modos de Operación:**
+    *   **DMA Stream:** Generación continua con rampas de entrada y salida.
+    *   **Burst Mode:** Generación de una cantidad exacta de pulsos (útil para posicionamiento preciso).
+    *   **Infinite Mode:** Generación de frecuencia constante.
+
+## Estructura del Proyecto
+
+*   **`stepgen.pio`**: Código ensamblador del PIO. Define cómo la máquina de estado interpreta los datos del FIFO para alternar el pin de salida (STEP).
+*   **`stepgen.c`**: Lógica principal en C.
+    *   Cálculo de tablas de tiempos para la curva S.
+    *   Configuración de canales DMA (encadenamiento de rampa -> velocidad constante).
+    *   Manejo de interrupciones para detención suave.
+
+## Configuración de Hardware
+
+Por defecto, el proyecto está configurado para usar el **GPIO 16** como salida de pulsos (STEP).
+
+Para cambiar el pin, puedes definir `STEPGEN_PIN` antes de compilar o modificar la línea en `stepgen.c`:
+
+```c
+#if !defined(STEPGEN_PIN)
+#define STEPGEN_PIN 16
+#endif
+```
+
+## Compilación
+
+Este proyecto utiliza el estándar **Pico SDK**.
+
+1.  Asegúrate de tener el entorno del Pico SDK configurado.
+2.  Crea el directorio de construcción:
+    ```bash
+    mkdir build
+    cd build
+    ```
+3.  Ejecuta CMake y Make:
+    ```bash
+    cmake ..
+    make
+    ```
+
+## Uso de la API
+
+### Inicialización
+
+El sistema se inicializa automáticamente en `main()`, configurando el PIO y reservando canales DMA.
+
+### Movimiento con Rampa (DMA)
+
+Para iniciar un movimiento con aceleración suave:
+
+```c
+// Inicia en 10 Hz, acelera hasta 1 kHz, 50% duty cycle, rampa de 128 pasos
+stepgen_start_s_curve_dma(10.0f, 1000.0f, 0.5f, 128u);
+```
+
+*   **`freq_start_hz`**: Frecuencia inicial.
+*   **`freq_target_hz`**: Frecuencia objetivo (velocidad crucero).
+*   **`duty_cycle`**: Ciclo de trabajo (usualmente 0.5).
+*   **`ramp_steps`**: Cantidad de pasos discretos para construir la curva de aceleración.
+
+### Detención
+
+Para detener el motor suavemente (desaceleración):
+
+```c
+// Desacelera desde la velocidad actual hasta 10 Hz en 128 pasos y se detiene
+stepgen_stop_s_curve_dma(10.0f, 128u);
+```
+
+### Funciones Bloqueantes (Test)
+
+Existen funciones auxiliares que no usan DMA, útiles para pruebas rápidas o movimientos simples donde no importa bloquear la CPU:
+
+*   `stepgen_square_wave_ms(...)`
+*   `stepgen_square_wave_us(...)`
+*   `stepgen_burst_us(...)` (Genera N pulsos y se detiene)
+
+## Funcionamiento Técnico (PIO + DMA)
+
+1.  **Cálculo:** La CPU pre-calcula los tiempos de encendido (High) y apagado (Low) para cada paso de la rampa y los guarda en un buffer (`g_ramp_buf`).
+2.  **DMA Rampa:** Un canal DMA envía estos tiempos al FIFO del PIO.
+3.  **DMA Steady:** Al terminar la rampa, el DMA encadena automáticamente a un segundo canal (`dma_steady_ch`) que alimenta al PIO con la velocidad constante de forma circular.
+4.  **PIO:** La máquina de estado toma los valores de tiempo, enciende el pin, espera los ciclos indicados, apaga el pin, espera y repite.
+
+---
+Autor: [Tu Nombre/Organización]
