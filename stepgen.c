#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "stepgen.pio.h"

#if !defined(STEPGEN_PIN)
#define STEPGEN_PIN 16
#endif

#if STEPGEN_PIN >= NUM_BANK0_GPIOS
#error STEPGEN_PIN debe ser < NUM_BANK0_GPIOS
#endif

#ifndef STEPGEN_DMA_MAX_STEPS
#define STEPGEN_DMA_MAX_STEPS 256u
#endif

typedef struct {
    PIO pio;
    uint sm;
    uint offset;
    uint pin;

    int dma_ramp_ch;
    int dma_steady_ch;
    int dma_stop_ch;

    float duty_cycle;
    float freq_start_hz;
    float freq_target_hz;
    uint ramp_steps;

    // Buffers propios para cada instancia
    uint32_t ramp_buf[2u * STEPGEN_DMA_MAX_STEPS];
    uint32_t stop_buf[2u * STEPGEN_DMA_MAX_STEPS];
    uint32_t steady_buf[2];
} stepgen_t;

// Mapa para enrutar interrupciones DMA a la instancia correcta
static stepgen_t *g_dma_ctx_map[NUM_DMA_CHANNELS] = {NULL};

static inline float smoothstep(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static inline uint32_t clamp_u32(uint64_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return (uint32_t)v;
}

static inline void split_total_cycles(uint32_t total_cycles, float duty_cycle, uint32_t *high_cycles_out, uint32_t *low_cycles_out) {
    if (total_cycles < 3u) total_cycles = 3u;

    uint32_t high_cycles = (uint32_t)((float)total_cycles * duty_cycle);
    if (high_cycles < 1u) high_cycles = 1u;
    if (high_cycles >= total_cycles) high_cycles = total_cycles - 1u;

    *high_cycles_out = high_cycles;
    *low_cycles_out = total_cycles - high_cycles;
}

static void build_s_curve_cycles(uint32_t *out_words, uint steps, float f0_hz, float f1_hz, float duty_cycle) {
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    if (steps < 2) steps = 2;

    // Evita valores extremos: el bucle con y-- necesita al menos 1 iteración
    const uint32_t min_cycles = 1u;
    const uint32_t max_cycles = 0x7fffffffu;

    for (uint i = 0; i < steps; ++i) {
        const float t = (float)i / (float)(steps - 1u);
        const float s = smoothstep(t);
        const float f_hz = f0_hz + (f1_hz - f0_hz) * s;

        float safe_f_hz = f_hz;
        if (safe_f_hz < 0.1f) safe_f_hz = 0.1f;

        const uint64_t total_cycles_64 = (uint64_t)((double)sys_hz / (double)safe_f_hz);
        const uint32_t total_cycles = clamp_u32(total_cycles_64, min_cycles + 2u, max_cycles);

        uint32_t high_cycles = 0;
        uint32_t low_cycles = 0;
        split_total_cycles(total_cycles, duty_cycle, &high_cycles, &low_cycles);

        out_words[2u * i + 0u] = high_cycles;
        out_words[2u * i + 1u] = low_cycles;
    }
}

static void build_constant_cycles_pair(uint32_t out_pair[2], float freq_hz, float duty_cycle) {
    const uint32_t sys_hz = clock_get_hz(clk_sys);

    float safe_f_hz = freq_hz;
    if (safe_f_hz < 0.1f) safe_f_hz = 0.1f;

    const uint64_t total_cycles_64 = (uint64_t)((double)sys_hz / (double)safe_f_hz);
    const uint32_t total_cycles = clamp_u32(total_cycles_64, 3u, 0x7fffffffu);

    split_total_cycles(total_cycles, duty_cycle, &out_pair[0], &out_pair[1]);
}

static void stepgen_dma_irq0_handler(void) {
    uint32_t ints = dma_hw->ints0;
    
    // Revisamos todos los canales activos para ver cuál disparó la IRQ
    for (int ch = 0; ch < NUM_DMA_CHANNELS; ch++) {
        if (ints & (1u << ch)) {
            stepgen_t *ctx = g_dma_ctx_map[ch];
            // Si este canal pertenece a una instancia y es su canal de parada:
            if (ctx && ctx->dma_stop_ch == ch) {
                dma_hw->ints0 = 1u << ch; // Limpiar flag
                pio_sm_set_enabled(ctx->pio, ctx->sm, false);
                pio_sm_set_pins(ctx->pio, ctx->sm, 0);
                pio_sm_clear_fifos(ctx->pio, ctx->sm);
            }
        }
    }
}

static void stepgen_init(stepgen_t *ctx, PIO pio, uint sm, uint offset, uint pin) {
    ctx->pio = pio;
    ctx->sm = sm;
    ctx->offset = offset;
    ctx->pin = pin;

    ctx->dma_ramp_ch = dma_claim_unused_channel(true);
    ctx->dma_steady_ch = dma_claim_unused_channel(true);
    ctx->dma_stop_ch = dma_claim_unused_channel(true);

    // Registrar esta instancia en el mapa global para la IRQ
    g_dma_ctx_map[ctx->dma_stop_ch] = ctx;

    irq_set_exclusive_handler(DMA_IRQ_0, stepgen_dma_irq0_handler);
    irq_set_enabled(DMA_IRQ_0, true);
}

static void stepgen_start_s_curve_dma(stepgen_t *ctx, float freq_start_hz, float freq_target_hz, float duty_cycle, uint ramp_steps) {
    if (ramp_steps > STEPGEN_DMA_MAX_STEPS) ramp_steps = STEPGEN_DMA_MAX_STEPS;
    if (ramp_steps < 2u) ramp_steps = 2u;

    ctx->freq_start_hz = freq_start_hz;
    ctx->freq_target_hz = freq_target_hz;
    ctx->duty_cycle = duty_cycle;
    ctx->ramp_steps = ramp_steps;

    build_s_curve_cycles(ctx->ramp_buf, ramp_steps, freq_start_hz, freq_target_hz, duty_cycle);
    build_constant_cycles_pair(ctx->steady_buf, freq_target_hz, duty_cycle);

    const uint dreq = pio_get_dreq(ctx->pio, ctx->sm, true);
    volatile void *pio_txf = &ctx->pio->txf[ctx->sm];

    dma_channel_abort(ctx->dma_ramp_ch);
    dma_channel_abort(ctx->dma_steady_ch);
    dma_channel_abort(ctx->dma_stop_ch);

    pio_sm_set_enabled(ctx->pio, ctx->sm, false);
    pio_sm_clear_fifos(ctx->pio, ctx->sm);
    pio_sm_exec(ctx->pio, ctx->sm, pio_encode_jmp(ctx->offset + stepgen_offset_dma_stream));
    pio_sm_set_enabled(ctx->pio, ctx->sm, true);

    // DMA steady: lectura circular de 2 words (alto/bajo) para pulsos indefinidos
    dma_channel_config c_steady = dma_channel_get_default_config(ctx->dma_steady_ch);
    channel_config_set_transfer_data_size(&c_steady, DMA_SIZE_32);
    channel_config_set_read_increment(&c_steady, true);
    channel_config_set_write_increment(&c_steady, false);
    channel_config_set_dreq(&c_steady, dreq);
    // Ring en READ sobre 8 bytes = 2 words
    channel_config_set_ring(&c_steady, false /* write */, 3 /* 2^3 bytes */);
    dma_channel_configure(
        ctx->dma_steady_ch,
        &c_steady,
        pio_txf,
        ctx->steady_buf,
        0xffffffffu,
        false
    );

    // DMA de rampa: one-shot, y encadena a steady al terminar
    dma_channel_config c_ramp = dma_channel_get_default_config(ctx->dma_ramp_ch);
    channel_config_set_transfer_data_size(&c_ramp, DMA_SIZE_32);
    channel_config_set_read_increment(&c_ramp, true);
    channel_config_set_write_increment(&c_ramp, false);
    channel_config_set_dreq(&c_ramp, dreq);
    channel_config_set_chain_to(&c_ramp, ctx->dma_steady_ch);
    dma_channel_configure(
        ctx->dma_ramp_ch,
        &c_ramp,
        pio_txf,
        ctx->ramp_buf,
        2u * ramp_steps,
        true
    );
}

static void stepgen_stop_s_curve_dma(stepgen_t *ctx, float freq_end_hz, uint ramp_steps) {
    if (ramp_steps > STEPGEN_DMA_MAX_STEPS) ramp_steps = STEPGEN_DMA_MAX_STEPS;
    if (ramp_steps < 2u) ramp_steps = 2u;

    // Construir rampa de bajada: freq_target -> freq_end, y al final deshabilitar SM
    float f_end = freq_end_hz;
    if (f_end < 0.1f) f_end = 0.1f;
    build_s_curve_cycles(ctx->stop_buf, ramp_steps, ctx->freq_target_hz, f_end, ctx->duty_cycle);

    const uint dreq = pio_get_dreq(ctx->pio, ctx->sm, true);
    volatile void *pio_txf = &ctx->pio->txf[ctx->sm];

    dma_channel_abort(ctx->dma_ramp_ch);
    dma_channel_abort(ctx->dma_steady_ch);
    dma_channel_abort(ctx->dma_stop_ch);

    // Hacer que la rampa empiece "ya": limpiar FIFO y reiniciar el bucle del PIO
    pio_sm_set_enabled(ctx->pio, ctx->sm, false);
    pio_sm_clear_fifos(ctx->pio, ctx->sm);
    pio_sm_exec(ctx->pio, ctx->sm, pio_encode_jmp(ctx->offset + stepgen_offset_dma_stream));
    pio_sm_set_enabled(ctx->pio, ctx->sm, true);

    dma_channel_set_irq0_enabled(ctx->dma_stop_ch, true);

    dma_channel_config c_stop = dma_channel_get_default_config(ctx->dma_stop_ch);
    channel_config_set_transfer_data_size(&c_stop, DMA_SIZE_32);
    channel_config_set_read_increment(&c_stop, true);
    channel_config_set_write_increment(&c_stop, false);
    channel_config_set_dreq(&c_stop, dreq);
    dma_channel_configure(
        ctx->dma_stop_ch,
        &c_stop,
        pio_txf,
        ctx->stop_buf,
        2u * ramp_steps,
        true
    );
}

// Helpers bloqueantes (sin DMA). Útiles para pruebas rápidas.
void stepgen_square_wave_ms(stepgen_t *ctx, uint period_ms, float duty_cycle) {
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    const uint64_t total_cycles_64 = (uint64_t)period_ms * ((uint64_t)sys_hz / 1000u);
    const uint32_t total_cycles = clamp_u32(total_cycles_64, 3u, 0x7fffffffu);

    uint32_t high_cycles = 0;
    uint32_t low_cycles = 0;
    split_total_cycles(total_cycles, duty_cycle, &high_cycles, &low_cycles);

    // Reiniciar y configurar para modo infinito
    pio_sm_set_enabled(ctx->pio, ctx->sm, false);
    pio_sm_clear_fifos(ctx->pio, ctx->sm);
    pio_sm_exec(ctx->pio, ctx->sm, pio_encode_jmp(ctx->offset + stepgen_offset_infinite));
    pio_sm_set_enabled(ctx->pio, ctx->sm, true);

    // Enviar los tiempos al FIFO (High, luego Low)
    pio_sm_put_blocking(ctx->pio, ctx->sm, high_cycles);
    pio_sm_put_blocking(ctx->pio, ctx->sm, low_cycles);
}

void stepgen_square_wave_us(stepgen_t *ctx, uint period_us, float duty_cycle) {
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    // Usamos uint64_t para evitar desbordamiento en la multiplicación
    const uint64_t total_cycles_64 = (uint64_t)period_us * (uint64_t)sys_hz / 1000000u;
    const uint32_t total_cycles = clamp_u32(total_cycles_64, 3u, 0x7fffffffu);

    uint32_t high_cycles = 0;
    uint32_t low_cycles = 0;
    split_total_cycles(total_cycles, duty_cycle, &high_cycles, &low_cycles);

    pio_sm_set_enabled(ctx->pio, ctx->sm, false);
    pio_sm_clear_fifos(ctx->pio, ctx->sm);
    pio_sm_exec(ctx->pio, ctx->sm, pio_encode_jmp(ctx->offset + stepgen_offset_infinite));
    pio_sm_set_enabled(ctx->pio, ctx->sm, true);

    pio_sm_put_blocking(ctx->pio, ctx->sm, high_cycles);
    pio_sm_put_blocking(ctx->pio, ctx->sm, low_cycles);
}

void stepgen_square_wave_ns(stepgen_t *ctx, uint period_ns, float duty_cycle) {
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    const uint64_t total_cycles_64 = (uint64_t)period_ns * (uint64_t)sys_hz / 1000000000u;
    const uint32_t total_cycles = clamp_u32(total_cycles_64, 3u, 0x7fffffffu);

    uint32_t high_cycles = 0;
    uint32_t low_cycles = 0;
    split_total_cycles(total_cycles, duty_cycle, &high_cycles, &low_cycles);

    pio_sm_set_enabled(ctx->pio, ctx->sm, false);
    pio_sm_clear_fifos(ctx->pio, ctx->sm);
    pio_sm_exec(ctx->pio, ctx->sm, pio_encode_jmp(ctx->offset + stepgen_offset_infinite));
    pio_sm_set_enabled(ctx->pio, ctx->sm, true);

    pio_sm_put_blocking(ctx->pio, ctx->sm, high_cycles);
    pio_sm_put_blocking(ctx->pio, ctx->sm, low_cycles);
}

void stepgen_burst_ms(stepgen_t *ctx, uint count, uint period_ms, float duty_cycle) {
    if (count == 0) return;
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    const uint64_t total_cycles_64 = (uint64_t)period_ms * ((uint64_t)sys_hz / 1000u);
    const uint32_t total_cycles = clamp_u32(total_cycles_64, 3u, 0x7fffffffu);

    uint32_t high_cycles = 0;
    uint32_t low_cycles = 0;
    split_total_cycles(total_cycles, duty_cycle, &high_cycles, &low_cycles);

    pio_sm_set_enabled(ctx->pio, ctx->sm, false);
    pio_sm_clear_fifos(ctx->pio, ctx->sm);
    pio_sm_exec(ctx->pio, ctx->sm, pio_encode_jmp(ctx->offset + stepgen_offset_burst));
    pio_sm_set_enabled(ctx->pio, ctx->sm, true);

    pio_sm_put_blocking(ctx->pio, ctx->sm, count - 1); // N-1 porque el bucle es do-while
    pio_sm_put_blocking(ctx->pio, ctx->sm, high_cycles);
    pio_sm_put_blocking(ctx->pio, ctx->sm, low_cycles);
}

void stepgen_burst_us(stepgen_t *ctx, uint count, uint period_us, float duty_cycle) {
    if (count == 0) return;
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    const uint64_t total_cycles_64 = (uint64_t)period_us * (uint64_t)sys_hz / 1000000u;
    const uint32_t total_cycles = clamp_u32(total_cycles_64, 3u, 0x7fffffffu);

    uint32_t high_cycles = 0;
    uint32_t low_cycles = 0;
    split_total_cycles(total_cycles, duty_cycle, &high_cycles, &low_cycles);

    pio_sm_set_enabled(ctx->pio, ctx->sm, false);
    pio_sm_clear_fifos(ctx->pio, ctx->sm);
    pio_sm_exec(ctx->pio, ctx->sm, pio_encode_jmp(ctx->offset + stepgen_offset_burst));
    pio_sm_set_enabled(ctx->pio, ctx->sm, true);

    pio_sm_put_blocking(ctx->pio, ctx->sm, count - 1);
    pio_sm_put_blocking(ctx->pio, ctx->sm, high_cycles);
    pio_sm_put_blocking(ctx->pio, ctx->sm, low_cycles);
}

void stepgen_burst_ns(stepgen_t *ctx, uint count, uint period_ns, float duty_cycle) {
    if (count == 0) return;
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    const uint64_t total_cycles_64 = (uint64_t)period_ns * (uint64_t)sys_hz / 1000000000u;
    const uint32_t total_cycles = clamp_u32(total_cycles_64, 3u, 0x7fffffffu);

    uint32_t high_cycles = 0;
    uint32_t low_cycles = 0;
    split_total_cycles(total_cycles, duty_cycle, &high_cycles, &low_cycles);

    pio_sm_set_enabled(ctx->pio, ctx->sm, false);
    pio_sm_clear_fifos(ctx->pio, ctx->sm);
    pio_sm_exec(ctx->pio, ctx->sm, pio_encode_jmp(ctx->offset + stepgen_offset_burst));
    pio_sm_set_enabled(ctx->pio, ctx->sm, true);

    pio_sm_put_blocking(ctx->pio, ctx->sm, count - 1);
    pio_sm_put_blocking(ctx->pio, ctx->sm, high_cycles);
    pio_sm_put_blocking(ctx->pio, ctx->sm, low_cycles);
}

int main(void) {
    PIO pio;
    uint sm;
    uint offset;

    // Instancia para el Motor 1
    static stepgen_t motor1;

    setup_default_uart();

    // Carga el programa PIO y reclama una SM libre para el GPIO configurado
    bool success = pio_claim_free_sm_and_add_program_for_gpio_range(&stepgen_program, &pio, &sm, &offset, STEPGEN_PIN, 1, true);
    hard_assert(success);

    printf("Motor 1 STEP pin: GPIO %d\n", STEPGEN_PIN);
    stepgen_program_init(pio, sm, offset, STEPGEN_PIN);

    // Inicializamos la estructura del Motor 1
    stepgen_init(&motor1, pio, sm, offset, STEPGEN_PIN);

    printf("Iniciando Motor 1 por DMA...\n");

    // Prueba bloqueante (sin DMA):
    // stepgen_square_wave_ns(&motor1, 100, 0.5f);
    
    // Ejemplo: pulsos infinitos por DMA con rampa S de arranque
    stepgen_start_s_curve_dma(&motor1, 10.0f, 1000.0f, 0.5f, 128u);
    // Para detener: llamar cuando quieras
    // stepgen_stop_s_curve_dma(&motor1, 10.0f, 128u);

    // El PIO + DMA se encargan de todo; el CPU puede dormir o hacer otras tareas
    while (true) {
        tight_loop_contents();
    }
}
